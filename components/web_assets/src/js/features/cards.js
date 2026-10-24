// Card composition (buildCard/buildVacuumCard) + the shared section
// builders; per-type builders live in cards-light/-climate/-cover.

import { t } from '../core/i18n.js';
import { $, $$, el, esc, icon } from '../core/utils.js';
import { emit } from '../core/events.js';
import { buildSlider, buildColorWheel } from './cards-light.js';
import { buildTempStepper, buildValueStepper, buildFloorStepper } from './cards-climate.js';
import { buildCoverButtons, buildPositionSlider, buildExtraSliders } from './cards-cover.js';
import { rangeInfo, dpWritable, numRange, enumRange, scaleVal } from './cards-caps.js';
import { activityBadge, activityKey, ACT_CLASS } from './cards-refine.js';
import { sendCommands } from './cards-commands.js';
import { beginCard, finishCard, registerUpdater, syncCardState } from './cards-registry.js';

/* ---- card composition (shared sections) ---- */

const MODE_LABELS = {
  cool: 'mode_cool', cold: 'mode_cool', heat: 'mode_heat', hot: 'mode_heat',
  fan: 'mode_fan', wind: 'mode_fan', dry: 'mode_dry', auto: 'mode_auto',
  off: 'mode_off', white: 'mode_white', colour: 'mode_colour',
  color: 'mode_colour', scene: 'mode_scene', music: 'mode_music',
  low: 'mode_low', middle: 'mode_middle', high: 'mode_high',
  sleep: 'mode_sleep', normal: 'mode_normal', strong: 'mode_strong',
  level_1: 'mode_level_1', level_2: 'mode_level_2', level_3: 'mode_level_3',
};

function modeLabel(v) {
  const key = MODE_LABELS[String(v).toLowerCase()];
  return key ? t(key) : String(v);
}

/** Compose one device card from whatever the device has. */
export function buildCard(dev, caps) {
  if (caps.vacuum) return buildVacuumCard(dev, caps);
  const on = caps.switch ? !!dev.status[caps.switch] : true;
  // wide ignores the on/off state: the grid placement must never jump.
  const wide = !!(caps.colourCode || caps.hero || caps.channels.length > 1 ||
    caps.countdowns.length > 0 || caps.tempSet || caps.cover || caps.filterReset ||
    caps.stepper || caps.speedCode || caps.pctCode ||
    caps.modeValues.length > 0 || caps.sprayValues.length > 0);
  const node = el(`<article class="card${wide ? ' wide' : ''}" data-dev="${esc(dev.id)}"></article>`);
  const root = el('<div></div>');
  beginCard(node, dev);

  root.innerHTML = cardHead(dev, caps, caps.switch ? toggleHtml(on, t(caps.labelKey)) : '');
  registerHeadUpdaters(root, dev, caps);
  if (caps.switch) bindToggle(root, dev, () => caps.switch);
  // Card gear -> the entity editor / remove dialog.
  const gearBtn = $('.card-gear', root);
  if (gearBtn) gearBtn.addEventListener('click', () => emit('entity:edit', dev.id));
  if (caps.channels.length) root.appendChild(buildChannelToggles(dev, caps));

  // Controls — always mounted; CSS hides them while the head switch is off.
  const ctrl = el('<div class="ctrl"></div>');
  if (caps.brightCode) ctrl.appendChild(buildSlider(dev, caps, 'bright'));
  if (caps.cctCode) ctrl.appendChild(buildSlider(dev, caps, 'cct'));
  if (caps.colourCode) ctrl.appendChild(buildColorWheel(dev, caps));
  if (caps.tempSet) ctrl.appendChild(buildTempStepper(dev, caps));
  if (caps.stepper) {
    ctrl.appendChild(buildValueStepper(dev, caps.stepper.code, caps.stepper.fb, caps.stepper.labelKey));
  }
  if (caps.speedFirst && caps.speedValues.length) {
    ctrl.appendChild(buildEnumRow(dev, caps.speedCode, caps.speedValues, t('fan_speed_lbl')));
  }
  if (caps.modeValues.length) {
    // modeCode may BE fan_speed_enum (no work_mode/mode) — label it so.
    const modeKey = caps.modeCode === 'fan_speed_enum' ? 'fan_speed' : 'mode_lbl';
    ctrl.appendChild(buildEnumRow(dev, caps.modeCode, caps.modeValues, t(modeKey)));
  }
  if (!caps.speedFirst && caps.speedValues.length) {
    ctrl.appendChild(buildEnumRow(dev, caps.speedCode, caps.speedValues, t('fan_speed_lbl')));
  }
  if (caps.sprayValues.length) {
    ctrl.appendChild(buildEnumRow(dev, caps.sprayCode, caps.sprayValues, t('spray_mode_lbl')));
  }
  if (caps.pctCode) ctrl.appendChild(buildFanSlider(dev, caps));
  if (caps.cover && (caps.cover.ctrl || caps.cover.pos)) {
    ctrl.appendChild(buildCoverButtons(dev, caps));
    if (caps.cover.pos) ctrl.appendChild(buildPositionSlider(dev, caps));
  }
  for (const ex of caps.coverExtras) ctrl.appendChild(buildExtraSliders(dev, ex));
  root.appendChild(ctrl);

  // Labeled config rows + momentary actions — visible whatever the state.
  if (caps.toggleRows.length) root.appendChild(buildToggleRows(dev, caps.toggleRows));
  if (caps.filterReset) root.appendChild(buildMomentary(dev, caps.filterReset, 'filter_reset_lbl'));

  // Readouts — always visible, whatever the switch state.
  if (caps.metrics.length) root.appendChild(buildMetrics(dev, caps));
  const timers = buildCountdowns(dev, caps);
  if (timers) root.appendChild(timers);
  const floor = buildGeneric(dev, caps);
  if (floor) root.appendChild(floor);

  node.appendChild(root);
  finishCard(node, dev, caps);
  syncCardState(node, dev, caps);
  return node;
}

