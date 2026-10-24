// Light card: brightness/CCT sliders + the HSV colour wheel + the Tuya
// colour wire formats (canonical h 0-360, s/v 0-1000).

import { state } from '../core/state.js';
import { t } from '../core/i18n.js';
import { $, el, clamp, esc, icon } from '../core/utils.js';
import { numRange } from './cards-caps.js';
import { sendCommands } from './cards-commands.js';
import { registerUpdater } from './cards-registry.js';
import { bindSlider } from './cards.js';

function rgb2hsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b);
  const d = max - min;
  let h = 0;
  if (d !== 0) {
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  const s = max === 0 ? 0 : d / max;
  return { h: Math.round(h), s: Math.round(s * 1000), v: Math.round(max * 1000) };
}

function hsv2rgb(h, s, v) {
  h = ((h % 360) + 360) % 360;
  s = clamp(s, 0, 1000) / 1000;
  v = clamp(v, 0, 1000) / 1000;
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  let r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  return {
    r: Math.round((r + m) * 255),
    g: Math.round((g + m) * 255),
    b: Math.round((b + m) * 255),
  };
}

function hsvCss(h, s, v) {
  const c = hsv2rgb(h, s, Math.max(v, 60));
  return `rgb(${c.r},${c.g},${c.b})`;
}

const HEX = n => Math.round(clamp(n, 0, 9999)).toString(16).padStart(4, '0');
const HEX2 = n => clamp(Math.round(n), 0, 255).toString(16).padStart(2, '0');

/** Parse a colour status value. Returns {fmt, h, s, v} or null;
 *  `code` disambiguates the JSON/hex scales. */
function parseColourValue(code, raw) {
  if (typeof raw !== 'string') return null;
  const s = raw.trim();
  const v1Code = code === 'colour_data';

  // "h:210,s:800,v:900"
  let m = s.match(/^h:(\d+),s:(\d+),v:(\d+)$/i);
  if (m) {
    return { fmt: 'hsv', h: +m[1] % 360, s: clamp(+m[2], 0, 1000), v: clamp(+m[3], 0, 1000) };
  }

  // JSON {"h":..,"s":..,"v":..}
  if (s[0] === '{') {
    try {
      const j = JSON.parse(s);
      if (j && isFinite(j.h) && isFinite(j.s) && isFinite(j.v)) {
        const svMax = v1Code ? 255 : 1000;
        return {
          fmt: 'json', h: +j.h % 360,
          s: clamp(Math.round((+j.s / svMax) * 1000), 0, 1000),
          v: clamp(Math.round((+j.v / svMax) * 1000), 0, 1000),
        };
      }
    } catch (e) { /* fallthrough */ }
  }

  // 14 hex chars — v1: RGB + "0168ffff" suffix, or h(16bit)+s+v layout
  if (/^[0-9a-f]{14}$/i.test(s)) {
    const tail = s.slice(6).toLowerCase();
    if (tail === '0168ffff') {
      const hsv = rgb2hsv(parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16));
      return Object.assign({ fmt: 'hex14' }, hsv);
    }
    const h = parseInt(s.slice(6, 10), 16);       // 16-bit hue
    const sat = parseInt(s.slice(10, 12), 16);    // 0-255
    const val = parseInt(s.slice(12, 14), 16);    // 0-255
    return {
      fmt: 'hex14b', h: h % 360,
      s: clamp(Math.round((sat / 255) * 1000), 0, 1000),
      v: clamp(Math.round((val / 255) * 1000), 0, 1000),
    };
  }

  // 12 hex chars — v2: h,s,v as 4-hex each (s/v 0-1000 scale)
  if (/^[0-9a-f]{12}$/i.test(s)) {
    return {
      fmt: 'hex12',
      h: parseInt(s.slice(0, 4), 16) % 360,
      s: clamp(parseInt(s.slice(4, 8), 16), 0, 1000),
      v: clamp(parseInt(s.slice(8, 12), 16), 0, 1000),
    };
  }

  // "48,186,76" — legacy RGB decimal
  m = s.match(/^(\d{1,3}),(\d{1,3}),(\d{1,3})$/);
  if (m && +m[1] <= 255 && +m[2] <= 255 && +m[3] <= 255) {
    const hsv = rgb2hsv(+m[1], +m[2], +m[3]);
    return Object.assign({ fmt: 'rgb' }, hsv);
  }

  return null;
}

/** Default wire format per code when the current value is unknown. */
function defaultColourFmt(code) {
  if (code === 'colour_data_hsv') return 'hsv';
  if (code === 'colour_data_v2') return 'hex12';
  return 'hex14'; // colour_data v1: RGB + 0168ffff
}

/** Build the command value string, mirroring the format parsed from the
 *  current status value (or the per-code default). */
