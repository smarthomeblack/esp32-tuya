// The Tuya IoT section (settings tab + the post-link wizard form): status
// poll, save/clear, and the shared credential-form binding.

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, $$, esc, icon } from '../core/utils.js';
import { toast } from '../ui/toast.js';
import { confirmDialog } from '../ui/dialogs.js';

// Tuya IoT datacenter zones (tuya_cloud openapi_constants.h), full URLs
// — the value POSTed as `datacenter`.
const TIOT_DATACENTERS = [
  { value: 'https://openapi.tuyaus.com', key: 'openapi_dc_us' },
  { value: 'https://openapi.tuyaeu.com', key: 'openapi_dc_eu' },
  { value: 'https://openapi.tuyacn.com', key: 'openapi_dc_cn' },
  { value: 'https://openapi.tuyain.com', key: 'openapi_dc_in' },
  { value: 'https://openapi-sg.iotbing.com', key: 'openapi_dc_sg' },
];

const TIOT_POLL_MS = 1000;   // poll cadence while connecting
// The server reports a definitive failure well before 90 s.
const TIOT_TIMEOUT_MS = 90000;

/** Form defaults: Vietnam country code, tuyaSmart app, US datacenter. */
function tuyaIotForm() {
  if (!state.tuyaIot.form) {
    state.tuyaIot.form = {
      access_id: '', access_secret: '', username: '', password: '',
      country_code: '84', schema: 'tuyaSmart', datacenter: TIOT_DATACENTERS[0].value,
    };
  }
  return state.tuyaIot.form;
}

/** Should the form be shown (derived from state). */
function tuyaIotShowForm() {
  const d = state.tuyaIot.data;
  const connected = !!(d && d.configured && d.has_token);
  return !connected || state.tuyaIot.editing;
}

/** The colored state dot + label. */
function tuyaIotStateHtml() {
  const d = state.tuyaIot.data;
  const connected = !!(d && d.configured && d.has_token);
  const configured = !!(d && d.configured);
  // A background login is in flight — show "Connecting…" so the boot
  // transition reads yellow -> connecting -> green.
  const connecting = !state.tuyaIot.submitting && !!(d && d.login_in_flight);
  let text, cls, dot;
  if (state.tuyaIot.submitting || connecting) {
    text = t('openapi_connecting'); cls = 'warn'; dot = 'warn';
  } else if (connected) {
    text = t('openapi_status_connected'); cls = 'ok'; dot = 'ok';
  } else if (configured) {
    text = t('openapi_status_configured'); cls = 'warn'; dot = 'warn';
  } else {
    text = t('openapi_not_configured'); cls = ''; dot = 'off';
  }
  return `<span class="end oa-state ${cls}"><i class="oa-dot ${dot}"></i>${esc(text)}</span>`;
}

/** The status + quota + action rows (patchable inner island). */
function tuyaIotStatusHtml() {
  const d = state.tuyaIot.data || {};
  const q = d.quota || {};
  const connected = !!(d.configured && d.has_token);
  const desc = [];
  if (connected && d.uid) desc.push('UID: ' + d.uid);
  if (d.endpoint) desc.push(d.endpoint);
  // "2026-10" -> "month 10"; an empty month drops the parenthetical.
  let quotaTxt;
  if (q.month) {
    const m = String(q.month).split('-')[1] || '';
    quotaTxt = t('openapi_quota_fmt', {
      devices: q.devices != null ? q.devices : 0,
      limit: q.limit != null ? q.limit : 10,
      month: m,
    });
  } else {
    quotaTxt = t('openapi_quota_fmt_nomonth', {
      devices: q.devices != null ? q.devices : 0,
      limit: q.limit != null ? q.limit : 10,
    });
  }
  const warn = !!q.warning;
  return `
    <div class="row">
      <span class="ico">${icon('cpu')}</span>
      <div class="body">
        <div class="title">${t('openapi_title')}</div>
        <div class="desc">${esc(desc.join(' · ') || '—')}</div>
      </div>
      ${tuyaIotStateHtml()}
    </div>
    <div class="row">
      <span class="ico">${icon('meter')}</span>
      <div class="body"><div class="title">${t('openapi_quota')}</div></div>
      <span class="end${warn ? ' warn' : ''}">${esc(quotaTxt)}</span>
    </div>
    ${d.configured ? `
    <div class="row oa-actions">
      <button class="btn-ghost" id="oa-edit">${t('openapi_edit')}</button>
      <button class="btn-danger" id="oa-clear">${t('openapi_clear')}</button>
    </div>` : ''}`;
}

