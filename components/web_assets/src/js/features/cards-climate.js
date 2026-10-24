// Climate card: the target-temperature stepper (temp_set DP) + the
// percent stepper (humidity_set / dehumidify_set_value DPs).

import { t } from '../core/i18n.js';
import { $, $$, clamp, el, esc, icon } from '../core/utils.js';
import { numRange } from './cards-caps.js';
import { sendCommands } from './cards-commands.js';
import { registerUpdater } from './cards-registry.js';

/** Stepper node: raw value / 10^scale display, step-rounded sends; range
 *  from status_range or `fb`. `opts.dash` -> "—" while no value exists. */
function buildStepper(dev, code, fb, label, unit, opts) {
  const r = numRange(dev, code, fb);
  const dash = !!(opts && opts.dash);
  const hasVal = isFinite(+dev.status[code]);
  const setVal = hasVal ? +dev.status[code] : Math.round((r.min + r.max) / 2);
  const fmt = v => (v / Math.pow(10, r.scale)).toFixed(r.scale > 0 ? 1 : 0);
  const shown = (hasVal || !dash) ? fmt(setVal) : '—';
  const node = el(`<div>
    <div class="stepper">
      <button data-step="-1" aria-label="-">${icon('minus')}</button>
      <div class="target">${shown}${unit ? `<span class="u"> ${esc(unit)}</span>` : ''}</div>
      <button data-step="1" aria-label="+">${icon('plus')}</button>
    </div>
    <div class="card-sub" style="margin:6px 2px 0">${esc(label)}</div>
  </div>`);
  const txt = $('.target', node).firstChild;
  const sendSet = v => {
    const val = clamp(Math.round(v / r.step) * r.step, r.min, r.max);
    sendCommands(dev.id, [{ code, value: val }]);
  };
  $$('[data-step]', node).forEach(btn => {
    btn.addEventListener('click', () => {
      const dir = +btn.getAttribute('data-step');
      const cur = isFinite(+dev.status[code]) ? +dev.status[code] : setVal;
      sendSet(cur + dir * r.step);
    });
  });
  registerUpdater(() => {
    const v = +dev.status[code];
    if (!isFinite(v)) { if (dash) txt.textContent = '—'; return; }
    txt.textContent = fmt(v);
  });
  return node;
}

export function buildTempStepper(dev, caps) {
  return buildStepper(dev, caps.tempSet, { min: 160, max: 300, step: 5, scale: 1 },
    t('climat_target'), t('unit_c'));
}

/** Percent stepper for humidity_set / dehumidify_set_value (jsq/cs). */
export function buildValueStepper(dev, code, fb, labelKey) {
  return buildStepper(dev, code, fb, t(labelKey), t('unit_percent'));
}

/** Generic-floor stepper: literal label/unit, "—" until a value exists. */
export function buildFloorStepper(dev, code, fb, label, unit) {
  return buildStepper(dev, code, fb, label, unit, { dash: true });
}