function writeColourValue(devId, code, h, s, v) {
  let fmt = state.colourFmt.get(devId);
  if (!fmt) fmt = defaultColourFmt(code);
  const rgb = hsv2rgb(h, s, v);
  switch (fmt) {
    case 'hsv': return `h:${Math.round(h)},s:${Math.round(s)},v:${Math.round(v)}`;
    case 'hex12': return HEX(h) + HEX(s) + HEX(v);
    case 'hex14': return HEX2(rgb.r) + HEX2(rgb.g) + HEX2(rgb.b) + '0168ffff';
    case 'hex14b': {
      // v1 alternate layout: 6 zero pad + hue(4 hex, 16-bit) + s(2) + v(2).
      const hh = Math.round(clamp(h, 0, 360));
      const ss = clamp(Math.round((s / 1000) * 255), 0, 255);
      const vv = clamp(Math.round((v / 1000) * 255), 0, 255);
      const h4 = hh.toString(16).padStart(4, '0');
      return '000000' + h4 + HEX2(ss) + HEX2(vv);
    }
    case 'rgb': return `${rgb.r},${rgb.g},${rgb.b}`;
    case 'json': {
      const svMax = code === 'colour_data' ? 255 : 1000;
      return `{"h":${Math.round(h)},"s":${Math.round(s / 1000 * svMax)},"v":${Math.round(v / 1000 * svMax)}}`;
    }
    default: return `h:${Math.round(h)},s:${Math.round(s)},v:${Math.round(v)}`;
  }
}

/** Read the current colour of a device (canonical hsv) + remember format. */
function readColour(dev, caps) {
  const raw = dev.status[caps.colourCode];
  if (typeof raw === 'string') {
    const p = parseColourValue(caps.colourCode, raw);
    if (p) {
      state.colourFmt.set(dev.id, p.fmt);
      return { h: p.h, s: p.s, v: p.v };
    }
  }
  return null;
}

/* ---- light controls: brightness / cct sliders + colour wheel ---- */

export function buildSlider(dev, caps, kind) {
  const code = kind === 'bright' ? caps.brightCode : caps.cctCode;
  const isCct = kind === 'cct';
  const r = numRange(dev, code, isCct ? { min: 0, max: 255, step: 1 } : { min: 25, max: 255, step: 1 });
  const cur = isFinite(+dev.status[code]) ? +dev.status[code] : r.min;
  const label = isCct ? t('light_cct') : t('light_brightness');
  const track = isCct ? ' style="--track:linear-gradient(90deg,#fbbf24,#22d3ee)"' : '';
  const node = el(`<div>
    <div class="slider-row" data-slider="${kind}">
      <div class="slider-head"><span>${esc(label)}</span><span class="val">${pctOf(cur, r)}</span></div>
      <div class="slider-wrap">
        <input type="range" min="${r.min}" max="${r.max}" step="${r.step}" value="${cur}"${track} aria-label="${esc(label)}">
        <span class="slider-bubble">${pctOf(cur, r)}</span>
      </div>
      ${isCct ? `<div class="cct-labels"><span>${t('cct_warm')}</span><span>${t('cct_cool')}</span></div>` : ''}
    </div>
  </div>`);
  bindSlider(node, dev, `[data-slider="${kind}"]`, code, v => {
    const cmds = [];
    if (caps.switch && !dev.status[caps.switch]) cmds.push({ code: caps.switch, value: true });
    if (caps.workMode && dev.status[caps.workMode] !== 'white') cmds.push({ code: caps.workMode, value: 'white' });
    cmds.push({ code, value: v });
    sendCommands(dev.id, cmds);
  }, undefined, () => {
    const v = +dev.status[code];
    return isFinite(v) ? v : null;
  });
  return node;
}

export function buildColorWheel(dev, caps) {
  const cur = readColour(dev, caps) || { h: 210, s: 800, v: 1000 };
  const node = el(`<div>
    <div class="slider-head" style="margin-top:14px"><span>${t('light_color')}</span></div>
    <div class="wheel-row" data-wheel>
      <div class="wheel-box">
        <canvas width="140" height="140" tabindex="0" aria-label="${t('light_color')}"></canvas>
        <span class="wheel-thumb" style="left:${wheelX(cur.h, cur.s)}px;top:${wheelY(cur.h, cur.s)}px;background:${hsvCss(cur.h, cur.s, 1000)}"></span>
        <button class="wheel-center" aria-label="${t('color_white')}">${icon('sun')}</button>
      </div>
      <div class="wheel-side">
        <div class="wheel-swatch" style="background:${hsvCss(cur.h, cur.s, cur.v)}"></div>
      </div>
    </div>
  </div>`);
  bindWheel(node, dev, caps);
  return node;
}

function pctOf(cur, r) {
  const p = clamp(Math.round(((cur - r.min) / (r.max - r.min)) * 100), 0, 100);
  return p + '%';
}

const WHEEL_R = 70;
const KEY_HUE_STEP = 5;    // hue degrees per arrow key
const KEY_SAT_STEP = 50;   // saturation units (0-1000) per arrow key
function wheelX(h, s) { return 70 + Math.cos(h * Math.PI / 180) * (s / 1000) * (WHEEL_R - 10); }
function wheelY(h, s) { return 70 - Math.sin(h * Math.PI / 180) * (s / 1000) * (WHEEL_R - 10); }

