"""Mock fixture data (REST shapes, exactly) — imported by mock_server.py."""

import json
import os

HOMES = [
    {"id": "h1001", "name": "Main home"},
    {"id": "h2002", "name": "Small apartment"},
]

# GET /api/rooms rows ([{id,name,sort,home_id,device_count}]); the queue order is
# deliberately NOT the walk-through order, so the UI's own sort is what QA sees.
ROOMS = [
    {"id": "r_bep", "name": "Bếp", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_khach", "name": "Phòng Khách", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_ngu1", "name": "Phòng Ngủ 1", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_ngu2", "name": "Phòng Ngủ 2", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_lamviec", "name": "Phòng Làm Việc", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_cua", "name": "Trước Cửa", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_ngo", "name": "Đèn Chiếu Ngõ", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_tam", "name": "Nhà Tắm", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_gara", "name": "Gara", "sort": 0, "home_id": "h1001", "device_count": 0},
    {"id": "r_khach2", "name": "Khách", "sort": 0, "home_id": "h2002", "device_count": 0},
]

# Device id -> room id ("" / absent = unassigned). Mirrors the firmware's
# sweep: every room-tagged device also appears in the room's device list.
DEVICE_ROOMS = {
    "light_rgb": "r_khach", "light_dim": "r_lamviec", "switch_kitchen": "r_bep",
    "sensor_bed": "r_ngu1", "climate_lr": "r_khach", "cover_lr": "r_khach2",
    "fan_tower": "r_khach2", "socket_2": "r_khach2", "ac_bed": "r_ngu1",
    "fan_stand": "r_ngu2", "humid_bed": "r_ngu2", "purifier_lr": "r_khach",
    "dehumid_lr": "r_khach", "vacuum_lr": "r_khach2", "mockfan_barecd": "",
    "mockcover_situ": "", "mockkt_emptymode": "",
}

# QA mode: MOCK_ROOMS_SCENARIO=<n> generates n rooms in h1001 (collapse
# policy QA needs 8+); devices are spread over the first rooms.
SCENARIO = os.environ.get("MOCK_ROOMS_SCENARIO", "").strip()
FALLBACK = os.environ.get("MOCK_ROOMS_FALLBACK") == "1"


def scenario_rooms(n: int) -> list:
    names = ["Bếp", "Phòng Khách", "Phòng Ngủ 1", "Phòng Ngủ 2", "Phòng Ngủ 3",
             "Phòng Làm Việc", "Nhà Tắm", "Trước Cửa", "Đèn Chiếu Ngõ"]
    ids = ["r_bep", "r_khach", "r_ngu1", "r_ngu2", "r_ngu3", "r_lamviec", "r_tam",
           "r_cua", "r_ngo"]
    while len(ids) < n:
        ids.append(f"r_extra{len(ids)}")
        names.append(f"Phòng {len(ids)}")
    rows = [{"id": "r_khach2", "name": "Khách", "sort": 0, "home_id": "h2002",
             "device_count": 0}]
    rows.extend({"id": ids[i], "name": names[i], "sort": 0, "home_id": "h1001",
                 "device_count": 0} for i in range(n))
    return rows


def rooms_with_counts(devices: dict) -> list:
    """The room rows for GET /api/rooms: scenario rooms (or ROOMS), counts
    recomputed from the live registry, empty when rooms are unavailable."""
    if FALLBACK:
        return []
    counts: dict = {}
    for dev in devices.values():
        rid = dev.get("room_id") or ""
        if rid:
            key = (dev.get("home_id", ""), rid)
            counts[key] = counts.get(key, 0) + 1
    rows = []
    for room in (scenario_rooms(int(SCENARIO)) if SCENARIO.isdigit() else ROOMS):
        room = dict(room)
        room["device_count"] = counts.get((room["home_id"], room["id"]), 0)
        rows.append(room)
    return rows


