// cg-runtime: cg-editor 프로젝트(JSON)를 CEF 에서 재생하는 실행엔진.
// cg-editor 의 model.ts / effects.ts / text-render.ts / editor-canvas.tsx 를 Konva 없이 Canvas2D 로 옮긴 것.
// 에디터 쪽 로직을 바꾸면 여기도 맞춰야 한다(미리보기 = 방송 출력).
//
// 네이티브(cg-streamer)와의 약속
//   cefQuery('load')  : 프로젝트 JSON 문자열
//   cefQuery('base')  : 프로젝트 폴더의 file:// URL (이미지 상대경로 기준)
//   cefQuery('video:play:<경로>' | 'video:rect:x,y,w,h' | 'video:stop') : 영상은 네이티브(MPP)가 그림
//   cefQuery('state:<json>') : 상태 보고 (HTTP /status 용)
//   cefQuery('ready') : 첫 화면 준비 완료 -> 인코딩 시작
//   window.cg.cmd(name, arg, body) : HTTP 명령 진입점
'use strict';
const W = 1920, H = 1080;

// ---------- 기본값 (model.ts make) ----------
const ITEM_DEFAULTS = {
  type: 'rect', name: '', text: '', x: 250, y: 400, w: 1400, h: 180, size: 100, fill: '#ffffff', stroke: '#000000',
  strokeWidth: 0, edge2: '#ffffff', edge2Width: 0, edge3: '#000000', edge3Width: 0, bold: false, italic: false,
  underline: false, outline: false, family: 'Arial', align: 'left', opacity: 1, rotation: 0, cRotate: 0,
  textWidth: 100, space: 100, thickness: 0, kerning: 0, leading: 20, flipX: false, flipY: false, shadow: false,
  shadowColor: '#000000', shadowBlur: 4, shadowDepth: 8, shadowAngle: 45, hidden: false, start: 0, duration: 10,
  effect: 'none', outEffect: 'none', inDuration: 1, outDuration: 1, direction: 'left', speed: 1, volume: 1, trim: 0,
  mediaLoop: false, background: false, runs: [], clockFormat: 'HH:mm:ss', timerSeconds: 300, timerCount: 'down',
  moves: [], effectPreset: 0, tileX: 8, tileY: 8, softness: 0, effectBorder: 0, curlRadius: 60, effectAngle: 0, blinkCount: 4,
};
const PAGE_OPTION_KEYS = ['direction', 'effectPreset', 'tileX', 'tileY', 'softness', 'effectBorder', 'curlRadius',
  'effectAngle', 'blinkCount', 'speed'];

const fixItem = (i) => Object.assign({}, ITEM_DEFAULTS, i);
function fixPage(p) {
  return Object.assign({
    bg: 'transparent', items: [], duration: 30, mode: 'Still', effect: 'none', outEffect: 'none',
    inDuration: 1, outDuration: 1,
  }, p, {
    options: Object.assign({ direction: 'left', effectPreset: 0, tileX: 8, tileY: 8, softness: 0, effectBorder: 0,
      curlRadius: 60, effectAngle: 0, blinkCount: 4, speed: 1 }, p.options),
    items: (p.items || []).map(fixItem),
  });
}
function fixProject(d) {
  if (!d || d.format !== 'cg-editor' || !Array.isArray(d.pages) || !d.pages.length) throw new Error('cg-editor 프로젝트가 아님');
  const pr = { name: d.name || '', pages: d.pages.map(fixPage), channels: null,
    runSettings: Object.assign({ allPages: true, mode: 'auto', startPage: 1, endPage: d.pages.length, loops: 1,
      loopDelay: 0 }, d.runSettings) };
  if (d.channels) {
    pr.channels = {
      stamps: [0, 1].map((n) => {
        const v = d.channels.stamps && d.channels.stamps[n];
        return v ? { name: v.name || 'Stamp ' + (n + 1), page: fixPage(v.page), position: v.position === 'bottom' ? 'bottom' : 'top',
          mode: v.mode === 'manual' ? 'manual' : 'auto', loops: v.loops || 100, delay: v.delay || 0 } : null;
      }),
      global: d.channels.global ? Object.assign({ loops: 0, mark: false }, d.channels.global, { video: fixItem(d.channels.global.video) }) : null,
    };
  }
  return pr;
}

