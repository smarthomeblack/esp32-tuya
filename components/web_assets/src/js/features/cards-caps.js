// Capability detection: status_range parsing, METRIC_DEFS,
// detectCapabilities + the generic-floor entry builder.

import { refineByCategory } from './cards-refine.js';

/** Union of status + status_range DP codes. */
export function deviceKeys(d) {
  const keys = new Set();
  if (d.status) for (const k in d.status) keys.add(k);
  if (d.status_range) for (const k in d.status_range) keys.add(k);
  return keys;
}

/** Parse the raw status_range values JSON string. */
export function rangeInfo(d, code) {
  try {
    const sr = d.status_range && d.status_range[code];
    if (!sr || typeof sr.values !== 'string') return null;
    const v = JSON.parse(sr.values);
    return (v && typeof v === 'object') ? v : null;
  } catch (e) { return null; }
}

/** Raw status_range entry (type/name/writable fields) or null. */
function srEntry(d, code) {
  return (d.status_range && d.status_range[code]) || null;
}

/** DP display label: status_range name, else the code. */
function dpName(dev, code) {
  const sr = srEntry(dev, code);
  return sr && typeof sr.name === 'string' && sr.name ? sr.name : code;
}

/** Writable per contract: field absent -> false (display-only). */
export function dpWritable(dev, code) {
  const sr = srEntry(dev, code);
  return !!(sr && sr.writable === true);
}

/** Numeric range {min,max,step,scale} with fallback. */
export function numRange(d, code, fb) {
  const r = rangeInfo(d, code) || {};
  const num = x => (typeof x === 'number' && isFinite(x)) ? x : null;
  return {
    min: num(r.min) != null ? r.min : fb.min,
    max: num(r.max) != null ? r.max : fb.max,
    step: num(r.step) != null && r.step > 0 ? r.step : fb.step,
    // Fallback scale honored (raw/10^scale display); absent -> integer.
    scale: num(r.scale) != null ? r.scale : (fb.scale || 0),
  };
}

/** Enum range values (dropdown options). */
export function enumRange(d, code) {
  const r = rangeInfo(d, code);
  return (r && Array.isArray(r.range)) ? r.range.filter(v => v != null) : [];
}

/** A range entry declares an Integer spec (numeric family claim is safe). */
function hasIntegerRange(d, code) {
  const r = rangeInfo(d, code);
  return !!(r && (r.min != null || r.max != null || r.scale != null));
}

/** Display value: raw DP value / 10^scale. */
export function scaleVal(raw, scale) {
  const n = Number(raw);
  if (!isFinite(n)) return null;
  const f = n / Math.pow(10, scale || 0);
  return Math.abs(f - Math.round(f)) < 1e-9 ? Math.round(f) : Math.round(f * 10) / 10;
}

/** Metric readout families: DP codes -> {label, unit, icon, hero} in
 *  priority order; per-channel codes render as labeled chips. */
const METRIC_DEFS = [
  { codes: ['cur_power', 'cur_power_1', 'cur_power1', 'cur_power2'], kind: 'power',
    label: 'meter_power_short', unit: 'kW', hero: true },
  { codes: ['total_forward_energy', 'forward_energy_total', 'total_energy1', 'total_energy2'], kind: 'energy',
    label: 'meter_energy_short', unit: 'kWh', hero: true },
  { codes: ['cur_voltage', 'cur_voltage_1', 'cur_voltage1', 'cur_voltage2'], kind: 'voltage',
    label: 'meter_voltage_short', unit: 'V', hero: true },
  { codes: ['cur_current', 'cur_current_1', 'cur_current1', 'cur_current2'], kind: 'current',
    label: 'meter_current_short', unit: 'A', hero: true },
  // Air-quality sensor pass: pm25 wins the hero over temp.
  { codes: ['pm25'], kind: 'pm25', label: 'pm25_lbl', unitKey: 'unit_ugm3', hero: true },
  { codes: ['tvoc'], kind: 'tvoc', unit: 'ppb', hero: false },
  { codes: ['eco2', 'co2'], kind: 'co2', unitKey: 'unit_ppm', hero: false },
  { codes: ['air_quality'], kind: 'air', hero: false },
  { codes: ['temp_current', 'temp'], kind: 'temp',
    label: 'meter_temp_short', unit: '°C', hero: true },
  { codes: ['humidity', 'humi_value', 'humidity_current', 'humidity_indoor'], kind: 'humidity',
    label: null, icon: 'drop', unit: '%', hero: true },
  { codes: ['supply_frequency'], kind: 'freq',
    label: null, unit: 'Hz', hero: true },
  { codes: ['leakage_current'], kind: 'leak',
    label: 'meter_leak_short', unit: 'mA', hero: false },
  { codes: ['today_acc_energy1', 'today_acc_energy2'], kind: 'energy_today',
    label: 'meter_energy_today', unit: 'kWh', hero: false },
  { codes: ['filter'], kind: 'filter', label: 'filter_lbl', unit: '%', hero: false },
  { codes: ['battery'], kind: 'battery',
    label: null, icon: 'battery', unit: '%', hero: false },
  { codes: ['clean_area'], kind: 'clean_area', label: 'clean_area_lbl', unitKey: 'unit_m2', hero: false },
  { codes: ['clean_time'], kind: 'clean_time', label: 'clean_time_lbl', unitKey: 'unit_min', hero: false },
  { codes: ['clean_count'], kind: 'clean_count', label: 'clean_count_lbl', unit: '', hero: false },
];

