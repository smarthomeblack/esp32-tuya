// Entry module: boot sequence, the status/auth dispatchers, the screen+tab
// composition, and the event subscriptions.

import { state } from './core/state.js';
import { on, off, emit } from './core/events.js';
import { t, loadDict } from './core/i18n.js';
import { api } from './core/api.js';
import { wsConnect, onWsEvent as addWsHandler, setWsHandlers } from './core/ws.js';
import { $, $$, esc, icon, brandLogo } from './core/utils.js';
import {
  updateStatusDots, updateProgress, setConnBanner, bindConnBanner, homeLabelHtml,
  renderTab, syncTab, dataLoaded, resetTabPanels, openHomeMenu, closeHomeMenu,
} from './ui/appbar.js';
import { renderOnboarding, stopOnboardingTimers, startPostLinkWizard, paintQrRound } from './features/onboarding.js';
import { refreshTuyaIotStatus } from './features/tuya-iot.js';
import { scanSyncFromStatus, openScanPanel, closeScanPanel, renderScanPanel } from './features/scan.js';
import { onDeviceUpdate, onDeviceOnline } from './features/dashboard.js';
import { detectCapabilities } from './features/cards-caps.js';
import { bindSettingsActions } from './features/settings.js';
import { syncLogsPoll, stopLogsPoll } from './features/settings-logs.js';
import { showApiKeyGate } from './features/gate.js';
import './features/gate.js';          // mounts the 401 login gate ('auth:required')
import './features/entity-editor.js'; // mounts the card-gear editor ('entity:edit')

/* ---- screens + tabs ---- */

function renderScreen() {
  if (state.screen === 'onboarding') renderOnboarding();
  else if (state.screen === 'app') renderApp();
  else if (state.screen === 'wait') renderHubWaiting();
}

/** The app shell: appbar, conn banner, progress, tab area, bottom nav. */
function renderApp() {
  const app = $('#app');
  const homeLabel = homeLabelHtml();
  resetTabPanels();   // the shell owns fresh mounted panels
  app.innerHTML = `
    <div class="screen">
      <header class="appbar">
        <div class="home-pill" id="home-pill">
          <button class="home-btn" id="home-btn" aria-haspopup="listbox">
            <span class="lbl">${esc(homeLabel)}</span>
            <span class="chev">${icon('chev')}</span>
          </button>
        </div>
        <button class="icon-btn" id="btn-add" aria-label="${t('scan_title')}">${icon('plus')}</button>
        <div class="status-dots">
          <span class="sdot" id="dot-cloud" role="img" aria-label="${t('cloud')}"></span>
          <span class="sdot" id="dot-mqtt" role="img" aria-label="${t('mqtt')}"></span>
        </div>
        <button class="icon-btn" id="btn-refresh" style="display:none" aria-label="${t('refresh')}">${icon('refresh')}</button>
        <button class="icon-btn" id="btn-gear" aria-label="${t('tab_settings')}">${icon('gear')}</button>
      </header>
      <div class="conn-banner" id="conn-banner"><span class="pulse"></span><span>${t('conn_lost')}</span></div>
      <div class="progress-track" id="progress-track"><div class="bar"></div></div>
      <div class="progress-lbl" id="progress-lbl" role="status" aria-live="polite"></div>
      <main id="main"></main>
      <nav class="bottom-nav">
        <div class="inner">
          <button data-tab="home" class="${state.tab === 'home' ? 'active' : ''}">
            ${icon('home')}<span>${t('tab_home')}</span>
          </button>
          <button data-tab="scenes" class="${state.tab === 'scenes' ? 'active' : ''}">
            ${icon('play')}<span>${t('tab_scenes')}</span>
          </button>
          <button data-tab="settings" class="${state.tab === 'settings' ? 'active' : ''}">
            ${icon('gear')}<span>${t('tab_settings')}</span>
          </button>
        </div>
      </nav>
    </div>`;
  bindShell();
  renderTab();
}

/** Nav / gear: a re-click on the active tab is a no-op (renderTab keeps
 *  the mounted panel and only re-applies its in-place patches). */
