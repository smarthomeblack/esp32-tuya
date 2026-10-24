// Category refine layer (kt/fs/jsq/kj/cs/sd/cl) + the activity badge
// helpers that feed cardHead.

import { t } from '../core/i18n.js';
import { esc } from '../core/utils.js';
import { enumRange } from './cards-caps.js';

/** work_status -> activity key (kt). */
const KT_ACTIVITY = {
  standby: 'act_idle', idle: 'act_idle',
  cooling: 'act_cooling', cold: 'act_cooling',
  heating: 'act_heating', hot: 'act_heating',
  wet: 'act_dry', dehumidify: 'act_dry',
  wind: 'act_fan', fan: 'act_fan',
};
// Vacuum status classes.
const VAC_IDLE = ['standby', 'sleep', 'standby_st'];
const VAC_DOCKED = ['charging', 'chargecompleted', 'charge_done', 'docked', 'chargego'];
const VAC_RETURNING = ['docking', 'to_charge', 'goto_charge', 'returning'];

/** Vacuum status -> activity key, checked in order: idle, docked, returning, paused, fault, running. */
function vacuumActivityKey(dev, v) {
  const s = v.status ? String(dev.status[v.status] || '') : '';
  if (VAC_IDLE.includes(s)) return 'act_idle';
  if (VAC_DOCKED.includes(s)) return 'act_docked';
  if (VAC_RETURNING.includes(s)) return 'act_returning';
  if (s === 'paused' || s === 'pause') return 'act_paused';
  if (v.fault) {
    const f = +dev.status[v.fault];
    if (isFinite(f) && f !== 0) return 'act_error';
  }
  return s ? 'act_cleaning' : null;
}

/** Category refine: a layer ON TOP of the signature detection — claims
 *  its sections' DP codes and may override the identity. */
export function refineByCategory(dev, caps, keys, firstOf, claim) {
  switch (dev.category) {
    case 'kt': return refineKt(dev, caps, keys, firstOf, claim);
    case 'fs': case 'fsd': return refineFan(dev, caps, keys, firstOf, claim);
    case 'jsq': return refineHumidifier(dev, caps, keys, firstOf, claim);
    case 'kj': return refinePurifier(dev, caps, keys, firstOf, claim);
    case 'cs': return refineDehumidifier(dev, caps, keys, firstOf, claim);
    case 'sd': return refineVacuum(dev, caps, keys, firstOf, claim);
    case 'cl': return refineCurtain(dev, caps, keys, firstOf, claim);
    default: return null;
  }
}

/** kt AC: the mode select stays on mode/work_mode; fan_speed_enum gets
 *  its own labeled speed row. */
function refineKt(dev, caps, keys, firstOf, claim) {
  caps.modeCode = firstOf(['work_mode', 'mode']);
  caps.modeValues = caps.modeCode ? enumRange(dev, caps.modeCode) : [];
  caps.speedCode = keys.has('fan_speed_enum') ? 'fan_speed_enum' : null;
  caps.speedValues = caps.speedCode ? enumRange(dev, caps.speedCode) : [];
  caps.workStatus = keys.has('work_status') ? 'work_status' : null;
  if (keys.has('switch_horizontal')) caps.toggleRows.push({ code: 'switch_horizontal', key: 'osc_h' });
  if (keys.has('switch_vertical')) caps.toggleRows.push({ code: 'switch_vertical', key: 'osc_v' });
  if (keys.has('shake')) caps.toggleRows.push({ code: 'shake', key: 'swing_lbl' });
  if (keys.has('c_f')) caps.toggleRows.push({ code: 'c_f', text: '°C/°F' });
  // Enum claims mirror their mounts (cards.js guards on values length);
  // a valueless enum falls to the floor row instead of going invisible.
  if (caps.modeValues.length) claim(caps.modeCode);
  if (caps.speedValues.length) claim(caps.speedCode);
  for (const c of ['work_status', 'switch_horizontal', 'switch_vertical',
                   'shake', 'c_f']) claim(c);
  return { icon: 'climate', labelKey: 'type_ac' };
}

