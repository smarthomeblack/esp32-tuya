// WebSocket client: connection, heartbeat, exponential reconnect, 5 s
// poll fallback + the event router registry. Emits 'ws:conn'/'ws:event'.

import { state } from './state.js';
import { emit } from './events.js';
import { api } from './api.js';
import { $ } from './utils.js';

const wsHandlers = [];

// The banner debounce: a reconnect that lands inside this window is a blip,
// not an outage — 'ws:conn true' fires only once the link stays down.
const BANNER_DELAY_MS = 3000;
let bannerTimer = null;

function cancelBannerTimer() {
  if (bannerTimer) { clearTimeout(bannerTimer); bannerTimer = null; }
}

/** Register a WS event router (app.js registers the dispatcher). */
export function onWsEvent(handler) {
  wsHandlers.push(handler);
}

/** Replace every router (a shell rebuild owns exactly one dispatcher). */
export function setWsHandlers(handler) {
  wsHandlers.length = 0;
  wsHandlers.push(handler);
}

function dispatchWsEvent(event, data) {
  for (const h of wsHandlers) h(event, data);
  emit('ws:event', event, data);
}

export function wsConnect() {
  const w = state.ws;
  if (w.socket) {
    try { w.socket.onclose = null; w.socket.close(); } catch (e) { /* ignore */ }
    w.socket = null;
  }
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  // Browser WS cannot carry an Authorization header — the key rides the
  // query string (checked before the 101).
  const url = proto + '//' + location.host + '/ws?token=' +
    encodeURIComponent(state.apikey || '');
  let socket;
  try {
    socket = new WebSocket(url);
  } catch (e) {
    scheduleReconnect();
    return;
  }
  w.socket = socket;

  socket.onopen = () => {
    w.connected = true;
    w.attempts = 0;
    // v6 completes the handshake without invoking the handler — the
    // first frame registers this client.
    try { socket.send('{"type":"ping"}'); } catch (e) { /* ignore */ }
    startHeartbeat();
    stopPollFallback();
    cancelBannerTimer();      // the link is back: no banner, no pending show
    emit('ws:conn', false);   // appbar: setConnBanner(false)
  };
  socket.onmessage = ev => {
    let msg = null;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    if (msg && msg.event) dispatchWsEvent(msg.event, msg.data || {});
  };
  socket.onclose = () => {
    w.connected = false;
    w.socket = null;
    stopHeartbeat();
    // The outage banner trails by BANNER_DELAY_MS: a sub-second reconnect
    // (the hub's LRU socket purge, a wifi blip) never flashes it.
    cancelBannerTimer();
    bannerTimer = setTimeout(() => {
      bannerTimer = null;
      emit('ws:conn', true);   // appbar: setConnBanner(true)
    }, BANNER_DELAY_MS);
    scheduleReconnect();
    startPollFallback();
  };
  socket.onerror = () => { /* onclose follows */ };
}

function scheduleReconnect() {
  const w = state.ws;
  if (w.reconnectTimer) return;
  // Only the AUTO-retry pauses behind the gate (no 30 s churn while logged
  // out); the login success path calls wsConnect() itself (app.js).
  if ($('#gate-card')) return;
  // 1s -> 2s -> 4s ... capped at 30s, which stays the retry cadence.
  const delay = Math.min(1000 * Math.pow(2, Math.max(0, w.attempts)), 30000);
  w.attempts += 1;
  w.reconnectTimer = setTimeout(() => {
    w.reconnectTimer = null;
    wsConnect();
  }, delay);
}

function startHeartbeat() {
  stopHeartbeat();
  // 15 s beats the hub's 60 s stale window with room for one lost ping.
  state.ws.hbTimer = setInterval(() => {
    const s = state.ws.socket;
    if (s && s.readyState === 1) {
      try { s.send('{"type":"ping"}'); } catch (e) { /* onclose handles */ }
    }
  }, 15000);
}

function stopHeartbeat() {
  if (state.ws.hbTimer) { clearInterval(state.ws.hbTimer); state.ws.hbTimer = null; }
}

/** Polling fallback while the WS is down: /api/status every 5s. Skipped
 *  while the login gate is up — the key is being entered there; repeated
 *  401s would feed the per-IP lockout. */
function startPollFallback() {
  if (state.ws.pollTimer) return;
  state.ws.pollTimer = setInterval(async () => {
    if (state.ws.connected || $('#gate-card')) return;
    try {
      const data = await api('/api/status', { silent: true });
      if (data && !state.ws.connected) dispatchWsEvent('status', data);
    } catch (e) { /* hub unreachable — banner already shown */ }
  }, 5000);
}

function stopPollFallback() {
  if (state.ws.pollTimer) { clearInterval(state.ws.pollTimer); state.ws.pollTimer = null; }
}
