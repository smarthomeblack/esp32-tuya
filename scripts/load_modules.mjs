// Repo/CI Node loader: loads the web_assets ES module graph with browser
// shims and asserts export surfaces — catches unbound imports / cycle TDZ
// errors that `node --check` cannot. Run: node scripts/load_modules.mjs

// --- browser shims (before any import) ---
const store = new Map();
globalThis.window = globalThis;
globalThis.localStorage = {
  getItem: k => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
  removeItem: k => store.delete(k),
};
globalThis.document = {
  createElement: () => ({ innerHTML: '', firstElementChild: null }),
  documentElement: { lang: 'en' },
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener: () => {},
};
globalThis.fetch = async () => { throw new Error('offline'); };

const base = '../components/web_assets/src/js';
let failures = 0;
function step(name, fn) {
  try {
    const r = fn();
    console.log(`OK   ${name}${r ? ' -> ' + r : ''}`);
  } catch (e) {
    failures += 1;
    console.log(`FAIL ${name}: ${e && e.stack ? e.stack.split('\n').slice(0, 2).join(' | ') : e}`);
  }
}

// 1) Load the whole graph from multiple entry points — module evaluation
//    through the cyclic imports must not throw (no TDZ / unbound refs).
const cards = await import(`${base}/features/cards.js`);
const registry = await import(`${base}/features/cards-registry.js`);
const caps = await import(`${base}/features/cards-caps.js`);
const commands = await import(`${base}/features/cards-commands.js`);
const refine = await import(`${base}/features/cards-refine.js`);
const onboarding = await import(`${base}/features/onboarding.js`);
const tuyaIot = await import(`${base}/features/tuya-iot.js`);
const rooms = await import(`${base}/features/rooms.js`);
console.log('OK   module graph loaded (cards.js, registry, caps, refine, commands, onboarding, tuya-iot, rooms)');

// 2) Export surfaces: every moved symbol lives in exactly one module.
const expect = (mod, name, want) => {
  const got = Object.keys(mod).sort().join(',');
  if (got !== want) throw new Error(`unexpected exports: ${got}`);
  return got;
};
step('cards.js exports', () => expect(cards, 'cards', 'bindSlider,buildCard'));
step('cards-registry.js exports', () => expect(registry, 'registry',
  'beginCard,finishCard,registerUpdater,syncCardState,updateCardInPlace'));
step('cards-caps.js exports', () => expect(caps, 'caps',
  'detectCapabilities,deviceKeys,dpWritable,enumRange,numRange,rangeInfo,scaleVal'));
step('cards-refine.js exports', () => expect(refine, 'refine',
  'ACT_CLASS,activityBadge,activityKey,refineByCategory'));
step('cards-commands.js exports', () => expect(commands, 'commands',
  'clearPending,sendCommands'));
step('onboarding.js exports', () => expect(onboarding, 'onboarding',
  'paintQrRound,renderOnboarding,startPostLinkWizard,stopOnboardingTimers'));
step('tuya-iot.js exports', () => expect(tuyaIot, 'tuyaIot', [
  'bindTuyaIotForm', 'bindTuyaIotSection', 'refreshTuyaIotStatus',
  'saveTuyaIotCredentials', 'stopTuyaIotPoll', 'tuyaIotFieldsHtml', 'tuyaIotSectionHtml',
].sort().join(',')));
step('rooms.js exports', () => expect(rooms, 'rooms', 'roomsFor'));

