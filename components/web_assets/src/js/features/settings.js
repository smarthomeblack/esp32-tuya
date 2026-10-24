// Settings tab assembly: language / account / network / system rows +
// reboot / factory actions + the dynamic-value patch (other sections
// come from their own modules).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t, setLang } from '../core/i18n.js';
import { $, $$, esc, icon, rssiBars, fmtUptime, fmtKb, clamp } from '../core/utils.js';
import { emit } from '../core/events.js';
import { toast } from '../ui/toast.js';
import { confirmDialog } from '../ui/dialogs.js';
import { securitySectionHtml, bindSecuritySection, refreshSecurityStatus } from './settings-security.js';
import { logsSectionHtml, bindLogsSection } from './settings-logs.js';
import { assetSectionHtml, bindAssetSection } from './settings-asset.js';
import { otaSectionHtml, bindOtaSection } from './settings-ota.js';
import { tuyaIotSectionHtml, bindTuyaIotSection } from './tuya-iot.js';

// appbar.js mounts the account box in #main next to the Settings panel.

/** The account section (settings tab). */
export function accountSectionHtml() {
  const auth = state.auth || {};
  return `<div class="section"><div class="section-title">${t('set_account')}</div>
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('user')}</span>
        <div class="body">
          <div class="title">${t('set_account')}</div>
          <div class="desc" id="set-auth-user">${esc(auth.username || '—')}</div>
        </div>
        <span class="end ${auth.state === 'linked' ? 'ok' : 'warn'}" id="set-auth-state">${t('auth_state_' + (auth.state || 'idle'))}</span>
      </div>
      <div class="row">
        <button class="btn-danger" id="btn-unlink" style="width:100%;height:44px">${t('set_unlink')}</button>
      </div>
    </div></div>`;
}

/** Patch the account values in place (a data load/status tick), so the
 *  header row never needs a rebuild. */
export function patchAuthSection() {
  const auth = state.auth || {};
  const u = document.getElementById('set-auth-user');
  if (u) u.textContent = auth.username || '—';
  const s = document.getElementById('set-auth-state');
  if (s) s.textContent = t('auth_state_' + (auth.state || 'idle'));
}

