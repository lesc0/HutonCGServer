// cg-editor 파일 서버: 에디터(Cloudflare Worker 런타임)가 할 수 없는 디스크 접근을 맡는다.
//   bin/media   : 이미지·영상·음성 원본. 에디터는 여기 있는 파일만 가져온다(목록 + 스트리밍).
//   bin/project : 프로젝트 JSON 저장/열기.
//
//   GET  /media[?kind=image|video|audio]   목록
//   GET  /media/<파일>                      파일 (Range 지원, 영상 탐색용)
//   GET  /projects                          프로젝트 목록
//   GET  /projects/<이름>                    프로젝트 JSON
//   PUT  /projects/<이름>                    프로젝트 저장 (data: 미디어는 bin/media 로 풀고 ../media/<파일> 로 바꿈)
//   DELETE /projects/<이름>                  프로젝트 삭제 (bin/media 의 미디어는 지우지 않음)
//
//   /ctl/*  : cg-streamer 의 HTTP 컨트롤 포트(기본 127.0.0.1:5555)로 그대로 중계.
//             브라우저가 직접 그 포트를 부르면 CORS 로 막히므로 같은 오리진인 이 서버를 거친다.
//             예) POST /ctl/next  GET /ctl/status  POST /ctl/goto/2
//
// 환경변수: CG_FILES_PORT(8081) CG_FILES_HOST(127.0.0.1) CG_MEDIA_DIR CG_PROJECT_DIR
//          CG_CTL_PORT(5555) CG_CTL_HOST(127.0.0.1) CG_CTL_TOKEN(없음, cg-streamer --token 과 맞출 것)
// 의존성 없음(Node 내장 모듈만).
import http from 'node:http';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');                       // 저장소 루트 (CGServer)
const MEDIA_DIR = path.resolve(process.env.CG_MEDIA_DIR || path.join(root, 'bin/media'));
const PROJECT_DIR = path.resolve(process.env.CG_PROJECT_DIR || path.join(root, 'bin/project'));
const PORT = Number(process.env.CG_FILES_PORT || 8081);
const HOST = process.env.CG_FILES_HOST || '127.0.0.1';
const CTL_PORT = Number(process.env.CG_CTL_PORT || 5555);
const CTL_HOST = process.env.CG_CTL_HOST || '127.0.0.1';
const CTL_TOKEN = process.env.CG_CTL_TOKEN || '';
const MAX_PROJECT = 50 * 1024 * 1024;