// ---------- 시간 -> 상태 (model.ts) ----------
function clockText(i, time, now = new Date()) {
  if (i.type === 'timer') {
    const s = i.timerCount === 'down' ? Math.max(0, i.timerSeconds - Math.floor(Math.max(0, time - i.start)))
      : i.timerSeconds + Math.max(0, Math.floor(time - i.start));
    return [Math.floor(s / 3600), Math.floor(s % 3600 / 60), s % 60].map((n) => String(n).padStart(2, '0')).join(':');
  }
  const p2 = (n) => String(n).padStart(2, '0');
  return i.clockFormat.replaceAll('YYYY', String(now.getFullYear())).replaceAll('MM', p2(now.getMonth() + 1))
    .replaceAll('DD', p2(now.getDate())).replaceAll('HH', p2(now.getHours())).replaceAll('mm', p2(now.getMinutes()))
    .replaceAll('ss', p2(now.getSeconds()));
}

function effectState(i, time, mode, duration) {
  const active = time >= i.start && time <= i.start + i.duration;
  const t = Math.max(0, time - i.start);
  const end = i.start + i.duration - time;
  const f = Math.min(1, t / Math.max(0.01, i.inDuration)), out = Math.min(1, end / Math.max(0.01, i.outDuration));
  let effect = i.effect, progress = f;
  if (end < i.outDuration && i.outEffect !== 'none') { effect = i.outEffect; progress = out; }
  if ((effect === 'crawl' || effect === 'roll') || (!i.background && (mode === 'Crawl' || mode === 'Roll'))) {
    effect = mode === 'Roll' ? 'roll' : mode === 'Crawl' ? 'crawl' : effect;
    progress = t / Math.max(0.1, i.duration);
  }
  return { active, effect, progress: Math.max(0, Math.min(1, progress)), elapsed: t, duration };
}

function playbackStep(pages, s, index, time, cycle, delta) {
  const start = s.allPages ? 0 : Math.max(0, Math.min(pages.length - 1, s.startPage - 1));
  const end = s.allPages ? pages.length - 1 : Math.max(start, Math.min(pages.length - 1, s.endPage - 1));
  let next = time + Math.max(0, delta), i = Math.max(start, Math.min(end, index)), c = cycle;
  for (let guard = 0; guard < 10000; guard++) {
    const duration = pages[i].duration;
    if (next < duration) return { index: i, time: next, cycle: c, playing: true };
    if (s.mode === 'manual') return { index: i, time: duration, cycle: c, playing: false };
    if (i < end) { next -= duration; i++; continue; }
    if (c >= s.loops) return { index: i, time: duration, cycle: c, playing: false };
    if (next < duration + s.loopDelay) return { index: i, time: next, cycle: c, playing: true };
    next -= duration + s.loopDelay; i = start; c++;
  }
  return { index: i, time: Math.min(next, pages[i].duration), cycle: c, playing: false };
}

const idleChannel = () => ({ time: 0, cycle: 1, playing: false, visible: false });
function channelStep(st, delta, duration, loops, delay = 0, manual = false) {
  if (!st.playing) return st;
  const length = Math.max(0.1, duration), period = length + Math.max(0, delay), total = st.time + Math.max(0, delta);
  if (manual && total >= length) return Object.assign({}, st, { time: length, playing: false });
  const cycle = st.cycle + Math.floor(total / period);
  if (loops > 0 && cycle > loops) return Object.assign({}, st, { time: length, cycle: loops, playing: false, visible: false });
  return Object.assign({}, st, { time: total % period, cycle });
}

