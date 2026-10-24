// In-place card update registry (cardUpdaters + the "building" entry).
// All card builders must import registerUpdater from THIS module: builds
// are synchronous, so the current-entry flag lives in one instance.

import { state } from '../core/state.js';
import { deviceKeys, dpWritable } from './cards-caps.js';

/** The "on" state used for card styling (icon glow). */
function isCardOn(dev, caps) {
  if (caps.vacuum && caps.vacuum.powerGo) return !!dev.status[caps.vacuum.powerGo];
  if (caps.channels.length) return caps.channels.some(c => !!dev.status[c]);
  return caps.switch ? !!dev.status[caps.switch] : false;
}

const cardUpdaters = new WeakMap();  // card node -> {dev, updaters}
let building = null;                 // entry of the card being composed

/** Register an update closure onto the card being composed (builds are
 *  synchronous, so a module-level "current" entry is safe). */
export function registerUpdater(fn) {
  if (building) building.updaters.push(fn);
}

export function beginCard(node, dev) {
  const entry = { dev: dev, updaters: [] };
  cardUpdaters.set(node, entry);
  building = entry;
}

export function finishCard(node, dev, caps) {
  building = null;
  node.setAttribute('data-struct', structKey(dev, caps));
}

/** Structural signature (caps + status key set): any change must force a
 *  full rebuild because the composed sections would differ. */
function structKey(dev, caps) {
  return [
    dev.category || '', caps.icon, caps.labelKey,
    caps.switch || '', caps.channels.join(','),
    caps.countdowns.map(c => c + ':' + (dpWritable(dev, c) ? 1 : 0)).join(','),
    caps.brightCode || '', caps.colourCode || '', caps.cctCode || '',
    caps.workMode || '', caps.modeCode || '', caps.modeValues.join(','),
    caps.tempSet || '',
    caps.cover ? [caps.cover.pos, caps.cover.ctrl, caps.cover.display,
      caps.cover.values.join(',')].join(',') : '',
    caps.speedCode || '', caps.speedValues.join(','), caps.speedFirst ? 1 : 0,
    caps.pctCode || '', caps.stepper ? caps.stepper.code : '',
    caps.sprayCode || '', caps.sprayValues.join(','),
    caps.workStatus || '', caps.toggleRows.map(r => r.code).join(','),
    caps.filterReset || '',
    caps.vacuum ? [caps.vacuum.status, caps.vacuum.powerGo, caps.vacuum.mode,
      caps.vacuum.seek, caps.vacuum.suction, caps.vacuum.fault].join(',') : '',
    caps.coverExtras.map(e => e.pos).join(','),
    caps.metrics.map(m => m.code + ':' + m.kind + ':' + m.scale).join(','),
    (caps.generic || []).map(g => g.code + ':' + g.type + ':' + (g.writable ? 1 : 0)).join(','),
    Array.from(deviceKeys(dev)).sort().join(','),
  ].join('|');
}

/** Patch the existing card node in place. False -> the caller rebuilds
 *  (stale node, replaced dev object, or a caps/structure change). */
export function updateCardInPlace(cardEl) {
  const entry = cardUpdaters.get(cardEl);
  if (!entry) return false;
  const dev = state.devices.get(cardEl.getAttribute('data-dev'));
  if (!dev || !dev._caps || dev !== entry.dev) return false;
  if (cardEl.getAttribute('data-struct') !== structKey(dev, dev._caps)) return false;
  syncCardState(cardEl, dev, dev._caps);
  // A throwing updater must not starve the other rows of this card.
  for (const fn of entry.updaters) {
    try { fn(); } catch (e) { console.error('card updater failed', entry.dev.id, e); }
  }
  return true;
}

export function syncCardState(node, dev, caps) {
  node.classList.toggle('offline', dev.online === false);
  node.classList.toggle('on', isCardOn(dev, caps));
  node.classList.toggle('pending', state.pending.has(dev.id));
  // ctrl visibility follows the head switch (multi-channel cards have none).
  node.classList.toggle('ctrl-off', !!(caps.switch && !caps.vacuum && !dev.status[caps.switch]));
}