/** The sd robot-vacuum card: activity badge + Start/Pause (power_go) +
 *  Home/Locate + suction select + battery/clean metrics. */
function buildVacuumCard(dev, caps) {
  const v = caps.vacuum;
  const node = el(`<article class="card wide" data-dev="${esc(dev.id)}"></article>`);
  const root = el('<div></div>');
  beginCard(node, dev);
  root.innerHTML = cardHead(dev, caps, '');
  registerHeadUpdaters(root, dev, caps);
  const gearBtn = $('.card-gear', root);
  if (gearBtn) gearBtn.addEventListener('click', () => emit('entity:edit', dev.id));

  const btn = (act, key, ic, primary) =>
    `<button class="vac-btn${primary ? ' primary' : ''}" data-act="${act}">${icon(ic)} ${t(key)}</button>`;
  const acts = el(`<div>
    <div class="vac-btns">
      ${v.powerGo ? btn('start', 'vac_start', 'play', true) + btn('pause', 'vac_pause', 'stop', true) : ''}
    </div>
    <div class="vac-btns">
      ${v.mode ? btn('home', 'vac_home', 'home', false) : ''}
      ${v.seek ? btn('locate', 'vac_locate', 'locate', false) : ''}
    </div>
  </div>`);
  $$('[data-act]', acts).forEach(b => {
    b.addEventListener('click', () => {
      const act = b.getAttribute('data-act');
      if (act === 'start' && v.powerGo) sendCommands(dev.id, [{ code: v.powerGo, value: true }]);
      else if (act === 'pause' && v.powerGo) sendCommands(dev.id, [{ code: v.powerGo, value: false }]);
      else if (act === 'home' && v.mode) sendCommands(dev.id, [{ code: v.mode, value: 'chargego' }]);
      else if (act === 'locate' && v.seek) sendCommands(dev.id, [{ code: v.seek, value: true }]);
    });
  });
  root.appendChild(acts);

  if (v.suction) {
    const vals = enumRange(dev, v.suction);
    if (vals.length) root.appendChild(buildEnumRow(dev, v.suction, vals, t('suction_lbl')));
  }
  if (caps.metrics.length) root.appendChild(buildMetrics(dev, caps));
  // Unclaimed rows survive on the vacuum too (mop/voice/countdown...).
  if (caps.toggleRows.length) root.appendChild(buildToggleRows(dev, caps.toggleRows));
  const timers = buildCountdowns(dev, caps);
  if (timers) root.appendChild(timers);
  const floor = buildGeneric(dev, caps);
  if (floor) root.appendChild(floor);

  node.appendChild(root);
  finishCard(node, dev, caps);
  syncCardState(node, dev, caps);
  return node;
}

/* ---- shared card header ---- */

