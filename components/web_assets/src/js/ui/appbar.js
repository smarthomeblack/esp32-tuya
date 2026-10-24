// App shell parts: status dots, home selector, the mounted tab panels +
// the connection banner. app.js renders the shell itself.

import { state } from '../core/state.js';
import { $, $$, el, esc, clamp } from '../core/utils.js';
import { t } from '../core/i18n.js';
import { on, off } from '../core/events.js';
import { tabHome, bindHome, addCard, removeCard, homeSig, reapplyPending } from '../features/dashboard.js';
import { buildCard } from '../features/cards.js';
import { updateCardInPlace } from '../features/cards-registry.js';
import { tabScenes, bindScenes } from '../features/scenes.js';
import { tabSettings, bindSettings, patchSettingsDynamic } from '../features/settings.js';
import { accountSectionHtml, patchAuthSection } from '../features/settings.js';
import { stopLogsPoll } from '../features/settings-logs.js';

/** Appbar status dots: cloud = account linked, mqtt = broker connected. */
export function updateStatusDots() {
  const cloud = $('#dot-cloud'), mqtt = $('#dot-mqtt');
  if (!cloud || !mqtt) return;
  const st = state.status || {};
  const linked = !!(st.auth && st.auth.state === 'linked');
  const mq = !!(st.mqtt && st.mqtt.connected);
  cloud.classList.toggle('on', linked);
  cloud.title = linked ? t('dot_cloud_on') : t('dot_cloud_off');
  cloud.setAttribute('aria-label', cloud.title);
  mqtt.classList.toggle('on', mq);
  mqtt.title = t('mqtt') + ' · ' + t(mq ? 'online' : 'offline_badge');
  mqtt.setAttribute('aria-label', mqtt.title);
}

export function updateProgress() {
  const track = $('#progress-track'), lbl = $('#progress-lbl');
  if (!track || !lbl) return;
  const d = state.status && state.status.discovery;
  const active = !!(d && d.active && !d.done && !d.failed);
  track.classList.toggle('show', active);
  lbl.classList.toggle('show', active);
  if (active) {
    $('.bar', track).style.width = clamp(d.percent || 0, 0, 100) + '%';
    lbl.textContent = t('discovering') + ' ' + Math.round(d.percent || 0) + '%';
  }
}

export function homeLabelHtml() {
  if (state.homeFilter === 'all') return t('all_homes');
  const h = state.homes.find(x => x.id === state.homeFilter);
  return h ? h.name : t('all_homes');
}

// One outside-click closer serves every menu session; openPill is armed
// deferred so the opening click itself never closes the menu.
let openPill = null;
document.addEventListener('click', () => { if (openPill) closeHomeMenu(openPill); });

// The home picker only filters the data; the tab refresh (same home = no-op)
// applies it.
function selectHome(id, name) {
  state.homeFilter = id;
  $('.lbl', $('#home-btn')).textContent = name;
  renderTab();
}

export function openHomeMenu(pill) {
  pill.classList.add('open');
  const opts = [{ id: 'all', name: t('all_homes') }]
    .concat(state.homes.map(h => ({ id: h.id, name: h.name })));
  const menu = el(`<div class="home-menu" role="listbox"></div>`);
  for (const o of opts) {
    const item = el(`<button role="option" class="${state.homeFilter === o.id ? 'sel' : ''}" data-home="${esc(o.id)}"><span class="dot"></span><span>${esc(o.name)}</span></button>`);
    item.addEventListener('click', e => {
      e.stopPropagation();
      closeHomeMenu(pill);
      selectHome(o.id, o.name);
    });
    menu.appendChild(item);
  }
  pill.appendChild(menu);
  setTimeout(() => { openPill = pill; }, 0);
}

export function closeHomeMenu(pill) {
  pill.classList.remove('open');
  const m = $('.home-menu', pill);
  if (m) m.remove();
  if (openPill === pill) openPill = null;
}

export function setConnBanner(down) {
  const banner = $('#conn-banner');
  if (!banner) return;
  banner.classList.toggle('show', !!down);
  const btn = $('#btn-refresh');
  if (btn) btn.style.display = down ? '' : 'none';
}