/** The credential FIELD SET — shared by the settings section ('oa') and
 *  the wizard ('wz'); both write into state.tuyaIot.form. */
export function tuyaIotFieldsHtml(p) {
  const f = tuyaIotForm();
  const eyeBtn = id => `
    <button type="button" class="oa-eye" data-eye="${id}" aria-label="${t('openapi_show')}">${icon('eye')}</button>`;
  return `
    <div class="oa-field">
      <label for="${p}-access-id">${t('openapi_form_access_id')}</label>
      <input class="oa-input" id="${p}-access-id" type="text" autocomplete="off"
        autocapitalize="none" spellcheck="false" value="${esc(f.access_id)}">
    </div>
    <div class="oa-field">
      <label for="${p}-secret">${t('openapi_form_access_secret')}</label>
      <div class="oa-pw">
        <input class="oa-input" id="${p}-secret" type="password" autocomplete="off"
          value="${esc(f.access_secret)}">${eyeBtn(p + '-secret')}
      </div>
    </div>
    <div class="oa-field">
      <label for="${p}-username">${t('openapi_form_username')}</label>
      <input class="oa-input" id="${p}-username" type="text" autocomplete="off"
        autocapitalize="none" spellcheck="false" value="${esc(f.username)}">
    </div>
    <div class="oa-field">
      <label for="${p}-password">${t('openapi_form_password')}</label>
      <div class="oa-pw">
        <input class="oa-input" id="${p}-password" type="password" autocomplete="off"
          value="${esc(f.password)}">${eyeBtn(p + '-password')}
      </div>
    </div>
    <div class="oa-field">
      <label for="${p}-country">${t('openapi_form_country')}</label>
      <input class="oa-input" id="${p}-country" type="text" inputmode="numeric"
        autocomplete="off" placeholder="84" value="${esc(f.country_code)}">
    </div>
    <div class="oa-field">
      <label>${t('openapi_form_schema')}</label>
      <div class="seg" id="${p}-schema">
        <button type="button" data-v="tuyaSmart" aria-pressed="${f.schema === 'tuyaSmart' ? 'true' : 'false'}" class="${f.schema === 'tuyaSmart' ? 'active' : ''}">${t('openapi_schema_tuyasmart')}</button>
        <button type="button" data-v="smartlife" aria-pressed="${f.schema === 'smartlife' ? 'true' : 'false'}" class="${f.schema === 'smartlife' ? 'active' : ''}">${t('openapi_schema_smartlife')}</button>
      </div>
    </div>
    <div class="oa-field">
      <label for="${p}-dc">${t('openapi_form_datacenter')}</label>
      <select class="oa-input oa-select" id="${p}-dc">
        ${TIOT_DATACENTERS.map(dc =>
          `<option value="${dc.value}"${f.datacenter === dc.value ? ' selected' : ''}>${t(dc.key)}</option>`).join('')}
      </select>
    </div>`;
}

/** The settings-section form (a real <form> so Enter submits and Chrome's
 *  password-manager heuristics stay quiet). */
function tuyaIotFormHtml() {
  return `
    <form class="oa-form" id="oa-form" novalidate>
    ${tuyaIotFieldsHtml('oa')}
    <button class="btn" id="oa-save"${state.tuyaIot.submitting ? ' disabled' : ''}>${t('openapi_save')}</button>
    </form>`;
}