function cardHead(dev, caps, rightHtml, sub) {
  const subText = sub != null ? sub : cardSubText(dev, caps);
  // Badges always mount (stable structure); CSS hides the inactive ones.
  const badge = `<span class="badge-offline">${esc(t('offline_badge'))}</span>`;
  // The per-card gear — its own button so it never triggers the toggle.
  const gear = `<button class="card-gear" data-gear="${esc(dev.id)}" aria-label="${esc(t('entity_title'))}">${icon('gear')}</button>`;
  return `
    <div class="card-top">
      <span class="pending-mark" aria-hidden="true"></span>
      <div class="card-ico">${icon(caps.icon)}</div>
      <div style="flex:1;min-width:0">
        <div class="card-name">${esc(dev.name)}</div>
        <div class="card-sub">${esc(subText)}</div>
      </div>
      ${badge}${activityBadge(dev, caps)}${gear}${rightHtml || ''}
    </div>`;
}

/** Head in-place patch: name, sub-label, activity badge, head toggle. */
function registerHeadUpdaters(root, dev, caps) {
  const nameEl = $('.card-name', root);
  const subEl = $('.card-sub', root);
  const actEl = $('.act-badge', root);
  const toggle = $('.toggle', root);
  registerUpdater(() => {
    if (nameEl) nameEl.textContent = dev.name;
    if (subEl) subEl.textContent = cardSubText(dev, caps);
    if (actEl) {
      const key = activityKey(dev, caps);
      actEl.className = 'act-badge' + (key && ACT_CLASS[key] ? ' ' + ACT_CLASS[key] : '');
      actEl.textContent = key ? t(key) : '';
    }
    if (toggle && caps.switch) {
      const on = !!dev.status[caps.switch];
      toggle.classList.toggle('on', on);
      toggle.setAttribute('aria-checked', String(on));
    }
  });
}

/** Head sub-label: the primary metric ("25.4 kW"), else the on/off
 *  state, else the capability label. */
function cardSubText(dev, caps) {
  const hero = pickHero(dev, caps);
  if (hero) {
    const v = metricValue(dev, hero);
    if (v != null) return v + ' ' + metricUnit(hero);
  }
  if (caps.switch) {
    const on = !!dev.status[caps.switch];
    // Lights surface their active mode instead of a bare on/off.
    if (on && caps.workMode && dev.status[caps.workMode] &&
        (caps.brightCode || caps.cctCode || caps.colourCode)) {
      return modeLabel(dev.status[caps.workMode]);
    }
    return on ? t('value_on') : t('value_off');
  }
  return t(caps.labelKey);
}

/** Toggle control bound to a command. */
function toggleHtml(on, ariaLabel) {
  return `<button class="toggle${on ? ' on' : ''}" role="switch" aria-checked="${!!on}" aria-label="${esc(ariaLabel)}"></button>`;
}

function bindToggle(node, dev, getCode) {
  const btn = $('.toggle', node);
  if (!btn) return;
  btn.addEventListener('click', () => {
    const code = getCode();
    if (!code) return;
    const cur = !!dev.status[code];
    sendCommands(dev.id, [{ code, value: !cur }]);
  });
}

/* ---- generic slider wiring (bright / cct / pos rows) ---- */

/** Slider wiring: bubble + gradient fill while dragging, command on
 *  release. `fmt` (optional) relabels the bubble with the raw value;
 *  `read` (optional) syncs the slider from status (skipped mid-drag). */
export function bindSlider(root, dev, sel, code, commit, fmt, read) {
  const row = $(sel, root);
  if (!row) return;
  const input = $('input[type="range"]', row);
  const bubble = $('.slider-bubble', row);
  const valEl = $('.val', row);
  const wrap = $('.slider-wrap', row);
  const isCct = sel.indexOf('cct') >= 0;

  const paint = () => {
    const min = +input.min, max = +input.max;
    const v = +input.value;
    const pct = (v - min) / (max - min);
    if (!isCct) {
      input.style.setProperty('--track',
        `linear-gradient(90deg,var(--accent) 0%,var(--accent2) ${pct * 100}%,rgba(255,255,255,.12) ${pct * 100}%)`);
    }
    const w = wrap.clientWidth;
    bubble.style.left = (12 + pct * (w - 24)) + 'px';
    const label = fmt ? fmt(v) : Math.round(pct * 100) + '%';
    bubble.textContent = label;
    if (valEl) valEl.textContent = label;
  };

  const send = () => { commit(+input.value); };

  input.addEventListener('input', paint);
  input.addEventListener('change', send);
  input.addEventListener('pointerdown', () => wrap.classList.add('dragging'));
  const endDrag = () => wrap.classList.remove('dragging');
  input.addEventListener('pointerup', endDrag);
  input.addEventListener('pointercancel', endDrag);
  paint();
  if (read) registerUpdater(() => {
    // A drag whose ctrl got hidden (off flip) never sees pointerup —
    // clear the stale flag; a live drag still owns the thumb.
    if (wrap.classList.contains('dragging')) {
      if (input.offsetParent) return;
      wrap.classList.remove('dragging');
    }
    const v = read();
    if (v == null || !isFinite(v)) return;
    if (+input.value !== v) input.value = v;
    paint();
  });
}

