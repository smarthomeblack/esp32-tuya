// Command queue (pending tracking + the optimistic boolean toggles).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $ } from '../core/utils.js';
import { emit } from '../core/events.js';
import { toast } from '../ui/toast.js';
import { detectCapabilities } from './cards-caps.js';

const PENDING_TIMEOUT_MS = 10000;

const CMD_ERR_MS = 2500;

/** The red flash outlives its node: a Home rebuild re-applies it from state. */
function flashErr(devId) {
  const sel = `.card[data-dev="${CSS.escape(devId)}"]`;
  const card = $(sel);
  if (card) card.classList.add('cmd-err');
  const prev = state.cmdErr.get(devId);
  if (prev) clearTimeout(prev);
  state.cmdErr.set(devId, setTimeout(() => {
    state.cmdErr.delete(devId);
    const node = $(sel);
    if (node) node.classList.remove('cmd-err');
  }, CMD_ERR_MS));
}

/** POST a command batch; pending until a matching device_update (or the
 *  10s timeout). Booleans apply optimistically; failures revert. */
export async function sendCommands(devId, commands) {
  const p = markPending(devId, commands.map(c => c.code));
  applyOptimistic(devId, commands, p);
  try {
    await api(`/api/devices/${encodeURIComponent(devId)}/commands`, {
      method: 'POST', body: { commands },
    });
  } catch (e) {
    clearPending(devId, null, true);
  }
}

function markPending(devId, codes) {
  let p = state.pending.get(devId);
  if (!p) {
    p = { timer: null, codes: new Set(), old: new Map() };
    state.pending.set(devId, p);
  }
  for (const c of codes) p.codes.add(c);
  if (p.timer) clearTimeout(p.timer);
  p.timer = setTimeout(async () => {
    // No confirming device_update in 10s (WS down?): take the server
    // truth; blind revert + error mark only if even the refetch fails.
    state.pending.delete(devId);
    try {
      const list = await api('/api/devices', { silent: true });
      const fresh = (Array.isArray(list) ? list : []).find(x => x && x.id === devId);
      if (!fresh) throw new Error('device missing');
      fresh.status = fresh.status || {};
      fresh._caps = detectCapabilities(fresh);
      state.devices.set(devId, fresh);
      emit('card:refresh', devId);
      const card = $(`.card[data-dev="${CSS.escape(devId)}"]`);
      if (card) card.classList.remove('pending');
    } catch (e) {
      revertOptimistic(devId, p);
      flashErr(devId);
      toast(t('toast_timeout'), 'err');
    }
  }, PENDING_TIMEOUT_MS);
  const card = $(`.card[data-dev="${CSS.escape(devId)}"]`);
  if (card) card.classList.add('pending');
  return p;
}

/** Boolean commands land in state immediately; the pre-command value is
 *  stashed for the revert. First stash per code wins (rapid re-toggle). */
function applyOptimistic(devId, commands, p) {
  const dev = state.devices.get(devId);
  // Offline devices cannot confirm — stay pessimistic (no false flip).
  if (!dev || dev.online === false || !p.old) return;
  let touched = false;
  for (const c of commands) {
    if (typeof c.value !== 'boolean') continue;
    if (typeof dev.status[c.code] !== 'boolean') continue;
    if (!p.old.has(c.code)) p.old.set(c.code, dev.status[c.code]);
    dev.status[c.code] = c.value;
    touched = true;
  }
  if (touched) emit('card:refresh', devId);
}

/** Restore the stashed pre-optimistic values, then repaint (dashboard
 *  renders; cards must not import it — events bus). Select/slider/stepper
 *  commands stash nothing, so the repaint from state.devices (still the
 *  true value) is what undoes the control the user moved. */
function revertOptimistic(devId, p) {
  const dev = state.devices.get(devId);
  if (!dev) return;
  if (p.old) {
    for (const [code, old] of p.old) dev.status[code] = old;
  }
  emit('card:refresh', devId);
}

export function clearPending(devId, codes, failed) {
  const p = state.pending.get(devId);
  if (!p) return;
  if (codes && codes.length) {
    for (const c of codes) {
      p.codes.delete(c);
      if (p.old) p.old.delete(c);   // confirmed — never revert it later
    }
    if (p.codes.size > 0) return; // still waiting for other codes
  }
  if (p.timer) clearTimeout(p.timer);
  state.pending.delete(devId);
  // Failure: restore the old values + repaint BEFORE the cmd-err flash.
  if (failed) revertOptimistic(devId, p);
  const card = $(`.card[data-dev="${CSS.escape(devId)}"]`);
  if (card) card.classList.remove('pending');
  if (failed) flashErr(devId);
}