export function tabSettings() {
  const st = state.status || {};
  const wifi = st.wifi || {};
  const heap = st.heap || {};
  const counts = t('counts_fmt', {
    devices: st.device_count != null ? st.device_count : state.devices.size,
    homes: st.home_count != null ? st.home_count : state.homes.length,
    scenes: st.scene_count != null ? st.scene_count : state.scenes.length,
  });
  const langRow = `
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('generic')}</span>
        <div class="body"><div class="title">${t('set_language')}</div></div>
        <div class="seg seg-sm" id="seg-lang">
          <button data-lang="vi" title="Tiếng Việt" aria-pressed="${state.lang === 'vi' ? 'true' : 'false'}" class="${state.lang === 'vi' ? 'active' : ''}">${t('set_language_vi')}</button>
          <button data-lang="en" title="English" aria-pressed="${state.lang === 'en' ? 'true' : 'false'}" class="${state.lang === 'en' ? 'active' : ''}">${t('set_language_en')}</button>
        </div>
      </div>
    </div>`;

  const netRow = `
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('wifi')}</span>
        <div class="body"><div class="title">${t('set_network_ssid')}</div></div>
        <span class="end">${esc(wifi.ssid || '—')}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('cpu')}</span>
        <div class="body"><div class="title">${t('set_network_ip')}</div></div>
        <span class="end">${esc(wifi.ip || '—')}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('wifi')}</span>
        <div class="body"><div class="title">${t('set_network_rssi')}</div></div>
        <span class="end" id="set-rssi">${isFinite(wifi.rssi) ? rssiBars(wifi.rssi) : '—'}</span>
      </div>
    </div>`;

  // RAM row tooltip = the internal heap watermark.
  const ramTip = t('set_ram_watermark') + ': ' +
    (isFinite(heap.internal_min_free) ? fmtKb(heap.internal_min_free) : '—');
  // PSRAM row only on boards that actually report a total.
  const psramRow = heap.psram_total > 0 ? `
      <div class="row">
        <span class="ico">${icon('meter')}</span>
        <div class="body"><div class="title">${t('set_psram')}</div><div class="meter"><i id="set-psram-bar" class="ok"></i></div></div>
        <span class="end" id="set-psram">—</span>
      </div>` : '';
  const sysRow = `
    <div class="rows">
      <div class="row">
        <span class="ico">${icon('cpu')}</span>
        <div class="body"><div class="title">${t('set_version')}</div></div>
        <span class="end">${esc(st.version || '—')}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('timer')}</span>
        <div class="body"><div class="title">${t('set_uptime')}</div></div>
        <span class="end" id="set-uptime">${st.uptime_ms != null ? fmtUptime(st.uptime_ms) : '—'}</span>
      </div>
      <div class="row">
        <span class="ico">${icon('cpu')}</span>
        <div class="body"><div class="title">${t('set_cpu')}</div><div class="meter"><i id="set-cpu-bar" class="ok"></i></div></div>
        <span class="end" id="set-cpu">—</span>
      </div>
      <div class="row">
        <span class="ico">${icon('meter')}</span>
        <div class="body" title="${esc(ramTip)}"><div class="title">${t('set_ram')}</div><div class="meter"><i id="set-ram-bar" class="ok"></i></div></div>
        <span class="end" id="set-ram">—</span>
      </div>
      ${psramRow}
      <div class="row">
        <span class="ico">${icon('generic')}</span>
        <div class="body"><div class="title">${t('set_counts')}</div></div>
        <span class="end" id="set-counts">${counts}</span>
      </div>
    </div>
    <div class="mt"><button class="btn-ghost" id="btn-reboot">${t('set_reboot')}</button></div>`;

  const dangerRow = `
    <div class="danger-zone"><button class="btn-danger" id="btn-factory">${t('set_factory')}</button></div>`;

  return `<div class="tab-panel">
    <h2 style="margin:4px 4px 14px">${t('tab_settings')}</h2>
    <div class="section"><div class="section-title">${t('set_language')}</div>${langRow}</div>
    <div class="section">${securitySectionHtml()}</div>
    <div class="section">${tuyaIotSectionHtml()}</div>
    <div class="section"><div class="section-title">${t('set_network')}</div>${netRow}</div>
    <div class="section"><div class="section-title">${t('set_system')}</div>${sysRow}</div>
    <div class="section">${logsSectionHtml()}</div>
    <div class="section">${assetSectionHtml()}</div>
    <div class="section">${otaSectionHtml()}</div>
    <div class="section"><div class="section-title" style="color:var(--err)">${t('set_factory')}</div>${dangerRow}</div>
  </div>`;
}

/** Bind the Settings panel's own nodes (root = the panel section; the
 *  section modules take the same root for their lookups). */
export function bindSettings(root) {
  $$('#seg-lang button', root).forEach(btn => {
    btn.addEventListener('click', () => {
      const lang = btn.getAttribute('data-lang');
      if (lang === state.lang) return;
      // aria-pressed follows the full re-render on 'lang:changed'.
      setLang(lang, true);
    });
  });

  bindLogsSection(root);      // level filter + logs poll + cached paint
  bindTuyaIotSection(root);   // Tuya IoT status/form
  bindSecuritySection(root);  // change-key form (delegated on #sec-form)
  refreshSecurityStatus();    // initial-key badge fetch on tab mount
  bindAssetSection(root);     // asset info + .bin upload
  bindOtaSection(root);       // OTA info + upload + rollback
}

let actionsBound = false;

/** reboot / factory / unlink — bound ONCE on the document (delegated), so a
 *  panel rebuild can never double-fire them (the account section rebuilds
 *  on a language switch while the Settings panel stays mounted). */
export function bindSettingsActions() {
  if (actionsBound) return;
  actionsBound = true;
  document.addEventListener('click', async e => {
    const btn = e.target && e.target.closest ? e.target.closest('#btn-unlink,#btn-reboot,#btn-factory') : null;
    if (!btn) return;
    if (btn.id === 'btn-unlink') await unlinkAuth();
    else if (btn.id === 'btn-reboot') await rebootHub();
    else await factoryReset();
  });
}