// ---------- 위치 이동 (model.ts moveOffset): moves=[{t,dur,x?,y?}] ----------
function moveOffset(i, time) {
  let cx = i.x, cy = i.y;
  for (const m of [...(i.moves || [])].sort((a, b) => a.t - b.t)) {
    const tx = m.x ?? cx, ty = m.y ?? cy;
    if (time >= m.t + m.dur) { cx = tx; cy = ty; continue; }
    if (time > m.t) {
      const q = Math.max(0, Math.min(1, (time - m.t) / Math.max(0.001, m.dur))), e = q < 0.5 ? 4 * q * q * q : 1 - Math.pow(-2 * q + 2, 3) / 2;
      return { x: cx + (tx - cx) * e - i.x, y: cy + (ty - cy) * e - i.y };
    }
    break;
  }
  return { x: cx - i.x, y: cy - i.y };
}

// ---------- 효과 (effects.ts visual) ----------
function visual(i, time, mode, duration) {
  const state = effectState(i, time, mode, duration), q = state.progress, f = state.effect, d = i.direction, preset = i.effectPreset;
  const v = { opacity: 1, x: 0, y: 0, scaleX: 1, scaleY: 1, rotation: 0 };
  if (f === 'fade') v.opacity = q;
  if (f === 'blink') v.opacity = Math.floor(state.elapsed * Math.max(1, i.blinkCount) * 2 / Math.max(1, i.duration)) % 2 ? 0 : 1;
  if (f === 'move') {
    v.x = (d === 'left' ? -W : d === 'right' ? W : 0) * (1 - q);
    v.y = (d === 'up' ? -H : d === 'down' ? H : 0) * (1 - q);
    if (preset >= 4) { v.x = (preset % 2 ? -W : W) * (1 - q); v.y = (preset % 4 < 2 ? -H : H) * (1 - q); }
  }
  if (f === 'scale') {
    v.scaleX = preset === 5 ? 1 : q; v.scaleY = preset === 4 ? 1 : q;
    v.x = d === 'right' ? i.w * (1 - v.scaleX) : d === 'left' ? 0 : i.w * (1 - v.scaleX) / 2;
    v.y = d === 'down' ? i.h * (1 - v.scaleY) : d === 'up' ? 0 : i.h * (1 - v.scaleY) / 2;
  }
  if (f === 'crawl') v.x = (d === 'right' ? 1 : -1) * q * (W + i.w) * i.speed;
  if (f === 'roll') v.y = (d === 'down' ? 1 : -1) * q * (H + i.h) * i.speed;
  if (f === 'wipe' || f === 'banner' || f === 'curl') {
    v.clip = (c) => {
      c.beginPath();
      if (preset % 8 === 4) c.rect(i.w * (1 - q) / 2, 0, i.w * q, i.h);
      else if (preset % 8 === 5) c.rect(0, i.h * (1 - q) / 2, i.w, i.h * q);
      else if (preset % 8 === 6) { c.moveTo(0, 0); c.lineTo(i.w * q * 2, 0); c.lineTo(0, i.h * q * 2); }
      else if (preset % 8 === 7) { c.moveTo(i.w, 0); c.lineTo(i.w - i.w * q * 2, 0); c.lineTo(i.w, i.h * q * 2); }
      else if (d === 'up' || d === 'down') c.rect(0, d === 'up' ? i.h * (1 - q) : 0, i.w, i.h * q);
      else c.rect(d === 'left' ? i.w * (1 - q) : 0, 0, i.w * q, i.h);
      c.closePath();
    };
    if (f === 'curl') {
      v.rotation = i.effectAngle * (1 - q);
      if (q > 0 && q < 1) v.fold = { x: i.w * q, width: Math.min(i.curlRadius * 2, i.w * q, i.w * (1 - q)), height: Math.min(i.h, i.curlRadius * 2) };
    }
    if (f === 'banner') {
      v.y = (1 - q) * i.h * (d === 'up' ? -1 : 1);
      v.scaleY = Math.max(0.01, q);
      v.rotation = Math.sin(q * Math.PI) * (preset % 2 ? 8 : -8);
    }
  }
  if ((f === 'wipe' || f === 'organic') && i.softness > 0 && q < 1) v.opacity = Math.min(1, q * 4);
  if (f === 'organic') {
    v.clip = (c) => {
      c.beginPath();
      const nx = 16, ny = 12;
      for (let a = 0; a < nx; a++) for (let b = 0; b < ny; b++) {
        const seed = ((a * 73856093) ^ (b * 19349663) ^ ((preset + 1) * 83492791)) >>> 0;
        const t = (seed % 1000) / 1000, k = Math.max(0, Math.min(1, (q - t) * 4));
        if (k > 0) {
          const w = i.w / nx, h = i.h / ny;
          if (preset % 3 === 0) c.rect(a * w, b * h, w * k, h * k);
          else if (preset % 3 === 1) c.rect(a * w + (1 - k) * w / 2, b * h, w * k, h);
          else { c.moveTo(a * w + w / 2 + w * k, b * h + h / 2); c.arc(a * w + w / 2, b * h + h / 2, Math.hypot(w, h) * k / 2, 0, Math.PI * 2); }
        }
      }
      if (q === 1) c.rect(0, 0, i.w, i.h);
      c.closePath();
    };
  }
  if (f === 'tile') {
    v.clip = (c) => {
      c.beginPath();
      const nx = Math.min(32, Math.max(1, i.tileX)), ny = Math.min(32, Math.max(1, i.tileY));
      for (let a = 0; a < nx; a++) for (let b = 0; b < ny; b++) {
        const offset = preset % 3 === 0 ? 0 : preset % 3 === 1 ? (a + b) / (nx + ny) * 0.7 : (((a * 17 + b * 31) % 97) / 97) * 0.7;
        const k = Math.max(0, Math.min(1, (q - offset) / (1 - offset)));
        c.rect(a * i.w / nx + (1 - k) * i.w / nx / 2, b * i.h / ny + (1 - k) * i.h / ny / 2, i.w * k / nx, i.h * k / ny);
      }
      c.closePath();
    };
  }
  return v;
}

