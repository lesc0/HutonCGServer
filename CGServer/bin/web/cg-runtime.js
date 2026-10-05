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

// ---- 부드러운 스크롤 옵션 (하단 자막 끊김 실험용, 단말에서 측정해 정함) ----
// SMOOTH_DT: 프레임 시간을 rAF 타임스탬프 그대로(들쭉날쭉) 쓰지 않고 균일한 간격으로 진행시키되 실제 시계에 서서히 맞춘다.
// INT_STEP : crawl 을 프레임당 정수 픽셀씩 움직인다(속도가 반올림되어 기본 속도와 조금 달라짐 -> 끝나는 시각이 앞당겨질 수 있음).
const SMOOTH_DT = true;
const INT_STEP = false;
let framePeriod = 1 / 60;   // rAF 간격의 느린 이동평균(초)
const W = 1920, H = 1080;

// ---------- 기본값 (model.ts make) ----------
const ITEM_DEFAULTS = {
  type: 'rect', name: '', text: '', x: 250, y: 400, w: 1400, h: 180, size: 100, fill: '#ffffff', stroke: '#000000',
  strokeWidth: 0, edge2: '#ffffff', edge2Width: 0, edge3: '#000000', edge3Width: 0, bold: false, italic: false,
  underline: false, outline: false, family: 'NotoSansKR', align: 'left', opacity: 1, rotation: 0, cRotate: 0,
  textWidth: 100, space: 100, thickness: 0, kerning: 0, leading: 20, flipX: false, flipY: false, shadow: false,
  shadowColor: '#000000', shadowBlur: 4, shadowDepth: 8, shadowAngle: 45, hidden: false, start: 0, duration: 10,
  effect: 'none', outEffect: 'none', inDuration: 1, outDuration: 1, direction: 'left', speed: 1, volume: 1, trim: 0,
  mediaLoop: false, background: false, runs: [], clockFormat: 'HH:mm:ss', timerSeconds: 300, timerCount: 'down',
  moves: [], effectPreset: 0, tileX: 8, tileY: 8, softness: 0, effectBorder: 0, curlRadius: 60, effectAngle: 0, blinkCount: 4,
  radius: 0, shapeKind: '', gradient: '', stripe: '',
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
  if (f === 'crawl') {
    let dist = q * (W + i.w) * i.speed;
    if (INT_STEP) {   // 프레임당 정수 픽셀: step = 초당 이동거리 / fps 를 반올림, 이동량 = 지난 프레임 수 * step
      const fps = Math.max(1, Math.round(1 / framePeriod));
      const step = Math.max(1, Math.round((W + i.w) * i.speed / Math.max(0.1, i.duration) / fps));
      dist = Math.round(state.elapsed * fps) * step;
    }
    v.x = (d === 'right' ? 1 : -1) * dist;
  }
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

// ---------- 도형(rect/ellipse) 확장: 모서리 반경·다각형·그라데이션·줄무늬 ----------
// editor 의 src/cg-editor/app/shapes.ts 와 동일한 로직. 한쪽을 고치면 다른 쪽도 같이 고칠 것.
const arcPts = (cx, cy, r, a0, a1, n = 18) => Array.from({ length: n + 1 }, (_, k) => {
  const a = (a0 + (a1 - a0) * k / n) * Math.PI / 180; return [cx + r * Math.cos(a), cy + r * Math.sin(a)]; });
const SHAPE_POINTS = {
  triangle: [[.5, 0], [1, 1], [0, 1]], wedge: [[0, .62], [1, 0], [.5, 1]], quarter: [[0, 1], ...arcPts(0, 1, 1, -90, 0)],
  parallelogram: [[.2, 0], [1, 0], [.8, 1], [0, 1]], trapezoid: [[.15, 0], [.85, 0], [1, 1], [0, 1]],
  slant: [[.04, 0], [1, 0], [.96, 1], [0, 1]], diamond: [[.5, 0], [1, .5], [.5, 1], [0, .5]],
  pentagon: [[.5, 0], [1, .38], [.82, 1], [.18, 1], [0, .38]], hexagon: [[.25, 0], [.75, 0], [1, .5], [.75, 1], [.25, 1], [0, .5]],
  chevron: [[0, 0], [.7, 0], [1, .5], [.7, 1], [0, 1], [.3, .5]], blob: [[.1, .2], [.55, 0], [1, .15], [.9, .8], [.45, 1], [0, .7]],
  leaf: [[0, 1], [.05, .45], [.3, .12], [1, 0], [.92, .6], [.55, .92]],
};
const COLOR_RE = /^#[0-9a-f]{6}([0-9a-f]{2})?$/i;
function parseGradient(s) {
  if (!s) return null;
  const p = s.split(':');
  if ((p[0] !== 'linear' && p[0] !== 'radial') || p.length < 4) return null;
  const angle = Number(p[1]), colors = p.slice(2);
  if (!Number.isFinite(angle) || colors.length < 2 || colors.length > 8 || !colors.every(x => COLOR_RE.test(x))) return null;
  return { type: p[0], angle, colors };
}
function parseStripe(s) {
  if (!s) return null;
  const p = s.split(':');
  if (p.length !== 3) return null;
  const angle = Number(p[0]), size = Number(p[1]);
  if (!Number.isFinite(angle) || !Number.isFinite(size) || size < 2 || size > 200 || !COLOR_RE.test(p[2])) return null;
  return { angle, size, color: p[2] };
}
function pathRounded(c, i) {   // 사각형 / 타원 / 둥근 사각형 / 다각형
  c.beginPath();
  const pts = i.shapeKind ? SHAPE_POINTS[i.shapeKind] : null;
  if (pts) { pts.forEach(([x, y], n) => n ? c.lineTo(x * i.w, y * i.h) : c.moveTo(x * i.w, y * i.h)); c.closePath(); }
  else if (i.type === 'ellipse') c.ellipse(i.w / 2, i.h / 2, i.w / 2, i.h / 2, 0, 0, Math.PI * 2);
  else if (i.radius > 0) c.roundRect(0, 0, i.w, i.h, Math.min(i.radius, i.w / 2, i.h / 2));
  else c.rect(0, 0, i.w, i.h);
}
function shapePaint(c, i) {
  const g = parseGradient(i.gradient);
  if (!g) return i.fill;
  let grad;
  if (g.type === 'radial') grad = c.createRadialGradient(i.w / 2, i.h / 2, 0, i.w / 2, i.h / 2, Math.max(i.w, i.h) / 2);
  else {
    const a = g.angle * Math.PI / 180, dx = Math.sin(a), dy = -Math.cos(a), len = Math.abs(i.w * dx) + Math.abs(i.h * dy);
    grad = c.createLinearGradient(i.w / 2 - dx * len / 2, i.h / 2 - dy * len / 2, i.w / 2 + dx * len / 2, i.h / 2 + dy * len / 2);
  }
  g.colors.forEach((col, n) => grad.addColorStop(n / (g.colors.length - 1), col));
  return grad;
}
function drawShape(c, i) {
  c.save();
  if (i.shadow) { c.shadowColor = i.shadowColor; c.shadowBlur = i.shadowBlur; }
  pathRounded(c, i); c.fillStyle = shapePaint(c, i); c.fill();
  c.restore();
  const st = parseStripe(i.stripe);
  if (st) {
    c.save(); pathRounded(c, i); c.clip();
    const d = Math.hypot(i.w, i.h);
    c.translate(i.w / 2, i.h / 2); c.rotate((st.angle - 90) * Math.PI / 180); c.fillStyle = st.color;
    for (let x = -d, n = 0; x < d && n < 600; x += st.size * 2, n++) c.fillRect(x, -d, st.size, d * 2);
    c.restore();
  }
  if (i.strokeWidth > 0) { pathRounded(c, i); c.lineWidth = i.strokeWidth; c.strokeStyle = i.stroke; c.stroke(); }
}

// crawl/roll 텍스트: 매 프레임 글자를 통째로 다시 그리는 대신, 내용이 안 바뀌는 동안은
// 오프스크린 캔버스에 한 번만 그려두고 매 프레임은 drawImage로 위치만 옮겨 찍는다.
// (방송 CG 장비가 텍스트를 텍스처로 구워두고 GPU로 이동만 시키는 것과 같은 발상.)
// 캔버스에만 적용하는 캐시라 레이어 순서/폰트 렌더링은 기존 그대로 유지됨(CSS 전환과 달리 안전).
const textCache = new Map();   // item.id -> {canvas, sig}
let fontEpoch = 0;   // 폰트가 (늦게) 로드되면 올라간다: 대체 글꼴로 구워 둔 캐시를 버리기 위함
if (document.fonts && document.fonts.addEventListener) document.fonts.addEventListener('loadingdone', () => { fontEpoch++; });
function cachedTextCanvas(item, time) {
  const text = item.type === 'clock' || item.type === 'timer' ? clockText(item, time) : item.text;
  // 매 프레임 큰 배열을 JSON 으로 직렬화하면 CPU/GC 부담으로 프레임이 튈 수 있다. 같은 항목 객체이고 글자·runs 가 그대로면 (프로젝트는
  // 읽을 때 새 객체가 되므로 속성 변경은 항상 새 객체) 이전에 그려 둔 캔버스를 그대로 쓴다.
  const hit = textCache.get(item.id);
  if (hit && hit.item === item && hit.text === text && hit.runs === item.runs && hit.epoch === fontEpoch) return hit.canvas;
  const sig = JSON.stringify([fontEpoch, text, item.size, item.family, item.bold, item.italic, item.fill, item.stroke,
    item.strokeWidth, item.edge2, item.edge2Width, item.edge3, item.edge3Width, item.w, item.h, item.align,
    item.kerning, item.space, item.textWidth, item.leading, item.thickness, item.underline, item.cRotate,
    item.outline, item.shadow, item.shadowColor, item.shadowBlur, item.shadowDepth, item.shadowAngle, item.runs]);
  let rec = textCache.get(item.id);
  if (!rec || rec.sig !== sig) {
    const oc = rec ? rec.canvas : document.createElement('canvas');
    oc.width = Math.max(1, Math.ceil(item.w));
    oc.height = Math.max(1, Math.ceil(item.h));
    const octx = oc.getContext('2d');
    octx.clearRect(0, 0, oc.width, oc.height);
    drawText(octx, item, time);
    rec = { canvas: oc, sig };
    textCache.set(item.id, rec);
  }
  rec.item = item; rec.text = text; rec.runs = item.runs; rec.epoch = fontEpoch;
  return rec.canvas;
}

// 프로젝트를 읽은 직후 모든 페이지의 (캐시되는) 텍스트를 미리 구워 둔다. 안 그러면 페이지가 바뀌는 순간 그 페이지의 글자들을
// 한꺼번에 처음 그리느라 한 프레임이 100ms 넘게 걸린다(단말 측정: render 131ms). 재생(ready) 시작 전에 해 두므로 끊김이 보이지 않는다.
function prewarmTextCache(proj) {
  let n = 0;
  for (const page of proj.pages) {
    for (const item of page.items) {
      if (item.type !== 'text' && item.type !== 'clock' && item.type !== 'timer') continue;
      if (item.hidden || item.effect === 'text') continue;
      if (!(item.effect === 'crawl' || item.effect === 'roll' || !item.shadow)) continue;   // drawItem 의 캐시 조건과 같아야 함
      cachedTextCanvas(item, 0);
      n++;
    }
  }
  return n;
}

// 진단(--jsprof): 텍스트가 자기 박스(w×h)에 들어가는지 점검한다. 단말에 없는 글꼴(맑은 고딕, Arial 등)은 다른 글꼴로 대체되어 글자 폭이 달라지므로
// 에디터(PC)에서 맞춰 둔 박스를 넘칠 수 있다. drawText 의 줄바꿈 규칙(폭 item.w - 8, 높이 item.h 를 넘으면 그리지 않음)을 그대로 따른다.
function fitReport(proj) {
  const m = document.createElement('canvas').getContext('2d');
  const out = [];
  for (const [pi, page] of proj.pages.entries()) for (const item of page.items) {
    if (item.type !== 'text' || !item.text) continue;
    const scale = item.textWidth / 100;
    let lines = 1, lw = 0, maxw = 0, y = 4, cut = false, at = 0;
    for (const ch of item.text) {
      const run = (item.runs || []).reduce((s, r) => (at >= r.start && at < r.end ? Object.assign({}, s, r) : s), {});
      m.font = `${(run.italic ?? item.italic) ? 'italic ' : ''}${(run.bold ?? item.bold) ? 'bold ' : ''}${run.size ?? item.size}px "${run.family ?? item.family}"`;
      const w = (m.measureText(ch).width * (ch === ' ' ? item.space / 100 : 1) + item.kerning) * scale;
      if (ch === '\n' || (lw + w > item.w - 8 && lw > 0)) { maxw = Math.max(maxw, lw); lines++; lw = 0; y += item.size * (1 + item.leading / 100); if (y > item.h) cut = true; }
      if (ch !== '\n') lw += w;
      at += ch.length;
    }
    maxw = Math.max(maxw, lw);
    const crawl = item.effect === 'crawl' || item.effect === 'roll';
    // crawl/roll 은 박스가 글자 전체를 담아야 하고(w), 일반 텍스트는 줄 수가 박스 높이에 들어가야 한다(h)
    if (crawl ? (lines > 1 || cut) : cut) out.push(`p${pi + 1} "${item.name}" ${item.family} ${item.size}px: lines=${lines} 필요폭=${maxw.toFixed(0)} 박스=${item.w}x${item.h}${cut ? ' 잘림' : ''}`);
  }
  q(`prof:fit ${out.length ? out.length + '건 넘침 ' + out.join(' | ') : '모든 텍스트가 박스에 들어감'}`).catch(() => {});
}

function drawItem(c, item, time, page) {
  if (item.hidden || item.type === 'audio' || item.type === 'video') return;   // 영상은 네이티브가 그림
  const state = effectState(item, time, page.mode, page.duration);
  if (!state.active) return;
  const v = visual(item, time, page.mode, page.duration);
  c.save();
  c.globalAlpha *= item.opacity * v.opacity;
  const mo = moveOffset(item, time);
  // 주의: crawl/roll 위치를 정수 픽셀로 스냅하지 않는다. 느린 스크롤(예: 3.66px/프레임)은 정수로 자르면 3,4,3,4 로 번갈아 움직여 오히려 규칙성이 나빠지고
  // (단말에서 송출 영상으로 측정: 튄 프레임 7.4% -> 9.7%), 소수점 위치의 보간이 더 부드럽다.
  c.translate(item.x + v.x + mo.x, item.y + v.y + mo.y);
  c.rotate((item.rotation + v.rotation) * Math.PI / 180);
  c.scale(v.scaleX, v.scaleY);
  if (v.clip) { v.clip(c); c.clip(); }
  c.save();
  c.translate(item.flipX ? item.w : 0, item.flipY ? item.h : 0);
  c.scale(item.flipX ? -1 : 1, item.flipY ? -1 : 1);
  if (item.type === 'text' || item.type === 'clock' || item.type === 'timer') {
    const shown = state.effect === 'text' ? Object.assign({}, item, { text: item.text.slice(0, Math.ceil(item.text.length * state.progress)) }) : item;
    // crawl/roll 은 물론, 글자가 점점 나타나는 'text' 효과와 그림자(캐시 캔버스 경계에서 잘림)가 없는 일반 텍스트/시계도 캐시해서
    // 매 프레임 글자 단위 fillText 를 하지 않는다(정적 텍스트가 많은 프로젝트에서 CEF 렌더러가 60fps 를 못 맞추던 원인).
    if (state.effect === 'crawl' || state.effect === 'roll' || (state.effect !== 'text' && !item.shadow)) c.drawImage(cachedTextCanvas(shown, time), 0, 0);
    else drawText(c, shown, time);
  } else if (item.type === 'image') {
    const im = imageFor(item.src);
    if (im) c.drawImage(im, 0, 0, item.w, item.h);
  } else {   // rect, ellipse
    drawShape(c, item);
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

// bin/fonts 의 폰트(player.html 이 ../fonts/fonts.css 로 등록)는 캔버스에 그리기 전에 미리 읽어 둬야 처음부터 제 글꼴로 나온다.
// 시스템 폰트·없는 폰트는 load() 가 바로 끝나므로 부담이 없다.
async function preloadFonts(proj) {
  const fams = new Set();
  for (const p of proj.pages) for (const i of p.items) {
    if (!['text', 'clock', 'timer'].includes(i.type)) continue;
    if (i.family) fams.add(i.family);
    for (const r of i.runs || []) if (r.family) fams.add(r.family);
  }
  const jobs = [];
  for (const f of fams) for (const style of ['', 'italic ']) for (const weight of ['', 'bold '])
    jobs.push(document.fonts.load(`${style}${weight}16px ${JSON.stringify(f)}`).catch(() => {}));
  await Promise.all(jobs);
}

// 디스크의 project 파일을 다시 읽어 적용한다 (에디터에서 저장한 내용을 반영).
// --autoplay 로 띄운 엔진이면 처음 실행 때처럼 바로 재생 시작, 아니면(수동 제어) 정지 상태로 1페이지에 둔다.
async function reloadProject() {
  if (!window.cefQuery) return false;
  let next;
  try { next = fixProject(JSON.parse(await q('load'))); } catch (e) { console.error('[cg] reload 실패:', e); return false; }
  await preloadFonts(next);
  prewarmTextCache(next);
  project = next;
  stamps = [idleChannel(), idleChannel()];
  globalCh = idleChannel();
  // nativeVideo.key 는 건드리지 않는다: syncVideo() 가 이전 값과 비교해서 네이티브 영상을 끄므로,
  // 여기서 미리 ''로 리셋해버리면 새 프로젝트도 영상이 없을 때 "변화 없음"으로 오판해 video:stop 이 안 나가고
  // 이전 프로젝트의 네이티브 비디오 디코더가 그대로 켜진 채 남는다(비트레이트/CPU 이상의 원인이었음).
  const srcs = new Set();
  for (const p of project.pages) for (const i of p.items) if (i.type === 'image' && i.src) srcs.add(i.src);
  for (const s of srcs) imageFor(s);
  if (new URLSearchParams(location.search).get('autoplay') === '1') startMain(rangeStart());
  else { main.index = 0; main.time = 0; main.cycle = 1; main.playing = false; main.visible = false; dirty = true; }
  return true;
}

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
      case 'reload': reloadProject(); return true;
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

let vclock = -1;   // 가상 시계(초)
function frameDelta(now) {
  const real = Math.min(0.25, (now - lastT) / 1000);
  lastT = now;
  if (real > 0.002 && real < 0.1) framePeriod += (real - framePeriod) * 0.02;   // 간격 추정(정지/탭 전환 같은 큰 값은 제외)
  if (!SMOOTH_DT) return real;
  const t = now / 1000;
  if (vclock < 0 || Math.abs(t - vclock) > 3 * framePeriod + 0.05) { vclock = t; return real; }   // 처음이거나 크게 어긋나면(멈춤) 실제 시간으로 재동기
  const prev = vclock;
  vclock += framePeriod;                 // 균일한 간격으로 진행
  vclock += (t - vclock) * 0.02;         // 실제 시계에 아주 천천히 맞춤(드리프트 보정)
  return vclock - prev;
}

// ---------- 프레임 시간 측정 (진단: player.html?jsprof=1, cg-streamer --jsprof) ----------
// 1초마다 native 로 보내 [jstat] 로그에 남긴다: rAF 간격(놓친 프레임), frame()/render() 소요, GC 추정(JS 힙이 줄어든 횟수).
const PROF = new URLSearchParams(location.search).get('jsprof') === '1';
const prof = { t0: 0, lastNow: 0, n: 0, dtMax: 0, late: 0, frSum: 0, frMax: 0, rnSum: 0, rnMax: 0, slow: 0, gc: 0, heap: 0 };
function profRender(ms) { prof.rnSum += ms; if (ms > prof.rnMax) prof.rnMax = ms; }
function profFrame(now, ms) {
  if (prof.lastNow) {
    const dt = now - prof.lastNow;
    if (dt > prof.dtMax) prof.dtMax = dt;
    if (dt > 25) prof.late++;   // 60Hz 에서 1.5프레임 넘게 벌어짐 = 프레임을 놓침
  }
  prof.lastNow = now;
  prof.n++; prof.frSum += ms; if (ms > prof.frMax) prof.frMax = ms;
  if (ms > 8) prof.slow++;
  const heap = performance.memory ? performance.memory.usedJSHeapSize : 0;
  if (prof.heap && heap < prof.heap - 512 * 1024) prof.gc++;   // 프레임 사이에 힙이 0.5MB 넘게 줄면 GC 로 본다
  prof.heap = heap;
  if (!prof.t0) prof.t0 = now;
  if (now - prof.t0 >= 1000) {
    const f = (x) => x.toFixed(1);
    q(`prof:raf=${prof.n} dt_max=${f(prof.dtMax)}ms late=${prof.late} frame avg=${f(prof.frSum / prof.n)} max=${f(prof.frMax)}ms ` +
      `render avg=${f(prof.rnSum / prof.n)} max=${f(prof.rnMax)}ms slow8=${prof.slow} gc=${prof.gc} heap=${f(heap / 1048576)}MB`).catch(() => {});
    Object.assign(prof, { t0: now, n: 0, dtMax: 0, late: 0, frSum: 0, frMax: 0, rnSum: 0, rnMax: 0, slow: 0, gc: 0 });
  }
}

function frame(now) {
  requestAnimationFrame(frame);
  const p0 = PROF ? performance.now() : 0;
  const delta = frameDelta(now);
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
  if (dirty) { dirty = false; const r0 = PROF ? performance.now() : 0; render(); if (PROF) profRender(performance.now() - r0); }
  report(now);
  if (PROF) profFrame(now, performance.now() - p0);
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
  await preloadFonts(project);
  await document.fonts.ready;
  prewarmTextCache(project);
  if (PROF) fitReport(project);
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