/** Scans status + status_range keys once -> a CAPABILITY descriptor
 *  (which UI sections the device supports; no type classification). */
export function detectCapabilities(dev) {
  const keys = deviceKeys(dev);
  const has = c => keys.has(c);
  const firstOf = list => list.find(c => keys.has(c)) || null;
  const claimed = new Set();
  const claim = c => { if (c) claimed.add(c); };

  const caps = { claimed: claimed };

  // ---- switch family: head toggle + per-channel rows ----
  caps.channels = [];
  for (let i = 1; i <= 6; i++) {
    const c = 'switch_' + i;
    if (has(c)) { caps.channels.push(c); claim(c); }
  }
  // Whole countdown family: every key starting countdown*/fan_countdown*
  // (countdown_1..6, countdown_usb*, countdown_left, fan_countdown*...).
  caps.countdowns = [];
  for (const k of keys) {
    if (/^(countdown|fan_countdown)/.test(k)) caps.countdowns.push(k);
  }
  caps.countdowns.sort();
  for (const c of caps.countdowns) claim(c);
  // Primary toggle: lights use switch_led; multi-channel devices get no
  // head toggle; else switch/switch_fan/switch_1.
  caps.switch = null;
  if (has('switch_led')) caps.switch = 'switch_led';
  else if (caps.channels.length >= 2) caps.switch = null;
  else if (has('switch')) caps.switch = 'switch';
  else if (has('switch_fan')) caps.switch = 'switch_fan';
  else if (caps.channels.length === 1) caps.switch = caps.channels[0];

  // ---- light controls ----
  caps.brightCode = firstOf(['bright_value', 'bright_value_1', 'bright_value_v2']);
  caps.colourCode = firstOf(['colour_data', 'colour_data_v2', 'colour_data_hsv']);
  caps.cctCode = firstOf(['temp_value', 'colour_temp_value']);
  caps.workMode = has('work_mode') ? 'work_mode' : null;

  // ---- mode select (any enum DP: work_mode / mode / fan_speed_enum) ----
  caps.modeCode = firstOf(['work_mode', 'mode', 'fan_speed_enum']);
  caps.modeValues = caps.modeCode ? enumRange(dev, caps.modeCode) : [];

  // ---- climate stepper ----
  caps.tempSet = has('temp_set') ? 'temp_set' : null;

  // ---- cover buttons + position slider ----
  caps.cover = (has('percent_control') || has('situation_set') || has('control')) ? {
    pos: has('percent_control') ? 'percent_control' : null,
    ctrl: has('control') ? 'control' : null,
    values: enumRange(dev, 'control'),
  } : null;

  // ---- claimed: codes consumed by a card section ----
  // A claimed code never re-renders in the generic floor (the head
  // toggle, sliders, wheel, steppers, chips... own their codes).
  claim(caps.switch);
  claim(caps.brightCode); claim(caps.colourCode); claim(caps.cctCode);
  // The mode select renders only with a known enum range; without one
  // code keeps the raw-row fallback (present-but-unpicked).
  if (caps.modeValues.length) claim(caps.modeCode);
  claim(caps.tempSet);
  if (caps.cover) { claim(caps.cover.pos); claim(caps.cover.ctrl); }

  // ---- fan identity (its speed select arrives via modeCode) ----
  caps.fanSpeed = has('fan_speed_enum') || has('switch_fan');

  // ---- category refine: claims its DPs BEFORE the leftover scan ----
  caps.speedCode = null; caps.speedValues = []; caps.speedFirst = false;
  caps.pctCode = null; caps.stepper = null;
  caps.sprayCode = null; caps.sprayValues = [];
  caps.workStatus = null; caps.toggleRows = [];
  caps.filterReset = null; caps.vacuum = null;
  caps.coverExtras = [];
  const refine = refineByCategory(dev, caps, keys, firstOf, claim);

  // ---- metric readouts (known families; everything else -> the floor) ----
  caps.metrics = [];
  for (const def of METRIC_DEFS) {
    for (const code of def.codes) {
      if (!has(code)) continue;
      // Non-numeric values (e.g. Enum air_quality) skip the metric chips.
      if (typeof dev.status[code] !== 'number' && !hasIntegerRange(dev, code)) continue;
      const r = numRange(dev, code, { min: 0, max: 1e9, step: 1, scale: 0 });
      caps.metrics.push({
        code: code, kind: def.kind, label: def.label || null, icon: def.icon || null,
        unit: def.unit, unitKey: def.unitKey || null, hero: !!def.hero, scale: r.scale,
      });
      claim(code);
    }
  }
  caps.hero = caps.metrics.find(m => m.hero) || null;

  // ---- generic floor: every remaining DP renders from its metadata ----
  caps.generic = [];
  for (const code of Array.from(keys).sort()) {
    if (claimed.has(code)) continue;
    caps.generic.push(genericEntry(dev, code));
  }

  // ---- card identity (icon + fallback sub-label; refine may override) ----
  const id = capIdentity(caps);
  caps.icon = (refine && refine.icon) || id.icon;
  caps.labelKey = (refine && refine.labelKey) || id.labelKey;
  return caps;
}