/* ---- channel toggles + countdown timers ---- */

/** Countdown DP meta: display unit (values JSON, default "s"), minutes
 *  per raw unit, raw min/max/step and writability. */
function cdMeta(dev, code) {
  const r = rangeInfo(dev, code) || {};
  const unit = typeof r.unit === 'string' && r.unit ? r.unit : 's';
  // minutes per raw unit: s -> 1/60, ms -> 1/60000, min -> 1, h -> 60.
  const per = unit === 'ms' || unit === 'msec' ? 1 / 60000
    : (unit === 'min' || unit === 'minute' ? 1 : (unit === 'h' || unit === 'hour' ? 60 : 1 / 60));
  const num = x => (typeof x === 'number' && isFinite(x)) ? x : null;
  return {
    per: per,
    min: num(r.min) != null ? r.min : 0,
    max: num(r.max) != null ? r.max : 86400,
    step: num(r.step) != null && r.step > 0 ? r.step : 60,
    writable: dpWritable(dev, code),
  };
}

/** Minutes text: integer when whole, else 1 decimal. */
function fmtCdMin(m) {
  const r = Math.round(m);
  return Math.abs(m - r) < 1e-9 ? String(r) : String(Math.round(m * 10) / 10);
}

/** Duration text: whole minutes from 1 min up, raw seconds below (a 12 s
 *  timer read "0.2 phút"); "s" needs no dict key. */
function fmtCdDur(sec) {
  if (sec > 0 && sec < 60) return Math.round(sec) + ' s';
  return fmtCdMin(sec / 60) + ' ' + t('unit_min');
}

/** Timer display: raw value -> unit-converted duration; 0 = off; a
 *  missing value shows "—" (the row stays visible). */
function cdText(dev, code, meta) {
  const raw = dev.status[code];
  if (raw == null || raw === '') return { text: '—', active: false };
  const v = +raw;
  if (!isFinite(v) || v <= 0) return { text: t('socket_countdown_off'), active: false };
  return { text: fmtCdDur(v * meta.per * 60), active: true };
}

/** Preset raw values (0 = off) for the picker options, step-aligned and
 *  clamped to the DP max. */
function cdPresets(meta) {
  const out = [0];
  const stepMax = Math.floor(meta.max / meta.step) * meta.step;
  for (const m of [5, 10, 15, 30, 60, 120, 240, 480]) {
    let raw = Math.round(m / meta.per / meta.step) * meta.step;
    if (raw > stepMax) raw = stepMax;
    if (raw <= 0 || raw < meta.min || out.includes(raw)) continue;
    out.push(raw);
    if (out.length >= 7) break;
  }
  return out;
}

/** Sync a <select> to the current value: valueless -> the "" placeholder
 *  option; an off-list value gets a transient [data-extra] option (fmt
 *  label) so the select always shows the truth. */
function syncSelect(sel, raw, fmt) {
  const target = (raw == null || raw === '') ? '' : String(raw);
  // "known" counts REAL options only — the extra never satisfies itself.
  const known = Array.from(sel.options)
    .some(o => o.value === target && !o.hasAttribute('data-extra'));
  let extra = sel.querySelector('option[data-extra]');
  if (target && !known) {
    if (!extra) {
      extra = document.createElement('option');
      extra.setAttribute('data-extra', '');
      sel.appendChild(extra);
    }
    extra.value = target;
    extra.textContent = fmt(target);
  } else if (extra) {
    extra.remove();
  }
  if (sel.value !== target) sel.value = target;
  sel.classList.toggle('nov', target === '');
}

/** Hidden preset dropdown revealed by tapping a writable timer. */
function cdSetHtml(meta) {
  const opts = cdPresets(meta).map(raw => {
    const label = raw === 0 ? t('socket_countdown_off')
      : fmtCdDur(raw * meta.per * 60);
    return `<option value="${raw}">${esc(label)}</option>`;
  }).join('');
  return `<div class="cd-set" hidden style="margin:6px 2px 0"><select class="ch-select" aria-label="${esc(t('socket_countdown'))}"><option value="" disabled selected>—</option>${opts}</select></div>`;
}

/** Wire one picker dropdown: change sends (raw DP units) + hides the row;
 *  the timer toggles it by click or Enter/Space. Returns the select-sync
 *  fn (WS patches) or null. */