function switchTab(tab) {
  state.tab = tab;
  $$('.bottom-nav button').forEach(b => b.classList.toggle('active', b.getAttribute('data-tab') === tab));
  syncTab();
  renderTab();
}

function bindShell() {
  // The shell is rebuilt on every language switch: off() then re-register the
  // page-level routes HERE, or they die on the first rebuild.
  off('ws:conn');
  off('auth:status');
  off('status:apply');
  off('data:refresh');
  bindShellRoutes();
  setWsHandlers(onWsEvent);
  $$('.bottom-nav button').forEach(btn => {
    btn.addEventListener('click', () => switchTab(btn.getAttribute('data-tab')));
  });
  $('#btn-gear').addEventListener('click', () => switchTab('settings'));
  $('#btn-refresh').addEventListener('click', async () => {
    await refreshData();
  });
  // Home tab "Add devices" — opens the scan/candidates overlay.
  $('#btn-add').addEventListener('click', () => openScanPanel('app'));

  const pill = $('#home-pill');
  const btn = $('#home-btn');
  btn.addEventListener('click', e => {
    e.stopPropagation();
    if ($('.home-menu', pill)) { closeHomeMenu(pill); return; }
    openHomeMenu(pill);
  });
  updateStatusDots();
  updateProgress();
  setConnBanner(!state.ws.connected);
  bindSettingsActions();   // reboot / factory / unlink (delegated, bound once)
}

/** Transition onboarding -> app after linking. force=true re-enters from a
 *  screen left at 'app' (the gate's re-login) and re-renders the shell. */
async function enterApp(force) {
  if ((state.screen === 'app' && !force) || state.enteringApp) return;
  state.enteringApp = true;
  stopOnboardingTimers();
  state.screen = 'app';
  state.tab = 'home';
  state.homeFilter = 'all';
  renderApp();
  await refreshData();
  state.enteringApp = false;
}

/** The onboarding screen (user code step 1); called on link loss. */
function showOnboarding() {
  stopOnboardingTimers();
  closeScanPanel();   // an open "Add devices" overlay must not linger
  stopLogsPoll();     // the logs poll too
  state.screen = 'onboarding';
  state.enteringApp = false;
  state.onb.step = 1;
  state.devices.clear();
  state.homes = [];
  state.rooms = null;
  state.roomExpanded.clear();
  state.scenes = [];
  renderOnboarding();
}

/* ---- hub wait screen (unreachable / still booting) ---- */

/** The waiting splash (logo + spinner + status line). */
function renderHubWaiting() {
  // Fall back to literals only when the dict itself could not be fetched.
  const haveDict = !!(state.dicts && state.dicts[state.lang]);
  const msg = haveDict ? t('hub_wait')
    : (state.lang === 'en' ? 'Waiting for the hub to start…' : 'Đang chờ hub khởi động…');
  $('#app').innerHTML = `
    <div class="splash" id="wait-splash">
      <div class="splash-logo" aria-hidden="true">${brandLogo()}</div>
      <h1 class="splash-title">Tuya Hub</h1>
      <div class="splash-spin" aria-hidden="true"></div>
      <div class="splash-sub">${msg}</div>
    </div>`;
}

const HUB_WAIT_POLL_MS = 3000;
let hubWaitTimer = null;

/** Self-scheduling poll — a slow in-flight probe never stacks ticks. */
function startHubWaitPoll() {
  if (hubWaitTimer) return;
  const tick = async () => {
    if (state.screen !== 'wait' || $('#gate-card')) { hubWaitTimer = null; return; }
    try { await boot(); } catch (e) { /* next tick retries */ }
    hubWaitTimer = (state.screen === 'wait' && !$('#gate-card'))
      ? setTimeout(tick, HUB_WAIT_POLL_MS) : null;
  };
  hubWaitTimer = setTimeout(tick, HUB_WAIT_POLL_MS);
}

/** Enter the wait state: the hub cannot YET tell setup vs linked. The
 *  3 s poll re-runs boot() until the hub answers, then routes. */
function showHubWaiting() {
  if ($('#gate-card') || state.screen === 'wait') return;
  state.screen = 'wait';
  renderHubWaiting();
  startHubWaitPoll();
}

/* ---- status / discovery / auth dispatch ---- */

