// Onboarding (user code -> QR -> linked) + the post-link wizard (Tuya IoT
// opt-in -> scan step); the Tuya IoT form machinery is in tuya-iot.js.

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, mmss, brandLogo, esc } from '../core/utils.js';
import { emit } from '../core/events.js';
import { mountScanPanel, scanPanelStart } from './scan.js';
import { tuyaIotFieldsHtml, bindTuyaIotForm, saveTuyaIotCredentials } from './tuya-iot.js';

const AUTH_COUNTDOWN_SEC = 180;
const AUTH_POLL_MS = 1000;   // /api/auth/status cadence

/* ---- onboarding (user code -> QR -> linked) ---- */

// Bumped by stopOnboardingTimers so an in-flight poll tick cannot
// reschedule itself after the screen moved on.
let authPollGen = 0;
// Bumped per QR attempt: a retry answer carrying the payload already on
// screen must not re-arm the countdown (the hub rotates the token per
// round, and a changed payload re-arms it via applyAuthStatus).
let linkAttempt = 0;

export function stopOnboardingTimers() {
  const o = state.onb;
  authPollGen += 1;
  if (o.pollTimer) { clearTimeout(o.pollTimer); o.pollTimer = null; }
  if (o.countTimer) { clearInterval(o.countTimer); o.countTimer = null; }
}

/** Repaint the countdown + its ring from state.onb.countdown. */
function paintCountdown() {
  const num = $('#qr-count-num');
  const ring = $('#qr-ring');
  if (num) num.textContent = mmss(state.onb.countdown);
  if (ring) {
    const frac = state.onb.countdown / AUTH_COUNTDOWN_SEC;
    ring.style.strokeDashoffset = String(144.5 * (1 - frac));
  }
}

export function renderOnboarding() {
  const o = state.onb;
  const app = $('#app');
  if (o.step === 2) {
    const hasQr = !!state.auth.qr_payload;
    const failed = state.auth.state === 'failed';
    const qrHtml = hasQr ? `
      <div class="qr-box">
        <div class="qr-fill" id="qr-fill"></div>
        <div class="qr-count">
          <svg viewBox="0 0 52 52">
            <circle cx="26" cy="26" r="23" fill="none" stroke="rgba(255,255,255,.08)" stroke-width="4"/>
            <circle id="qr-ring" cx="26" cy="26" r="23" fill="none" stroke="#22d3ee" stroke-width="4"
              stroke-linecap="round" stroke-dasharray="144.5" stroke-dashoffset="${(144.5 * (1 - o.countdown / AUTH_COUNTDOWN_SEC)).toFixed(1)}"/>
          </svg>
          <span class="num" id="qr-count-num">${mmss(o.countdown)}</span>
        </div>
      </div>` : `
      <div class="qr-wait">
        <div class="spin"></div>
        <span>${t('onb_scan_waiting')}</span>
      </div>`;
    app.innerHTML = `
      <div class="onb-wrap">
        <div class="onb-card">
          <div class="onb-logo">${brandLogo()}</div>
          <div class="onb-title">${t('app_name')}</div>
          <div class="onb-sub">${t('onb_title')}</div>
          <div class="qr-panel">
            ${qrHtml}
            ${failed
              ? `<div class="onb-err show">${esc(state.auth.error || t('onb_error'))}</div>`
              : hasQr
                ? `<div class="qr-status"><span class="dot-pulse"></span>${t('onb_waiting')}</div>`
                : ''}
            <div class="qr-cap">${t('onb_scan_caption')}</div>
            <div class="onb-actions">
              <button class="btn" id="onb-retry">${t('onb_retry')}</button>
              <button class="btn-ghost" id="onb-back">${t('onb_change_code')}</button>
            </div>
          </div>
        </div>
      </div>`;
    if (hasQr) {
      renderQrCode(state.auth.qr_payload);
      if (!state.onb.countTimer) startCountdown();  // ring starts when the QR appears
      paintCountdown();   // a repaint resumes at the LIVE remaining time
    }
    $('#onb-retry').addEventListener('click', () => startLinking(state.onb.code, true));
    $('#onb-back').addEventListener('click', () => { state.onb.step = 1; renderOnboarding(); });
    return;
  }

  // Step 3: the Tuya IoT opt-in (opt / form / error variants).
  if (o.step === 3) {
    renderPostLinkWizard(app);
    return;
  }

  // Step 4: the shared scan/candidates panel (wizard mode).
  if (o.step === 4) {
    mountScanPanel('wizard');
    return;
  }

  // Step 1: user code entry.
  app.innerHTML = `
    <div class="onb-wrap">
      <div class="onb-card">
        <div class="onb-logo">${brandLogo()}</div>
        <div class="onb-title">${t('app_name')}</div>
        <div class="onb-sub">${t('onb_subtitle')}</div>
        <label class="onb-label" for="onb-code">${t('onb_user_code')}</label>
        <input class="onb-input" id="onb-code" type="text" inputmode="numeric"
          placeholder="${t('onb_user_code_hint')}" autocomplete="off" autocapitalize="none"
          spellcheck="false" value="${esc(state.onb.code)}">
        <div class="onb-err" id="onb-err"></div>
        <div class="onb-actions">
          <button class="btn" id="onb-start">${t('onb_start')}</button>
        </div>
      </div>
    </div>`;
  const input = $('#onb-code');
  const start = () => startLinking(input.value.trim());
  $('#onb-start').addEventListener('click', start);
  input.addEventListener('keydown', e => { if (e.key === 'Enter') start(); });
  input.focus();
}

