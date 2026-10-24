// Settings: the API-key section (initial-key badge + change-key form).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, esc, icon } from '../core/utils.js';
import { toast } from '../ui/toast.js';

function secForm() {
  if (!state.security.form) {
    state.security.form = { new: '', confirm: '' };
  }
  return state.security.form;
}

/** The "initial key" badge text (empty = rotated / not fetched). */
function secBadgeText() {
  return state.security.initial ? t('sec_initial_badge') : '';
}

/** Security section: key row (badge) + change form. Values live in
 *  state.security.form so a re-render never loses typing. */
export function securitySectionHtml() {
  const f = secForm();
  return `
    <div class="section-title">${t('sec_title')}</div>
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('lock')}</span>
        <div class="body"><div class="title">${t('sec_apikey')}</div></div>
        <span class="end${state.security.initial ? ' warn' : ''}" id="sec-badge">${secBadgeText()}</span>
      </div>
    </div>
    <form class="oa-form sec-form" id="sec-form" novalidate>
      <div class="oa-field">
        <label for="sec-new">${t('sec_apikey_new')}</label>
        <div class="oa-pw">
          <input class="oa-input" id="sec-new" type="password" autocomplete="new-password"
            value="${esc(f.new)}">
          <button type="button" class="oa-eye" data-eye="sec-new" aria-label="${t('openapi_show')}">${icon('eye')}</button>
        </div>
      </div>
      <div class="oa-field">
        <label for="sec-confirm">${t('sec_apikey_confirm')}</label>
        <div class="oa-pw">
          <input class="oa-input" id="sec-confirm" type="password" autocomplete="new-password"
            value="${esc(f.confirm)}">
          <button type="button" class="oa-eye" data-eye="sec-confirm" aria-label="${t('openapi_show')}">${icon('eye')}</button>
        </div>
      </div>
      <button class="btn" id="sec-save">${t('sec_apikey_save')}</button>
    </form>`;
}

/** In-place badge patch (the form is never torn down by WS re-renders). */
function patchSecuritySection() {
  const badge = $('#sec-badge');
  if (!badge) return;
  badge.textContent = secBadgeText();
  badge.className = 'end' + (state.security.initial ? ' warn' : '');
}

/** Silent GET /api/settings/apikey -> state + in-place badge patch. */
export async function refreshSecurityStatus() {
  try {
    const data = await api('/api/settings/apikey', { silent: true });
    if (data) state.security.initial = !!data.initial;
  } catch (e) { /* keep the last snapshot */ }
  patchSecuritySection();
}

/** Change-key submit: validate locally, POST, then keep the local key in
 *  sync (localStorage + state) so the next WS reconnect uses it. */
async function saveApiKey() {
  const f = secForm();
  if (f.new.length < 8) {
    toast(t('sec_apikey_short'), 'err');
    return;
  }
  if (f.new !== f.confirm) {
    toast(t('sec_apikey_mismatch'), 'err');
    return;
  }
  try {
    await api('/api/settings/apikey', { method: 'POST', body: { new: f.new } });
  } catch (e) {
    return;  // toast shown; keep the still-valid old key
  }
  state.apikey = f.new;
  try { localStorage.setItem('hub_api_key', f.new); } catch (e) { /* private mode */ }
  toast(t('sec_apikey_changed'), 'ok');
  f.new = '';
  f.confirm = '';
  const n = $('#sec-new'), c = $('#sec-confirm');
  if (n) n.value = '';
  if (c) c.value = '';
  refreshSecurityStatus();
}

/** Delegated binding (survives the in-place badge patch). */
export function bindSecuritySection(panel) {
  const root = $('#sec-form', panel);
  if (!root) return;
  root.addEventListener('submit', e => { e.preventDefault(); saveApiKey(); });
  root.addEventListener('click', e => {
    const btn = e.target.closest('button');
    if (!btn || !btn.classList.contains('oa-eye')) return;
    const input = $('#' + btn.getAttribute('data-eye'), root);
    if (!input) return;
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    btn.innerHTML = icon(show ? 'eyeOff' : 'eye');
  });
  const onInput = e => {
    const id = e.target && e.target.id;
    const map = { 'sec-new': 'new', 'sec-confirm': 'confirm' };
    if (id && map[id]) secForm()[map[id]] = e.target.value;
  };
  root.addEventListener('input', onInput);
  root.addEventListener('change', onInput);
}