function bindCdSet(row, dev, code, meta) {
  const timer = $('.ch-timer', row);
  const set = row.nextElementSibling;
  if (!timer || !set || !set.classList.contains('cd-set')) return null;
  const sel = $('select', set);
  const fmt = raw => fmtCdDur(+raw * meta.per * 60);
  const sync = () => { syncSelect(sel, dev.status[code], fmt); };
  sync();
  sel.addEventListener('change', () => {
    if (sel.value === '') return;
    sendCommands(dev.id, [{ code: code, value: +sel.value }]);
    set.hidden = true;
  });
  const toggle = () => {
    if (set.hidden) { sync(); set.hidden = false; }
    else { set.hidden = true; }
  };
  timer.addEventListener('click', toggle);
  timer.addEventListener('keydown', ev => {
    if (ev.key !== 'Enter' && ev.key !== ' ') return;
    ev.preventDefault();   // Space must not scroll the page
    toggle();
  });
  return sync;
}

function buildChannelToggles(dev, caps) {
  const chans = caps.channels.map((code, i) => {
    // countdown_N pairs with switch_N; a bare "switch" is the head toggle,
    // so its countdowns render standalone via buildCountdowns.
    const m = code.match(/^switch_(\d)$/);
    const cdCode = (m && caps.countdowns.includes('countdown_' + m[1]))
      ? 'countdown_' + m[1] : null;
    // Label by the switch_N DIGIT, not the index — surviving channels
    // keep their true numbers after one is disabled.
    return { code: code, cdCode: cdCode, n: m ? m[1] : String(i + 1),
      meta: cdCode ? cdMeta(dev, cdCode) : null };
  });
  let rows = '';
  for (const ch of chans) {
    const on = !!dev.status[ch.code];
    const d = ch.meta ? cdText(dev, ch.cdCode, ch.meta) : null;
    const cdHtml = ch.meta ? `
      <span class="ch-timer${d.active ? ' active' : ''}${ch.meta.writable ? ' settable' : ''}"${ch.meta.writable ? ' role="button" tabindex="0"' : ''}>
        ${icon('timer')}
        <span class="cd-t">${esc(d.text)}</span>
      </span>` : '';
    const setHtml = ch.meta && ch.meta.writable ? cdSetHtml(ch.meta) : '';
    rows += `
      <div class="ch-row" data-ch="${esc(ch.code)}">
        <span class="ch-name">${t('socket_channel', { n: ch.n })}</span>
        ${cdHtml}
        ${toggleHtml(on, t('socket_channel', { n: ch.n }))}
      </div>${setHtml}`;
  }
  const node = el(`<div>${rows}</div>`);
  const refs = $$('.ch-row', node).map((row, i) => {
    const ch = chans[i];
    const btn = $('.toggle', row);
    btn.addEventListener('click', () => {
      sendCommands(dev.id, [{ code: ch.code, value: !dev.status[ch.code] }]);
    });
    return {
      ch: ch, btn: btn, timer: $('.ch-timer', row), cdTxt: $('.cd-t', row),
      sync: ch.meta && ch.meta.writable ? bindCdSet(row, dev, ch.cdCode, ch.meta) : null,
    };
  });
  registerUpdater(() => {
    for (const r of refs) {
      const on = !!dev.status[r.ch.code];
      r.btn.classList.toggle('on', on);
      r.btn.setAttribute('aria-checked', String(on));
      if (r.cdTxt && r.ch.meta) {
        const d = cdText(dev, r.ch.cdCode, r.ch.meta);
        r.timer.classList.toggle('active', d.active);
        r.cdTxt.textContent = d.text;
        if (r.sync) r.sync();
      }
    }
  });
  return node;
}

/** Countdown timers whose switch_N channel is absent. */
function buildCountdowns(dev, caps) {
  const paired = new Set();
  caps.channels.forEach(c => {
    const m = c.match(/^switch_(\d)$/);
    if (m) paired.add('countdown_' + m[1]);
  });
  const rows = [];
  for (const cd of caps.countdowns) {
    if (paired.has(cd)) continue;
    const meta = cdMeta(dev, cd);
    const n = (cd.match(/(\d)$/) || [])[1] || '';
    const d = cdText(dev, cd, meta);
    rows.push({ code: cd, meta: meta, html: `
      <div class="ch-row" data-cd="${esc(cd)}">
        <span class="ch-name">${t('socket_countdown')}${n ? ' ' + n : ''}</span>
        <span class="ch-timer${d.active ? ' active' : ''}${meta.writable ? ' settable' : ''}"${meta.writable ? ' role="button" tabindex="0"' : ''}>${icon('timer')} <span class="cd-t">${esc(d.text)}</span></span>
      </div>` });
  }
  if (!rows.length) return null;
  const node = el(`<div>${rows.map(r => r.html + (r.meta.writable ? cdSetHtml(r.meta) : '')).join('')}</div>`);
  const refs = $$('.ch-row', node).map((row, i) => {
    const r = rows[i];
    return {
      code: r.code, meta: r.meta,
      timer: $('.ch-timer', row), txt: $('.cd-t', row),
      sync: r.meta.writable ? bindCdSet(row, dev, r.code, r.meta) : null,
    };
  });
  registerUpdater(() => {
    for (const r of refs) {
      const d = cdText(dev, r.code, r.meta);
      r.timer.classList.toggle('active', d.active);
      r.txt.textContent = d.text;
      if (r.sync) r.sync();
    }
  });
  return node;
}

