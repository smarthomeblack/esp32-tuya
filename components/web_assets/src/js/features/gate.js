// API-key login gate. Subscribes 'auth:required' (api() 401s) and emits
// 'auth:ok' after a successful submit (app.js re-runs boot()). While the
// initial (never rotated) key is active, it first swaps into a
// change-key step.

import { state } from '../core/state.js';
import { $, brandLogo } from '../core/utils.js';
import { t } from '../core/i18n.js';
import { api } from '../core/api.js';
import { on, emit } from '../core/events.js';
import { stopTuyaIotPoll } from './tuya-iot.js';

function clearApiKey() {
  state.apikey = null;
  try { localStorage.removeItem('hub_api_key'); } catch (e) { /* private mode */ }
}

function showGateError(msg) {
  const err = $('#gate-err');
  if (err && msg) { err.textContent = msg; err.classList.add('show'); }
}

/** Full-screen login gate. Idempotent (a mounted gate is never wiped).
 *  rotate=true mounts the change-key step directly (cached initial key).
 *  Contract: #app is kept (its CONTENT is replaced by the gate); the app
 *  screen is NOT preserved — a successful re-login re-runs the full boot,
 *  which re-renders the shell and refetches the data with the new key. */
let rotateMode = false;
export function showApiKeyGate({ rotate = false } = {}) {
  const app = $('#app');
  if (!app || $('#gate-card')) return;
  rotateMode = false;
  state.screen = 'boot';
  stopTuyaIotPoll();
  app.innerHTML = `
    <div class="onb-wrap">
      <form class="onb-card gate-card" id="gate-card" novalidate>
        <div class="onb-logo">${brandLogo()}</div>
        <div class="onb-title">${t('login_title')}</div>
        <div class="onb-sub">${t('login_desc')}</div>
        <label class="onb-label" for="gate-key">${t('login_key')}</label>
        <input class="onb-input" id="gate-key" type="password"
          autocomplete="current-password" autocapitalize="none" spellcheck="false">
        <div class="onb-err" id="gate-err"></div>
        <div class="onb-actions">
          <button class="btn" id="gate-submit" type="submit">${t('login_btn')}</button>
        </div>
      </form>
    </div>`;
  $('#gate-card').addEventListener('submit', e => {
    e.preventDefault();
    if (rotateMode) rotateSubmit(); else gateSubmit();
  });
  $('#gate-key').focus();
  if (rotate) renderKeyRotation();
}

/** Gate submit: probe /api/status; success persists + re-runs boot, an
 *  un-rotated initial key goes to the rotation step instead. */
let gateBusy = false;
async function gateSubmit() {
  const input = $('#gate-key');
  const err = $('#gate-err');
  if (!input || gateBusy) return;
  const key = input.value.trim();
  if (err) err.classList.remove('show');
  if (!key) return;
  gateBusy = true;
  state.apikey = key;
  try {
    await api('/api/status', { silent: true, noGate: true });
  } catch (e) {
    state.apikey = null;
    gateBusy = false;
    if (err) { err.textContent = t('login_failed'); err.classList.add('show'); }
    return;
  }
  try {
    const info = await api('/api/settings/apikey', { silent: true, noGate: true });
    if (info && info.initial) {
      gateBusy = false;
      renderKeyRotation();
      return;
    }
  } catch (e) { /* probe failed — fall through to the normal exit */ }
  try { localStorage.setItem('hub_api_key', key); } catch (e) { /* private mode */ }
  gateBusy = false;
  emit('auth:ok');
}

/** Same rule as the firmware store: 8-64 printable chars, no edge
 *  spaces/tabs (a stored key must round-trip both the Bearer + WS paths). */
function validNewKey(k) {
  return k.length >= 8 && k.length <= 64 && /^[\x20-\x7e]+$/.test(k) &&
    !/^[ \t]/.test(k) && !/[ \t]$/.test(k);
}

/** Forced first-use rotation: swap the gate card into a change-key step
 *  (state.apikey holds the validated initial key as the POST Bearer). */
function renderKeyRotation() {
  const card = $('#gate-card');
  if (!card) return;
  rotateMode = true;
  card.innerHTML = `
    <div class="onb-logo">${brandLogo()}</div>
    <div class="onb-title">${t('gate_rotate_title')}</div>
    <div class="onb-sub">${t('gate_rotate_desc')}</div>
    <label class="onb-label" for="gate-new">${t('sec_apikey_new')}</label>
    <input class="onb-input" id="gate-new" type="password" autocomplete="new-password"
      autocapitalize="none" spellcheck="false">
    <label class="onb-label" for="gate-confirm">${t('sec_apikey_confirm')}</label>
    <input class="onb-input" id="gate-confirm" type="password" autocomplete="new-password"
      autocapitalize="none" spellcheck="false">
    <div class="onb-sub">${t('gate_rotate_hint')}</div>
    <div class="onb-err" id="gate-err"></div>
    <div class="onb-actions">
      <button class="btn" id="gate-rotate-submit" type="submit">${t('sec_apikey_save')}</button>
    </div>`;
  $('#gate-new').focus();
}

/** Rotation submit: validate locally, POST {new}, then persist the new
 *  key and continue with the normal gate exit (auth:ok). */
let rotateBusy = false;
async function rotateSubmit() {
  const n = $('#gate-new');
  const c = $('#gate-confirm');
  if (!n || !c || rotateBusy) return;
  const key = n.value;
  const err = $('#gate-err');
  if (err) err.classList.remove('show');
  if (!validNewKey(key)) {
    showGateError(t('sec_apikey_short'));
    return;
  }
  if (key !== c.value) {
    showGateError(t('gate_rotate_mismatch'));
    return;
  }
  rotateBusy = true;
  try {
    await api('/api/settings/apikey', {
      method: 'POST', body: { new: key }, silent: true, noGate: true,
    });
  } catch (e) {
    rotateBusy = false;
    showGateError(e && e.code === 'invalid_param' ? t('sec_apikey_short') : t('login_failed'));
    return;
  }
  state.apikey = key;
  try { localStorage.setItem('hub_api_key', key); } catch (e) { /* private mode */ }
  rotateBusy = false;
  const card = $('#gate-card');
  if (card) {
    card.innerHTML = `
      <div class="onb-logo">${brandLogo()}</div>
      <div class="onb-title">${t('sec_apikey_changed')}</div>`;
  }
  setTimeout(() => emit('auth:ok'), 700);
}

// api() 401s mount the gate.
on('auth:required', () => {
  clearApiKey();
  showApiKeyGate();
});