function bindWheel(root, dev, caps) {
  const box = $('[data-wheel]', root);
  if (!box) return;
  const canvas = $('canvas', box);
  const thumb = $('.wheel-thumb', box);
  const swatch = $('.wheel-swatch', root);
  const center = $('.wheel-center', box);
  drawWheel(canvas);

  let hsv = readColour(dev, caps) || { h: 210, s: 800, v: 1000 };
  let dragging = false;

  const apply = () => {
    thumb.style.left = wheelX(hsv.h, hsv.s) + 'px';
    thumb.style.top = wheelY(hsv.h, hsv.s) + 'px';
    thumb.style.background = hsvCss(hsv.h, hsv.s, 1000);
    if (swatch) swatch.style.background = hsvCss(hsv.h, hsv.s, hsv.v);
  };
  apply();

  const fromEvent = ev => {
    const rect = canvas.getBoundingClientRect();
    const x = ev.clientX - rect.left - rect.width / 2;
    const y = ev.clientY - rect.top - rect.height / 2;
    const R = rect.width / 2 - 4;
    const d = clamp(Math.sqrt(x * x + y * y), 0, R);
    let h = Math.atan2(-y, x) * 180 / Math.PI;
    if (h < 0) h += 360;
    hsv = { h: Math.round(h), s: Math.round((d / R) * 1000), v: Math.max(hsv.v, 100) };
    apply();
  };

  canvas.addEventListener('pointerdown', ev => {
    dragging = true;
    try { canvas.setPointerCapture(ev.pointerId); } catch (e) { /* ignore */ }
    fromEvent(ev);
  });
  canvas.addEventListener('pointermove', ev => { if (dragging) fromEvent(ev); });
  const release = () => {
    if (!dragging) return;
    dragging = false;
    sendColour(dev, caps, hsv);
  };
  canvas.addEventListener('pointerup', release);
  canvas.addEventListener('pointercancel', () => { dragging = false; });

  // Keyboard: arrows nudge hue/saturation through the same apply path.
  canvas.addEventListener('keydown', ev => {
    let dh = 0, ds = 0;
    if (ev.key === 'ArrowLeft') dh = -KEY_HUE_STEP;
    else if (ev.key === 'ArrowRight') dh = KEY_HUE_STEP;
    else if (ev.key === 'ArrowUp') ds = KEY_SAT_STEP;
    else if (ev.key === 'ArrowDown') ds = -KEY_SAT_STEP;
    else return;
    ev.preventDefault();   // otherwise the page scrolls
    hsv = {
      h: ((hsv.h + dh) % 360 + 360) % 360,
      s: clamp(hsv.s + ds, 0, 1000),
      v: Math.max(hsv.v, 100),
    };
    apply();
    sendColour(dev, caps, hsv);
  });

  // WS colour sync; a drag whose ctrl got hidden never sees pointerup.
  registerUpdater(() => {
    if (dragging) {
      if (canvas.offsetParent) return;
      dragging = false;
    }
    const c = readColour(dev, caps);
    if (!c) return;
    hsv = c;
    apply();
  });

  // White shortcut: work_mode=white + (brightness if known).
  if (center) {
    center.addEventListener('click', () => {
      const cmds = [];
      if (caps.switch && !dev.status[caps.switch]) cmds.push({ code: caps.switch, value: true });
      if (caps.workMode) cmds.push({ code: caps.workMode, value: 'white' });
      if (cmds.length) sendCommands(dev.id, cmds);
    });
  }
}

function sendColour(dev, caps, hsv) {
  const cmds = [];
  if (caps.switch && !dev.status[caps.switch]) cmds.push({ code: caps.switch, value: true });
  if (caps.workMode && dev.status[caps.workMode] !== 'colour') {
    cmds.push({ code: caps.workMode, value: 'colour' });
  }
  const value = writeColourValue(dev.id, caps.colourCode, hsv.h, hsv.s, hsv.v);
  cmds.push({ code: caps.colourCode, value });
  sendCommands(dev.id, cmds);
}

function drawWheel(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const S = 140;
  canvas.width = S * dpr;
  canvas.height = S * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const R = S / 2;
  // Hue wedges.
  for (let a = 0; a < 360; a += 1) {
    const a0 = (a - 0.75) * Math.PI / 180;
    const a1 = (a + 0.75) * Math.PI / 180;
    ctx.beginPath();
    ctx.moveTo(R, R);
    ctx.arc(R, R, R, a0, a1);
    ctx.closePath();
    ctx.fillStyle = `hsl(${a}, 100%, 50%)`;
    ctx.fill();
  }
  // Saturation: white center fade.
  const g = ctx.createRadialGradient(R, R, 0, R, R, R);
  g.addColorStop(0, 'rgba(255,255,255,1)');
  g.addColorStop(0.8, 'rgba(255,255,255,0)');
  g.addColorStop(1, 'rgba(255,255,255,0)');
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(R, R, R, 0, Math.PI * 2);
  ctx.fill();
}