/* ---- metric readouts: hero value + chips ---- */

/** First metric in priority order that currently has a value. */
function pickHero(dev, caps) {
  for (const m of caps.metrics) {
    if (!m.hero) continue;
    if (metricValue(dev, m) != null) return m;
  }
  return null;
}

/** Right-side hero caption (e.g. "ACTIVE POWER", "CURRENT"). */
function heroLabelKey(caps, m) {
  switch (m.kind) {
    case 'power': return 'meter_power';
    case 'energy': return 'meter_energy';
    case 'voltage': return 'meter_voltage';
    case 'current': return 'meter_current';
    case 'temp': return caps.tempSet ? 'climat_current' : null;
    case 'pm25': return 'pm25_lbl';
    default: return null;
  }
}

/** Chip label: family label + channel suffix (cur_power_1 -> "P1"). */
function metricChipLabel(m) {
  if (!m.label) return null;
  const lbl = t(m.label);
  const d = String(m.code).match(/(\d)$/);
  if (!d) return lbl;
  return lbl + (lbl.length >= 4 ? ' ' : '') + d[1];
}

/** Metric unit: i18n key (refine-layer families) or the literal. */
function metricUnit(m) {
  return m.unitKey ? t(m.unitKey) : (m.unit || '');
}

/** Display value, with the per-kind massage (clean_time sec -> min). */
function metricValue(dev, m) {
  const v = scaleVal(dev.status[m.code], m.scale);
  if (v == null) return null;
  if (m.kind === 'clean_time') return Math.round(v / 60);
  return v;
}

/** Stable structure: the hero block mounts whenever a hero-capable metric
 *  exists; chips stay mounted (valueless -> "—"), the hero's own hides. */
function buildMetrics(dev, caps) {
  const root = el('<div></div>');
  let heroEl = null, bigEl = null, unitEl = null, lblEl = null;
  if (caps.hero) {
    root.innerHTML = `
      <div class="hero-val">
        <span class="big"></span><span class="unit"></span>
        <span class="hero-label"></span>
      </div>`;
    heroEl = $('.hero-val', root);
    bigEl = $('.big', root);
    unitEl = $('.unit', root);
    lblEl = $('.hero-label', root);
  }
  const chips = [];
  let secEl = null;
  if (caps.metrics.length) {
    const sec = el('<div class="hero-secondary"></div>');
    for (const m of caps.metrics) {
      const chip = el(`<span class="hero-chip">${m.icon ? icon(m.icon) : ''}</span>`);
      const txt = document.createTextNode('');
      chip.appendChild(txt);
      const unit = metricUnit(m);
      if (unit) chip.appendChild(el(`<span class="u">${esc(unit)}</span>`));
      sec.appendChild(chip);
      chips.push({ m: m, chip: chip, txt: txt, lbl: metricChipLabel(m) });
    }
    root.appendChild(sec);
    secEl = sec;
  }
  const patch = () => {
    const h = pickHero(dev, caps);
    const v = h ? metricValue(dev, h) : null;
    if (heroEl) {
      // pm25 air-quality band: <35 ok, <75 warn, else err.
      const band = h && h.kind === 'pm25' && v != null ? (v < 35 ? 'ok' : v < 75 ? 'warn' : 'err') : '';
      heroEl.className = 'hero-val' + (v == null ? ' no-val' : '') + (band ? ' ' + band : '');
      bigEl.textContent = v != null ? v : '';
      if (unitEl) unitEl.textContent = h ? metricUnit(h) : '';
      if (lblEl) {
        const lk = h ? heroLabelKey(caps, h) : null;
        lblEl.textContent = lk ? t(lk) : '';
      }
    }
    let anyChip = false;
    for (const c of chips) {
      const cv = metricValue(dev, c.m);
      // Only the hero's own chip hides (folds into the hero readout);
      // a valueless chip stays visible with a "—" placeholder.
      const hid = c.m === h;
      c.chip.classList.toggle('hid', hid);
      c.txt.textContent = (c.lbl ? c.lbl + ' ' : '') + (cv == null ? '—' : cv);
      if (!hid) anyChip = true;
    }
    if (secEl) secEl.classList.toggle('hid', !anyChip);
  };
  patch();
  registerUpdater(patch);
  return root;
}