/** Head icon + fallback sub-label, from the dominant capability. */
function capIdentity(caps) {
  if (caps.brightCode || caps.colourCode || caps.cctCode) return { icon: 'bulb', labelKey: 'type_light' };
  if (caps.tempSet) return { icon: 'climate', labelKey: 'type_climate' };
  if (caps.cover) return { icon: 'cover', labelKey: 'type_cover' };
  if (caps.fanSpeed) return { icon: 'fan', labelKey: 'type_fan' };
  if (caps.hero) {
    const sense = caps.hero.kind === 'temp' || caps.hero.kind === 'humidity';
    return sense ? { icon: 'thermo', labelKey: 'type_sensor' } : { icon: 'meter', labelKey: 'type_meter' };
  }
  if (caps.channels.length > 1 || caps.countdowns.length > 0) return { icon: 'plug', labelKey: 'type_socket' };
  if (caps.switch) return { icon: 'power', labelKey: 'type_switch' };
  return { icon: 'generic', labelKey: 'type_generic' };
}

/** Floor entry: type (status_range, else inferred from values/value),
 *  name, writable and the parsed values JSON bits. */
function genericEntry(dev, code) {
  const sr = srEntry(dev, code);
  const vals = rangeInfo(dev, code);
  let type = sr && typeof sr.type === 'string' ? sr.type : null;
  if (!type && vals) {
    if (Array.isArray(vals.range)) type = 'Enum';
    else if (vals.min != null || vals.max != null || vals.scale != null) type = 'Integer';
    else if (Array.isArray(vals.label)) type = 'Bitmap';
  }
  if (!type) {
    const v = dev.status[code];
    type = typeof v === 'boolean' ? 'Boolean' : (typeof v === 'number' ? 'Integer' : 'String');
  }
  // Json/Raw/unknown -> the String row (labeled raw value).
  if (type !== 'Boolean' && type !== 'Integer' && type !== 'Enum' && type !== 'Bitmap') {
    type = 'String';
  }
  return {
    code: code,
    name: dpName(dev, code),
    writable: dpWritable(dev, code),
    type: type,
    scale: vals && typeof vals.scale === 'number' && isFinite(vals.scale) ? vals.scale : 0,
    unit: vals && typeof vals.unit === 'string' ? vals.unit : '',
    range: vals && Array.isArray(vals.range) ? vals.range.filter(v => v != null) : [],
    // Label list: "label" is the wire key, "labels" the firmware alias.
    labels: vals && Array.isArray(vals.label) ? vals.label
      : (vals && Array.isArray(vals.labels) ? vals.labels : null),
    hasRange: !!(vals && (vals.min != null || vals.max != null)),
  };
}