const KINDS = {
  image: {png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', webp: 'image/webp', gif: 'image/gif'},
  video: {mp4: 'video/mp4', webm: 'video/webm', mov: 'video/quicktime'},
  audio: {mp3: 'audio/mpeg', wav: 'audio/wav', ogg: 'audio/ogg', m4a: 'audio/mp4'},
};
const TYPES = Object.fromEntries(Object.entries(KINDS).flatMap(([kind, exts]) => Object.entries(exts).map(([ext, mime]) => [ext, {kind, mime}])));
const EXT_OF_MIME = {'image/png': 'png', 'image/jpeg': 'jpg', 'image/webp': 'webp', 'image/gif': 'gif', 'video/mp4': 'mp4', 'video/webm': 'webm',
  'video/quicktime': 'mov', 'audio/mpeg': 'mp3', 'audio/wav': 'wav', 'audio/x-wav': 'wav', 'audio/ogg': 'ogg', 'audio/mp4': 'm4a'};

const extOf = (name) => path.extname(name).slice(1).toLowerCase();
// 경로 구분자/상위 경로/제어문자가 없는 평범한 파일 이름만 허용
const safeName = (name) => typeof name === 'string' && name.length > 0 && name.length <= 200 && name !== '.' && name !== '..'
  && !/[\\/:*?"<>|\x00-\x1f]/.test(name) && path.basename(name) === name;

function cors(req, res) {
  const origin = req.headers.origin;
  if (!origin) return;
  try {
    const o = new URL(origin).hostname, h = String(req.headers.host || '').replace(/:\d+$/, '').replace(/^\[|\]$/g, '');
    const loopback = ['localhost', '127.0.0.1', '::1', '[::1]'];
    if (o === h || (loopback.includes(o) && loopback.includes(h))) {     // 에디터와 같은 호스트에서 온 요청만
      res.setHeader('Access-Control-Allow-Origin', origin);
      res.setHeader('Vary', 'Origin');
      res.setHeader('Access-Control-Allow-Methods', 'GET,PUT,POST,DELETE,OPTIONS');
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type,Range');
      res.setHeader('Access-Control-Expose-Headers', 'Content-Range,Accept-Ranges,Content-Length');
    }
  } catch { /* 잘못된 Origin 은 CORS 헤더를 주지 않는다 */ }
}

const json = (res, code, body) => {
  const text = JSON.stringify(body);
  res.writeHead(code, {'Content-Type': 'application/json; charset=utf-8', 'Content-Length': Buffer.byteLength(text), 'Cache-Control': 'no-store'});
  res.end(text);
};
const fail = (res, code, error) => json(res, code, {error});

async function listMedia(kind) {
  const entries = (await fsp.readdir(MEDIA_DIR, {withFileTypes: true}).catch(() => [])).filter((e) => {
    const t = e.isFile() && TYPES[extOf(e.name)];
    return t && (!kind || t.kind === kind);
  });
  const out = await Promise.all(entries.map(async (e) => {
    const st = await fsp.stat(path.join(MEDIA_DIR, e.name));
    return {name: e.name, kind: TYPES[extOf(e.name)].kind, size: st.size, updated: st.mtimeMs};
  }));
  return out.sort((a, b) => a.name.localeCompare(b.name, 'ko'));
}

function serveMedia(req, res, name) {
  const t = TYPES[extOf(name)];
  const file = path.join(MEDIA_DIR, name);
  if (!safeName(name) || !t) return fail(res, 404, '지원하지 않는 파일입니다.');
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) return fail(res, 404, '파일이 없습니다: ' + name);
    const etag = `"${st.size.toString(16)}-${Math.floor(st.mtimeMs).toString(16)}"`;
    const headers = {'Content-Type': t.mime, 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-cache', 'ETag': etag};
    if (!req.headers.range && req.headers['if-none-match'] === etag) { res.writeHead(304, {'ETag': etag, 'Cache-Control': 'no-cache'}); return res.end(); }
    const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
    if (m && (m[1] || m[2])) {
      let start = m[1] ? Number(m[1]) : Math.max(0, st.size - Number(m[2])), end = m[1] && m[2] ? Number(m[2]) : st.size - 1;
      end = Math.min(end, st.size - 1);
      if (start > end || start >= st.size) { res.writeHead(416, {'Content-Range': `bytes */${st.size}`}); return res.end(); }
      res.writeHead(206, {...headers, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1});
      return req.method === 'HEAD' ? res.end() : fs.createReadStream(file, {start, end}).pipe(res);
    }
    res.writeHead(200, {...headers, 'Content-Length': st.size});
    req.method === 'HEAD' ? res.end() : fs.createReadStream(file).pipe(res);
  });
}

