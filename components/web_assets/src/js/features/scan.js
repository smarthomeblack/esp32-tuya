// The shared scan / candidates panel: ONE renderer with two entry points
// (wizard = post-link scan step; app = the Home tab "Add devices" overlay).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, $$, el, esc, icon, clamp } from '../core/utils.js';
import { emit } from '../core/events.js';
import { toast } from '../ui/toast.js';

/** Entry (b): the Home tab "Add devices" button -> the app-mode overlay. */
export function openScanPanel(mode) {
  mountScanPanel(mode);
  scanPanelStart();
}

/** Mount the panel. Wizard mode hosts it inside #app; app mode appends a
 *  full-screen overlay. Events delegated on the root. */
export function mountScanPanel(mode) {
  if (!state.scan.open || state.scan.mode !== mode) {
    state.scan = {
      open: true, mode: mode, phase: 'scanning', candidates: null, sel: null,
      expanded: {}, committing: false, tuyaIot: null,
    };
  }
  let root = $('#scan-sheet');
  if (root) root.remove();
  root = el('<div class="scan-sheet" id="scan-sheet"></div>');
  if (mode === 'wizard') {
    const app = $('#app');
    app.innerHTML = '';
    app.appendChild(root);
  } else {
    document.body.appendChild(root);
  }
  bindScanPanel(root);
  renderScanPanel();
  requestAnimationFrame(() => root.classList.add('show'));
  scanSyncFromStatus();   // paint the live progress immediately
}

/** Drop the panel (any mode) and reset its state. */
export function closeScanPanel() {
  const root = $('#scan-sheet');
  if (root) root.remove();
  state.scan = {
    open: false, mode: null, phase: 'idle', candidates: null, sel: null,
    expanded: {}, committing: false, tuyaIot: null,
  };
}

/** Cancel from the footer: app mode just closes; wizard mode lands in
 *  the app (the scan keeps running server-side). */
function scanCancel() {
  const mode = state.scan.mode;
  closeScanPanel();
  if (mode === 'wizard') emit('app:enter');
}

function scanRoot() {
  return state.scan.open ? $('#scan-sheet') : null;
}

/** Full panel re-render (phase changes; cheap — no form inputs inside). */
export function renderScanPanel() {
  const root = scanRoot();
  if (!root) return;
  const s = state.scan;
  root.innerHTML = `
    <header class="scan-head">
      <div class="scan-title">${t('scan_title')}</div>
      ${s.phase === 'list' ? `<button class="scan-icon-btn" id="scan-rescan" aria-label="${t('scan_rescan')}">${icon('refresh')}</button>` : ''}
      <button class="scan-icon-btn" id="scan-close" aria-label="${t('dlg_cancel')}">${icon('close')}</button>
    </header>
    <div class="scan-body" id="scan-body">${scanBodyHtml()}</div>
    <div class="scan-foot" id="scan-foot">${scanFootInner()}</div>`;
}

function scanBodyHtml() {
  const s = state.scan;
  if (s.phase === 'error') return scanErrorHtml();
  if (s.phase === 'list') return candListHtml();
  return scanScanningHtml();
}

/** The inline progress island + the Tuya IoT enrichment hint. */
function scanScanningHtml() {
  const d = (state.status && state.status.discovery) || {};
  const pct = Math.round(clamp(d.percent || 0, 0, 100));
  const tuyaIotHint = oa => oa && oa.configured ? `
    <div class="scan-oa">
      <span>Tuya IoT</span>
      <i class="oa-dot${oa.has_token ? ' ok' : ' warn'}"></i>
      <span class="scan-oa-txt">${oa.has_token ? t('openapi_status_connected')
        : oa.login_in_flight ? t('openapi_connecting') : t('openapi_status_configured')}</span>
    </div>` : '';
  return `
    <div class="scan-progress">
      <div class="spin"></div>
      <div class="scan-prog-info">
        <div class="scan-lbl">${t('discovering')} <span id="scan-pct">${pct}%</span></div>
        <div class="scan-bar"><div class="scan-bar-fill" id="scan-bar" style="width:${pct}%"></div></div>
      </div>
    </div>
    ${tuyaIotHint(state.scan.tuyaIot)}`;
}