async function startLinking(userCode, isRetry) {
  const err = $('#onb-err');
  if (!userCode) {
    if (err) { err.textContent = t('onb_user_code_hint'); err.classList.add('show'); }
    return;
  }
  state.onb.code = userCode;
  if (err) err.classList.remove('show');
  const retrying = !!(isRetry && state.auth.qr_payload);  // QR still on screen
  // An older answer must not resync the live countdown (double click).
  const attempt = ++linkAttempt;
  let payload = '';
  try {
    const res = await api('/api/auth/start', { method: 'POST', body: { user_code: userCode } });
    if (res && typeof res.qr_payload === 'string') payload = res.qr_payload;
  } catch (e) {
    if (err) { err.textContent = e.message || t('onb_error'); err.classList.add('show'); }
    return;
  }
  state.onb.step = 2;
  state.auth.state = 'waiting_scan';
  // Retry: keep the QR + its running countdown; only a different payload
  // from the hub repaints (still without re-arming).
  if (retrying) {
    if (payload && payload !== state.auth.qr_payload) applyQrPayload(payload, false);
    return;
  }
  state.onb.countdown = AUTH_COUNTDOWN_SEC;
  state.onb.qrPainted = payload;
  state.auth.qr_payload = payload;
  renderOnboarding();
  startAuthPolling();
  syncCountdown(attempt);
}

/** Re-read /api/auth/status once right after a QR round starts: the poll
 *  would otherwise paint the FIRST waiting payload and re-arm the countdown
 *  the user just watched start. The payload already painted is ignored. */
async function syncCountdown(attempt) {
  let data = null;
  try {
    data = await api('/api/auth/status', { silent: true });
  } catch (e) { return; }
  if (attempt !== linkAttempt || state.onb.step !== 2 || state.screen !== 'onboarding') return;
  if (!data) return;
  if (data.qr_payload && data.qr_payload !== state.auth.qr_payload) {
    applyQrPayload(data.qr_payload, false);
  }
  emit('auth:status', data);
}

/** Self-scheduling chain (setTimeout, never setInterval — a slow request
 *  never stacks ticks). Each tick re-checks the screen and stops itself. */
function startAuthPolling() {
  stopOnboardingTimers();
  const o = state.onb;
  const gen = (authPollGen += 1);
  const tick = async () => {
    o.pollTimer = null;
    if (gen !== authPollGen || state.screen !== 'onboarding') return;
    try {
      const data = await api('/api/auth/status', { silent: true });
      if (gen !== authPollGen) return;
      emit('auth:status', data);   // app.js: applyAuthStatus(data)
    } catch (e) { /* keep polling silently */ }
    if (gen !== authPollGen || state.screen !== 'onboarding') return;
    o.pollTimer = setTimeout(tick, AUTH_POLL_MS);
  };
  o.pollTimer = setTimeout(tick, AUTH_POLL_MS);
}

function startCountdown() {
  state.onb.countTimer = setInterval(() => {
    if (!$('#qr-count-num') || !$('#qr-ring')) { stopOnboardingTimers(); return; }
    state.onb.countdown -= 1;
    if (state.onb.countdown <= 0) {
      stopOnboardingTimers();
      state.auth.state = 'failed';
      state.auth.error = t('onb_error_timeout');
      renderOnboarding();
      return;
    }
    paintCountdown();
  }, 1000);
}

/** Restart the countdown window (only a round the USER starts re-arms). */
function resetCountdown() {
  state.onb.countdown = AUTH_COUNTDOWN_SEC;
  if (!state.onb.countTimer) startCountdown();
}

/** QR payload from the poll/refetch. A rotation inside the SAME round only
 *  repaints: the countdown runs to its own deadline (a retry must not
 *  restart it). rearm=true = the user started a fresh round. */
function applyQrPayload(payload, rearm) {
  state.auth.qr_payload = payload;
  state.onb.qrPainted = payload;
  if (rearm) resetCountdown();
  renderOnboarding();   // a repaint keeps the live countdown (paintCountdown)
}

