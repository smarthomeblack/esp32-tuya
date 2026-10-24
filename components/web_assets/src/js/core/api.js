// REST client: JSON in/out, error schema, Bearer key. 401 emits
// 'auth:required' (core never imports features). window.api = QA hook.

import { state } from './state.js';
import { emit } from './events.js';
import { t } from './i18n.js';
import { toast } from '../ui/toast.js';

const FETCH_TIMEOUT_MS = 15000;

/** fetch() capped at 15 s (timeoutMs overrides per call); abort surfaces
 *  as a network error. Uploads (asset/OTA) use XHR and are not routed through here. */
function fetchTimeout(path, init, timeoutMs) {
  const ctrl = new AbortController();
  init.signal = ctrl.signal;
  const timer = setTimeout(() => ctrl.abort(), timeoutMs || FETCH_TIMEOUT_MS);
  return fetch(path, init).finally(() => clearTimeout(timer));
}

/** Fetch wrapper: JSON in/out, error schema -> toast (silent=true
 *  suppresses it). Attaches Bearer <apikey>; 401 -> login gate. */
export async function api(path, opts) {
  opts = opts || {};
  try {
    const init = { method: opts.method || 'GET', cache: 'no-store' };
    init.headers = {};
    if (state.apikey) init.headers['Authorization'] = 'Bearer ' + state.apikey;
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(opts.body);
    }
    const res = await fetchTimeout(path, init, opts.timeoutMs);
    if (res.status === 401) {
      if (!opts.noGate) {
        if (!opts.silent) toast(t('err_auth'), 'err');
        emit('auth:required');   // gate.js: clearApiKey() + showApiKeyGate()
      }
      const err = new Error('unauthorized');
      err.handled = true;
      err.status = 401;
      throw err;
    }
    let data = null;
    try { data = await res.json(); } catch (e) { /* non-JSON body */ }
    if (!res.ok || (data && data.success === false)) {
      // The error code ("discovery_running", ...): callers branch on it.
      // The HTTP status wins for gateway timeouts — firmware bodies are JSON.
      const code = res.status === 408 || res.status === 504 ? 'timeout'
        : (data && data.error ? data.error.code : '');
      const msg = data && data.error ? (data.error.message || code)
        : (res.status + ' ' + res.statusText);
      // Server text stays in the console; the toast is localized.
      if (!opts.silent) {
        console.warn('api', path, res.status, msg);
        toast(t(res.status === 401 ? 'err_auth' : 'err_server'), 'err');
      }
      const err = new Error(msg);
      err.handled = true;
      err.status = res.status;
      err.code = code;
      throw err;
    }
    return data;
  } catch (e) {
    // Tag client-side aborts so callers can branch on code === 'timeout'.
    if (e && e.name === 'AbortError') { e.code = 'timeout'; }
    if (!e.handled) {
      console.error('api', path, e);
      if (!opts.silent) toast(t('err_network'), 'err');
    }
    throw e;
  }
}

window.api = api;
