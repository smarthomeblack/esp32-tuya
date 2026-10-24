// Pure DOM/format helpers + the inline SVG icon set. No state access
// except i18n labels (fmtUptime).

import { t } from './i18n.js';

export const $ = (sel, root) => (root || document).querySelector(sel);
export const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

/** Build one element from an HTML string. */
export function el(html) {
  const t = document.createElement('template');
  t.innerHTML = html.trim();
  return t.content.firstElementChild;
}

/** Escape text for safe HTML interpolation (device/scene names). */
export function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

export const clamp = (v, min, max) => Math.min(max, Math.max(min, v));

const ICONS = {
  bulb: '<path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 0 0-4 10.5c.6.5 1 1.3 1 2.1h6c0-.8.4-1.6 1-2.1A6 6 0 0 0 12 3Z"/>',
  power: '<path d="M12 3v8"/><path d="M6.3 6.3a8 8 0 1 0 11.4 0"/>',
  plug: '<path d="M9 7V3M15 7V3"/><path d="M7 7h10v4a5 5 0 0 1-10 0V7Z"/><path d="M12 21v-4"/>',
  thermo: '<path d="M10 4a2 2 0 0 1 4 0v9.5a4.5 4.5 0 1 1-4 0V4Z"/><circle cx="12" cy="17" r="1.6" fill="currentColor" stroke="none"/>',
  climate: '<rect x="3" y="5" width="18" height="7" rx="2"/><path d="M6 9h5"/><path d="M7 16v2M12 16v4M17 16v2"/>',
  cover: '<rect x="4" y="4" width="16" height="5" rx="1.5"/><path d="M4 12h16M4 15.5h16"/><path d="M12 18v3"/>',
  fan: '<circle cx="12" cy="12" r="1.8" fill="currentColor" stroke="none"/><path d="M12 10.2c.4-2.8-.5-5.4-2.6-6-1.9-.5-3 1.7-1.4 4.6M13.8 12c2.8.4 5.4-.5 6-2.6.5-1.9-1.7-3-4.6-1.4M12 13.8c-.4 2.8.5 5.4 2.6 6 1.9.5 3-1.7 1.4-4.6M10.2 12c-2.8-.4-5.4.5-6 2.6-.5 1.9 1.7 3 4.6 1.4"/>',
  generic: '<rect x="5" y="5" width="14" height="14" rx="4"/><path d="M9 9h6v6H9Z"/>',
  gear: '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09a1.65 1.65 0 0 0-1.08-1.51 1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09a1.65 1.65 0 0 0 1.51-1.08 1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33h.01a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51h.01a1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82v.01a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1Z"/>',
  refresh: '<path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/>',
  chev: '<path d="M6 9l6 6 6-6"/>',
  home: '<path d="M3 10.5 12 3l9 7.5"/><path d="M5 9.5V21h14V9.5"/><path d="M9 21v-6h6v6"/>',
  play: '<path d="M8 5v14l11-7Z" fill="currentColor" stroke="none"/>',
  timer: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5"/><path d="M9 2h6"/>',
  battery: '<rect x="2" y="7" width="17" height="10" rx="2"/><path d="M22 10.5v3"/>',
  meter: '<path d="M13 2 4 14h6l-1 8 9-12h-6l1-8Z"/>',
  drop: '<path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11Z"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  minus: '<path d="M5 12h14"/>',
  up: '<path d="M12 19V5"/><path d="M5 12l7-7 7 7"/>',
  down: '<path d="M12 5v14"/><path d="M19 12l-7 7-7-7"/>',
  stop: '<rect x="7" y="7" width="10" height="10" rx="2" fill="currentColor" stroke="none"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  alert: '<path d="M12 9v4"/><path d="M12 17h.01"/><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/>',
  eye: '<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12Z"/><circle cx="12" cy="12" r="3.2"/>',
  close: '<path d="M18 6 6 18M6 6l12 12"/>',
  eyeOff: '<path d="M4 4l16 16"/><path d="M10.6 5.7A9.9 9.9 0 0 1 12 5.5c6 0 9.5 6.5 9.5 6.5a17.6 17.6 0 0 1-3 3.7M6.5 6.8A17.3 17.3 0 0 0 2.5 12s3.5 6.5 9.5 6.5a9.4 9.4 0 0 0 3.5-.7"/><path d="M9.9 9.9a3 3 0 0 0 4.2 4.2"/>',
  wifi: '<path d="M2 8.8a15 15 0 0 1 20 0M5.5 12.5a10 10 0 0 1 13 0M9 16.2a5 5 0 0 1 6 0"/><path d="M12 20h.01"/>',
  user: '<circle cx="12" cy="8" r="4"/><path d="M4 21c1.5-4 5-5.5 8-5.5s6.5 1.5 8 5.5"/>',
  lock: '<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  cpu: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  cloud: '<path d="M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z"/>',
  vacuum: '<rect x="4" y="7" width="16" height="11" rx="5.5"/><circle cx="12" cy="11" r="2" fill="currentColor" stroke="none"/><path d="M4.5 15.5h15"/>',
  purifier: '<circle cx="12" cy="12" r="8.5"/><path d="M8 10h8M8 13.5h5"/>',
  locate: '<circle cx="12" cy="12" r="6.5"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3"/><circle cx="12" cy="12" r="1.6" fill="currentColor" stroke="none"/>',
  brand: '<path d="M256 96 108 220v172a16 16 0 0 0 16 16h264a16 16 0 0 0 16-16V220Z" stroke-width="28"/><path d="M256 168a62 62 0 0 0-34 114v22a8 8 0 0 0 8 8h52a8 8 0 0 0 8-8v-22a62 62 0 0 0-34-114Z"/><path d="M238 342h36"/>',
};