async function unlinkAuth() {
  const ok = await confirmDialog({
    title: t('set_unlink_confirm_title'),
    msg: t('set_unlink_confirm_msg'),
    ok: t('set_unlink'), danger: true,
  });
  if (!ok) return;
  try {
    await api('/api/auth/unlink', { method: 'POST', body: {} });
    toast(t('toast_unlinked'), 'ok');
  } catch (e) { /* toast shown */ }
  state.auth = { state: 'idle', qr_payload: '' };
  emit('app:show-onboarding');   // app.js: showOnboarding()
}

async function rebootHub() {
  const ok = await confirmDialog({
    title: t('set_reboot_confirm_title'),
    msg: t('set_reboot_confirm_msg'),
    ok: t('set_reboot'), danger: false,
  });
  if (!ok) return;
  try {
    await api('/api/system/reboot', { method: 'POST', body: {} });
    toast(t('toast_rebooting'), 'ok');
  } catch (e) { /* toast shown */ }
}

async function factoryReset() {
  const ok1 = await confirmDialog({
    title: t('set_factory_confirm_title'),
    msg: t('set_factory_confirm_msg'),
    ok: t('dlg_confirm'), danger: true,
  });
  if (!ok1) return;
  const ok2 = await confirmDialog({
    title: t('set_factory_confirm2_title'),
    msg: t('set_factory_confirm2_msg'),
    ok: t('set_factory'), danger: true,
  });
  if (!ok2) return;
  try {
    // 60 s: the rare store-failure fallback erases the whole 7.2 MB partition.
    await api('/api/system/factory-reset', { method: 'POST', body: {}, timeoutMs: 60000 });
    toast(t('toast_resetting'), 'ok');
  } catch (e) { /* toast shown */ }
}

/** In-place patch of the settings tab's dynamic values; every id is
 *  optional (an outdated DOM simply skips a value). */
export function patchSettingsDynamic() {
  const st = state.status || {};
  const wifi = st.wifi || {};
  const heap = st.heap || {};
  const cpu = st.cpu || {};
  const auth = state.auth || {};
  const setText = (id, text) => {
    const node = $(id);
    if (node) node.textContent = text;
  };
  setText('#set-uptime', st.uptime_ms != null ? fmtUptime(st.uptime_ms) : '—');
  setText('#set-counts', t('counts_fmt', {
    devices: st.device_count != null ? st.device_count : state.devices.size,
    homes: st.home_count != null ? st.home_count : state.homes.length,
    scenes: st.scene_count != null ? st.scene_count : state.scenes.length,
  }));
  // The account row may have been re-rendered by the shell: patch by id.
  setText('#set-auth-user', auth.username || '—');
  const authState = $('#set-auth-state');
  if (authState) {
    authState.textContent = t('auth_state_' + (auth.state || 'idle'));
    authState.className = 'end ' + (auth.state === 'linked' ? 'ok' : 'warn');
  }
  // rssiBars() returns HTML (4 bar <i> elements), not plain text.
  const rssi = $('#set-rssi');
  if (rssi) rssi.innerHTML = isFinite(wifi.rssi) ? rssiBars(wifi.rssi) : '—';
  // CPU / RAM / PSRAM meters — text plus a width% bar.
  const setBar = (id, pct, cls) => {
    const bar = $(id);
    if (!bar) return;
    bar.style.width = pct + '%';
    bar.className = cls;
  };
  if (isFinite(cpu.usage)) {
    const v = clamp(Math.round(cpu.usage), 0, 100);
    setText('#set-cpu', v + '%');
    setBar('#set-cpu-bar', v, v < 60 ? 'ok' : v < 85 ? 'warn' : 'err');
  } else {
    setText('#set-cpu', '—');
    setBar('#set-cpu-bar', 0, 'ok');
  }
  const patchMem = (id, used, total) => {
    if (!isFinite(total) || total <= 0) {
      setText(id, '—');
      setBar(id + '-bar', 0, 'ok');
      return;
    }
    const u = clamp(isFinite(used) ? used : 0, 0, total);
    setText(id, fmtKb(u) + ' / ' + fmtKb(total));
    setBar(id + '-bar', Math.round(u / total * 100), 'ok');
  };
  patchMem('#set-ram', heap.internal_total - heap.internal_free, heap.internal_total);
  if ($('#set-psram')) {
    patchMem('#set-psram', heap.psram_total - heap.psram_free, heap.psram_total);
  }
}