// ---------- 글자 그리기 (text-render.ts drawText) ----------
function drawText(c, item, time) {
  const text = item.type === 'clock' || item.type === 'timer' ? clockText(item, time) : item.text;
  const lines = [];
  let line = { chars: [], width: 0, height: item.size };
  const scale = item.textWidth / 100;
  let at = 0;
  for (const char of text) {
    const run = item.runs.reduce((style, r) => (at >= r.start && at < r.end ? Object.assign({}, style, r) : style), {});
    const size = run.size ?? item.size;
    const font = `${(run.italic ?? item.italic) ? 'italic ' : ''}${(run.bold ?? item.bold) ? 'bold ' : ''}${size}px "${run.family ?? item.family}"`;
    c.font = font;
    const width = (c.measureText(char).width * (char === ' ' ? item.space / 100 : 1) + item.kerning) * scale;
    if (char === '\n' || (line.width + width > item.w - 8 && line.chars.length)) {
      lines.push(line);
      line = { chars: [], width: 0, height: item.size };
    }
    if (char !== '\n') {
      line.chars.push({ char, width, font, fill: run.fill ?? item.fill, size, rotate: run.rotate ?? item.cRotate,
        underline: run.underline ?? item.underline, start: at });
      line.width += width;
      line.height = Math.max(line.height, size);
    }
    at += char.length;
  }
  lines.push(line);
  let y = 4;
  for (const l of lines) {
    if (y > item.h) break;
    let x = item.align === 'center' ? (item.w - l.width) / 2 : item.align === 'right' ? item.w - l.width - 4 : 4;
    for (const g of l.chars) {
      c.save();
      c.font = g.font; c.textBaseline = 'top'; c.lineJoin = 'round';
      c.translate(x + g.width / 2, y + g.size / 2);
      c.rotate(-g.rotate * Math.PI / 180);
      c.scale(scale, 1);
      c.translate(-g.width / (2 * scale), -g.size / 2);
      if (item.shadow) {
        c.shadowColor = item.shadowColor; c.shadowBlur = item.shadowBlur;
        c.shadowOffsetX = Math.cos(item.shadowAngle * Math.PI / 180) * item.shadowDepth;
        c.shadowOffsetY = Math.sin(item.shadowAngle * Math.PI / 180) * item.shadowDepth;
      }
      const e1 = item.strokeWidth, e2 = e1 + item.edge2Width, e3 = e2 + item.edge3Width;
      for (const [w, col] of [[e3, item.edge3], [e2, item.edge2], [e1, item.stroke]]) {
        if (w > 0) { c.lineWidth = w * 2; c.strokeStyle = col; c.strokeText(g.char, 0, 0); }
      }
      if (!item.outline) {
        c.fillStyle = g.fill; c.fillText(g.char, 0, 0);
        if (item.thickness > 0) { c.lineWidth = item.thickness; c.strokeStyle = g.fill; c.strokeText(g.char, 0, 0); }
      }
      if (g.underline) { c.fillStyle = g.fill; c.fillRect(0, g.size, Math.max(1, g.width / scale), Math.max(1, g.size / 20)); }
      c.restore();
      x += g.width;
    }
    y += l.height * (1 + item.leading / 100);
  }
}