// 프로젝트 이름 -> 파일 이름 ("a/b" 같은 이름이 경로가 되지 않도록 정리)
const projectFile = (name) => {
  const base = String(name).replace(/\.json$/i, '').replace(/[\\/:*?"<>|\x00-\x1f]/g, '_').trim().slice(0, 100);
  return base && base !== '.' && base !== '..' ? base + '.json' : null;
};

// 프로젝트 안의 data: 미디어를 bin/media 파일로 풀고 src 를 ../media/<파일> 로 바꾼다.
async function extractMedia(node) {
  let count = 0;
  const walk = async (v) => {
    if (Array.isArray(v)) { for (const x of v) await walk(x); return; }
    if (!v || typeof v !== 'object') return;
    for (const [k, x] of Object.entries(v)) {
      if (k === 'src' && typeof x === 'string' && x.startsWith('data:')) {
        const m = /^data:([\w.+-]+\/[\w.+-]+);base64,([A-Za-z0-9+/=]+)$/.exec(x);
        const ext = m && EXT_OF_MIME[m[1]];
        if (!ext) throw new Error('지원하지 않는 미디어 형식입니다.');
        const buf = Buffer.from(m[2], 'base64');
        const name = 'import-' + crypto.createHash('sha1').update(buf).digest('hex').slice(0, 12) + '.' + ext;
        const file = path.join(MEDIA_DIR, name);
        if (!(await fsp.access(file).then(() => true, () => false))) await fsp.writeFile(file, buf);
        v[k] = '../media/' + name;
        count++;
      } else await walk(x);
    }
  };
  await walk(node);
  return count;
}

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = []; let size = 0;
    req.on('data', (c) => { size += c.length; if (size > limit) { reject(Object.assign(new Error('too large'), {code: 413})); req.destroy(); } else chunks.push(c); });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

// cg-streamer 의 HTTP 컨트롤 포트로 요청을 그대로 넘긴다 (메서드/본문 보존, 토큰은 여기서 붙임).
function proxyCtl(req, res, segRest, search) {
  const headers = {};
  if (CTL_TOKEN) headers['Authorization'] = 'Bearer ' + CTL_TOKEN;
  if (req.headers['content-type']) headers['Content-Type'] = req.headers['content-type'];
  // Content-Length 를 그대로 넘겨야 한다: 없으면 Node 가 chunked 로 보내는데
  // cg-streamer 의 단순 HTTP 서버는 chunked 를 모르고 Content-Length 만 본다(본문이 빈 것으로 읽힘).
  if (req.headers['content-length']) headers['Content-Length'] = req.headers['content-length'];
  const preq = http.request({host: CTL_HOST, port: CTL_PORT, method: req.method,
    path: '/' + segRest.map(encodeURIComponent).join('/') + (search || ''), headers}, (pres) => {
    res.writeHead(pres.statusCode || 502, {'Content-Type': pres.headers['content-type'] || 'application/json; charset=utf-8', 'Cache-Control': 'no-store'});
    pres.pipe(res);
  });
  preq.on('error', () => fail(res, 502, '송출 엔진(cg-streamer)에 연결하지 못했습니다. 실행 중인지 확인하세요.'));
  req.pipe(preq);
}

const server = http.createServer(async (req, res) => {
  cors(req, res);
  try {
    if (req.method === 'OPTIONS') { res.writeHead(204); return res.end(); }
    const url = new URL(req.url, 'http://x');
    const seg = url.pathname.split('/').filter(Boolean).map((s) => decodeURIComponent(s));

    if (seg[0] === 'media' && ['GET', 'HEAD'].includes(req.method)) {
      if (seg.length === 1) return json(res, 200, {dir: MEDIA_DIR, files: await listMedia(url.searchParams.get('kind') || '')});
      if (seg.length === 2) return serveMedia(req, res, seg[1]);
    }
    if (seg[0] === 'projects') {
      if (seg.length === 1 && req.method === 'GET') {
        const entries = (await fsp.readdir(PROJECT_DIR, {withFileTypes: true}).catch(() => [])).filter((e) => e.isFile() && /\.json$/i.test(e.name));
        const out = await Promise.all(entries.map(async (e) => {
          const st = await fsp.stat(path.join(PROJECT_DIR, e.name));
          return {name: e.name.replace(/\.json$/i, ''), size: st.size, updated: st.mtimeMs};
        }));
        return json(res, 200, {dir: PROJECT_DIR, projects: out.sort((a, b) => b.updated - a.updated)});
      }
      if (seg.length === 2) {
        const file = projectFile(seg[1]);
        if (!file) return fail(res, 400, '프로젝트 이름이 올바르지 않습니다.');
        const target = path.join(PROJECT_DIR, file);
        if (req.method === 'GET') {
          const text = await fsp.readFile(target, 'utf8').catch(() => null);
          return text === null ? fail(res, 404, '프로젝트가 없습니다: ' + seg[1]) : (res.writeHead(200, {'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store'}), res.end(text));
        }
        if (req.method === 'DELETE') {
          try { await fsp.unlink(target); } catch (e) { return e.code === 'ENOENT' ? fail(res, 404, '프로젝트가 없습니다: ' + seg[1]) : fail(res, 500, '삭제하지 못했습니다.'); }
          return json(res, 200, {ok: true, name: file.replace(/\.json$/i, '')});
        }
        if (req.method === 'PUT') {
          let project;
          try { project = JSON.parse((await readBody(req, MAX_PROJECT)).toString('utf8')); } catch (e) {
            return fail(res, e.code === 413 ? 413 : 400, e.code === 413 ? '프로젝트가 50MB 를 넘습니다.' : '프로젝트 파일을 읽지 못했습니다.');
          }
          if (!project || project.format !== 'cg-editor' || !Array.isArray(project.pages)) return fail(res, 400, 'CG 편집기 프로젝트가 아닙니다.');
          const extracted = await extractMedia(project);
          const tmp = target + '.tmp-' + process.pid;               // 쓰다가 끊겨도 기존 파일이 깨지지 않게 임시 파일 후 교체
          await fsp.writeFile(tmp, JSON.stringify(project));
          await fsp.rename(tmp, target);
          const st = await fsp.stat(target);
          return json(res, 200, {name: file.replace(/\.json$/i, ''), updated: st.mtimeMs, extractedMedia: extracted, project});
        }
      }
    }
    if (seg[0] === 'ctl' && seg.length > 1) return proxyCtl(req, res, seg.slice(1), url.search);
    fail(res, 404, 'not found');
  } catch (e) {
    console.error('[files]', e);
    fail(res, 500, e instanceof Error ? e.message : '서버 오류');
  }
});

fs.mkdirSync(MEDIA_DIR, {recursive: true});
fs.mkdirSync(PROJECT_DIR, {recursive: true});
server.listen(PORT, HOST, () => {
  console.log(`[files] http://${HOST}:${PORT}  media=${MEDIA_DIR}  project=${PROJECT_DIR}`);
});