/* ---- refine-layer builders: labeled toggle rows ---- */

/** Labeled toggle rows (oscillation / swing / child lock / °C-°F). */
function buildToggleRows(dev, rows) {
  let html = '';
  for (const r of rows) {
    const on = !!dev.status[r.code];
    const label = r.key ? t(r.key) : (r.text || r.code);
    html += `<div class="ch-row" data-tg="${esc(r.code)}"><span class="ch-name">${esc(label)}</span>${toggleHtml(on, label)}</div>`;
  }
  const node = el(`<div>${html}</div>`);
  const refs = $$('.ch-row', node).map(row => {
    const code = row.getAttribute('data-tg');
    const btn = $('.toggle', row);
    if (btn) {
      btn.addEventListener('click', () => {
        sendCommands(dev.id, [{ code, value: !dev.status[code] }]);
      });
    }
    return { code: code, btn: btn };
  });
  registerUpdater(() => {
    for (const r of refs) {
      if (!r.btn) continue;
      const on = !!dev.status[r.code];
      r.btn.classList.toggle('on', on);
      r.btn.setAttribute('aria-checked', String(on));
    }
  });
  return node;
}

/** fan_speed_percent / fan_speed numeric slider (status_range or 0..100). */
function buildFanSlider(dev, caps) {
  const code = caps.pctCode;
  const r = numRange(dev, code, { min: 0, max: 100, step: 1 });
  const cur = isFinite(+dev.status[code]) ? +dev.status[code] : r.min;
  const label = t('fan_speed_lbl');
  const fmt = v => Math.round(v) + t('unit_percent');
  const node = el(`<div>
    <div class="slider-row" data-slider="fspeed">
      <div class="slider-head"><span>${label}</span><span class="val">${fmt(cur)}</span></div>
      <div class="slider-wrap">
        <input type="range" min="${r.min}" max="${r.max}" step="${r.step}" value="${cur}" aria-label="${label}">
        <span class="slider-bubble">${fmt(cur)}</span>
      </div>
    </div>
  </div>`);
  bindSlider(node, dev, '[data-slider="fspeed"]', code, v => {
    sendCommands(dev.id, [{ code, value: v }]);
  }, fmt, () => {
    const v = +dev.status[code];
    return isFinite(v) ? v : null;
  });
  return node;
}

/** Momentary action button (filter_reset: sends {code:true}). */
function buildMomentary(dev, code, labelKey) {
  const node = el(`<div>
    <button class="action-btn" data-mom="${esc(code)}">${icon('refresh')} ${esc(t(labelKey))}</button>
  </div>`);
  $('[data-mom]', node).addEventListener('click', () => {
    sendCommands(dev.id, [{ code, value: true }]);
  });
  return node;
}

/* ---- generic floor: every unclaimed DP renders from metadata ---- */

const FLOOR_RANGE_FB = { min: 0, max: 65535, step: 1 };

/** Bitmap: set bits -> labels (comma-joined); null keeps the raw value. */
function bitmapText(v, labels) {
  const n = +v;
  if (!isFinite(n) || !Array.isArray(labels) || !labels.length) return null;
  const parts = [];
  for (let i = 0; i < labels.length && i < 31; i++) {
    if (n & (1 << i)) parts.push(String(labels[i]));
  }
  return parts.length ? parts.join(', ') : null;
}

/** Free-form value text; objects stringify, missing -> "—". */
function fmtGenericValue(v) {
  if (v == null || v === '') return '—';
  if (typeof v === 'object') return JSON.stringify(v);
  return String(v);
}

/** Per-type read-only display formatter. */
function floorValueFmt(g) {
  switch (g.type) {
    case 'Boolean':
      return v => (v == null || v === '') ? '—' : (v ? t('value_on') : t('value_off'));
    case 'Enum':
      return v => (v == null || v === '') ? '—' : modeLabel(v);
    case 'Integer': {
      const suffix = g.unit ? ' ' + g.unit : '';
      return v => {
        const n = scaleVal(v, g.scale);
        return n == null ? '—' : n + suffix;
      };
    }
    case 'Bitmap':
      return v => {
        if (v == null || v === '') return '—';
        const dec = bitmapText(v, g.labels);
        return dec != null ? dec : fmtGenericValue(v);
      };
    default:
      return fmtGenericValue;
  }
}

