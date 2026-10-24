// Settings: the web logbox — 4 s /api/logs poll + level filter.

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, $$, esc, fmtLogTime } from '../core/utils.js';

const LOGS_POLL_MS = 4000;   // /api/logs cadence (settings tab only)

/** The logs section: level filter segment + scrollable logbox. */
export function logsSectionHtml() {
  return `
    <div class="section-title">${t('logs_title')}</div>
    <div class="seg" id="seg-loglvl">
      <button data-loglvl="all" aria-pressed="${state.logs.filter === 'all' ? 'true' : 'false'}" class="${state.logs.filter === 'all' ? 'active' : ''}">${t('logs_all')}</button>
      <button data-loglvl="warn" aria-pressed="${state.logs.filter === 'warn' ? 'true' : 'false'}" class="${state.logs.filter === 'warn' ? 'active' : ''}">${t('logs_warn')}</button>
      <button data-loglvl="err" aria-pressed="${state.logs.filter === 'err' ? 'true' : 'false'}" class="${state.logs.filter === 'err' ? 'active' : ''}">${t('logs_err')}</button>
    </div>
    <div class="logbox" id="log-box" tabindex="0"></div>`;
}

/** The logs section binding: level filter (client-side re-render, no
 *  refetch) + the poll start + cached-lines paint. */
export function bindLogsSection(root) {
  $$('#seg-loglvl button', root).forEach(btn => {
    btn.addEventListener('click', () => {
      const lvl = btn.getAttribute('data-loglvl');
      if (lvl === state.logs.filter) return;
      state.logs.filter = lvl;
      $$('#seg-loglvl button', root).forEach(b => {
        b.classList.toggle('active', b === btn);
        b.setAttribute('aria-pressed', String(b === btn));
      });
      renderLogs(root);
    });
  });
  syncLogsPoll();      // the logs poll follows the tab (immediate fetch)
  renderLogs(root);    // paint cached lines (language switch rebuilds DOM)
}

function logsPollActive() {
  return state.screen === 'app' && state.tab === 'settings';
}

export function stopLogsPoll() {
  const l = state.logs;
  l.gen += 1;   // kills any in-flight tick of the old chain
  if (l.timer) { clearTimeout(l.timer); l.timer = null; }
}

/** Self-scheduling chain (setTimeout, never setInterval — no stacking).
 *  Each tick re-checks the screen/tab and stops itself. */
function startLogsPoll() {
  const l = state.logs;
  if (l.timer) return;
  const gen = (l.gen += 1);
  const tick = async () => {
    l.timer = null;
    if (l.gen !== gen || !logsPollActive()) return;
    await refreshLogs();
    if (l.gen !== gen) return;
    renderLogs();
    l.timer = setTimeout(tick, LOGS_POLL_MS);
  };
  l.timer = setTimeout(tick, 0);
}

/** Start/stop the poll for the CURRENT screen/tab. */
export function syncLogsPoll() {
  if (logsPollActive()) startLogsPoll();
  else stopLogsPoll();
}

/** Silent GET /api/logs -> state.logs.lines (keeps the last lines on
 *  failure). Double-fire safe via the fetching flag. */
async function refreshLogs() {
  const l = state.logs;
  if (l.fetching) return;
  l.fetching = true;
  try {
    const data = await api('/api/logs', { silent: true });
    if (data && Array.isArray(data.lines)) l.lines = data.lines;
  } catch (e) { /* keep the last snapshot */ }
  l.fetching = false;
}

/** Render the logbox from state.logs (filter applied client-side).
 *  Auto-scrolls only when the box was already near its bottom. */
function renderLogs(root) {
  if (state.screen !== 'app' || state.tab !== 'settings') return;   // settings-only box
  const box = $('#log-box', root);
  if (!box) return;
  const l = state.logs;
  const keep = (lvl) => {
    if (l.filter === 'err') return lvl === 'E';
    if (l.filter === 'warn') return lvl === 'E' || lvl === 'W';
    return true;
  };
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
  let html = '';
  for (const line of l.lines || []) {
    if (!keep(line.lvl)) continue;
    const cls = line.lvl === 'E' ? 'lvl-e' : line.lvl === 'W' ? 'lvl-w'
      : (line.lvl === 'D' || line.lvl === 'V') ? 'lvl-d' : 'lvl-i';
    html += `<div class="log-line ${cls}"><span class="log-t">${esc(fmtLogTime(line.t))}</span>` +
      `<span class="log-tag">${esc(line.tag || '')}</span>` +
      `<span class="log-msg">${esc(line.msg || '')}</span></div>`;
  }
  box.innerHTML = html || `<div class="log-empty">${esc(t('logs_empty'))}</div>`;
  if (atBottom) box.scrollTop = box.scrollHeight;
}