/** Tuya IoT status probe floor: a WS status burst must not refetch it per
 *  event (the section also fetches on tab mount). */
const TUYA_IOT_REFRESH_MS = 10000;
let tuyaIotRefreshedAt = 0;

function applyStatus(data) {
  state.status = Object.assign({}, state.status, data);
  state.auth = Object.assign({}, state.auth, data.auth || {});
  updateStatusDots();
  updateProgress();
  scanSyncFromStatus();   // the scan panel tracks discovery live
  syncScreensWithAuth();
  if (state.screen === 'app') {
    // Mounted panels: only a structural change rebuilds; dynamic values
    // patch in place (appbar.syncTab).
    syncTab();
    if (state.tab === 'settings') {
      syncLogsPoll();         // the logs poll follows the tab
      if (Date.now() - tuyaIotRefreshedAt >= TUYA_IOT_REFRESH_MS) {
        tuyaIotRefreshedAt = Date.now();
        refreshTuyaIotStatus();
      }
    }
  }
}

/** WS discovery_progress event. */
function applyDiscovery(data) {
  if (state.status) state.status.discovery = data;
  updateProgress();
  scanSyncFromStatus();   // the open scan panel follows the run
}

/** Screen transitions driven by the auth state. */
function syncScreensWithAuth() {
  const linked = state.auth.state === 'linked';
  if (linked && state.screen === 'onboarding') {
    // A FRESH link starts the post-link wizard (step>=3 keeps a running
    // wizard intact).
    if (state.onb.step < 3) startPostLinkWizard();
  }
  // Only needs_setup pulls the app back to the wizard — a rebooting hub
  // answers with starting/idle while auth is still idle.
  if (!linked && state.screen === 'app' &&
      state.status && state.status.state === 'needs_setup') {
    showOnboarding();
  }
}

/** WS lost while past boot (the hub restarted / key rotated): 401s already
 *  mount the gate; otherwise re-run the post-auth boot (it routes the
 *  linked hub back to the last painted app screen and an unlinked one to
 *  the wizard/wait screen) and keep retrying until it answers. */
function onWsLost() {
  if (state.screen === 'app') {
    startOfflineWatch();
  } else if (state.screen === 'boot') {
    if (!$('#gate-card')) bootAfterAuth();
  } else {
    bootAfterAuth();   // awaits showOfflineScreen(); its rejection is moot
  }
}

/** /api/auth/status + WS auth_status both land here. */
function applyAuthStatus(data) {
  state.auth = Object.assign({}, state.auth, data);
  if (state.screen === 'onboarding') {
    if (state.auth.state === 'failed') {
      stopOnboardingTimers();
      renderOnboarding();
      return;
    }
    // A CHANGED payload repaints the QR and re-arms the countdown — the
    // hub rotates the QR token each round (onboarding.js owns that).
    if (paintQrRound()) return;
    if (state.auth.state === 'linked') {
      // A FRESH link opens the post-link wizard (step>=3 guard).
      if (state.onb.step < 3) startPostLinkWizard();
      return;
    }
    if (state.onb.step >= 3 && state.auth.state) {
      // The link was revoked mid-wizard — restart onboarding.
      showOnboarding();
    }
  } else if (state.screen === 'app' && state.auth.state && state.auth.state !== 'linked') {
    // Same needs_setup guard as syncScreensWithAuth — a rebooting hub
    // reports auth idle before the link is restored.
    if (state.status && state.status.state === 'needs_setup') showOnboarding();
  }
}

/** Data-generation bump: in-flight refreshData runs must not repaint the
 *  app while a newer screen (gate/wait) owns #app. */
function abandonDataRefresh() {
  state.dataGen += 1;
  state.loadingData = false;
}

/* ---- offline: the screen the auth state calls for ---- */

const OFFLINE_RETRY_MS = 5000;
let offlineTimer = null;

function stopOfflineWatch() {
  if (offlineTimer) { clearTimeout(offlineTimer); offlineTimer = null; }
}

/** WS failed while logged in: retry boot() every 5 s so the screen recovers
 *  on its own (the WS itself keeps its own backoff). Never fires behind the
 *  gate — those calls would re-401 and clear the key the user is typing. */