// ---------- 화면 그리기 (editor-canvas.tsx, 실행 모드) ----------
const images = new Map();   // src -> HTMLImageElement
let baseUrl = '';
function imageFor(src) {
  if (!src) return null;
  if (!images.has(src)) {
    const im = new Image();
    im.onload = () => { dirty = true; };
    im.onerror = () => console.warn('[cg] 이미지 로드 실패', src);
    im.src = /^(data:|file:|https?:)/.test(src) ? src : baseUrl + src;
    images.set(src, im);
  }
  const im = images.get(src);
  return im.complete && im.naturalWidth ? im : null;
}

function pathRounded(c, i) {   // 사각형 / 타원
  c.beginPath();
  if (i.type === 'ellipse') c.ellipse(i.w / 2, i.h / 2, i.w / 2, i.h / 2, 0, 0, Math.PI * 2);
  else c.rect(0, 0, i.w, i.h);
}

function drawItem(c, item, time, page) {
  if (item.hidden || item.type === 'audio' || item.type === 'video') return;   // 영상은 네이티브가 그림
  const state = effectState(item, time, page.mode, page.duration);
  if (!state.active) return;
  const v = visual(item, time, page.mode, page.duration);
  c.save();
  c.globalAlpha *= item.opacity * v.opacity;
  const mo = moveOffset(item, time);
  c.translate(item.x + v.x + mo.x, item.y + v.y + mo.y);
  c.rotate((item.rotation + v.rotation) * Math.PI / 180);
  c.scale(v.scaleX, v.scaleY);
  if (v.clip) { v.clip(c); c.clip(); }
  c.save();
  c.translate(item.flipX ? item.w : 0, item.flipY ? item.h : 0);
  c.scale(item.flipX ? -1 : 1, item.flipY ? -1 : 1);
  if (item.type === 'text' || item.type === 'clock' || item.type === 'timer') {
    const shown = state.effect === 'text' ? Object.assign({}, item, { text: item.text.slice(0, Math.ceil(item.text.length * state.progress)) }) : item;
    drawText(c, shown, time);
  } else if (item.type === 'image') {
    const im = imageFor(item.src);
    if (im) c.drawImage(im, 0, 0, item.w, item.h);
  } else {   // rect, ellipse
    c.save();
    if (item.shadow) { c.shadowColor = item.shadowColor; c.shadowBlur = item.shadowBlur; }
    pathRounded(c, item);
    c.fillStyle = item.fill; c.fill();
    c.restore();
    if (item.strokeWidth > 0) { pathRounded(c, item); c.lineWidth = item.strokeWidth; c.strokeStyle = item.stroke; c.stroke(); }
  }
  c.restore();
  if (v.fold) {
    const f = v.fold;
    c.beginPath(); c.moveTo(f.x, 0); c.lineTo(f.x - f.width, 0); c.lineTo(f.x, f.height); c.closePath();
    const g = c.createLinearGradient(f.x - f.width, 0, f.x, f.height);
    g.addColorStop(0, '#fff'); g.addColorStop(0.7, '#bbb'); g.addColorStop(1, '#333');
    c.fillStyle = g; c.fill();
  }
  c.restore();
}