def seed_rooms(devices: dict) -> None:
    """Seed room ids ("" = unassigned). The scenario spreads h1001 devices
    over its rooms so every room owns a card; fallback mode strips them all."""
    for dev in devices.values():
        dev["room_id"] = DEVICE_ROOMS.get(dev["id"], "") if not FALLBACK else ""
    if not SCENARIO.isdigit():
        return
    rooms = [r["id"] for r in scenario_rooms(int(SCENARIO)) if r["home_id"] == "h1001"]
    ids = [d["id"] for d in INITIAL_DEVICES if d["home_id"] == "h1001"]
    for i, dev_id in enumerate(ids):
        if (i // 2) < len(rooms) and dev_id in devices:
            devices[dev_id]["room_id"] = rooms[i // 2]

INITIAL_DEVICES = [
    {
        "id": "light_rgb", "name": "Living room light", "category": "dj",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {
            "switch_led": True, "work_mode": "colour", "bright_value": 180,
            "temp_value": 128, "colour_data_hsv": "h:210,s:800,v:900",
        },
        "status_range": {
            "bright_value": {"type": "Integer", "values": json.dumps({"min": 25, "max": 255, "scale": 0, "step": 1})},
            "temp_value": {"type": "Integer", "values": json.dumps({"min": 0, "max": 255, "scale": 0, "step": 1})},
            "work_mode": {"type": "Enum", "values": json.dumps({"range": ["white", "colour", "scene", "music"]})},
            "switch_led": {"type": "Boolean", "values": json.dumps({})},
            "colour_data_hsv": {"type": "Json", "values": json.dumps({})},
        },
        # Writable codes (the firmware's function map drives "writable").
        "function": ["switch_led", "work_mode", "bright_value", "temp_value", "colour_data_hsv"],
    },
    {
        "id": "light_dim", "name": "Desk light", "category": "xdd",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch_led": True, "bright_value": 640},
        "status_range": {
            "bright_value": {"type": "Integer", "values": json.dumps({"min": 10, "max": 1000, "scale": 0, "step": 1})},
            "switch_led": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch_led", "bright_value"],
    },
    {
        "id": "switch_kitchen", "name": "Kitchen switch", "category": "kg",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch": False},
        "status_range": {
            "switch": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch"],
    },
    {
        "id": "sensor_bed", "name": "Bedroom sensor", "category": "wsdcg",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"temp_current": 273, "humidity": 62, "battery": 88},
        "status_range": {
            "temp_current": {"type": "Integer", "values": json.dumps({"min": 0, "max": 600, "scale": 1, "step": 1})},
            "humidity": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
        },
        # Read-only sensor: no writable codes (writable=false everywhere).
        "function": [],
    },
    {
        # NOTE: carries a power `switch` DP — must classify as CLIMATE, not SWITCH.
        "id": "climate_lr", "name": "Living room AC", "category": "kt",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch": True, "temp_set": 240, "temp_current": 272, "mode": "cold"},
        "status_range": {
            "temp_set": {"type": "Integer", "values": json.dumps({"min": 160, "max": 300, "scale": 1, "step": 5})},
            "temp_current": {"type": "Integer", "values": json.dumps({"min": 0, "max": 600, "scale": 1, "step": 1})},
            "mode": {"type": "Enum", "values": json.dumps({"range": ["cold", "heat", "dry", "fan", "auto"]})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "temp_set", "mode"],
    },
    {
        "id": "cover_lr", "name": "Living room curtains", "category": "ck",
        "online": True, "support_local": True, "home_id": "h2002",
        "status": {"percent_control": 40, "control": "open", "situation_set": "0xff00"},
        "status_range": {
            "percent_control": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
            "control": {"type": "Enum", "values": json.dumps({"range": ["open", "stop", "close"]})},
        },
        "function": ["percent_control", "control"],
    },
    {
        "id": "fan_tower", "name": "Tower fan", "category": "fs",
        "online": False, "support_local": True, "home_id": "h2002",
        "status": {"switch_fan": False, "fan_speed_enum": "2"},
        "status_range": {
            "fan_speed_enum": {"type": "Enum", "values": json.dumps({"range": ["1", "2", "3"]})},
            "switch_fan": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch_fan", "fan_speed_enum"],
    },
    {
        "id": "socket_2", "name": "Double socket", "category": "cz",
        "online": True, "support_local": True, "home_id": "h2002",
        "status": {"switch_1": True, "switch_2": False, "countdown_1": 0, "countdown_2": 30},
        "status_range": {
            "countdown_1": {"type": "Integer", "values": json.dumps({"min": 0, "max": 86400, "scale": 0, "step": 1})},
            "countdown_2": {"type": "Integer", "values": json.dumps({"min": 0, "max": 86400, "scale": 0, "step": 1})},
            "switch_1": {"type": "Boolean", "values": json.dumps({})},
            "switch_2": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch_1", "switch_2", "countdown_1", "countdown_2"],
    },
    # ---- mock devices (kt/fs/jsq/kj/cs + sd) ----
    {
        "id": "ac_bed", "name": "Bedroom AC", "category": "kt",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {
            "switch": True, "mode": "cold", "temp_set": 24, "temp_current": 272,
            "fan_speed_enum": "middle", "work_status": "cooling",
            "switch_horizontal": False, "c_f": False,
        },
        "status_range": {
            "temp_set": {"type": "Integer", "values": json.dumps({"min": 16, "max": 30, "scale": 0, "step": 1})},
            "temp_current": {"type": "Integer", "values": json.dumps({"min": 0, "max": 600, "scale": 1, "step": 1})},
            "mode": {"type": "Enum", "values": json.dumps({"range": ["cold", "hot", "wind", "auto"]})},
            "fan_speed_enum": {"type": "Enum", "values": json.dumps({"range": ["low", "middle", "high", "auto"]})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
            "switch_horizontal": {"type": "Boolean", "values": json.dumps({})},
            "c_f": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "mode", "temp_set", "fan_speed_enum",
                     "switch_horizontal", "c_f"],
    },
    {
        "id": "fan_stand", "name": "Bedroom stand fan", "category": "fs",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {
            "switch_fan": True, "fan_speed_enum": "2", "fan_speed_percent": 40,
            "switch_horizontal": False,
        },
        "status_range": {
            "fan_speed_enum": {"type": "Enum", "values": json.dumps({"range": ["1", "2", "3", "4"]})},
            "fan_speed_percent": {"type": "Integer", "values": json.dumps({"min": 1, "max": 100, "scale": 0, "step": 1})},
            "switch_fan": {"type": "Boolean", "values": json.dumps({})},
            "switch_horizontal": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch_fan", "fan_speed_enum", "fan_speed_percent",
                     "switch_horizontal"],
    },
    {
        "id": "humid_bed", "name": "Bedroom humidifier", "category": "jsq",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch": True, "humidity_set": 55, "humidity_current": 62, "spray_mode": "level_2"},
        "status_range": {
            "humidity_set": {"type": "Integer", "values": json.dumps({"min": 30, "max": 80, "scale": 0, "step": 5})},
            "humidity_current": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
            "spray_mode": {"type": "Enum", "values": json.dumps({"range": ["level_1", "level_2", "level_3"]})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "humidity_set", "spray_mode"],
    },
    {
        "id": "purifier_lr", "name": "Living room air purifier", "category": "kj",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch": True, "pm25": 42, "filter": 76, "anion": True, "lock": False,
                   "filter_reset": False},
        "status_range": {
            "pm25": {"type": "Integer", "values": json.dumps({"min": 0, "max": 999, "scale": 0, "step": 1})},
            "filter": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
            "filter_reset": {"type": "Boolean", "values": json.dumps({})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
            "anion": {"type": "Boolean", "values": json.dumps({})},
            "lock": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "anion", "lock", "filter_reset"],
    },
    {
        "id": "dehumid_lr", "name": "Living room dehumidifier", "category": "cs",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {"switch": True, "humidity_indoor": 71, "dehumidify_set_value": 55, "fan_speed_enum": "low"},
        "status_range": {
            "humidity_indoor": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
            "dehumidify_set_value": {"type": "Integer", "values": json.dumps({"min": 30, "max": 70, "scale": 0, "step": 5})},
            "fan_speed_enum": {"type": "Enum", "values": json.dumps({"range": ["sleep", "low", "high"]})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "dehumidify_set_value", "fan_speed_enum"],
    },
    {
        "id": "vacuum_lr", "name": "Robot vacuum", "category": "sd",
        "online": True, "support_local": True, "home_id": "h1001",
        "status": {
            "status": "cleaning", "power_go": True, "mode": "smart", "seek": False,
            "suction": "normal", "battery": 78, "clean_area": 3250, "clean_time": 4520,
        },
        "status_range": {
            "status": {"type": "Enum", "values": json.dumps({"range": ["standby", "cleaning", "paused", "charging", "chargecompleted"]})},
            "mode": {"type": "Enum", "values": json.dumps({"range": ["standby", "random", "smart", "chargego"]})},
            "suction": {"type": "Enum", "values": json.dumps({"range": ["low", "normal", "strong"]})},
            "battery": {"type": "Integer", "values": json.dumps({"min": 0, "max": 100, "scale": 0, "step": 1})},
            "clean_area": {"type": "Integer", "values": json.dumps({"min": 0, "max": 999999, "scale": 1, "step": 1})},
            "clean_time": {"type": "Integer", "values": json.dumps({"min": 0, "max": 99999, "scale": 0, "step": 1})},
            "power_go": {"type": "Boolean", "values": json.dumps({})},
            "seek": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["power_go", "mode", "seek", "suction"],
    },
    {
        # QA: bare "countdown" code (no _N suffix) with named specs — the
        # countdown family render + unit handling. Fixture-only devices are
        # "mock"-prefixed and live in a home the hub does NOT list, so a
        # link auto-discovery commit never sees them (fixtures stay stable).
        # unit "ms" + 12000: the ms branch must read 12 s, not "200 phút".
        "id": "mockfan_barecd", "name": "Test fan", "category": "fs",
        "online": True, "support_local": True, "home_id": "h9999",
        "status": {"switch": False, "fan_speed_enum": "normal",
                   "fan_direction": "forward", "countdown": 12000},
        "status_range": {
            "switch": {"type": "Boolean", "values": json.dumps({}), "name": "Power"},
            "fan_speed_enum": {"type": "Enum", "values": json.dumps({"range": ["nature", "sleep", "normal"]}), "name": "Fan speed"},
            "countdown": {"type": "Integer", "values": json.dumps({"unit": "ms", "min": 0, "max": 86400000, "scale": 0, "step": 1000}), "name": "Timer"},
            "fan_direction": {"type": "Enum", "values": json.dumps({"range": ["forward", "reverse"]}), "name": "Fan direction"},
        },
        "function": ["switch", "fan_speed_enum", "countdown", "fan_direction"],
    },
    {
        # QA: cl curtain whose only DP is situation_set — no cover section
        # mounts, so the code must render as a floor row (never invisible).
        "id": "mockcover_situ", "name": "Test curtain", "category": "cl",
        "online": True, "support_local": True, "home_id": "h9999",
        "status": {"situation_set": "morning"},
        "status_range": {
            "situation_set": {"type": "Enum",
                              "values": json.dumps({"range": ["morning", "night", "away"]}),
                              "name": "Scene"},
        },
        "function": ["situation_set"],
    },
    {
        # QA: mode declared with an EMPTY values range — no enum select
        # mounts, so the code must fall to the floor as a raw value row.
        "id": "mockkt_emptymode", "name": "Test AC", "category": "kt",
        "online": True, "support_local": True, "home_id": "h9999",
        "status": {"switch": True, "mode": "cold", "temp_set": 24},
        "status_range": {
            "temp_set": {"type": "Integer", "values": json.dumps({"min": 16, "max": 30, "scale": 0, "step": 1})},
            "mode": {"type": "Enum", "values": json.dumps({"range": []})},
            "switch": {"type": "Boolean", "values": json.dumps({})},
        },
        "function": ["switch", "temp_set", "mode"],
    },
]

SCENES = [
    {"id": "sc_light", "name": "Entertainment lights", "enabled": True, "home_id": "h1001"},
    {"id": "sc_home", "name": "Come home", "enabled": True, "home_id": "h1001"},
    {"id": "sc_away", "name": "Away", "enabled": True, "home_id": "h1001"},
    {"id": "sc_movie", "name": "Movie time", "enabled": False, "home_id": "h2002"},
]

# Repair-flow QA fixture: the seeded REGISTRY copy of this device keeps only
# these entity codes (a degraded commit with a partial model); its staged
# candidate stays the full model, so re-committing it triggers the refresh
# (the one refresh-shaped commit on a freshly seeded registry).
REGISTRY_TRIMMED = {"light_rgb": ["switch_led", "bright_value"]}