// 3) Runtime exercise of the caps <-> refine cycle (call-time safety):
//    detectCapabilities -> refineByCategory -> enumRange back into caps.
const fan = {
  id: 'fan', category: 'fs', online: true,
  status: { switch_fan: true, fan_speed_enum: '2', fan_speed_percent: 40 },
  status_range: {
    fan_speed_enum: { type: 'Enum', values: JSON.stringify({ range: ['1', '2', '3'] }) },
    fan_speed_percent: { type: 'Integer', values: JSON.stringify({ min: 0, max: 100, step: 1, scale: 0 }) },
  },
};
const kt = {
  id: 'ac', category: 'kt', online: true,
  status: { switch: true, temp_set: 250, work_status: 'cooling', fan_speed_enum: '1' },
  status_range: {
    temp_set: { type: 'Integer', values: JSON.stringify({ min: 160, max: 300, step: 5, scale: 0 }) },
    fan_speed_enum: { type: 'Enum', values: JSON.stringify({ range: ['1', '2', '3'] }) },
  },
};
step('detectCapabilities (runtime, cycles) fan', () => {
  const c = caps.detectCapabilities(fan);
  if (c.speedCode !== 'fan_speed_enum' || c.speedValues.join(',') !== '1,2,3') {
    throw new Error('fan caps wrong: ' + JSON.stringify(c.speedValues));
  }
  if (c.pctCode !== 'fan_speed_percent') throw new Error('pctCode not detected');
  return `speed=[${c.speedValues.join(',')}] pct=${c.pctCode}`;
});
step('detectCapabilities (runtime, cycles) kt + activityKey', () => {
  const c = caps.detectCapabilities(kt);
  if (c.workStatus !== 'work_status') throw new Error('kt work_status not detected');
  const key = refine.activityKey(kt, c);
  if (key !== 'act_cooling') throw new Error('activityKey failed: ' + key);
  const badge = refine.activityBadge(kt, c);
  if (!badge.includes('act-badge') || !badge.includes('cooling')) throw new Error('badge html failed');
  return `activity=${key}`;
});
step('genericEntry via detectCapabilities (unclaimed floor rows)', () => {
  const dev = {
    id: 'x', category: 'kg', online: true,
    status: { switch: true, relay_status: 'relay_1' },
    status_range: {
      relay_status: { type: 'Enum', values: JSON.stringify({ range: ['relay_0', 'relay_1', 'relay_2'] }), name: 'Relay status', writable: false },
    },
  };
  const c = caps.detectCapabilities(dev);
  const row = c.generic.find(g => g.code === 'relay_status');
  if (!row || row.type !== 'Enum' || row.name !== 'Relay status' || row.writable !== false) {
    throw new Error('floor entry wrong: ' + JSON.stringify(row));
  }
  return `floor row: ${row.code} Enum "${row.name}"`;
});
step('numRange / scaleVal / dpWritable / enumRange sanity', () => {
  const r = caps.numRange(kt, 'temp_set', { min: 0, max: 100, step: 1 });
  if (r.min !== 160 || r.max !== 300 || r.step !== 5) throw new Error('numRange wrong: ' + JSON.stringify(r));
  if (caps.scaleVal(250, 0) !== 250) throw new Error('scaleVal wrong');
  if (caps.dpWritable(kt, 'temp_set') !== false) throw new Error('dpWritable should be false (absent)');
  if (caps.enumRange(fan, 'fan_speed_enum').length !== 3) throw new Error('enumRange wrong');
  return 'ok';
});

step('roomsFor (runtime): walk-through order + ngủ/ngõ hazard', () => {
  const homes = [{ id: 'h1', name: 'H', rooms: [
    { id: 'r1', name: 'Phòng Khách', sort: 0 }, { id: 'r2', name: 'Phòng Ngủ T2', sort: 0 },
    { id: 'r3', name: 'Bếp', sort: 0 }, { id: 'r4', name: 'Đèn Chiếu Ngõ', sort: 0 },
    { id: 'r5', name: 'Phòng làm việc 2', sort: 0 }, { id: 'r6', name: 'Phòng làm việc 1', sort: 0 },
    { id: 'r9', name: 'Phòng trống', sort: 0 },
  ] }];
  // The device row carries room_name (the firmware resolves it); homes.rooms
  // is only the /api/rooms table, never the grouping source.
  const roomNames = { r1: 'Phòng Khách', r2: 'Phòng Ngủ T2', r3: 'Bếp',
    r4: 'Đèn Chiếu Ngõ', r5: 'Phòng làm việc 2', r6: 'Phòng làm việc 1' };
  const dev = (id, room, home = 'h1') => ({ id, name: id, home_id: home, room_id: room,
    room_name: roomNames[room] || '', online: true });
  const out = rooms.roomsFor([
    dev('a', 'r1'), dev('b', 'r2'), dev('c', 'r3'), dev('d', 'r4'),
    dev('e', 'r5'), dev('f', 'r6'), dev('g', ''),
  ], homes, 'all', 'vi');
  const names = out.map(g => g.name).join('|');
  const want = 'Bếp|Phòng Khách|Phòng Ngủ T2|Phòng làm việc 1|Phòng làm việc 2|Đèn Chiếu Ngõ|Chưa gán phòng';
  if (names !== want) throw new Error('order: ' + names);
  if (out.some(g => g.id === 'r9')) throw new Error('empty room listed');
  const none = out[out.length - 1];
  if (!none.isNone || none.total !== 1) throw new Error('unassigned bucket wrong');
  if (rooms.roomsFor([dev('a', 'r1'), dev('b', 'r9')], homes, 'h2', 'en').length !== 0) {
    throw new Error('home filter ignored');
  }
  return names;
});

console.log(failures ? `MODULE LOAD AUDIT: FAIL (${failures})` : 'MODULE LOAD AUDIT: PASS');
process.exit(failures ? 1 : 0);