/** Whole section: header (collapse toggle) + patchable body. */
export function tuyaIotSectionHtml() {
  return `
    <button class="section-title oa-title" id="oa-toggle" aria-expanded="${state.tuyaIot.open}">
      <span>${t('openapi_title')}</span>
      <span class="oa-chev">${icon('chev')}</span>
    </button>
    <div class="rows oa-rows${state.tuyaIot.open ? '' : ' hide'}" id="oa-rows">
      <div id="oa-status">${tuyaIotStatusHtml()}</div>
      <div class="oa-form-wrap${tuyaIotShowForm() ? ' show' : ''}" id="oa-form-wrap">${tuyaIotFormHtml()}</div>
    </div>`;
}

/** Patch the status/quota/action rows + form/save visibility in place
 *  (also the wizard's form buttons, id-based no-ops when hidden). */
function patchTuyaIotSection() {
  const status = $('#oa-status');
  if (status) status.innerHTML = tuyaIotStatusHtml();
  const wrap = $('#oa-form-wrap');
  if (wrap) wrap.classList.toggle('show', tuyaIotShowForm());
  const save = $('#oa-save');
  if (save) save.disabled = state.tuyaIot.submitting;
  const wzSave = $('#wz-save');
  if (wzSave) wzSave.disabled = state.tuyaIot.submitting;
  const wzBack = $('#wz-back');
  if (wzBack) wzBack.disabled = state.tuyaIot.submitting;
  const wzStatus = $('#wz-oa-status');
  if (wzStatus) wzStatus.textContent = state.tuyaIot.submitting ? t('openapi_connecting') : '';
}

/** Silent /api/openapi/status fetch -> state + in-place patch. */
export async function refreshTuyaIotStatus() {
  try {
    const data = await api('/api/openapi/status', { silent: true });
    if (data) state.tuyaIot.data = data;
  } catch (e) { /* keep the last snapshot */ }
  patchTuyaIotSection();
}

export function stopTuyaIotPoll() {
  if (state.tuyaIot.pollTimer) {
    clearInterval(state.tuyaIot.pollTimer);
    state.tuyaIot.pollTimer = null;
  }
}

/** 1 s self-scheduling poll (setTimeout chain — no stacking) until
 *  has_token, failure, or the deadline; `onDone(done)` gets the result. */
function startTuyaIotPoll(onDone) {
  stopTuyaIotPoll();
  const deadline = Date.now() + TIOT_TIMEOUT_MS;
  const tick = async () => {
    let done = false;
    let failed = false;
    try {
      const data = await api('/api/openapi/status', { silent: true });
      if (data) state.tuyaIot.data = data;
      done = !!(data && data.has_token);
      // login_in_flight gone + no token = definitive failure (only trust
      // it after the server has had a moment to spawn the task).
      if (data && !data.login_in_flight && !data.has_token &&
          Date.now() > deadline - TIOT_TIMEOUT_MS + 2000) {
        failed = true;
      }
    } catch (e) { /* hub unreachable — keep polling */ }
    if (done || failed || Date.now() >= deadline) {
      stopTuyaIotPoll();
      state.tuyaIot.submitting = false;
      state.tuyaIot.editing = false;
      patchTuyaIotSection();
      toast(done ? t('openapi_connected') : t('openapi_connect_failed'), done ? 'ok' : 'err');
      if (onDone) onDone(done);
    } else {
      patchTuyaIotSection();
      state.tuyaIot.pollTimer = setTimeout(tick, TIOT_POLL_MS);
    }
  };
  state.tuyaIot.pollTimer = setTimeout(tick, TIOT_POLL_MS);
}

/** Save (POST credentials) then poll for the async login result;
 *  `onDone(null)` = POST failed (no callback = plain settings behavior). */
export async function saveTuyaIotCredentials(onDone) {
  // An Enter-key submit can re-fire while a save is in flight.
  if (state.tuyaIot.submitting) return;
  const f = tuyaIotForm();
  if (!f.access_id || !f.access_secret || !f.username || !f.password) {
    toast(t('openapi_form_incomplete'), 'err');
    return;
  }
  state.tuyaIot.submitting = true;
  patchTuyaIotSection();
  try {
    await api('/api/openapi/credentials', {
      method: 'POST',
      body: {
        access_id: f.access_id, access_secret: f.access_secret,
        username: f.username, password: f.password,
        country_code: f.country_code, schema: f.schema, datacenter: f.datacenter,
      },
    });
  } catch (e) {
    state.tuyaIot.submitting = false;
    patchTuyaIotSection();
    if (onDone) onDone(null);   // the toast already carried the message
    return;
  }
  startTuyaIotPoll(onDone);
}

