// Cover card: open/stop/close buttons + the position slider
// (percent_control / control DPs; cl adds percent_state display).

import { t } from '../core/i18n.js';
import { $, el, esc, icon } from '../core/utils.js';
import { numRange } from './cards-caps.js';
import { sendCommands } from './cards-commands.js';
import { bindSlider } from './cards.js';

/** Open/Close stay live only when their command has a value: a ctrl enum
 *  (FZ/ZZ/...) or a position DP; otherwise the fallback would be inert. */
export function buildCoverButtons(dev, caps) {
  const c = caps.cover;
  const hasCtrl = !!(c.ctrl && c.values.length > 0);
  const canOpen = hasCtrl || !!c.pos;
  // cl mach_operate enums use FZ/ZZ/STOP (open/close/stop).
  const openV = hasCtrl ? (c.values.find(v => /open|up/i.test(v)) ||
    c.values.find(v => String(v).toUpperCase() === 'FZ') || 'open') : null;
  const closeV = hasCtrl ? (c.values.find(v => /close|down/i.test(v)) ||
    c.values.find(v => String(v).toUpperCase() === 'ZZ') || 'close') : null;
  const stopV = hasCtrl ? (c.values.find(v => /stop|pause/i.test(v)) || 'stop') : null;

  const node = el(`<div>
    <div class="cover-btns">
      <button class="cover-btn" data-act="open" ${canOpen ? '' : 'disabled'}>${icon('up')} ${t('cover_open')}</button>
      <button class="cover-btn" data-act="stop" ${hasCtrl ? '' : 'disabled'}>${icon('stop')} ${t('cover_stop')}</button>
      <button class="cover-btn" data-act="close" ${canOpen ? '' : 'disabled'}>${icon('down')} ${t('cover_close')}</button>
    </div>
  </div>`);

  const send = (code, value) => sendCommands(dev.id, [{ code, value }]);
  const openBtn = $('[data-act="open"]', node);
  if (openBtn && !openBtn.disabled) {
    openBtn.addEventListener('click', () => {
      if (openV) send(c.ctrl, openV);
      else send(c.pos, 100);
    });
  }
  const closeBtn = $('[data-act="close"]', node);
  if (closeBtn && !closeBtn.disabled) {
    closeBtn.addEventListener('click', () => {
      if (closeV) send(c.ctrl, closeV);
      else send(c.pos, 0);
    });
  }
  const stopBtn = $('[data-act="stop"]', node);
  if (stopBtn && !stopBtn.disabled) {
    stopBtn.addEventListener('click', () => { if (stopV) send(c.ctrl, stopV); });
  }
  return node;
}

export function buildPositionSlider(dev, caps) {
  const c = caps.cover;
  const code = c.pos;
  const r = numRange(dev, code, { min: 0, max: 100, step: 1 });
  // cl: percent_state/position reports the ACTUAL position; the target
  // still posts to percent_control.
  const disp = c.display && isFinite(+dev.status[c.display]) ? c.display : code;
  const cur = isFinite(+dev.status[disp]) ? +dev.status[disp] : 0;
  const node = el(`<div>
    <div class="slider-row" data-slider="pos">
      <div class="slider-head"><span>${esc(t('cover_position'))}</span><span class="val">${cur}${t('unit_percent')}</span></div>
      <div class="slider-wrap">
        <input type="range" min="${r.min}" max="${r.max}" step="${r.step}" value="${cur}" aria-label="${esc(t('cover_position'))}">
        <span class="slider-bubble">${cur}${t('unit_percent')}</span>
      </div>
    </div>
  </div>`);
  bindSlider(node, dev, '[data-slider="pos"]', code, v => {
    sendCommands(dev.id, [{ code, value: v }]);
  }, v => v + t('unit_percent'), () => {
    // cl: percent_state/position reports the ACTUAL position.
    const d = c.display && isFinite(+dev.status[c.display]) ? c.display : code;
    const v = +dev.status[d];
    return isFinite(v) ? v : null;
  });
  return node;
}

/** cl percent_control_2..4: the extra curtain channels' sliders. */
export function buildExtraSliders(dev, ex) {
  const r = numRange(dev, ex.pos, { min: 0, max: 100, step: 1 });
  const disp = ex.display && isFinite(+dev.status[ex.display]) ? ex.display : ex.pos;
  const cur = isFinite(+dev.status[disp]) ? +dev.status[disp] : 0;
  const label = `${t('cover_position')} ${ex.n}`;
  const node = el(`<div>
    <div class="slider-row" data-slider="pos${ex.n}">
      <div class="slider-head"><span>${esc(label)}</span><span class="val">${cur}${t('unit_percent')}</span></div>
      <div class="slider-wrap">
        <input type="range" min="${r.min}" max="${r.max}" step="${r.step}" value="${cur}" aria-label="${esc(label)}">
        <span class="slider-bubble">${cur}${t('unit_percent')}</span>
      </div>
    </div>
  </div>`);
  bindSlider(node, dev, `[data-slider="pos${ex.n}"]`, ex.pos, v => {
    sendCommands(dev.id, [{ code: ex.pos, value: v }]);
  }, v => v + t('unit_percent'), () => {
    const d = ex.display && isFinite(+dev.status[ex.display]) ? ex.display : ex.pos;
    const v = +dev.status[d];
    return isFinite(v) ? v : null;
  });
  return node;
}