function drawPage(c, page, time) {
  const pageItem = fixItem(Object.assign({}, page.options, { type: 'rect', x: 0, y: 0, w: W, h: H, start: 0,
    duration: page.duration, effect: page.effect, outEffect: page.outEffect, inDuration: page.inDuration, outDuration: page.outDuration }));
  const v = visual(pageItem, time, 'Still', page.duration);
  c.save();
  c.globalAlpha *= v.opacity;
  c.translate(v.x, v.y);
  c.rotate(v.rotation * Math.PI / 180);
  c.scale(v.scaleX, v.scaleY);
  if (v.clip) { v.clip(c); c.clip(); }
  if (page.bg !== 'transparent') { c.fillStyle = page.bg; c.fillRect(0, 0, W, H); }
  for (const item of page.items) drawItem(c, item, time, page);
  c.restore();
}

// ---------- 재생 상태 ----------
let project = null;
const main = { index: 0, time: 0, cycle: 1, playing: false, visible: false };
let stamps = [idleChannel(), idleChannel()];
let globalCh = idleChannel();   // 배경 영상(네이티브). playing/visible 만 의미 있음
let dirty = true, lastReport = '', lastReportAt = 0;
const nativeVideo = { key: '' };

const runSettings = () => project.runSettings;
const pageAt = (i) => project.pages[Math.max(0, Math.min(project.pages.length - 1, i))];

function startMain(index, time = 0) {
  main.index = Math.max(0, Math.min(project.pages.length - 1, index));
  main.time = time; main.cycle = 1; main.playing = true; main.visible = true; dirty = true;
}
function rangeStart() {
  const s = runSettings();
  return s.allPages ? 0 : Math.max(0, Math.min(project.pages.length - 1, s.startPage - 1));
}
function stopMain() { main.playing = false; main.visible = false; main.time = 0; dirty = true; }

function stampCtl(n, action) {
  const ch = project.channels && project.channels.stamps[n];
  if (!ch) return false;
  if (action === 'play') stamps[n] = { time: 0, cycle: 1, playing: true, visible: true };
  else if (action === 'stop') stamps[n] = idleChannel();
  else if (action === 'pause') stamps[n] = Object.assign({}, stamps[n], { playing: !stamps[n].playing });
  else return false;
  dirty = true;
  return true;
}
function globalCtl(action) {
  if (!project.channels || !project.channels.global) return false;
  if (action === 'play') globalCh = { time: 0, cycle: 1, playing: true, visible: true };
  else if (action === 'stop') globalCh = idleChannel();
  else return false;
  dirty = true;
  return true;
}

// 링크 이름(linkName) 또는 아이템 id 로 텍스트 갱신. 메인 페이지·Stamp 페이지 모두 대상.
function setText(name, text) {
  let n = 0;
  const pages = project.pages.concat((project.channels ? project.channels.stamps : []).filter(Boolean).map((s) => s.page));
  for (const p of pages) for (const it of p.items) {
    if ((it.linkName === name || it.id === name) && ['text', 'clock', 'timer'].includes(it.type)) { it.text = text; it.runs = []; n++; }
  }
  if (n) dirty = true;
  return n;
}