/** Disconnect (POST clear) -> back to the not-configured state. */
async function clearTuyaIot() {
  const ok = await confirmDialog({
    title: t('openapi_confirm_clear_title'),
    msg: t('openapi_confirm_clear_msg'),
    ok: t('openapi_clear'), danger: true,
  });
  if (!ok) return;
  stopTuyaIotPoll();
  state.tuyaIot.submitting = false;
  try {
    await api('/api/openapi/clear', { method: 'POST', body: {} });
  } catch (e) { /* toast shown */ }
  // Local snapshot of the cleared state (quota tracking survives — it
  // lives in the "hub" NVS namespace).
  state.tuyaIot.data = {
    configured: false, has_token: false,
    quota: (state.tuyaIot.data && state.tuyaIot.data.quota) || {},
  };
  state.tuyaIot.editing = false;
  patchTuyaIotSection();
  refreshTuyaIotStatus();
}

/** Shared credential-form binding: submit + eye toggles + schema segment
 *  + live-value capture, parameterized by the id prefix ('oa'/'wz'). */
export function bindTuyaIotForm(root, p, onSubmit) {
  root.addEventListener('submit', e => {
    if (e.target && e.target.id === p + '-form') {
      e.preventDefault();
      onSubmit();
    }
  });
  root.addEventListener('click', e => {
    const btn = e.target.closest('button');
    if (!btn) return;
    if (btn.classList.contains('oa-eye')) {
      const input = $('#' + btn.getAttribute('data-eye'));
      if (input) {
        const show = input.type === 'password';
        input.type = show ? 'text' : 'password';
        btn.innerHTML = icon(show ? 'eyeOff' : 'eye');
      }
      return;
    }
    if (btn.parentElement && btn.parentElement.id === p + '-schema') {
      const f = tuyaIotForm();
      f.schema = btn.getAttribute('data-v') || 'tuyaSmart';
      $$('#' + p + '-schema button').forEach(b => {
        b.classList.toggle('active', b === btn);
        b.setAttribute('aria-pressed', String(b === btn));
      });
      return;
    }
  });
  // Typing/selecting: keep the live values in state so a re-render
  // never loses them.
  const onInput = e => {
    const id = e.target && e.target.id;
    const map = {
      [p + '-access-id']: 'access_id', [p + '-secret']: 'access_secret',
      [p + '-username']: 'username', [p + '-password']: 'password',
      [p + '-country']: 'country_code', [p + '-dc']: 'datacenter',
    };
    if (id && map[id]) tuyaIotForm()[map[id]] = e.target.value;
  };
  root.addEventListener('input', onInput);
  root.addEventListener('change', onInput);
}

/** One-shot binding for the section (event delegation survives patches). */
export function bindTuyaIotSection(root) {
  const toggle = $('#oa-toggle', root);
  if (toggle) {
    toggle.addEventListener('click', () => {
      state.tuyaIot.open = !state.tuyaIot.open;
      toggle.setAttribute('aria-expanded', String(state.tuyaIot.open));
      const rows = $('#oa-rows', root);
      if (rows) rows.classList.toggle('hide', !state.tuyaIot.open);
    });
  }
  const body = $('#oa-rows', root);
  if (!body) return;
  // Submit (Save click or Enter): the single save path.
  bindTuyaIotForm(body, 'oa', () => saveTuyaIotCredentials());
  // Settings-only actions: edit + clear — delegated so the patchable
  // #oa-status island keeps its handlers.
  body.addEventListener('click', e => {
    const btn = e.target.closest('button');
    if (!btn) return;
    if (btn.id === 'oa-clear') { clearTuyaIot(); return; }
    if (btn.id === 'oa-edit') {
      state.tuyaIot.editing = true;
      patchTuyaIotSection();
      return;
    }
  });
  refreshTuyaIotStatus();
}
