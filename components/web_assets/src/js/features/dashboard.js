// Home tab: room sections (or the flat grid without room data), per-card
// refresh and the WS device_update / device_online handlers.

import { state } from '../core/state.js';
import { on, emit } from '../core/events.js';
import { t } from '../core/i18n.js';
import { $, el, esc, icon } from '../core/utils.js';
import { buildCard } from './cards.js';
import { updateCardInPlace } from './cards-registry.js';
import { clearPending } from './cards-commands.js';
import { roomsFor } from './rooms.js';
import { openScanPanel } from './scan.js';
function homeDevices() {
  return Array.from(state.devices.values())
    .filter(d => state.homeFilter === 'all' || d.home_id === state.homeFilter);
}

/** Collapse trips on the room COUNT, not devices: below 8 every room is open. */
const kCollapseFrom = 8;

/** Room rows for the current data: null = today's flat grid (no room data,
 *  or every device unassigned — a partial rollout is never worse). */
function homeRooms() {
  const groups = roomsFor(Array.from(state.devices.values()), state.homes,
    state.homeFilter, state.lang, null, state.rooms);
  if (!groups.length) return [];
  if (groups.length === 1 && groups[0].isNone) return null;
  return groups;
}

function roomExpanded(g, index, total) {
  if (g.online < g.total) return true;
  if (g.isNone) return false;   // never auto-collapsed
  const own = state.roomExpanded.get(g.key);
  if (own !== undefined) return own;
  return !(total >= kCollapseFrom && index > 0);
}

const peekText = g => {
  const names = g.devices.slice(0, 2).map(d => d.name || d.id);
  const more = g.total - names.length;
  return names.join(', ') + (more > 0 ? ` +${more}` : '');
};

/** `lang|homeFilter|homes|rooms(home_id,id,name,sort)->deviceIds` — status
 *  never rides the signature, so a WS value tick never rebuilds. */
export function homeSig() {
  if (state.loadingData) return state.lang + '|loading';
  const homes = state.homes.map(h => h.id + ':' + (h.rooms || []).length).join(',');
  const rooms = (state.rooms || []).map(r => `${r.home_id}/${r.id}:${r.name}:${r.sort}`).join(',');
  const devs = homeDevices().map(d => d.id + '/' + (d.room_id || '')).join(',');
  return [state.lang, state.homeFilter, homes, rooms, devs].join('|');
}

function roomSection(g, index, total) {
  const open = roomExpanded(g, index, total);
  const offline = g.total - g.online;
  const count = g.online === g.total ? t('room_count_fmt', { total: g.total })
    : t('room_count_online_fmt', { total: g.total, online: g.online });
  return `<section class="room${open ? '' : ' collapsed'}${g.isNone ? ' room-none' : ''}" data-room="${esc(g.key)}"><button class="room-head" type="button" aria-expanded="${open}" aria-controls="rg-${esc(g.key)}" aria-label="${esc(g.name + ' - ' + count)}">${icon('chev')}<span class="room-name">${esc(g.name)}</span>${open ? '' : `<span class="room-peek">${esc(peekText(g))}</span>`}${offline ? `<span class="room-off">${offline} ${esc(t('offline_badge').toLowerCase())}</span>` : ''}<span class="room-count">${esc(count)}</span></button><div class="grid room-grid" id="rg-${esc(g.key)}" data-room-grid="${esc(g.key)}">${g.total ? '' : `<div class="room-empty">${esc(t('room_empty'))}</div>`}</div></section>`;
}

export function tabHome() {
  if (state.loadingData) {
    return `<div class="tab-panel"><div class="grid">
      <div class="skel-card"><div class="skel-bar"></div><div class="skel-bar short"></div></div>
      <div class="skel-card"><div class="skel-bar"></div><div class="skel-bar short"></div></div>
      <div class="skel-card wide"><div class="skel-bar"></div><div class="skel-bar short"></div><div class="skel-bar"></div></div>
      <div class="skel-card"><div class="skel-bar"></div><div class="skel-bar short"></div></div>
    </div></div>`;
  }
  const devices = homeDevices();
  if (!devices.length) {
    return `<div class="tab-panel"><div class="empty-state">
      ${icon('generic')}<span>${t('dash_empty')}</span>
      <button class="btn-ghost empty-add" id="btn-add-empty">${t('scan_title')}</button>
    </div></div>`;
  }
  const groups = homeRooms();
  // Fallback: no room data keeps today's markup (and its #device-grid id).
  if (!groups) return `<div class="tab-panel"><div class="grid" id="device-grid"></div></div>`;
  return `<div class="tab-panel room-list">${groups.map((g, i) =>
    roomSection(g, i, groups.length)).join('')}</div>`;
}