/* ---- mounted tab panels: built once, toggled on switch ---- */

// tab id -> {sec, sig, empty}; a panel rebuilds only on a structural change
// or a language switch (Tab switches just toggle visibility).
let panels = new Map();

function tabBody(tab) {
  switch (tab) {
    case 'scenes': return { html: tabScenes(), bind: bindScenes };
    case 'settings': return { html: tabSettings(), bind: bindSettings };
    default: return { html: tabHome(), bind: bindHome };
  }
}

/** The devices the Home panel lists (the home filter applies). */
function homeDevices() {
  return Array.from(state.devices.values())
    .filter(d => state.homeFilter === 'all' || d.home_id === state.homeFilter);
}

/** The data signature of a panel: equal = the markup would be identical. */
function tabSig(tab) {
  const homes = state.homes.map(h => h.id).join(',');
  if (tab === 'scenes') {
    const scenes = state.scenes
      .filter(s => state.homeFilter === 'all' || s.home_id === state.homeFilter)
      .map(s => s.id + (s.enabled ? '' : '~'));
    return [state.lang, state.homeFilter, homes, scenes.join(',')].join('|');
  }
  if (tab === 'home') return homeSig();
  const st = state.status || {};
  return [state.lang, state.homeFilter, homes,
    st.wifi && st.wifi.ssid, st.wifi && st.wifi.ip,
    st.heap && st.heap.psram_total > 0 ? 1 : 0,
    st.version, state.auth && state.auth.username, state.auth && state.auth.state,
  ].join('|');
}

/** Why the settings values need their in-place patch: the live /api/status
 *  fields the static settings markup skips. */
function statusDynSig() {
  const st = state.status || {};
  const wifi = st.wifi || {};
  return [wifi.ssid, wifi.ip, st.uptime_ms, st.version,
    st.heap && st.heap.internal_min_free, st.device_count, st.home_count, st.scene_count,
    state.auth && state.auth.username, state.auth && state.auth.state].join('|');
}

/** Static shape of a tab body: the builder's own markup logic (its output
 *  captured before bind() runs — no side effect). */
function tabShape(tab) {
  try {
    return tabBody(tab).html;
  } catch (e) {
    return '';
  }
}

/** No data yet: the panels are skeletons (tabSig has no content signature). */
function pendingData(tab) {
  return state.loadingData || !state.homes.length ||
    (tab === 'home' && !homeDevices().length && !!state.devices.size);
}

/** Build (or rebuild) one panel. The binder runs while the section is still
 *  detached, so its scoped lookups see THIS panel's nodes — never another
 *  mounted tab's, and not the page only. */
function mountPanel(tab, force) {
  const prev = panels.get(tab);
  // The markup about to be built describes the data AS OF this signature.
  const builtFrom = tabSig(tab);
  // structural = the data the mounted markup was built from (a fresh
  // tabSig would always equal the current data and never rebuild).
  if (prev && !force && prev.structural === tabSig(tab)) return prev;
  const sec = el('<section class="tab-sec"></section>');
  sec.setAttribute('data-tab-sec', tab);
  sec.hidden = true;
  let body;
  try {
    body = tabBody(tab);
  } catch (e) {
    console.error('tab render failed', tab, e);
    // Error variant of the conn-banner pattern (no retry logic).
    body = { html: `<div class="tab-panel"><div class="conn-banner show" role="alert">
      <span class="pulse"></span><span>${esc(t('tab_error'))}</span></div></div>`, bind: null };
  }
  sec.innerHTML = body.html;
  const p = { sec: sec, sig: builtFrom, structural: builtFrom,
    // null = the skeleton; it never counts as a settled shape.
    shape: sec.querySelector('.skel-card') ? null : body.html,
    empty: !sec.querySelector('#device-grid, .room-grid') && !!sec.querySelector('.empty-state') };
  if (body.bind) {
    try {
      body.bind(sec);
    } catch (e) {
      // A throwing binder must not cost the user the whole tab.
      console.error('tab bind failed', tab, e);
    }
  }
  if (prev) {
    prev.sec.replaceWith(sec);   // same slot: the toolbar above stays put
  } else if (tab === 'settings') {
    const main = $('#main');
    if (main) main.appendChild(sec);   // last in #main: the account box is its last child
  } else {
    // Appended in nav order (home, scenes) — never inserted, so no panel
    // the user is on is displaced.
    const main = $('#main');
    const after = tab === 'home' ? null : Array.from(main.querySelectorAll('[data-tab-sec]'))
      .find(s => s.getAttribute('data-tab-sec') === 'home');
    if (after) main.insertBefore(sec, after.nextSibling);
    else main.appendChild(sec);
  }
  panels.set(tab, p);
  if (tab === 'home') {
    homeStructural = builtFrom;
    reapplyPending(sec);   // a rebuild swaps nodes; the spinner must stay
  }
  return p;
}