function startOfflineWatch() {
  if (offlineTimer || $('#gate-card')) return;
  offlineTimer = setTimeout(async () => {
    offlineTimer = null;
    if (state.screen !== 'boot' || $('#gate-card')) return;
    await bootAfterAuth();
  }, OFFLINE_RETRY_MS);
}

/** Screen for an authed-but-offline hub: the gate when the auth state is
 *  known (and not linked), else the wait splash + its poll. */
function showOfflineScreen() {
  if (state.screen === 'app') return;   // last painted data stays
  if (state.auth.state === 'needs_setup') {
    if (state.screen !== 'onboarding') showOnboarding();
    startOfflineWatch();
    return;
  }
  showHubWaiting();   // unreachable/timeout — poll until the hub answers
}

/* ---- data refresh ---- */

/** /api/rooms rows, normalized (the room list itself is the grouping source;
 *  a device's room_id comes from the device row). */
function mergeRooms(rows) {
  return rows.filter(r => r && r.id).map(r => ({
    id: String(r.id), name: r.name || '', sort: Number(r.sort) || 0,
    home_id: r.home_id || '', device_count: Number(r.device_count) || 0,
  }));
}

/** A room moved/renamed/created upstream (rooms_updated): refetch silently —
 *  rooms_updated is never merged in place, the REST data stays authoritative.
 *  Collapse overrides of rooms that are gone go with them (U9). */
function onRoomsUpdated() {
  const alive = new Set((state.rooms || []).map(r => `${r.home_id}/${r.id}`));
  for (const key of Array.from(state.roomExpanded.keys())) {
    if (!alive.has(key)) state.roomExpanded.delete(key);
  }
  emit('data:refresh', { silent: true });
}

let dataRefreshTimer = null;
function scheduleDataRefresh() {
  if (dataRefreshTimer) return;
  dataRefreshTimer = setTimeout(async () => {
    dataRefreshTimer = null;
    if (state.screen !== 'app') return;
    await refreshData({ silent: true });
  }, 300);
}

async function refreshData(opts) {
  opts = opts || {};
  const gen = ++state.dataGen;   // a newer refresh invalidates this run
  // WS device_update counters before the awaits: a device patched while
  // the fetches are in flight keeps its fresh object (status included).
  const seqBefore = new Map();
  for (const id of state.devices.keys()) seqBefore.set(id, state.devSeq.get(id) || 0);
  if (!state.loadingData) {
    state.loadingData = true;
    if (state.screen === 'app' && state.tab === 'home') syncTab();
  }
  try {
    // Each call degrades to null so a hiccup never wipes rendered data.
    const [homesR, devicesR, scenesR, statusR, roomsR] = await Promise.all([
      api('/api/homes', { silent: !!opts.silent }).catch(() => null),
      api('/api/devices', { silent: !!opts.silent }).catch(() => null),
      api('/api/scenes', { silent: !!opts.silent }).catch(() => null),
      api('/api/status', { silent: true }).catch(() => null),
      api('/api/rooms', { silent: true }).catch(() => null),
    ]);
    if (gen !== state.dataGen) return;   // stale — a newer run owns the state
    if (homesR && Array.isArray(homesR)) state.homes = homesR;
    if (roomsR && Array.isArray(roomsR)) state.rooms = mergeRooms(roomsR);
    if (devicesR && Array.isArray(devicesR)) {
      const map = new Map();
      for (const d of devicesR) {
        if (!d || !d.id) continue;
        d.status = d.status || {};
        // The flat /api/devices row carries no room field on some builds:
        // fall back to /api/rooms so an already-swept device still groups.
        if (!d.room_id) {
          const room = (state.rooms || []).find(r => r.home_id === d.home_id
            && r.device_ids && r.device_ids.includes(d.id));
          d.room_id = (room && room.id) || '';
          d.room_name = (room && room.name) || '';
        }
        d._caps = detectCapabilities(d);
        map.set(d.id, d);
      }
      for (const [id, before] of seqBefore) {
        if (!map.has(id)) continue;   // gone from the REST list — do not revive it
        if ((state.devSeq.get(id) || 0) !== before) {
          const fresh = state.devices.get(id);   // old map — the WS-patched object
          if (fresh) map.set(id, fresh);
        }
      }
      state.devices = map;
    }
    if (state.changeDevice && devicesR && Array.isArray(devicesR)) {
      // A model refresh (commit/rename): only that card's node is swapped.
      const id = state.changeDevice;
      state.changeDevice = null;
      const dev = state.devices.get(id);
      if (dev && dev._caps) emit('home:device-added', id);
    }
    if (scenesR && Array.isArray(scenesR)) state.scenes = scenesR;
    if (statusR) state.status = Object.assign({}, state.status, statusR);
  } finally {
    // Only the newest run drops the flag (an older run may still be in flight).
    if (gen === state.dataGen) state.loadingData = false;
  }
  if (state.homeFilter !== 'all' && !state.homes.some(h => h.id === state.homeFilter)) {
    state.homeFilter = 'all';
  }
  if (state.screen === 'app') {
    const lbl = $('#home-btn') && $('.lbl', $('#home-btn'));
    if (lbl) lbl.textContent = homeLabelHtml();
    // A Home that mounted before the first fetch still shows the empty
    // state; that one-time swap is what dataLoaded's adopt path expects.
    if (state.tab === 'home' && !$('#device-grid, .room-grid') && $('.empty-state') && state.devices.size) {
      renderTab();
    }
    dataLoaded();   // rebuild only what structurally changed
    syncTab();
    updateStatusDots();
    updateProgress();
  }
}