/** Bind the Home panel's own nodes (root = the panel section). */
export function bindHome(root) {
  // The empty-state "Add devices" CTA (the appbar "+" takes over later).
  const emptyAdd = $('#btn-add-empty', root);
  if (emptyAdd) emptyAdd.addEventListener('click', () => openScanPanel('app'));
  bindRoomHeads(root);
  const grid = $('#device-grid', root);
  if (grid) {
    for (const dev of homeDevices()) grid.appendChild(cardNode(dev));
    return;
  }
  const nodes = Array.from(root.querySelectorAll('.room[data-room]'));
  const groups = homeRooms() || [];
  for (const sec of nodes) {
    const g = groups.find(x => x.key === sec.getAttribute('data-room'));
    const target = $('.room-grid', sec);
    if (!g || !target) continue;
    const empty = $('.room-empty', target);
    if (empty) empty.remove();   // the explicit empty state gives way to cards
    for (const dev of g.devices) target.appendChild(cardNode(dev));
  }
}

/** Collapsible headers: the whole band is the toggle (click + Enter/Space). */
function bindRoomHeads(root) {
  for (const head of Array.from(root.querySelectorAll('.room-head'))) {
    const toggle = e => {
      if (e.type === 'keydown' && e.key !== 'Enter' && e.key !== ' ') return;
      if (e.type === 'keydown') e.preventDefault();
      e.stopPropagation();
      toggleRoom(head.closest('.room'));
    };
    head.addEventListener('click', toggle);
    head.addEventListener('keydown', toggle);
  }
}

/** Flip one room open/closed and remember it for this session. A room with
 *  an offline device always renders open (the next rebuild re-applies that). */
function toggleRoom(sec) {
  if (!sec) return;
  const key = sec.getAttribute('data-room');
  const open = !sec.classList.contains('collapsed');
  sec.classList.toggle('collapsed', open);
  state.roomExpanded.set(key, !open);
  const head = $('.room-head', sec);
  if (head) head.setAttribute('aria-expanded', String(!open));
  const groups = homeRooms() || [];
  const g = groups.find(x => x.key === key);
  if (!g) return;
  // The peek line only exists while collapsed: keep the markup in step.
  const peek = $('.room-peek', head);
  if (open && peek && !(g.total - g.online)) peek.remove();
  if (!open && !peek && !(g.total - g.online)) {
    head.insertBefore(el(`<span class="room-peek">${esc(peekText(g))}</span>`),
      head.querySelector('.room-count'));
  }
}

/** One card node for a device (or the error placeholder when the builder
 *  throws — the caller always gets a node). */
function cardNode(dev) {
  try {
    return buildCard(dev, dev._caps);
  } catch (e) {
    console.error('card build failed', dev.id, e);
    return buildErrorCard(dev);
  }
}

/** Re-apply the optimistic toggle look after a rebuild swapped the nodes
 *  (the pending timer keeps running; only the class needs re-adding). */
export function reapplyPending(root) {
  if (!root || (!state.pending.size && !state.cmdErr.size)) return;
  for (const id of state.pending.keys()) {
    const node = $('.card[data-dev="' + CSS.escape(id) + '"]', root);
    if (node) node.classList.add('pending');
  }
  for (const id of state.cmdErr.keys()) {
    const node = $('.card[data-dev="' + CSS.escape(id) + '"]', root);
    if (node) node.classList.add('cmd-err');   // its timer is still running
  }
}


/** Placeholder card for a device whose builder threw (cmd-err red icon). */
function buildErrorCard(dev) {
  const node = el(`<article class="card cmd-err" data-dev="${esc(dev.id)}"
    title="${esc(t('card_error_hint'))}"></article>`);
  node.innerHTML = `
    <div class="card-top">
      <div class="card-ico">${icon('alert')}</div>
      <div style="flex:1;min-width:0">
        <div class="card-name">${esc(dev.name || dev.id)}</div>
        <div class="card-sub">${esc(t('card_error_title'))}</div>
      </div>
    </div>`;
  return node;
}

/** Patch one card in place — no DOM swap, so CSS transitions survive.
 *  Falls back to a full rebuild only when the structure changed (first
 *  render, caps/entity change, dev object replaced). */
