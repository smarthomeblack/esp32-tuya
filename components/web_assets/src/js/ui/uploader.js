// Shared .bin uploader for the settings asset/OTA sections: XHR with
// upload progress (fetch has no progress events), meter paint + toasts.

import { state } from '../core/state.js';
import { $ } from '../core/utils.js';
import { t } from '../core/i18n.js';
import { emit } from '../core/events.js';
import { toast } from './toast.js';

/** POSTs one file with Bearer auth; onProgress(pct 0..100) fires while
 *  uploading. Rejects an Error with .code/.status set. */
export function uploadFile(url, file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    if (state.apikey) xhr.setRequestHeader('Authorization', 'Bearer ' + state.apikey);
    xhr.responseType = 'json';
    xhr.upload.onprogress = e => {
      if (e.lengthComputable && onProgress) onProgress(Math.round(e.loaded / e.total * 100));
    };
    xhr.onload = () => {
      if (xhr.status === 200 && xhr.response && xhr.response.success) {
        resolve(xhr.response);
      } else {
        const code = xhr.response && xhr.response.error ? xhr.response.error.code
          : String(xhr.status);
        const err = new Error(code);
        err.code = code;
        err.status = xhr.status;
        if (xhr.status === 401) {
          // Same handling as api(): mount the login gate (gate.js).
          err.gate = true;
          emit('auth:required');
        }
        reject(err);
      }
    };
    xhr.onerror = () => reject(new Error('network'));
    xhr.onabort = () => reject(new Error('abort'));
    xhr.send(file);
  });
}

/** Meter bar paint (pct 0..100, null = reset). */
export function setMeter(barId, pct) {
  const bar = $(barId);
  if (!bar) return;
  if (pct == null) {
    bar.style.width = '0';
    bar.className = '';
    return;
  }
  bar.style.width = pct + '%';
  bar.className = pct < 100 ? 'ok' : '';
}

/** Toasts a failed upload: invalid_asset/invalid_param map to the
 *  section's `invalidKey` ("invalid file"); other errors show raw. A 401
 *  already mounted the gate, so it stays silent. */
export function toastUploadError(e, invalidKey) {
  if (e && e.gate) return;
  const msg = (e.code === 'invalid_asset' || e.code === 'invalid_param')
    ? t(invalidKey) : (e.code || e.message);
  toast(msg, 'err');
}