/** A changed payload while step 2 is up: true = repainted (no re-arm). */
export function paintQrRound() {
  if (state.screen !== 'onboarding' || state.onb.step !== 2 ||
      !state.auth.qr_payload ||
      state.auth.qr_payload === state.onb.qrPainted) return false;
  applyQrPayload(state.auth.qr_payload, false);
  return true;
}

/** Render the QR payload with the embedded qrcode-generator lib. */
function renderQrCode(payload) {
  const box = $('#qr-fill');
  if (!box) return;
  try {
    const qr = qrcode(0, 'M');
    qr.addData(payload);
    qr.make();
    box.innerHTML = qr.createSvgTag(4, 8);
    const svg = $('svg', box);
    if (svg) { svg.removeAttribute('width'); svg.removeAttribute('height'); }
  } catch (e) {
    console.error('qr render failed', e);
    box.textContent = payload;
  }
}

/* ---- post-link wizard (state.onb.step 3/4) ---- */

/** Fresh link observed on the onboarding screen -> open the wizard. */
export function startPostLinkWizard() {
  stopOnboardingTimers();
  state.onb.step = 3;
  state.onb.tuyaIot = 'opt';
  renderOnboarding();
}

/** Advance the wizard to the shared scan step. */
function wizardGotoScan() {
  state.onb.step = 4;
  renderOnboarding();     // step 4 mounts the scan panel (wizard mode)
  scanPanelStart();       // decides/starts the scan
}

/** Terminal callback for the wizard's Tuya IoT save: true -> scan step,
 *  false/null -> error card (step guard stops zombie polls). */
function wizardTuyaIotOnDone(ok) {
  if (state.onb.step !== 3) return;  // wizard abandoned (revoke / restart)
  if (ok === true) {
    wizardGotoScan();
    return;
  }
  state.onb.tuyaIot = 'error';
  renderOnboarding();
}

/** Step 3 renderer: the opt-in / form / error cards. */
function renderPostLinkWizard(app) {
  const o = state.onb;
  if (o.tuyaIot === 'form') {
    // The wizard's credential form — same fields/save path as the
    // settings section (shares state.tuyaIot.form).
    app.innerHTML = `
      <div class="onb-wrap tall">
        <div class="onb-card wz-card">
          <div class="onb-title">${t('oa_opt_title')}</div>
          <div class="onb-sub">${t('oa_wiz_form_desc')}</div>
          <form class="oa-form wz-form" id="wz-form" novalidate>
            ${tuyaIotFieldsHtml('wz')}
            <div class="wz-status" id="wz-oa-status">${state.tuyaIot.submitting ? t('openapi_connecting') : ''}</div>
            <button class="btn" id="wz-save"${state.tuyaIot.submitting ? ' disabled' : ''}>${t('openapi_save')}</button>
          </form>
          <div class="onb-actions">
            <button class="btn-ghost" id="wz-back"${state.tuyaIot.submitting ? ' disabled' : ''}>${t('back')}</button>
          </div>
        </div>
      </div>`;
    const card = $('.wz-card', app);
    bindTuyaIotForm(card, 'wz', () => saveTuyaIotCredentials(wizardTuyaIotOnDone));
    $('#wz-back').addEventListener('click', () => {
      state.onb.tuyaIot = 'opt';
      renderOnboarding();
    });
    return;
  }
  if (o.tuyaIot === 'error') {
    app.innerHTML = `
      <div class="onb-wrap">
        <div class="onb-card">
          <div class="onb-logo">${brandLogo()}</div>
          <div class="onb-title">${t('oa_wiz_failed')}</div>
          <div class="onb-sub">${t('oa_wiz_failed_desc')}</div>
          <div class="onb-actions">
            <button class="btn" id="oa-err-retry">${t('retry')}</button>
            <button class="btn-ghost" id="oa-err-skip">${t('oa_opt_skip')}</button>
          </div>
        </div>
      </div>`;
    $('#oa-err-retry').addEventListener('click', () => {
      state.onb.tuyaIot = 'form';
      renderOnboarding();
    });
    $('#oa-err-skip').addEventListener('click', wizardGotoScan);
    return;
  }
  // 'opt': the Tuya IoT opt-in card.
  app.innerHTML = `
    <div class="onb-wrap">
      <div class="onb-card">
        <div class="onb-logo">${brandLogo()}</div>
        <div class="onb-title">${t('oa_opt_title')}</div>
        <div class="onb-sub">${t('oa_opt_desc')}</div>
        <div class="onb-actions">
          <button class="btn" id="oa-opt-configure">${t('oa_opt_configure')}</button>
          <button class="btn-ghost" id="oa-opt-skip">${t('oa_opt_skip')}</button>
        </div>
      </div>
    </div>`;
  $('#oa-opt-configure').addEventListener('click', () => {
    state.onb.tuyaIot = 'form';
    renderOnboarding();
  });
  $('#oa-opt-skip').addEventListener('click', wizardGotoScan);
}
