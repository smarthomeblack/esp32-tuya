// Settings: the OTA section — /api/ota/info rows, .bin upload via the
// shared ui/uploader.js, reboot-after-upload confirm + rollback.

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, esc, icon, fmtKb } from '../core/utils.js';
import { toast } from '../ui/toast.js';
import { uploadFile, setMeter, toastUploadError } from '../ui/uploader.js';
import { confirmDialog } from '../ui/dialogs.js';

/** The OTA section: slot rows + upload + rollback (the server answers
 *  500 ota_unavailable when no valid slot remains). */
export function otaSectionHtml() {
  const info = state.ota.info;
  return `
    <div class="section-title">${t('ota_title')}</div>
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('cpu')}</span>
        <div class="body"><div class="title">${t('ota_running')}</div></div>
        <span class="end" id="ota-running">${info && info.running ? esc(info.running) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('cpu')}</span>
        <div class="body"><div class="title">${t('ota_next')}</div></div>
        <span class="end" id="ota-next">${info && info.next ? esc(info.next) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('meter')}</span>
        <div class="body"><div class="title">${t('ota_slot_size')}</div></div>
        <span class="end" id="ota-slot">${info && info.slot_size ? fmtKb(info.slot_size) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('generic')}</span>
        <div class="body"><div class="title">${t('ota_version')}</div></div>
        <span class="end" id="ota-version">${info && info.version ? esc(info.version) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('timer')}</span>
        <div class="body"><div class="title">${t('ota_build_time')}</div></div>
        <span class="end" id="ota-time">${info && info.build_time ? esc(info.build_time) : '—'}</span>
      </div>
      <div class="row">
        <div class="body">
          <div class="meter"><i id="ota-bar"></i></div>
        </div>
      </div>
      <div class="row">
        <button class="btn" id="ota-upload">${t('ota_upload')}</button>
      </div>
      <div class="row">
        <button class="btn-danger" id="ota-rollback">${t('ota_rollback')}</button>
      </div>
    </div>
    <input type="file" id="ota-file" accept=".bin" hidden>`;
}

/** Patch the section rows from state.ota.info (fetched after mount). */
function patchOtaRows() {
  const info = state.ota.info;   // rows live on the settings panel only
  const set = (id, text) => {
    const node = $(id);
    if (node) node.textContent = text;
  };
  set('#ota-running', info && info.running ? info.running : '—');
  set('#ota-next', info && info.next ? info.next : '—');
  set('#ota-slot', info && info.slot_size ? fmtKb(info.slot_size) : '—');
  set('#ota-version', info && info.version ? info.version : '—');
  set('#ota-time', info && info.build_time ? info.build_time : '—');
}

/** Silent GET /api/ota/info -> state.ota.info, then patch the rows. */
async function refreshOtaInfo() {
  try {
    const data = await api('/api/ota/info', { silent: true });
    if (data && data.ota) {
      state.ota.info = data.ota;
      patchOtaRows();
    }
  } catch (e) { /* keep the last snapshot */ }
}

async function pickAndUpload() {
  if (state.ota.uploading) return;
  const input = $('#ota-file');
  if (!input || !input.files || !input.files[0]) {
    toast(t('ota_pick'), 'err');
    return;
  }
  const ok = await confirmDialog({
    title: t('ota_confirm_title'),
    msg: t('ota_confirm_msg'),
    ok: t('dlg_confirm'), danger: true,
  });
  if (!ok) return;
  const file = input.files[0];
  state.ota.uploading = true;
  const btn = $('#ota-upload');
  if (btn) btn.disabled = true;
  const setProgress = pct => setMeter('#ota-bar', pct);
  setProgress(0);
  try {
    await uploadFile('/api/ota/upload', file, setProgress);
    setProgress(100);
    toast(t('ota_done'), 'ok');
    const reboot = await confirmDialog({
      title: t('ota_reboot_ask'),
      msg: '',
      ok: t('set_reboot'), danger: false,
    });
    if (reboot) {
      try { await api('/api/system/reboot', { method: 'POST', body: {} }); }
      catch (e) { /* toast shown */ }
      toast(t('toast_rebooting'), 'ok');
    }
  } catch (e) {
    setProgress(null);
    toastUploadError(e, 'ota_failed');
  } finally {
    state.ota.uploading = false;
    if (btn) btn.disabled = false;
    input.value = '';
  }
}

/** Rollback: mark the running slot invalid -> reboot (confirm first). */
async function rollbackOta() {
  const ok = await confirmDialog({
    title: t('ota_rollback_title'),
    msg: t('ota_rollback_msg'),
    ok: t('dlg_confirm'), danger: true,
  });
  if (!ok) return;
  try {
    await api('/api/ota/rollback', { method: 'POST', body: {} });
    toast(t('toast_rebooting'), 'ok');
  } catch (e) { /* toast shown */ }
}

/** Section binding: upload + rollback buttons + the info fetch. */
export function bindOtaSection(root) {
  const btn = $('#ota-upload', root);
  const input = $('#ota-file', root);
  if (btn && input) {
    btn.addEventListener('click', () => input.click());
    input.addEventListener('change', pickAndUpload);
  }
  const rollback = $('#ota-rollback', root);
  if (rollback) rollback.addEventListener('click', rollbackOta);
  refreshOtaInfo();
}