/* ---- full re-render on language switch ---- */

function applyTranslations() {
  document.title = t('app_name');
  renderScreen();
  // The app-mode scan overlay is body-mounted — re-render it too.
  if (state.scan.open && state.scan.mode === 'app') renderScanPanel();
}

/* ---- boot ---- */

function renderBootError() {
  const app = $('#app');
  // Fall back to literals only when the dict itself could not be fetched.
  const haveDict = !!(state.dicts && state.dicts[state.lang]);
  const msg = haveDict ? t('err_network')
    : (state.lang === 'en' ? 'Cannot reach the hub' : 'Không kết nối được hub');
  const retry = haveDict ? t('retry')
    : (state.lang === 'en' ? 'Retry' : 'Thử lại');
  app.innerHTML = `
    <div class="onb-wrap">
      <div class="onb-card" style="text-align:center">
        <div class="onb-logo">${brandLogo()}</div>
        <div class="onb-title">Tuya Hub</div>
        <div class="onb-sub">${msg}</div>
        <div class="onb-actions">
          <button class="btn" id="boot-retry">${retry}</button>
        </div>
      </div>
    </div>`;
  $('#boot-retry').addEventListener('click', () => boot());
}

async function boot() {
  try {
    // Language: localStorage > hub setting > en.
    let lang = null;
    try { lang = localStorage.getItem('hub_lang'); } catch (e) { /* ignore */ }
    if (!lang) {
      try {
        // noGate: the 401 gate must mount only after the dict is loaded.
        const j = await api('/api/settings/language', { silent: true, noGate: true });
        if (j && j.lang) lang = j.lang;
      } catch (e) { /* default en */ }
    }
    await loadDict(lang === 'vi' ? 'vi' : 'en');
    state.lang = lang === 'vi' ? 'vi' : 'en';
    document.documentElement.lang = state.lang;
  } catch (e) {
    // TypeError = fetch rejected — the hub is unreachable, wait it out;
    // anything else is a broken dict — the error card + Retry fits.
    if (e instanceof TypeError) {
      showHubWaiting();
    } else {
      console.error('boot failed', e);
      renderBootError();
    }
    return;
  }
  // A 401 on the status probe renders the login gate from api() itself.
  await bootAfterAuth();
}