function scanErrorHtml() {
  const d = state.status && state.status.discovery;
  const msg = (d && d.error) || t('scan_failed');
  return `
    <div class="scan-empty">
      <div class="scan-empty-ico err">${icon('alert')}</div>
      <span>${esc(msg)}</span>
      <button class="btn-ghost" id="scan-retry">${t('retry')}</button>
    </div>`;
}

/** The candidates checklist (the discovery staging list). */
function candListHtml() {
  const s = state.scan;
  const list = s.candidates || [];
  if (!list.length) {
    return `
      <div class="scan-empty">
        <div class="scan-empty-ico">${icon('generic')}</div>
        <span>${t('scan_none')}</span>
        <button class="btn-ghost" id="scan-retry">${t('scan_rescan')}</button>
      </div>`;
  }
  let html = '<div class="cand-list">';
  for (const c of list) {
    html += candRowHtml(c);
  }
  html += '</div>';
  return html;
}

function candRowHtml(c) {
  const s = state.scan;
  const sel = s.sel && s.sel[c.id];
  const ents = Array.isArray(c.entities) ? c.entities : [];
  const added = !!c.already_added;
  // ZERO-ENTITY RULE applies to RENDERED entities only (server 400s an
  // empty array); never-expanded rows commit with entities omitted.
  const zero = sel && sel.on && c.detail_loaded && sel.ents.size === 0;
  const sub = [c.category, c.product_name].filter(Boolean).join(' · ');
  const all = sel && allEntsChecked(c, sel);
  const loading = !!detailLoading[c.id];
  return `
    <div class="cand${added ? ' added' : ''}${zero ? ' zero' : ''}${s.expanded[c.id] ? ' open' : ''}" data-cid="${esc(c.id)}">
      <div class="cand-head">
        <label class="cand-check">
          <input type="checkbox" data-dev="${esc(c.id)}" aria-label="${esc(c.name)}"${sel && sel.on ? ' checked' : ''}>
          <span class="cbx">${icon('check')}</span>
        </label>
        <button type="button" class="cand-info" data-exp="${esc(c.id)}" aria-expanded="${!!s.expanded[c.id]}">
          <span class="cand-name">${esc(c.name)}${added ? `<span class="cand-badge">${t('cand_added')}</span>` : ''}</span>
          <span class="cand-sub">${esc(sub || '—')}</span>
        </button>
        <span class="cand-online${c.online ? ' on' : ''}" title="${c.online ? t('online') : t('offline_badge')}"></span>
        <span class="cand-chev" data-exp="${esc(c.id)}">${icon('chev')}</span>
      </div>
      <div class="cand-ents${s.expanded[c.id] ? '' : ' hide'}">
        ${loading ? entLoadingHtml() : `<button type="button" class="ent-all" data-all="${esc(c.id)}">${all ? t('cand_uncheck_all') : t('cand_check_all')}</button>
        ${ents.map(e => candEntRowHtml(c.id, e, sel)).join('')}`}
      </div>
    </div>`;
}

/** Entities placeholder while the lazy detail fetch runs (no ticking yet). */
function entLoadingHtml() {
  return `<div class="ent-loading"><div class="spin"></div></div>`;
}

function candEntRowHtml(cid, e, sel) {
  const checked = !!(sel && sel.ents.has(e.code));
  return `
    <label class="ent-row">
      <input type="checkbox" data-ent="${esc(e.code)}"${checked ? ' checked' : ''}>
      <span class="cbx">${icon('check')}</span>
      <div class="ent-info">
        <span class="ent-name">${esc(e.name || e.code)}</span>
        <span class="ent-sub">${esc(e.code)}${e.type ? ' · ' + esc(e.type) : ''}</span>
      </div>
      ${e.writable ? `<span class="ent-w">${t('ent_writable')}</span>` : ''}
    </label>`;
}

