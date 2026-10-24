// The app's single mutable store. Property names are load-bearing (tests
// and i18n read state.dicts / dev._caps) — never rename them.
// window.state = QA hook (Playwright).
export const state = {
  lang: 'en',
  dicts: {},            // lang -> dictionary object
  screen: 'boot',       // 'boot' | 'onboarding' | 'app'
  tab: 'home',          // 'home' | 'scenes' | 'settings'
  status: null,         // /api/status snapshot (kept fresh by WS + poll)
  auth: { state: 'idle', user_code: '', qr_payload: '', username: '', error: '' },
  apikey: null,         // Bearer key — localStorage only, never from server
  homes: [],
  rooms: null,          // GET /api/rooms rows; null = not fetched yet (flat fallback)
  roomExpanded: new Map(), // 'home_id/room_id' -> bool (session-only collapse overrides)
  homeFilter: 'all',    // 'all' | home id
  devices: new Map(),   // id -> device (with cached _caps)
  devSeq: new Map(),    // id -> WS device_update counter (refreshData keep-old guard)
  changeDevice: null,   // devId whose model changed while a REST refresh runs
  scenes: [],
  loadingData: false,
  enteringApp: false,
  dataGen: 0,          // refreshData() generation — stale responses drop
  ws: {
    socket: null, connected: false, attempts: 0,
    hbTimer: null, reconnectTimer: null, pollTimer: null,
  },
  pending: new Map(),   // devId -> { timer, codes:Set, old:Map code->pre-cmd value }
  cmdErr: new Map(),    // devId -> the 2.5 s red-flash timer (survives a rebuild)
  colourFmt: new Map(), // devId -> wire format detected for the colour code
  onb: { step: 1, code: '', pollTimer: null, countTimer: null, countdown: 180, tuyaIot: 'opt',
         qrPainted: '' },
  // One scan renderer, two entry points (wizard = post-link, app = overlay).
  scan: {
    open: false,
    mode: null,         // 'wizard' | 'app'
    phase: 'idle',      // 'scanning' | 'list' | 'error'
    candidates: null,   // GET /api/discovery/candidates list
    sel: null,          // id -> { on: bool, ents: Set<code> }
    expanded: {},
    committing: false,
    tuyaIot: null,      // /api/openapi/status snapshot (hint line)
  },
  // Web logs: the /api/logs poll (settings tab only) + level filter.
  logs: { timer: null, gen: 0, filter: 'all', fetching: false, lines: [] },
  // Tuya IoT section: snapshot, flags, connect poll, live form values.
  tuyaIot: {
    data: null,         // last /api/openapi/status payload
    open: false,        // section expanded (collapsed by default)
    editing: false,     // "edit" pressed -> form visible even when connected
    submitting: false,  // save sent, polling for has_token
    pollTimer: null,
    form: null,         // lazily initialized by tuyaIotForm()
  },
  // Security section: {"initial"} badge + the live change-key form.
  security: {
    initial: null,      // null = not fetched yet (no badge)
    form: null,         // lazily initialized by secForm()
  },
  // Asset section: /api/asset/info snapshot + upload progress state.
  asset: {
    info: null,         // last /api/asset/info payload
    uploading: false,   // XHR in flight (progress bar guard)
  },
  // OTA section: /api/ota/info snapshot + upload progress state.
  ota: {
    info: null,         // last /api/ota/info payload
    uploading: false,   // XHR in flight (progress bar guard)
  },
};

// The Bearer key is a local secret (localStorage) — never fetched.
try { state.apikey = localStorage.getItem('hub_api_key') || null; } catch (e) { /* private mode */ }

window.state = state;
