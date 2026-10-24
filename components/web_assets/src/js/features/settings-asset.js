// Settings: the asset section — /api/asset/info rows + .bin upload
// (shared ui/uploader.js XHR plumbing).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, esc, icon, fmtKb } from '../core/utils.js';
import { toast } from '../ui/toast.js';
import { uploadFile, setMeter, toastUploadError } from '../ui/uploader.js';
import { confirmDialog } from '../ui/dialogs.js';

/** The asset section: build/size/files rows (badge when !valid) + the
 *  hidden file input + upload button + progress meter. */
export function assetSectionHtml() {
  const info = state.asset.info;
  const valid = info && info.valid;
  const badgeCls = info == null ? '' : (valid ? 'ok' : 'err');
  const badgeTxt = info == null ? '' :
    valid ? t('ast_valid') :
    (info.partition_size ? t('ast_invalid') : t('ast_no_partition'));
  return `
    <div class="section-title">${t('ast_title')}</div>
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('gear')}</span>
        <div class="body"><div class="title">${t('ast_build')}</div></div>
        <span class="end" id="ast-build">${info && info.build_id ? esc(info.build_id) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('meter')}</span>
        <div class="body"><div class="title">${t('ast_size')}</div></div>
        <span class="end" id="ast-size">${info && info.payload_len ? fmtKb(info.payload_len) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('generic')}</span>
        <div class="body"><div class="title">${t('ast_files')}</div></div>
        <span class="end" id="ast-files">${info && info.files != null ? info.files : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('check')}</span>
        <div class="body"><div class="title">${t('ast_valid')}</div></div>
        <span class="end ${badgeCls}" id="ast-valid-badge">${badgeTxt}</span>
      </div>
      <div class="row">
        <div class="body">
          <div class="meter ast-meter"><i id="ast-bar"></i></div>
        </div>
      </div>
      <div class="row">
        <button class="btn" id="ast-upload">${t('ast_upload')}</button>
      </div>
    </div>
    <input type="file" id="ast-file" accept=".bin" hidden>`;
}

/** Patch the section rows from state.asset.info (fetched after mount). */
function patchAssetRows() {
  const info = state.asset.info;   // rows live on the settings panel only
  const set = (id, text) => {
    const node = $(id);
    if (node) node.textContent = text;
  };
  set('#ast-build', info && info.build_id ? info.build_id : '—');
  set('#ast-size', info && info.payload_len ? fmtKb(info.payload_len) : '—');
  set('#ast-files', info && info.files != null ? info.files : '—');
  const badge = $('#ast-valid-badge');
  if (badge && info != null) {
    const valid = !!info.valid;
    badge.textContent = valid ? t('ast_valid') :
      (info.partition_size ? t('ast_invalid') : t('ast_no_partition'));
    badge.className = 'end ' + (valid ? 'ok' : 'err');
  }
}

/** Silent GET /api/asset/info -> state.asset.info, then patch the rows. */
async function refreshAssetInfo() {
  try {
    const data = await api('/api/asset/info', { silent: true });
    if (data && data.asset) {
      state.asset.info = data.asset;
      patchAssetRows();
    }
  } catch (e) { /* keep the last snapshot */ }
}

async function pickAndUpload() {
  if (state.asset.uploading) return;
  const input = $('#ast-file');
  if (!input || !input.files || !input.files[0]) {
    toast(t('ast_pick'), 'err');
    return;
  }
  const ok = await confirmDialog({
    title: t('ast_confirm_title'),
    msg: t('ast_confirm_msg'),
    ok: t('dlg_confirm'), danger: false,
  });
  if (!ok) return;
  const file = input.files[0];
  state.asset.uploading = true;
  const btn = $('#ast-upload');
  if (btn) btn.disabled = true;
  const setProgress = pct => setMeter('#ast-bar', pct);
  setProgress(0);
  try {
    await uploadFile('/api/asset/upload', file, setProgress);
    setProgress(100);
    toast(t('ast_done'), 'ok');
    // New UI is already live server-side — reload into it.
    setTimeout(() => location.replace('/'), 2000);
  } catch (e) {
    setProgress(null);
    toastUploadError(e, 'ast_failed');
  } finally {
    state.asset.uploading = false;
    if (btn) btn.disabled = false;
    input.value = '';
  }
}

/** Section binding: the upload button opens the hidden file input. */
export function bindAssetSection(root) {
  const btn = $('#ast-upload', root);
  const input = $('#ast-file', root);
  if (btn && input) {
    btn.addEventListener('click', () => input.click());
    input.addEventListener('change', pickAndUpload);
  }
  refreshAssetInfo();
}