function scanFootInner() {
  const s = state.scan;
  if (s.phase !== 'list') {
    return `<button class="btn-ghost" id="scan-cancel">${t('dlg_cancel')}</button>`;
  }
  const n = scanCommitCount();
  return `
    <button class="btn-ghost" id="scan-cancel">${t('dlg_cancel')}</button>
    <button class="btn" id="scan-commit"${n === 0 || s.committing ? ' disabled' : ''}>${t(n === 1 ? 'cand_commit_one' : 'cand_commit', { n })}</button>`;
}

/** Delegated panel binding (the root survives every innerHTML swap). */
function bindScanPanel(root) {
  root.addEventListener('change', e => {
    const input = e.target;
    if (!input || input.tagName !== 'INPUT' || input.type !== 'checkbox') return;
    const row = input.closest('.cand');
    if (!row) return;
    const id = row.getAttribute('data-cid');
    const sel = state.scan.sel && state.scan.sel[id];
    if (!sel) return;
    if (input.hasAttribute('data-dev')) {
      // Master toggle: the device checkbox checks/unchecks ALL of its
      // entities (mirrors the "check all" row).
      sel.on = input.checked;
      setAllEnts(id, input.checked);
    } else if (input.hasAttribute('data-ent')) {
      const code = input.getAttribute('data-ent');
      if (input.checked) sel.ents.add(code); else sel.ents.delete(code);
      // Ticking an entity selects the device too: the commit skips rows
      // whose sel.on is false, so the checked row would silently vanish.
      if (input.checked && !sel.on) {
        sel.on = true;
        const master = row.querySelector('[data-dev]');
        if (master) master.checked = true;
      }
    }
    updateScanRow(id);
  });
  root.addEventListener('click', e => {
    const exp = e.target.closest('[data-exp]');
    if (exp) {
      toggleScanExpanded(exp.getAttribute('data-exp'));
      return;
    }
    const all = e.target.closest('[data-all]');
    if (all) {
      toggleAllEnts(all.getAttribute('data-all'));
      return;
    }
    const btn = e.target.closest('button');
    if (!btn) return;
    if (btn.id === 'scan-close' || btn.id === 'scan-cancel') { scanCancel(); return; }
    if (btn.id === 'scan-commit') { scanCommit(); return; }
    if (btn.id === 'scan-rescan' || btn.id === 'scan-retry') { scanStart(); return; }
  });
}

/** In-place row updates after a checkbox change (no list re-render, so
 *  scroll position and focus are preserved). */
function updateScanRow(id) {
  const s = state.scan;
  const c = (s.candidates || []).find(x => x.id === id);
  const sel = s.sel && s.sel[id];
  const row = scanRowEl(id);
  if (row && c && sel) {
    const zero = sel.on && c.detail_loaded && sel.ents.size === 0;
    row.classList.toggle('zero', zero);
    const allBtn = $('.ent-all', row);
    if (allBtn) allBtn.textContent = allEntsChecked(c, sel) ? t('cand_uncheck_all') : t('cand_check_all');
  }
  updateScanFoot();
}

function updateScanFoot() {
  const foot = $('#scan-foot', scanRoot());
  if (foot) foot.innerHTML = scanFootInner();
}

function scanRowEl(id) {
  const root = scanRoot();
  if (!root) return null;
  return $(`.cand[data-cid="${CSS.escape(id)}"]`, root);
}

function allEntsChecked(c, sel) {
  const ents = Array.isArray(c.entities) ? c.entities : [];
  return ents.length > 0 && sel && ents.every(e => sel.ents.has(e.code));
}

/** Set every entity checkbox of one device — state first, then the DOM. */
function setAllEnts(id, on) {
  const s = state.scan;
  const c = (s.candidates || []).find(x => x.id === id);
  const sel = s.sel && s.sel[id];
  if (!c || !sel) return;
  sel.ents = on ? new Set((c.entities || []).map(e => e.code)) : new Set();
  const row = scanRowEl(id);
  if (row) $$('input[type="checkbox"][data-ent]', row).forEach(i => { i.checked = on; });
}