function renderCardNow(devId) {
  if (state.screen !== 'app' || state.tab !== 'home') return;
  const dev = state.devices.get(devId);
  if (!dev) return;
  const old = $(`.card[data-dev="${CSS.escape(devId)}"]`);
  if (!old) return;
  if (!old.closest('.grid')) return;
  if (updateCardInPlace(old)) return;
  old.replaceWith(cardNode(dev));
}

/** Re-render a single card in place (WS path), debounced ~200 ms per
 *  device — bursts land as one paint. A hidden Home panel queues the id
 *  (appbar.syncTab patches it in place when the tab comes back). */
const cardRefreshTimers = new Map();   // devId -> pending setTimeout
function refreshCard(devId) {
  if (state.screen !== 'app') return;
  if (cardRefreshTimers.has(devId)) return;   // a render is already scheduled
  cardRefreshTimers.set(devId, setTimeout(() => {
    cardRefreshTimers.delete(devId);
    if (state.tab === 'home') renderCardNow(devId);
    else emit('home:card-dirty', devId);   // patched on arrival (appbar)
  }, 200));
}

// cards.js must not import this module — the events bus routes its
// "repaint now" requests here.
on('card:refresh', devId => { if (state.tab === 'home') renderCardNow(devId); });

/* ---- device events ---- */

export function onDeviceUpdate(data) {
  const devId = data && data.devId;
  const dev = devId && state.devices.get(devId);
  if (!dev) return;
  // A REST refresh in flight must keep this (fresher) device object.
  state.devSeq.set(devId, (state.devSeq.get(devId) || 0) + 1);
  if (data.name) dev.name = data.name;
  const props = Array.isArray(data.props) ? data.props : [];
  // No props = a silent re-register: the card's structure may have changed,
  // so re-read the device and swap that one node, never the whole tab.
  if (!props.length && !data.name) {
    state.changeDevice = devId;
    emit('data:refresh', { silent: true });
    return;
  }
  for (const p of props) {
    if (p && p.code) dev.status[p.code] = p.value;
  }
  clearPending(devId, props.map(p => p.code));
  refreshCard(devId);
}

export function onDeviceOnline(data) {
  const dev = data.devId && state.devices.get(data.devId);
  if (!dev) return;
  dev.online = !!data.online;
  refreshCard(data.devId);
}

/* ---- structural device events: surgical grid edits ---- */

// info = the shell's Home panel view (appbar.homePanelInfo); these edit the
// VISIBLE panel only — the shell rebuilds a hidden Home on arrival (syncTab).
const roomSel = id => '.card[data-dev="' + CSS.escape(id) + '"]';

/** Drop ONE card (state already dropped the device) and with it a room
 *  section the removal emptied. */
export function removeCard(info, devId) {
  const sec = info && info.sec;
  const node = sec && $(roomSel(devId), sec);
  if (!node) return;
  const room = node.closest('.room');
  node.remove();
  if (room && !$('.card', room)) room.remove();
}

/** Place one card in ITS room's grid, creating the section when the room is
 *  new; without room data it appends to the flat grid. */
export function addCard(info, devId) {
  const dev = state.devices.get(devId);
  if (!dev || !dev._caps || !info || !info.sec) return;
  const sec = info.sec;
  const old = $(roomSel(devId), sec);
  if (old) {   // known id: a model refresh can change the card STRUCTURE
    old.replaceWith(cardNode(dev));   // (entities/caps), so always rebuild
    return;
  }
  // Respect the home filter: a device from another home has no card here.
  if (state.homeFilter !== 'all' && dev.home_id !== state.homeFilter) return;
  const groups = homeRooms();
  if (!groups) {
    const grid = $('#device-grid', sec);
    if (grid) grid.appendChild(cardNode(dev));
    return;
  }
  const key = dev.room_id ? `${dev.home_id}/${dev.room_id}` : 'none';
  const g = groups.find(x => x.key === key)
    || { key, id: dev.room_id || '', name: dev.room_name || t('room_none'),
      homeId: dev.home_id, sort: 0, devices: [dev], online: 0, total: 1,
      isNone: !dev.room_id };
  g.devices = [dev];
  g.total = 1;
  g.online = dev.online === false ? 0 : 1;
  g.homeName = '';
  let room = $(`.room[data-room="${CSS.escape(key)}"]`, sec);
  if (!room) {
    room = el(roomSection(g, groups.length, groups.length + 1));
    const list = $('.room-list', sec);
    if (list) list.appendChild(room);
    bindRoomHeads(room);
  }
  const grid = $('.room-grid', room);
  if (grid) grid.appendChild(cardNode(dev));
}