/** Post-auth boot (also the login gate's continuation). */
async function bootAfterAuth() {
  stopOfflineWatch();
  try {
    state.status = await api('/api/status', { silent: true });
    state.auth = Object.assign({}, state.auth, state.status.auth || {});
  } catch (e) {
    if (e && e.status === 401) {
      // Stale key: api() already cleared it + mounted the gate; the
      // dashboard behind it stays live for the post-login restore.
      abandonDataRefresh();
      return;
    }
    showOfflineScreen();   // unreachable/timeout — retried until answered
    return;
  }
  // Cached initial key: rotate before the app/wizard mounts (the hub
  // forces the change at first login).
  try {
    const info = await api('/api/settings/apikey', { silent: true, noGate: true });
    if (info && info.initial) {
      showApiKeyGate({ rotate: true });
      return;
    }
  } catch (e) { /* probe failed — continue normally */ }
  if (state.auth.state === 'linked') {
    // WS opens in parallel with the data fetches (state.apikey is
    // resolved by the gate/boot before this point). force: the re-login
    // path may have left the screen at 'app' without a live shell.
    const entering = enterApp(true);
    wsConnect();
    await entering;
  } else if (state.status.state === 'needs_setup') {
    showOnboarding();
    wsConnect();
  } else {
    // Reachable but still bring-up (state starting/idle) — a transient
    // auth-idle must never mount the wizard.
    showHubWaiting();
  }
}

/* ---- event wiring ---- */

// setLang: persist (when asked) + full re-render.
on('lang:changed', ({ lang, persist }) => {
  if (persist) {
    api('/api/settings/language', { method: 'POST', body: { lang } }).catch(() => {});
  }
  applyTranslations();
});

// The login gate's successful submit re-runs boot with the new key.
on('auth:ok', () => { boot(); });

/** Routes the shell owns: bindShell off()s and re-registers these, so they
 *  survive a rebuild. A lost WS past boot still goes 401 -> gate via api(). */
function bindShellRoutes() {
  on('ws:conn', down => {
    if (down) onWsLost();
    else if (state.screen === 'app') setConnBanner(false);
  });
  bindConnBanner();
  on('auth:status', data => { applyAuthStatus(data); });   // onboarding poll
  on('status:apply', data => { applyStatus(data); });
  on('data:refresh', opts => { refreshData(opts).then(() => syncTab()); });
}

on('auth:required', () => { abandonDataRefresh(); stopOfflineWatch(); });
on('app:enter', () => { enterApp(); });
on('app:show-onboarding', () => { showOnboarding(); });
on('progress:refresh', () => { updateProgress(); });

/** Structural device events: the payload is {devId} only. A known id edits
 *  the mounted grid surgically; an id we hold no model for (another
 *  session added it) needs the full refetch. */
function onDeviceStruct(event, data) {
  const devId = data && data.devId;
  if (!devId) return;
  if (event === 'device_removed') {
    if (!state.devices.has(devId)) return;
    state.devices.delete(devId);
    emit('home:device-removed', devId);
    return;
  }
  // An event that carries the model registers it (a re-add of a known id
  // replaces the model too) and the shell appends its one card; a bare
  // nudge from another session is a refetch.
  if (state.screen !== 'app') return;
  if (addDeviceFromEvent(data)) return;
  emit('data:refresh', { silent: true });
}

/** A device_added carrying its model: register it and let the shell append
 *  exactly one card node. False = the payload has nothing to build from. */
function addDeviceFromEvent(data) {
  const devId = data && data.devId;
  const ok = data && Array.isArray(data.status_range) && Object.keys(data.status_range).length;
  if (!devId || !ok) return false;
  const dev = Object.assign({ status: {} }, data, { id: devId, _caps: detectCapabilities(data) });
  dev.status_range = data.status_range;
  state.devices.set(devId, dev);
  emit('home:device-added', devId);
  return true;
}

/** The WS event router (registered into core/ws). */
function onWsEvent(event, data) {
  switch (event) {
    case 'status': applyStatus(data); break;
    case 'auth_status': applyAuthStatus(data); break;
    case 'device_update': onDeviceUpdate(data); break;
    case 'device_online': onDeviceOnline(data); break;
    case 'device_added':
    case 'device_removed': onDeviceStruct(event, data); break;
    case 'rooms_updated': onRoomsUpdated(); break;
    case 'discovery_progress': applyDiscovery(data); break;
    case 'mqtt_status':
      if (state.status) {
        state.status.mqtt = data;
        updateStatusDots();
      }
      break;
    default: break; // pong etc.
  }
}
addWsHandler(onWsEvent);

boot();