// 명령 진입점 (네이티브 HTTP 서버가 ExecuteJavaScript 로 호출). 문자열 인자만 받는다.
const cg = {
  cmd(name, arg, body) {
    if (!project) return false;
    const n = Number(arg);
    switch (name) {
      case 'play': startMain(Number.isInteger(n) && n >= 1 ? n - 1 : main.index); return true;
      case 'run': startMain(rangeStart()); return true;
      case 'stop': case 'clear': stopMain(); return true;
      case 'pause': if (main.visible) { main.playing = !main.playing; dirty = true; } return true;
      case 'cut': if (main.visible) { main.time = Math.max(main.time, pageAt(main.index).inDuration); dirty = true; } return true;
      case 'skip': case 'next': startMain(main.index + 1); return true;
      case 'prev': startMain(main.index - 1); return true;
      case 'goto': if (Number.isInteger(n) && n >= 1) { startMain(n - 1); return true; } return false;
      case 'stamp': { const [k, act] = String(arg).split('/'); return stampCtl(Number(k) - 1, act); }
      case 'global': return globalCtl(arg);
      case 'text': {
        try { return setText(String(arg), String(JSON.parse(body).text)) > 0; } catch (e) { return false; }
      }
    }
    return false;
  },
  // 구 UDP 명령 호환 (cgctl.sh / UdpLoop)
  next: () => cg.cmd('next'),
  prev: () => cg.cmd('prev'),
  goto: (n) => cg.cmd('goto', n),
};
window.cg = cg;
cg.status = () => status();

function status() {
  if (!project) return { ready: false };
  return {
    ready: true, project: project.name, page: main.index + 1, pages: project.pages.length, playing: main.playing,
    visible: main.visible, time: +main.time.toFixed(2), cycle: main.cycle,
    stamps: stamps.map((s, n) => (project.channels && project.channels.stamps[n] ? { playing: s.playing, visible: s.visible, cycle: s.cycle } : null)),
    global: project.channels && project.channels.global ? { playing: globalCh.playing, visible: globalCh.visible } : null,
  };
}

// ---------- 네이티브 영상 (MPP) ----------
// 켜져 있는 배경 영상(Global) 우선, 없으면 메인 페이지의 현재 활성 video 아이템 하나.
// 아이템 효과/페이드/회전/트림은 지원하지 않는다(위치·크기·반복만).
function desiredVideo() {
  const g = project.channels && project.channels.global;
  if (g && globalCh.visible && globalCh.playing) return { src: g.video.src, rect: [0, 0, W, H] };
  if (main.visible) {
    const p = pageAt(main.index);
    const it = p.items.find((i) => i.type === 'video' && !i.hidden && i.src && main.time >= i.start && main.time <= i.start + i.duration);
    if (it) return { src: it.src, rect: [Math.round(it.x), Math.round(it.y), Math.round(it.w), Math.round(it.h)] };
  }
  return null;
}
function syncVideo() {
  if (!window.cefQuery) return;
  const d = desiredVideo();
  const key = d ? d.src + '|' + d.rect.join(',') : '';
  if (key === nativeVideo.key) return;
  nativeVideo.key = key;
  if (!d) { q('video:stop').catch(() => {}); return; }
  if (/^data:/.test(d.src)) { console.warn('[cg] data: 영상은 네이티브로 재생할 수 없음(내보내기 시 파일로 풀어야 함)'); return; }
  q('video:rect:' + d.rect.join(',')).catch(() => {});
  q('video:play:' + d.src).catch(() => {});
}

// ---------- 메인 루프 ----------
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
let lastT = performance.now(), lastSec = -1;

