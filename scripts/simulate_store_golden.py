#!/usr/bin/env python3
"""Pre-build serializer simulation: rebuilds the golden store JSON with the
EXACT device_store.cc rules (field order, sorted maps/sets, cJSON number
printing), byte-exact against device_store_test.cc literals."""
import json
import re
import sys

SRC = r"components/hub_core/src/device_store_test.cc"
text = open(SRC, encoding="utf-8").read()


def extract(name: str) -> str:
    pattern = name + r"\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\"\s*)+);"
    m = re.search(pattern, text)
    if not m:
        sys.exit(f"constant {name} not found")
    raw = m.group(1)
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
    joined = "".join(parts)
    joined = joined.replace("\\\\", "\x00").replace('\\"', '"').replace("\x00", "\\")
    return joined


def jstr(s: str) -> str:
    return json.dumps(s, ensure_ascii=False)


def num(d: float) -> str:
    # cJSON print_number: d == (double)(int)d -> "%d"; else "%1.15g".
    if d == int(d):
        return str(int(d))
    return repr(d)  # %1.15g of e.g. 2.5 == "2.5"


def dp(value, tag):
    if tag == "b":
        return "[" + jstr("b") + "," + ("true" if value else "false") + "]"
    if tag == "i" or tag == "d":
        return "[" + jstr(tag) + "," + num(float(value)) + "]"
    return "[" + jstr("s") + "," + jstr(value) + "]"


def device_json(d) -> str:
    out = []
    for k in ("id", "name", "category", "product_id", "product_name",
              "model", "uuid", "asset_id", "icon", "time_zone"):
        out.append(jstr(k) + ":" + jstr(d[k]))
    out.append(jstr("sub") + ":" + ("true" if d["sub"] else "false"))
    out.append(jstr("online") + ":" + ("true" if d["online"] else "false"))
    for k in ("active_time", "create_time", "update_time"):
        out.append(jstr(k) + ":" + num(float(d[k])))
    out.append(jstr("home_id") + ":" + jstr(d["home_id"]))
    out.append(jstr("room_id") + ":" + jstr(d["room_id"]))
    out.append(jstr("support_local") + ":" + ("true" if d["support_local"] else "false"))
    # status: std::map key order
    status = ",".join(jstr(c) + ":" + dp(v, t) for c, (v, t) in sorted(d["status"].items()))
    out.append(jstr("status") + ":{" + status + "}")
    # function
    funcs = []
    for code, fn in sorted(d["function"].items()):
        body = ",".join(jstr(k) + ":" + jstr(fn[k]) for k in ("name", "desc", "type", "values"))
        funcs.append(jstr(code) + ":{" + body + "}")
    out.append(jstr("function") + ":{" + ",".join(funcs) + "}")
    # status_range
    ranges = []
    for code, rng in sorted(d["status_range"].items()):
        body = ",".join(jstr(k) + ":" + jstr(rng[k])
                        for k in ("name", "type", "values", "report_type"))
        ranges.append(jstr(code) + ":{" + body + "}")
    out.append(jstr("status_range") + ":{" + ",".join(ranges) + "}")
    # local_strategy: map<int> numeric order, dpId as string key
    strat = []
    for dp_id, cfg in sorted(d["local_strategy"].items()):
        body = []
        body.append(jstr("support_local") + ":" + ("true" if cfg["support_local"] else "false"))
        for k in ("value_convert", "status_code", "status_format_json", "value_type",
                  "value_desc_json"):
            body.append(jstr(k) + ":" + jstr(cfg[k]))
        emap = ",".join(jstr(k) + ":" + jstr(v) for k, v in sorted(cfg["enum_mapping"].items()))
        body.append(jstr("enum_mapping") + ":{" + emap + "}")
        body.append(jstr("use_open_api") + ":" + ("true" if cfg["use_open_api"] else "false"))
        body.append(jstr("property_update") + ":" +
                    ("true" if cfg["property_update"] else "false"))
        strat.append(jstr(str(dp_id)) + ":{" + ",".join(body) + "}")
    out.append(jstr("local_strategy") + ":{" + ",".join(strat) + "}")
    # disabled_entities: std::set order
    out.append(jstr("disabled_entities") + ":[" +
               ",".join(jstr(c) for c in sorted(d["disabled_entities"])) + "]")
    return "{" + ",".join(out) + "}"


def home_json(h) -> str:
    body = jstr("id") + ":" + jstr(h["id"]) + "," + jstr("name") + ":" + jstr(h["name"])
    # Optional key: written only when the rooms were fetched.
    if h.get("rooms"):
        rooms = ",".join(
            "{" + jstr("id") + ":" + jstr(r["id"]) + "," + jstr("name") + ":" + jstr(r["name"]) +
            "," + jstr("sort") + ":" + num(float(r["sort"])) + "}"
            for r in h["rooms"])
        body += "," + jstr("rooms") + ":[" + rooms + "]"
    return "{" + body + "}"