let shownTab = null;
let shownDyn = '';
// Home cards patched in place while the Home panel was hidden.
const homeDirty = new Set();
// The Home structure signature of the MOUNTED panel. A structure change
// (device in/out, room rename/reorder) must be honoured on the way back
// in, so it is tracked while the tab is hidden too (U4).
let homeStructural = null;


/** The settings account section (unlink) lives in #main right above the
 *  Settings panel; the shell owns it so any tab can show it. */
function authSection() {
  const main = $('#main');
  if (!main) return null;
  // Created once per shell, then never removed or moved: any #main childList
  // edit costs the mounted Home cards their node identity on a tab switch.
  let sec = $('#auth-section');
  if (!sec) {
    for (const stale of $$('#auth-section')) stale.remove();
    sec = el('<section id="auth-section" class="tab-panel"></section>');
    sec.innerHTML = accountSectionHtml();
    const first = main.querySelector('[data-tab-sec]');
    if (first) main.insertBefore(sec, first.nextSibling);
    else main.appendChild(sec);
  }
  return sec;
}

/** Only the active panel is display:visible; the other mounted tab sections
 *  (and the settings account section) are hidden — cards keep their DOM
 *  identity across switches. */
function showOnly(tab) {
  const p = panels.get(tab);
  const main = $('#main');
  if (!p || !main) return;
  for (const kid of Array.from(main.children)) {
    if (kid.hasAttribute('data-tab-sec')) kid.hidden = kid !== p.sec;
  }
  const auth = $('#auth-section');
  if (auth) auth.hidden = tab !== 'settings';
}

/** Point #main at state.tab: toggle visibility (no innerHTML swap on a
 *  plain switch); a panel whose data changed is rebuilt on arrival. */
export function renderTab() {
  const main = $('#main');
  if (!main) return;
  const tab = state.tab;
  // Leaving settings stops the logs poll (ticks self-check).
  if (tab !== 'settings') stopLogsPoll();
  // The appbar "+" lives on the Home tab only.
  const addBtn = $('#btn-add');
  if (addBtn) addBtn.style.display = tab === 'home' ? '' : 'none';
  authSection();
  // Replace the shell's Home subscriptions (they capture this panel list);
  // dashboard.js keeps its own module-level ones (no dupes on rebuild).
  registerTabEvents();
  const p = mountPanel(tab, false);
  // Home structure changes land while the tab is hidden (U4): rebuild the
  // stale markup on the way back in.
  if (tab === 'home' && homeSig() !== homeStructural) mountPanel('home', true);
  showOnly(tab);
  (panels.get(tab) || p).sec.hidden = false;
  shownTab = tab;
  if (tab === 'settings') {
    shownDyn = statusDynSig();
    patchSettingsDynamic();
    patchAuthSection();
  }
}

/** The mounted markup is only re-rendered when the builder's own output
 *  differs from the mount (a value patch never does); the signature alone
 *  only says the builder should be consulted again. */
function needsRebuild(tab, p) {
  return tabShape(tab) !== p.shape;
}

/** Fresh data after a skeleton mount: same structure keeps the nodes
 *  (values patch in place); a different one — the skeleton turning into the
 *  real grid or the empty state — rebuilds once. */
function adoptData(tab, cur) {
  if (cur.empty && tab === 'home' && !homeDevices().length) return;
  mountPanel(tab, true);
}

/** After a data:refresh the DATA is authoritative (the user asked for it):
 *  only a markup change rebuilds; Home cards patch in place. */