function frame(now) {
  requestAnimationFrame(frame);
  const delta = Math.min(0.25, (now - lastT) / 1000);
  lastT = now;
  if (!project) return;

  if (main.playing) {
    const r = playbackStep(project.pages, runSettings(), main.index, main.time, main.cycle, delta);
    Object.assign(main, { index: r.index, time: r.time, cycle: r.cycle, playing: r.playing });
    if (!r.playing && runSettings().mode !== 'manual') main.visible = false;   // 자동 모드: 마지막 페이지 끝나면 내려감
    dirty = true;
  }
  if (project.channels) {
    for (let n = 0; n < 2; n++) {
      const c = project.channels.stamps[n];
      if (c && stamps[n].playing) {
        stamps[n] = channelStep(stamps[n], delta, c.page.duration, c.loops === 100 ? 0 : c.loops, c.delay, c.mode === 'manual');
        dirty = true;
      }
    }
  }
  const sec = Math.floor(now / 1000);   // 시계/계수기는 초가 바뀔 때 다시 그림
  if (sec !== lastSec) { lastSec = sec; if (hasClock()) dirty = true; }

  syncVideo();
  if (dirty) { dirty = false; render(); }
  report(now);
}

function hasClock() {
  const pages = project.pages.concat((project.channels ? project.channels.stamps : []).filter(Boolean).map((s) => s.page));
  return pages.some((p) => p.items.some((i) => i.type === 'clock' || i.type === 'timer'));
}

function render() {
  ctx.clearRect(0, 0, W, H);
  const chans = project.channels;
  const stampLayer = (n) => {
    const c = chans && chans.stamps[n], s = stamps[n];
    if (c && s.visible && s.time <= c.page.duration) drawPage(ctx, c.page, s.time);
  };
  const order = { bottom: [1, 0].filter((n) => chans && chans.stamps[n] && chans.stamps[n].position === 'bottom'),
    top: [1, 0].filter((n) => chans && chans.stamps[n] && chans.stamps[n].position === 'top') };
  order.bottom.forEach(stampLayer);
  if (main.visible) drawPage(ctx, pageAt(main.index), main.time);
  order.top.forEach(stampLayer);
}

function report(now) {
  if (now - lastReportAt < 200 || !window.cefQuery) return;
  lastReportAt = now;
  const s = JSON.stringify(status());
  if (s === lastReport) return;
  lastReport = s;
  q('state:' + s).catch(() => {});
}

function q(request) {
  return new Promise((ok, ng) => window.cefQuery({ request, onSuccess: ok, onFailure: (c, m) => ng(m) }));
}

// 창이 1920x1080 보다 작으면(--view) 비율 유지 축소
function fit() {
  const s = Math.min(innerWidth / W, innerHeight / H);
  document.body.style.transformOrigin = '0 0';
  document.body.style.transform = s < 1 ? `scale(${s})` : '';
}
addEventListener('resize', fit);
fit();

(async () => {
  try {
    if (window.cefQuery) {
      baseUrl = await q('base');
      project = fixProject(JSON.parse(await q('load')));
    } else {   // 브라우저 단독 확인용: player.html?project=경로/project.json
      const url = new URLSearchParams(location.search).get('project') || 'project.json';
      baseUrl = new URL(url, location.href).href.replace(/[^/]*$/, '');
      project = fixProject(await (await fetch(url)).json());
    }
  } catch (e) {
    console.error('[cg] 프로젝트 로드 실패:', e);
    if (window.cefQuery) await q('ready').catch(() => {});
    return;
  }
  // 폰트 -> 이미지 로딩 후 첫 프레임 (기존 player.html 과 같은 절차)
  await document.fonts.ready;
  const srcs = new Set();
  for (const p of project.pages) for (const i of p.items) if (i.type === 'image' && i.src) srcs.add(i.src);
  for (const s of srcs) imageFor(s);
  await Promise.all([...images.values()].map((im) => im.decode().catch(() => {})));
  requestAnimationFrame(frame);
  await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  const params = new URLSearchParams(location.search);
  if (params.get('autoplay') === '1') startMain(rangeStart());
  if (window.cefQuery) await q('ready');
})();