/** fs/fsd fan: the speed row takes priority over the mode select. */
function refineFan(dev, caps, keys, firstOf, claim) {
  caps.speedCode = keys.has('fan_speed_enum') ? 'fan_speed_enum' : null;
  caps.speedValues = caps.speedCode ? enumRange(dev, caps.speedCode) : [];
  caps.speedFirst = true;
  caps.modeCode = firstOf(['work_mode', 'mode']);
  caps.modeValues = caps.modeCode ? enumRange(dev, caps.modeCode) : [];
  caps.pctCode = firstOf(['fan_speed_percent', 'fan_speed']);
  if (keys.has('switch_horizontal')) caps.toggleRows.push({ code: 'switch_horizontal', key: 'osc_h' });
  if (keys.has('switch_vertical')) caps.toggleRows.push({ code: 'switch_vertical', key: 'osc_v' });
  if (caps.speedValues.length) claim(caps.speedCode);
  // The mode row mounts on modeValues alone (cards.js) — claim it even
  // beside a fan speed enum; an empty enum stays on the floor row.
  if (caps.modeValues.length) claim(caps.modeCode);
  if (caps.pctCode) claim(caps.pctCode);
  for (const c of ['switch_horizontal', 'switch_vertical']) claim(c);
  return { icon: 'fan', labelKey: 'type_fan' };
}

/** jsq humidifier: humidity_set stepper + spray select; humidity_current
 *  is the hero via METRIC_DEFS. */
function refineHumidifier(dev, caps, keys, firstOf, claim) {
  caps.stepper = keys.has('humidity_set')
    ? { code: 'humidity_set', fb: { min: 30, max: 80, step: 5 }, labelKey: 'humidity_set_lbl' }
    : null;
  const spray = firstOf(['spray_mode', 'level', 'work_mode', 'mode']);
  // Integer-only codes (e.g. level 1..3) fall to the floor rows.
  const sprayValues = spray ? enumRange(dev, spray) : [];
  if (sprayValues.length > 0) {
    caps.sprayCode = spray;
    caps.sprayValues = sprayValues;
    caps.modeCode = null;
    caps.modeValues = [];
    claim(spray);
  } else {
    caps.sprayCode = null;
    caps.sprayValues = [];
  }
  if (keys.has('humidity_set')) claim('humidity_set');
  return { icon: 'drop', labelKey: 'type_humidifier' };
}

/** kj purifier: anion/uv/wet/lock labeled toggles + filter_reset. */
function refinePurifier(dev, caps, keys, firstOf, claim) {
  const TOGGLES = [['anion', 'anion_lbl'], ['uv', 'uv_lbl'], ['wet', 'wet_lbl'], ['lock', 'lock_lbl']];
  for (const [code, key] of TOGGLES) {
    if (keys.has(code)) caps.toggleRows.push({ code: code, key: key });
    claim(code);
  }
  caps.filterReset = keys.has('filter_reset') ? 'filter_reset' : null;
  claim('filter_reset');
  return { icon: 'purifier', labelKey: 'type_purifier' };
}

/** cs dehumidifier: dehumidify_set_value stepper + speed select. */
function refineDehumidifier(dev, caps, keys, firstOf, claim) {
  caps.stepper = keys.has('dehumidify_set_value')
    ? { code: 'dehumidify_set_value', fb: { min: 30, max: 70, step: 5 }, labelKey: 'humidity_set_lbl' }
    : null;
  caps.speedCode = keys.has('fan_speed_enum') ? 'fan_speed_enum' : null;
  caps.speedValues = caps.speedCode ? enumRange(dev, caps.speedCode) : [];
  caps.modeCode = firstOf(['work_mode', 'mode']);
  caps.modeValues = caps.modeCode ? enumRange(dev, caps.modeCode) : [];
  if (caps.stepper) claim('dehumidify_set_value');
  if (caps.speedValues.length) claim(caps.speedCode);
  if (caps.modeValues.length) claim(caps.modeCode);
  return { icon: 'drop', labelKey: 'type_dehumidifier' };
}