/** Inline SVG icon (currentColor). */
export function icon(name) {
  const body = ICONS[name] || ICONS.generic;
  const viewBox = name === 'brand' ? '0 0 512 512' : '0 0 24 24';
  const stroke = name === 'brand' ? 'stroke-width="24"' : 'stroke-width="1.8"';
  return `<svg viewBox="${viewBox}" fill="none" stroke="currentColor" ${stroke} stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;
}

/** The brand logo (house + bulb) sized for a gradient tile. */
export function brandLogo() {
  return icon('brand');
}

/** RSSI -> 1..4 bars. */
export function rssiBars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -80 ? 2 : 1;
  let h = '';
  for (let i = 1; i <= 4; i++) h += `<i${i <= n ? ' class="on"' : ''}></i>`;
  return `<span class="bars" title="${rssi} dBm">${h}</span>`;
}

/** Uptime ms -> localized "2 d 5 h". */
export function fmtUptime(ms) {
  const m = Math.floor(ms / 60000);
  const d = Math.floor(m / 1440);
  const h = Math.floor((m % 1440) / 60);
  const mm = m % 60;
  const parts = [];
  if (d > 0) parts.push(d + ' ' + t('dur_day'));
  if (h > 0) parts.push(h + ' ' + t('dur_hour'));
  if (parts.length === 0 && mm > 0) parts.push(mm + ' ' + t('dur_min'));
  if (parts.length === 0) parts.push('0 ' + t('dur_min'));
  return parts.join(' ');
}

export function fmtKb(bytes) {
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  return Math.round(bytes / 1024) + ' KB';
}

/** Log timestamp ms -> "m:ss" / "h:mm:ss". */
export function fmtLogTime(ms) {
  const sec = Math.floor((ms || 0) / 1000);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return h + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
  return m + ':' + String(s).padStart(2, '0');
}

export function mmss(sec) {
  sec = Math.max(0, Math.round(sec));
  return Math.floor(sec / 60) + ':' + String(sec % 60).padStart(2, '0');
}