function toggleAllEnts(id) {
  const s = state.scan;
  const c = (s.candidates || []).find(x => x.id === id);
  const sel = s.sel && s.sel[id];
  if (!c || !sel) return;
  const on = !allEntsChecked(c, sel);
  setAllEnts(id, on);
  // "Check all" while the master is unchecked must SELECT the device
  // too, or the row stays excluded from the commit.
  if (on && !sel.on) {
    sel.on = true;
    const master = $('input[type="checkbox"][data-dev]', scanRowEl(id));
    if (master) master.checked = true;
  }
  updateScanRow(id);
}

/** Re-render ONE candidate row in place (detail fetch / collapse). Event
 *  handling is delegated on the panel root, so node replacement is safe. */
function renderScanRow(id) {
  const s = state.scan;
  const c = (s.candidates || []).find(x => x.id === id);
  const row = scanRowEl(id);
  if (!row || !c) return;
  // outerHTML drops the focused node — remember which [data-exp] control
  // held focus so the swap does not move the keyboard cursor.
  const active = document.activeElement;
  const hadFocus = !!(active && row.contains(active));
  const exp = hadFocus ? active.getAttribute('data-exp') : null;
  row.outerHTML = candRowHtml(c);
  if (!hadFocus) return;
  const next = scanRowEl(id);
  if (!next) return;
  const same = exp ? next.querySelector(`[data-exp="${CSS.escape(exp)}"]`) : null;
  const target = same || next.querySelector('.cand-info');
  if (target) target.focus();
}

function toggleScanExpanded(id) {
  const s = state.scan;
  s.expanded[id] = !s.expanded[id];
  // Expanding a scan-stage row lazily fetches its entities first.
  if (s.expanded[id]) ensureDetailLoaded(id);
  const row = scanRowEl(id);
  if (!row) return;
  row.classList.toggle('open', !!s.expanded[id]);
  const ents = $('.cand-ents', row);
  if (ents) ents.classList.toggle('hide', !s.expanded[id]);
  const info = $('.cand-info', row);
  if (info) info.setAttribute('aria-expanded', String(!!s.expanded[id]));
}

/** Per-device in-flight flag for the lazy detail fetch. */
const detailLoading = {};

/** GET /api/discovery/candidate/detail for one row; retries briefly when
 *  another row's fetch holds the single-flight slot (409). */
async function ensureDetailLoaded(id) {
  const s = state.scan;
  const gen = s.gen || 0;   // the scan generation this request belongs to
  const c = (s.candidates || []).find(x => x.id === id);
  if (!c || c.detail_loaded || detailLoading[id]) return;
  detailLoading[id] = true;
  renderScanRow(id);   // swap the (partial) entity list for the spinner
  let cand = null;
  try {
    for (let attempt = 0; ; attempt++) {
      try {
        const d = await api(`/api/discovery/candidate/detail?id=${encodeURIComponent(id)}`,
          { silent: true });
        cand = d && d.id === id && d.detail_loaded ? d : null;
        if (!cand) throw new Error('bad detail response');
        break;
      } catch (e) {
        if (e.code !== 'detail_fetch_running' || attempt >= 2) throw e;
        await new Promise(r => setTimeout(r, 700));
      }
    }
  } catch (e) {
    delete detailLoading[id];
    // Collapse/notify only when the panel still owns this fetch (a
    // close+reopen or a rescan must not touch the new list).
    const cur = state.scan;
    if (cur === s && (cur.gen || 0) === gen && cur.open && cur.phase === 'list') {
      // Collapse back; re-expanding retries the fetch.
      cur.expanded[id] = false;
      renderScanRow(id);
      toast(t('scan_detail_failed'), 'err');
    }
    return;
  }
  delete detailLoading[id];
  // A stale generation (rescan / reopen) must not overwrite the new list.
  if (state.scan === s && (s.gen || 0) === gen) applyCandidateDetail(cand);
}