def store_json(homes, devices, uid, saved_at) -> str:
    out = [jstr("version") + ":" + num(1.0), jstr("uid") + ":" + jstr(uid),
           jstr("saved_at") + ":" + num(float(saved_at))]
    homes_body = ",".join(home_json(h) for h in homes)
    out.append(jstr("homes") + ":[" + homes_body + "]")
    out.append(jstr("devices") + ":[" + ",".join(device_json(d) for d in devices) + "]")
    return "{" + ",".join(out) + "}"


GOLDEN_DEVICE = {
    "id": "6cd4a1f9c9abcf3d", "name": "Kitchen Meter",
    "category": "cz", "product_id": "keyju9kk", "product_name": "Kitchen Meter Double",
    "model": "TS0111", "uuid": "7c25f2e5e0e9d5f2", "asset_id": "8796093022204",
    "icon": "smart/bn_icon_ci9j.png", "time_zone": "+07:00",
    "sub": True, "online": True, "active_time": 1234567890, "create_time": 1234567890,
    "update_time": 1234567890, "home_id": "123456789012", "support_local": True,
    "room_id": "178870394",
    "status": {
        "bright_value": (500, "i"),
        "colour_data": ("h:210,s:800,v:900", "s"),
        "cur_current": (2.5, "d"),
        "fault": ("", "s"),
        "switch_1": (True, "b"),
    },
    "function": {
        "bright_value": {"name": "Brightness", "desc": "Brightness level", "type": "Integer",
                          "values": "{\"min\":10,\"max\":1000,\"scale\":0,\"step\":1}"},
    },
    "status_range": {
        "cur_power": {"name": "Power", "type": "Integer",
                       "values": "{\"min\":0,\"max\":5000,\"scale\":1,\"step\":1}",
                       "report_type": "minux"},
    },
    "local_strategy": {
        20: {"support_local": True, "value_convert": "default", "status_code": "switch_1",
              "status_format_json": "{\"switch_1\":\"\"}", "value_type": "Boolean",
              "value_desc_json": "{}",
              "enum_mapping": {"0": "close", "1": "open"},
              "use_open_api": True, "property_update": True},
    },
    "disabled_entities": {"countdown_1", "add_ele"},
}

SOCKET_DEVICE = {
    "id": "6cd4a1f9d0e1f2a3", "name": "Kitchen Socket",
    "category": "kg", "product_id": "keyju9kk", "product_name": "Socket S1",
    "model": "", "uuid": "", "asset_id": "", "icon": "", "time_zone": "",
    "sub": False, "online": False, "active_time": 1000000000, "create_time": 0,
    "update_time": 0, "home_id": "987654321098", "support_local": False,
    "room_id": "",
    "status": {"switch_1": (False, "b")},
    "function": {}, "status_range": {}, "local_strategy": {}, "disabled_entities": set(),
}

HOMES = [{"id": "123456789012", "name": "Home A",
          "rooms": [{"id": "178870394", "name": "Phòng Làm Việc", "sort": 1}]},
         {"id": "987654321098", "name": "Home B"}]

failures = 0

# 1. golden device alone
sim = device_json(GOLDEN_DEVICE)
golden = extract("kGoldenDeviceJson")
if sim != golden:
    failures += 1
    for i, (a, b) in enumerate(zip(sim, golden)):
        if a != b:
            print(f"DEVICE MISMATCH at {i}:\n  sim:    {sim[max(0,i-30):i+50]!r}\n  golden: {golden[max(0,i-30):i+50]!r}")
            break
    else:
        print(f"DEVICE length differs: sim={len(sim)} golden={len(golden)}")
else:
    print(f"device golden: OK ({len(sim)} bytes)")

# 2. golden store (1 home + golden device)
sim = store_json(HOMES[:1], [GOLDEN_DEVICE], "az1757064314079LqZel", 1234567890)
golden = extract("kGoldenStoreJson")
if sim != golden:
    failures += 1
    for i, (a, b) in enumerate(zip(sim, golden)):
        if a != b:
            print(f"STORE MISMATCH at {i}:\n  sim:    {sim[max(0,i-30):i+50]!r}\n  golden: {golden[max(0,i-30):i+50]!r}")
            break
    else:
        print(f"STORE length differs: sim={len(sim)} golden={len(golden)}")
else:
    print(f"store golden: OK ({len(sim)} bytes)")

# 3. two-device snapshot round-trip stability (python-side self check)
s1 = store_json(HOMES, [GOLDEN_DEVICE, SOCKET_DEVICE], "az1757064314079LqZel", 1234567890)
json.loads(s1)  # validation only: raises if s1 is not valid JSON
print(f"snapshot: {len(s1)} bytes, parse-ok=True")

sys.exit(1 if failures else 0)
