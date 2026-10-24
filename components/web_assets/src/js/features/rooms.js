// Pure room grouping (no DOM): devices -> walk-through ordered room groups.
// Buckets are keyed home_id/room_id because room ids collide across homes.

// Walk-through ranks; the keyword tables are the only place places are named.
// Matching is whole-word on the diacritic-stripped name: "ngu" = bed, "ngo" = yard.
const TABLE = { kitchen: 10, living: 20, bed: 30, bath: 40, office: 50, garage: 60,
  entrance: 70, yard: 80, other: 90, none: 99 };
const ROOM_KEYWORDS = {
  kitchen: { rank: TABLE.kitchen, words: ['bep', 'kitchen', 'nau', 'cook'] },
  living: { rank: TABLE.living, words: ['khach', 'living', 'lounge', 'phong khach'] },
  bed: { rank: TABLE.bed, words: ['ngu', 'bedroom', 'bed', 'parent', 'master', 'guest'] },
  bath: { rank: TABLE.bath, words: ['tam', 'toilet', 'wc', 'bath', 'shower', 've sinh'] },
  office: { rank: TABLE.office, words: ['lam viec', 'office', 'study', 'work', 'desk'] },
  garage: { rank: TABLE.garage, words: ['gara', 'garage', 'xe', 'parking'] },
  entrance: { rank: TABLE.entrance, words: ['cua', 'entrance', 'hall', 'foyer', 'tien sanh'] },
  yard: { rank: TABLE.yard, words: ['ngo', 'san', 'yard', 'outdoor', 'balcony', 'garden', 'vuon'] },
};

/** NFKD -> drop combining marks -> lowercase -> collapse whitespace. */
function normName(s) {
  return String(s == null ? '' : s).normalize('NFKD').replace(/[\u0300-\u036f]/g, '')
    .replace(/đ/g, 'd').replace(/Đ/g, 'd').toLowerCase().replace(/\s+/g, ' ').trim();
}

const pad = n => String(n).padStart(12, '0');
function natKey(s) {
  return normName(s).replace(/\d+/g, m => pad(m.length) + m);
}

function rankOf(name, keywords) {
  const norm = normName(name);
  let best = TABLE.other;
  for (const key in keywords) {
    // Whole-word match on the leading words: a keyword may be a phrase
    // ("lam viec"), and `ngu` (bed) must never match `ngo` (yard).
    const hit = keywords[key].words.some(w => {
      if (!w.includes(' ')) return norm.split(' ').includes(w);
      return (' ' + norm + ' ').includes(' ' + w + ' ') || norm.startsWith(w + ' ');
    });
    if (hit && keywords[key].rank < best) best = keywords[key].rank;
  }
  return best;
}

// Order cache: key = visible rooms (home_id,id,name,sort) + lang + table size.
const orderCache = new Map();
function orderedIds(rooms, lang, keywords) {
  // Rooms are keyed by their resolved NAME and id: a room whose first device
  // (hence name) changed must not reuse a stale order.
  const key = rooms.map(r => `${r.id}\t${r.name}\t${r.sort}`).sort().join('\n')
    + `|${lang}|${Object.keys(keywords).length}`;
  const hit = orderCache.get(key);
  if (hit) return hit;
  const rank = new Map(rooms.map(r => [r.key, rankOf(r.name, keywords)]));
  const nat = new Map(rooms.map(r => [r.key, natKey(r.name)]));
  const out = rooms.map(r => r.key).sort((a, b) => (rank.get(a) - rank.get(b))
    || (nat.get(a) < nat.get(b) ? -1 : nat.get(a) > nat.get(b) ? 1 : 0));
  orderCache.clear();   // one entry is enough; rooms change rarely, WS ticks never
  orderCache.set(key, out);
  return out;
}

// Fallback label only: the renderer always passes t('room_none').
const label = (s, lang) => s || (lang === 'vi' ? 'Chưa gán phòng' : 'Unassigned');

/** Devices -> [{key,id,name,homeName,homeId,sort,devices,online,total,isNone}].
 *  Home filter first, then room buckets; empty rooms dropped, the unassigned
 *  bucket appended last only when non-empty. Keywords = the module table
 *  unless a caller overrides it. */
export function roomsFor(devices, homes, homeFilter, lang, keywords, roomRows) {
  const kw = keywords || ROOM_KEYWORDS;
  const homeOf = new Map((homes || []).map(h => [h.id, h]));
  // /api/rooms rows are the rename authority (a rename must reach the header
  // without a reload); homes[].rooms is the fallback table.
  const table = roomRows || (homes || []).flatMap(h =>
    (h.rooms || []).map(r => Object.assign({ home_id: h.id }, r)));
  const rowOf = new Map(table.map(r => [`${r.home_id}/${r.id}`, r]));
  const buckets = new Map();
  const none = { key: 'none', id: '', name: label('', lang), homeName: '', homeId: '', sort: 0,
    devices: [], online: 0, total: 0, isNone: true };
  let homeRuns = new Set();
  for (const d of devices || []) {
    if (homeFilter && homeFilter !== 'all' && d.home_id !== homeFilter) continue;
    const home = homeOf.get(d.home_id);
    if (homeFilter === 'all') homeRuns.add(d.home_id);
    const rid = d.room_id || '';
    if (!rid) { none.devices.push(d); none.total += 1; if (d.online !== false) none.online += 1; continue; }
    const key = `${d.home_id}/${rid}`;
    let g = buckets.get(key);
    if (!g) {
      g = { key, id: rid, name: label(rowOf.get(key) ? rowOf.get(key).name : '', lang),
        homeName: home ? home.name : '', homeId: d.home_id, sort: 0,
        devices: [d], online: d.online === false ? 0 : 1, total: 1, isNone: false };
      if (!rowOf.get(key) && d.room_name) g.name = d.room_name;
      buckets.set(key, g);
      continue;
    }
    g.devices.push(d);
    g.total += 1;
    if (d.online !== false) g.online += 1;
  }
  const list = Array.from(buckets.values());
  const order = orderedIds(list, lang || 'en', kw);
  const orderPos = new Map(order.map((roomKey, i) => [roomKey, i]));
  list.sort((a, b) => orderPos.get(a.key) - orderPos.get(b.key));
  // Multi-home view: home-by-home runs, the walk-through order inside each.
  const homesOrder = new Map((homes || []).map((h, i) => [h.id, i]));
  if (homeRuns.size > 1) {
    list.sort((a, b) => (homesOrder.get(a.homeId) ?? 99) - (homesOrder.get(b.homeId) ?? 99));
  }
  if (none.total) list.push(none);
  return list;
}