/** Swap one candidate with its fetched full model and re-render the row.
 *  Entities were never touchable before the fetch, so they re-check to
 *  the full list (unchecked devices stay unchecked). */
function applyCandidateDetail(cand) {
  const s = state.scan;
  const i = (s.candidates || []).findIndex(x => x.id === cand.id);
  if (i < 0) return;
  s.candidates[i] = cand;
  const sel = s.sel && s.sel[cand.id];
  if (sel) {
    const ents = new Set();
    for (const e of (Array.isArray(cand.entities) ? cand.entities : [])) {
      if (e && e.code && e.enabled !== false) ents.add(e.code);
    }
    sel.ents = sel.on ? ents : new Set();
  }
  renderScanRow(cand.id);
  updateScanRow(cand.id);
}

/** Devices that WILL be committed (the footer count). */
function scanCommitCount() {
  const s = state.scan;
  if (!s.candidates) return 0;
  let n = 0;
  for (const c of s.candidates) {
    const sel = s.sel && s.sel[c.id];
    if (!sel || !sel.on) continue;
    // Never-expanded rows commit with entities omitted — an empty (scan
    // stage) entity list does not exclude them; rendered rows need >= 1.
    if (!c.detail_loaded || sel.ents.size > 0) n++;
  }
  return n;
}

/** Entry-point starter: fetches a FRESH /api/status, then shows the
 *  progress, the list, the failure state — or starts a scan. */
export async function scanPanelStart() {
  if (!state.scan.open) return;
  api('/api/openapi/status', { silent: true }).then(d => {
    if (d && state.scan.open) {
      state.scan.tuyaIot = d;
      if (state.scan.phase === 'scanning') renderScanPanel();
    }
  }).catch(() => { /* hint stays hidden */ });
  try {
    const st = await api('/api/status', { silent: true });
    if (st) emit('status:apply', st);   // app.js: applyStatus(st)
  } catch (e) { /* keep the WS snapshot */ }
  if (!state.scan.open) return;
  const d = state.status && state.status.discovery;
  if (d && d.active && !d.done && !d.failed) return;  // already running
  if (d && d.failed) {
    state.scan.phase = 'error';
    renderScanPanel();
    return;
  }
  if (d && d.done) {
    await loadCandidates();
    return;
  }
  scanStart();
}

/** POST /api/discovery/scan + track the run (rescan / retry / auto-start). */
async function scanStart() {
  if (!state.scan.open) return;
  state.scan.phase = 'scanning';
  state.scan.candidates = null;
  state.scan.sel = null;
  // Fresh rows render collapsed — stale expanded state must not expose
  // the previous scan's partial entity lists as checkable.
  state.scan.expanded = {};
  renderScanPanel();
  try {
    await api('/api/discovery/scan', { method: 'POST', silent: true });
    // Optimistic reset: without this the panel would flash the PREVIOUS
    // run's done/percent until the first WS progress event lands.
    if (state.status) {
      state.status.discovery = { active: true, percent: 0, done: false, failed: false };
    }
    emit('progress:refresh');   // app.js: updateProgress()
  } catch (e) {
    if (e.code === 'discovery_running') {
    // A scan is already running (another session or the boot auto-scan
    // racing this click) — adopt its live state.
      try {
        const st = await api('/api/status', { silent: true });
        if (st && st.discovery) state.status.discovery = st.discovery;
      } catch (e2) { /* keep the snapshot */ }
    } else {
      state.scan.phase = 'error';
      renderScanPanel();
      toast(e.message || t('scan_failed'), 'err');
      return;
    }
  }
  scanSyncFromStatus();
}

/** WS discovery_progress + poll-fallback status -> panel sync (cheap
 *  no-op when no panel is open). */
export function scanSyncFromStatus() {
  if (!state.scan.open || state.scan.phase !== 'scanning') return;
  const d = state.status && state.status.discovery;
  if (!d) return;
  if (d.failed) {
    state.scan.phase = 'error';
    renderScanPanel();
    return;
  }
  if (d.done && !d.active) {
    loadCandidates();
    return;
  }
  // Active (or the brief spawn window) — paint the inline bar.
  const pct = Math.round(clamp(d.percent || 0, 0, 100));
  const bar = $('#scan-bar', scanRoot());
  if (bar) bar.style.width = pct + '%';
  const lbl = $('#scan-pct', scanRoot());
  if (lbl) lbl.textContent = pct + '%';
}