/** sd vacuum: dedicated card driven by caps.vacuum. */
function refineVacuum(dev, caps, keys, firstOf, claim) {
  const v = {
    status: keys.has('status') ? 'status' : null,
    powerGo: firstOf(['power_go', 'power', 'switch']),
    mode: keys.has('mode') ? 'mode' : null,
    seek: keys.has('seek') ? 'seek' : null,
    suction: keys.has('suction') ? 'suction' : null,
    fault: keys.has('fault') ? 'fault' : null,
  };
  caps.vacuum = v;
  caps.modeCode = null;
  caps.modeValues = [];
  // Claim only rendered codes: the card's power control is the resolved
  // powerGo (power_go -> power -> switch); unchosen siblings fall to floor.
  for (const c of ['status', 'mode', 'seek', 'fault']) claim(c);
  if (v.powerGo) claim(v.powerGo);
  // The suction select mounts only with a non-empty range (buildVacuumCard).
  if (v.suction && enumRange(dev, v.suction).length > 0) claim(v.suction);
  return { icon: 'vacuum', labelKey: 'type_vacuum' };
}

/** cl curtain: percent_state/position feeds the slider display while the
 *  target stays percent_control; mach_operate FZ/ZZ/STOP the buttons. */
function refineCurtain(dev, caps, keys, firstOf, claim) {
  const ctrl = firstOf(['control', 'mach_operate']);
  const pos = firstOf(['percent_control', 'position']);
  const display = firstOf(['percent_state', 'position']);
  if (ctrl || pos || display) {
    caps.cover = caps.cover || { pos: null, ctrl: null, values: [] };
    if (ctrl) {
      caps.cover.ctrl = ctrl;
      caps.cover.values = enumRange(dev, ctrl);
    }
    if (pos && !caps.cover.pos) caps.cover.pos = pos;
    caps.cover.display = display;
  }
  for (let n = 2; n <= 4; n++) {
    const p = 'percent_control_' + n;
    if (!keys.has(p)) continue;
    const ex = {
      n: n, pos: p,
      display: keys.has('percent_state_' + n) ? 'percent_state_' + n : null,
    };
    caps.coverExtras.push(ex);
    // The target and its percent_state_N read-back render in the extra
    // slider (buildExtraSliders); control_N falls to the floor rows.
    claim(p);
    if (ex.display) claim(ex.display);
  }
  // Claim only rendered codes: the slider target (pos), its display
  // (feeds the slider read-back), and a valued ctrl (buttons).
  if (pos) claim(pos);
  if (pos && display) claim(display);
  if (ctrl && enumRange(dev, ctrl).length > 0) claim(ctrl);
  return { icon: 'cover', labelKey: 'type_cover' };
}

/* ---- activity badge (kt work_status / sd status) ---- */

export const ACT_CLASS = {
  act_idle: 'c-idle', act_docked: 'c-ok', act_returning: 'c-ret',
  act_paused: 'c-warn', act_cleaning: 'c-clean', act_error: 'c-err',
  act_cooling: 'c-cool', act_heating: 'c-heat', act_dry: 'c-dry', act_fan: 'c-fan',
};

export function activityKey(dev, caps) {
  if (caps.vacuum) return vacuumActivityKey(dev, caps.vacuum);
  if (caps.workStatus) return KT_ACTIVITY[String(dev.status[caps.workStatus] || '').toLowerCase()] || null;
  return null;
}

/** Always an element (updater patches it; :empty hides the keyless one). */
export function activityBadge(dev, caps) {
  const key = activityKey(dev, caps);
  if (!key) return '<span class="act-badge"></span>';
  return `<span class="act-badge ${ACT_CLASS[key] || ''}">${esc(t(key))}</span>`;
}