/** Labeled read-only value row (relay_status-style + the name label). */
function buildValueRow(dev, g, fmt) {
  const row = el(`<div class="ch-row" data-x="${esc(g.code)}"><span class="ch-name">${esc(g.name)}</span><span class="ch-timer wrap">${esc(fmt(dev.status[g.code]))}</span></div>`);
  const val = $('.ch-timer', row);
  registerUpdater(() => { val.textContent = fmt(dev.status[g.code]); });
  return row;
}

/** Labeled enum dropdown row (floor values, fan speed, mode, spray,
 *  suction): one option per range value; Enum DPs take string values.
 *  labels[] (values JSON) is the display text of the same index. */
function buildEnumRow(dev, code, values, label, labels) {
  const opts = values.map((v, i) => {
    const lab = Array.isArray(labels) && labels[i] != null && labels[i] !== ''
      ? String(labels[i]) : null;
    return `<option value="${esc(v)}">${esc(lab || modeLabel(v))}</option>`;
  }).join('');
  const row = el(`<div class="ch-row" data-x="${esc(code)}">
    <span class="ch-name">${esc(label)}</span>
    <select class="ch-select" aria-label="${esc(label)}"><option value="" disabled selected>—</option>${opts}</select>
  </div>`);
  const sel = $('select', row);
  sel.addEventListener('change', () => {
    if (sel.value === '') return;
    sendCommands(dev.id, [{ code: code, value: sel.value }]);
  });
  const sync = () => { syncSelect(sel, dev.status[code], modeLabel); };
  sync();
  registerUpdater(sync);
  return row;
}

/** Writable boolean row: the toggleRows pattern with the metadata label. */
function floorToggleRow(dev, g) {
  const row = el(`<div class="ch-row" data-tg="${esc(g.code)}"><span class="ch-name">${esc(g.name)}</span>${toggleHtml(!!dev.status[g.code], g.name)}</div>`);
  const btn = $('.toggle', row);
  btn.addEventListener('click', () => {
    sendCommands(dev.id, [{ code: g.code, value: !dev.status[g.code] }]);
  });
  registerUpdater(() => {
    const on = !!dev.status[g.code];
    btn.classList.toggle('on', on);
    btn.setAttribute('aria-checked', String(on));
  });
  return row;
}

/** Read-only integers: labeled metric chips; valueless -> "name —". */
function buildFloorChips(dev, list) {
  const sec = el('<div class="hero-secondary"></div>');
  const chips = list.map(g => {
    const chip = el('<span class="hero-chip"></span>');
    const txt = document.createTextNode('');
    chip.appendChild(txt);
    if (g.unit) chip.appendChild(el(`<span class="u">${esc(g.unit)}</span>`));
    sec.appendChild(chip);
    return { g: g, txt: txt };
  });
  const patch = () => {
    for (const c of chips) {
      const v = scaleVal(dev.status[c.g.code], c.g.scale);
      c.txt.textContent = c.g.name + ' ' + (v == null ? '—' : v);
    }
  };
  patch();
  registerUpdater(patch);
  return sec;
}

/** The render floor: every remaining status ∪ status_range code gets a
 *  row (writable control / read-only label) — never invisible. */
function buildGeneric(dev, caps) {
  if (!caps.generic || !caps.generic.length) return null;
  const sec = el('<div></div>');
  const ints = [];
  for (const g of caps.generic) {
    if (g.type === 'Boolean') {
      sec.appendChild(g.writable ? floorToggleRow(dev, g)
        : buildValueRow(dev, g, floorValueFmt(g)));
    } else if (g.type === 'Integer') {
      // No range -> a value row: a stepper without bounds is unusable.
      if (g.writable && g.hasRange) {
        sec.appendChild(buildFloorStepper(dev, g.code, FLOOR_RANGE_FB, g.name, g.unit));
      } else if (g.writable) {
        sec.appendChild(buildValueRow(dev, g, floorValueFmt(g)));
      } else {
        ints.push(g);
      }
    } else if (g.type === 'Enum') {
      if (g.writable && g.range.length) {
        sec.appendChild(buildEnumRow(dev, g.code, g.range, g.name, g.labels));
      } else {
        sec.appendChild(buildValueRow(dev, g, floorValueFmt(g)));
      }
    } else {
      sec.appendChild(buildValueRow(dev, g, floorValueFmt(g)));
    }
  }
  if (ints.length) sec.appendChild(buildFloorChips(dev, ints));
  return sec;
}