export function dataLoaded() {
  if (state.screen !== 'app') return;
  syncTab();
}

/** Re-run the active tab's in-place patches and rebuild it when its data
 *  structurally changed (the signature check keeps a plain repaint free). */
export function syncTab() {
  if (state.screen !== 'app') return;
  const tab = state.tab;
  const p = panels.get(tab);
  // A skeleton resolves once the data lands; with no data yet the shape
  // check would only swap one skeleton for another on every WS status.
  if (p && tab === shownTab && !pendingData(tab)) {
    if (tab === 'home') {
      // The mounted grid patches values; a device in/out rebuilds through the
      // Home builder (a plain status tick is a no-op).
      if (tabSig(tab) !== p.structural) {
        homePanelInfo().sync();   // rebuilds; mountPanel re-records the sig
      }
    } else {
      const sig = tabSig(tab);
      if (sig !== p.structural) {
        p.structural = sig;
        if (needsRebuild(tab, p)) adoptData(tab, p);
      }
    }
  }
  if (tab === 'settings') {
    const sig = statusDynSig();
    if (sig !== shownDyn) {
      shownDyn = sig;
      patchSettingsDynamic();
    }
  }
  // Value-only updates on a Home panel that was hidden (dashboard.js).
  if (tab === 'home' && homeDirty.size) {
    const info = homePanelInfo();
    for (const id of homeDirty) {
      const node = info && info.card(id);
      // A card whose markup changed (online/offline look) is swapped as ONE
      // node: the grid and every other card keep their identity.
      const dev = state.devices.get(id);
      if (node && dev && !updateCardInPlace(node)) node.replaceWith(buildCard(dev, dev._caps));
    }
    homeDirty.clear();
  }
}

/** The not-yet-visible tabs' writes are deliberately unconditional (scene
 *  triggers, log ticks), so those renderers run on the active tab only. */
function tabActive(tab) {
  return state.screen === 'app' && state.tab === tab;
}

/** Home-panel hooks: grid/card lookups live in dashboard.js, the empty
 *  state decides whether a rebuild is needed at all. */
function homePanelInfo() {
  const p = panels.get('home');
  if (!p) return null;
  return {
    sec: p.sec,
    empty: p.empty,
    card: id => $('.card[data-dev="' + CSS.escape(id) + '"]', p.sec),
    grid: () => $('#device-grid, .room-grid', p.sec),
    // A structural sync (first data, a device in/out) rebuilds the mounted
    // panel; mountPanel's own signature check keeps a same-shape call free.
    sync: () => mountPanel('home', false),
    drop: () => p.sec.remove(),
  };
}

/** Forget the mounted panels (the shell (re)build owns fresh ones). */
export function resetTabPanels() {
  panels = new Map();
  shownTab = null;
  shownDyn = '';
  homeStructural = null;
}

/** Home-panel subscriptions. dashboard.js re-registers its own on module
 *  load; the shell's are replaced here on every rebuild. */
function registerTabEvents() {
  off('home:device-added');
  off('home:card-dirty');
  // A hidden Home cannot patch itself (syncTab's shownTab guard swallows it):
  // drop its latch so the arrival rebuild in renderTab is guaranteed.
  const staleHome = () => { homeStructural = null; };
  on('home:device-added', devId => {
    const info = homePanelInfo();
    if (state.screen !== 'app' || !info) return;
    // The visible grid appends that one card; a hidden panel rebuilds on arrival.
    if (state.tab === 'home' && !info.empty) { addCard(info, devId); return; }
    if (state.tab === 'home') syncTab();
    else staleHome();
  });
  on('home:device-removed', devId => {
    const info = homePanelInfo();
    if (state.screen !== 'app' || !info) return;
    if (state.tab === 'home') removeCard(info, devId);
    else staleHome();
  });
  on('home:card-dirty', devId => { if (state.screen === 'app') homeDirty.add(devId); });
}

// core/ws signals connect state (debounced there); the banner follows.
/** The shell calls this after every off('ws:conn') re-registration. */
export function bindConnBanner() {
  on('ws:conn', down => setConnBanner(down));
}