/** GET /api/discovery/candidates -> the checklist (phase 'list'). The
 *  loading flag collapses double-fires into one fetch. */
let candsLoading = false;
async function loadCandidates() {
  const s = state.scan;
  if (!s.open || candsLoading) return;
  candsLoading = true;
  s.gen = (s.gen || 0) + 1;   // a new list invalidates in-flight detail fetches
  try {
    const data = await api('/api/discovery/candidates', { silent: true });
    if (!s.open) return;   // panel closed while fetching
    if (data && data.active) {
      s.phase = 'scanning';
      renderScanPanel();
      return;
    }
    if (data && data.failed) {
      s.phase = 'error';
      renderScanPanel();
      return;
    }
    if (data && !data.done) {
      // Staging not done and not active — the spawn window or no scan.
      let running = false;
      try {
        const st = await api('/api/status', { silent: true });
        running = !!(st && st.discovery && st.discovery.active && !st.discovery.done);
      } catch (e) { /* fall through to the error state */ }
      if (running) {
        s.phase = 'scanning';
        renderScanPanel();
        return;
      }
      s.phase = 'error';
      renderScanPanel();
      return;
    }
    const list = (data && Array.isArray(data.candidates)) ? data.candidates : [];
    s.candidates = list;
    // Selection model: NEW devices checked (scan-stage entities are partial;
    // such rows commit with `entities` omitted); already-added default OFF,
    // tickable — a tick re-commits it; the server refreshes a strictly-poorer
    // model, else skips.
    const sel = {};
    for (const c of list) {
      const ents = new Set();
      for (const e of (Array.isArray(c.entities) ? c.entities : [])) {
        if (e && e.code && e.enabled !== false) ents.add(e.code);
      }
      sel[c.id] = { on: !c.already_added, ents: ents };
    }
    s.sel = sel;
    s.phase = 'list';
    renderScanPanel();
  } catch (e) {
    s.phase = 'error';
    renderScanPanel();
  } finally {
    candsLoading = false;
  }
}

/** POST /api/discovery/commit — checked devices; entities are SENT only
 *  for rows rendered from the fetched model, else omitted (all enabled).
 *  Server-side fetch-before-commit (~1.2 s/device) — 30 s, not 15 s. */
async function scanCommit() {
  const s = state.scan;
  if (!s.open || s.committing) return;
  const devices = [];
  for (const c of (s.candidates || [])) {
    const sel = s.sel && s.sel[c.id];
    if (!sel || !sel.on) continue;
    if (c.detail_loaded) {
      if (sel.ents.size === 0) continue;   // zero-entity rule
      devices.push({ id: c.id, entities: Array.from(sel.ents) });
    } else {
      devices.push({ id: c.id });          // entities omitted = all enabled
    }
  }
  if (!devices.length) return;
  s.committing = true;
  updateScanFoot();
  try {
    const r = await api('/api/discovery/commit',
      { method: 'POST', body: { devices }, timeoutMs: 30000 });
    const added = r && r.added != null ? r.added : devices.length;
    const okKey = added === 1 ? 'cand_committed_one' : 'cand_committed';
    toast(added > 0 ? t(okKey, { n: added }) : t('cand_committed_none'), 'ok');
    // A close+reopen during the POST swapped state.scan: the response must
    // not close the NEW panel (nor replay the old entry point).
    if (s === state.scan) {
      const mode = s.mode;
      closeScanPanel();
      if (mode === 'wizard') emit('app:enter');                       // app.js: enterApp()
      else emit('data:refresh', { silent: true });                    // app.js: refreshData()
    }
  } catch (e) { /* toast shown by api(); panel stays open */ }
  s.committing = false;
  if (s === state.scan) updateScanFoot();
}
