#!/usr/bin/env python3
"""Mock server — QA harness for the Web Control UI: serves the BUILT SPA
(dist/web_assets.bin) plus a REST + WebSocket mock of the API contract
(auth: Bearer /api, ?token= /ws). Run: python scripts/mock_server.py"""

from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import socket
import struct
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import web_asset_pack
from mock_data import (HOMES, INITIAL_DEVICES, REGISTRY_TRIMMED, SCENES, ROOMS,
                       rooms_with_counts, seed_rooms)

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DIST_DIR = PROJECT_ROOT / "components" / "web_assets" / "dist"

HOST = "127.0.0.1"
PORT = int(os.environ.get("MOCK_PORT", "8787"))

# Device ids are [A-Za-z0-9-] (rest_api_internal.h kMaxIdLength); the mock
# rejects anything else so QA cannot stumble onto a firmware-only 404.
kMaxIdLength = 64


def parse_id_path(path: str, prefix: str, suffix: str) -> str:
    """ParseIdPath mirror (rest_api_marshal.cc): percent-decoded segment
    between prefix and suffix; "" when the path does not match."""
    if not path.startswith(prefix) or not path.endswith(suffix):
        return ""
    if len(path) <= len(prefix) + len(suffix):
        return ""
    dev_id = unquote(path[len(prefix):len(path) - len(suffix)])
    if not dev_id or len(dev_id) > kMaxIdLength or "/" in dev_id:
        return ""
    return dev_id


# QA knobs: MOCK_LINK_DELAY stretches waiting_scan->linked (default 8s);
# MOCK_PORT moves the listener (default 8787).
LINK_DELAY = float(os.environ.get("MOCK_LINK_DELAY", "8"))
QR_DELAY = float(os.environ.get("MOCK_QR_DELAY", "1.2"))
# QA: MOCK_QR_REFRESH=1 rotates the QR token twice (~2 s apart) before linking.
QR_REFRESH = os.environ.get("MOCK_QR_REFRESH") == "1"
# OpenAPI mock: seconds between "credentials saved" and "login completed".
# login_in_flight stays True for the whole wait, so the UI's 2 s failure
# grace never trips (MOCK_OPENAPI_TOKEN_DELAY=4 to watch the connecting poll).
OPENAPI_TOKEN_DELAY = float(os.environ.get("MOCK_OPENAPI_TOKEN_DELAY", "1.5"))
# Candidate detail fetch: the simulated 3-call cloud round-trip.
DETAIL_DELAY = float(os.environ.get("MOCK_DETAIL_DELAY", "0.6"))
# API key: every /api call needs Bearer, /ws needs ?token (default "12345678").
MOCK_API_KEY = os.environ.get("MOCK_API_KEY", "12345678")
# QA: MOCK_FAIL_COMMANDS=1 makes device commands answer 502 command_failed (not applied).
FAIL_COMMANDS = os.environ.get("MOCK_FAIL_COMMANDS") == "1"
# QA: MOCK_PRESENCE=1 flips every device's online flag every ~6 s (WS
# device_online). MOCK_PRESENCE_ROUNDS caps the flips (default: forever).
PRESENCE_DRIVER = os.environ.get("MOCK_PRESENCE") == "1"
PRESENCE_ROUNDS = int(os.environ.get("MOCK_PRESENCE_ROUNDS", "0") or 0)

START_TIME = time.time()


def registry_seed(dev: dict) -> dict:
    """Registry seed copy; REGISTRY_TRIMMED degrades one device's entity
    set (repair-flow QA: fewer codes than its staged candidate)."""
    out = json.loads(json.dumps(dev))
    keep = REGISTRY_TRIMMED.get(dev["id"])
    if not keep:
        return out
    allowed = set(keep)
    out["status"] = {k: v for k, v in out.get("status", {}).items() if k in allowed}
    out["status_range"] = {k: v for k, v in out.get("status_range", {}).items()
                           if k in allowed}
    out["function"] = [c for c in out.get("function", []) if c in allowed]
    return out


# --- Mutable mock state (guarded by LOCK) ---

# DiscoveryProgress{} mirror: identical keys on every path, so the status and
# progress builders never hit a missing field.
EMPTY_DISCOVERY = {"active": False, "percent": 100, "done": False, "failed": False,
                   "homes_done": 0, "homes_total": 0, "devices_done": 0, "devices_total": 0,
                   "error": ""}

LOCK = threading.RLock()

# Bumped on every credentials save / clear: a stale login task must not
# publish its result (the firmware cancels the previous login task).
OPENAPI_LOGIN_GEN = [0]

state = {
    "auth": {"state": "idle", "user_code": "", "qr_payload": "", "username": "", "error": ""},
    "lang": "en",
    # QA: /api/qa/hub-down sets this; WS upgrades answer 503 until it passes.
    "hub_down_until": 0.0,
    "discovery": dict(EMPTY_DISCOVERY),
    # Single-flight latch for POST /api/rooms/refresh (None = idle).
    "rooms_refresh": None,
    # Staged candidates: cleared at scan Start, filled at 100%; a commit keeps
    # them (the firmware's staging only a new scan replaces).
    "candidates": [],
    # Staged ids whose full model was fetched (cleared with candidates).
    "detail_loaded": set(),
    # Staging generation: bumped at every scan start so a detail fetch
    # from an older generation discards its publish.
    "scan_generation": 0,
    "devices": {d["id"]: registry_seed(d) for d in INITIAL_DEVICES},
    "command_log": [],
    # The active API key + "never rotated" flag (mirrors the hub's
    # key_is_initial NVS flag).
    "api_key": MOCK_API_KEY,
    "apikey_initial": True,
    # OpenAPI provisioning state (mirrors the hub's NVS + in-memory token).
    "openapi": {
        "configured": False,
        "has_token": False,
        "login_in_flight": False,
        "uid": "",
        "endpoint": "",
        # Quota tracking SURVIVES /api/openapi/clear (hub-namespace state).
        "quota": {"month": "2026-10", "devices": 3, "limit": 10, "warning": False},
        "creds": None,  # in-memory copy of the saved credentials (never echoed)
    },
    # OTA mock — no real flash; upload only flips the slots.
    "ota": {
        "running": "ota_0",
        "next": "ota_1",
    },
}

# Room ids ride the registry devices (the firmware's room sweep landed them);
# seed after the literal so the fixture stays one dict per device.
seed_rooms(state["devices"])


def seed_state_devices() -> dict:
    """The registry as boot / factory-reset leaves it (rooms re-seeded)."""
    devices = {d["id"]: registry_seed(d) for d in INITIAL_DEVICES}
    seed_rooms(devices)
    return devices


def linked() -> bool:
    return state["auth"]["state"] == "linked"


def mock_cpu() -> dict:
    """Slowly jittering usage in the 8..40 band (sin of uptime)."""
    elapsed = time.time() - START_TIME
    usage = 24 + 16 * math.sin(elapsed / 30)
    core0 = clamp_pct(usage + 10 * math.sin(elapsed / 7))
    core1 = clamp_pct(usage - 8 * math.cos(elapsed / 11))
    return {"usage": round(clamp_pct(usage)), "core0": round(core0), "core1": round(core1)}


def clamp_pct(v: float) -> float:
    return min(100.0, max(0.0, v))


def status_payload() -> dict:
    a = state["auth"]
    return {
        "version": "0.1.0-mock",
        "language": state["lang"],
        "state": "linked" if linked() else "needs_setup",
        "wifi": {"connected": True, "ssid": "Mock-5G", "ip": "192.168.4.1", "rssi": -58},
        "heap": {
            "internal_free": 215040, "psram_free": 4194304,
            "internal_min_free": 180224,
            "internal_total": 327680, "internal_largest": 131072,
            "psram_total": 8388608, "psram_largest": 4194304,
        },
        "cpu": mock_cpu(),
        "uptime_ms": int((time.time() - START_TIME) * 1000),
        "auth": {"state": a["state"], "username": a["username"], "qr_payload": a["qr_payload"],
                 "initial": state["apikey_initial"]},
        "mqtt": {"connected": linked(), "error": ""},
        "discovery": discovery_status_payload(),
        "home_count": len(HOMES) if linked() else 0,
        "device_count": len(state["devices"]) if linked() else 0,
        "scene_count": len(SCENES) if linked() else 0,
    }


def auth_status_payload() -> dict:
    return dict(state["auth"])


def discovery_status_payload() -> dict:
    """BuildDiscoverySummaryObject mirror: + candidates = the staged count
    (0 while a scan runs — Start clears the staging)."""
    with LOCK:
        payload = dict(state["discovery"])
        payload["candidates"] = len(state["candidates"])
    return payload


def discovery_progress_payload() -> dict:
    """BuildDiscoveryProgressObject mirror: the extra per-home/device
    counters + error ride ONLY the discovery_progress WS event."""
    with LOCK:
        d = state["discovery"]
        return {
            # "active" derives from done/failed here (/api/status uses IsRunning).
            "active": not d["done"] and not d["failed"],
            "percent": d["percent"],
            "done": d["done"],
            "failed": d["failed"],
            "homes_done": d["homes_done"],
            "homes_total": d["homes_total"],
            "devices_done": d["devices_done"],
            "devices_total": d["devices_total"],
            "error": d["error"],
        }


def device_entity_rows(dev: dict) -> list:
    """BuildEntityRows mirror: {code,name,type,writable,value?,enabled} rows
    over the status/status_range/function union (sorted); writable follows
    the firmware's function-map rule, enabled from dev["entities"]."""
    codes = sorted(set(dev.get("status", {})) | set(dev.get("status_range", {}))
                   | set(dev.get("function", ())))
    writable = set(dev.get("function", ()))
    enabled = {e["code"]: e.get("enabled", True) for e in dev.get("entities", [])}
    rows = []
    for code in codes:
        rng = dev.get("status_range", {}).get(code)
        row = {"code": code, "name": (rng.get("name") if rng else "") or "",
               "type": rng.get("type", "") if rng else "",
               "writable": code in writable, "enabled": enabled.get(code, True)}
        if code in dev.get("status", {}):
            row["value"] = dev["status"][code]
        rows.append(row)
    return rows


def entity_code_union(dev: dict) -> set:
    """CollectEntityCodes mirror: the status + status_range + function keys."""
    return (set(dev.get("status", {})) | set(dev.get("status_range", {}))
            | set(dev.get("function", ())))


def apply_entity_flags(dev: dict, flags: dict) -> None:
    """SetEntityEnabled mirror: rebuild the device's {code,enabled} list."""
    codes = entity_code_union(dev)
    known = {e["code"]: e.get("enabled", True) for e in dev.get("entities", [])}
    dev["entities"] = [{"code": c, "enabled": bool(flags.get(c, known.get(c, True)))}
                       for c in sorted(codes)]


def device_api_view(dev: dict) -> dict:
    """BuildDeviceObject mirror: entities/function hidden; disabled codes
    invisible; every status_range entry gains name + writable."""
    disabled = {e["code"] for e in dev.get("entities", []) if not e.get("enabled", True)}
    writable = set(dev.get("function", ()))
    out = {k: v for k, v in dev.items() if k not in ("entities", "function")}
    # BuildDeviceObject mirror: room_name is resolved from the room table.
    if not out.get("room_name"):
        names = {r["id"]: r["name"] for r in ROOMS}
        out["room_name"] = names.get(dev.get("room_id") or "", "")
    out["status"] = {k: v for k, v in dev.get("status", {}).items() if k not in disabled}
    out["status_range"] = {
        code: {**spec, "name": spec.get("name") or code, "writable": code in writable}
        for code, spec in dev.get("status_range", {}).items() if code not in disabled
    }
    return out


def cand_room(dev: dict) -> tuple:
    """(room_id, room_name) for a candidate: its own field, else the registry
    row of the same id, else unassigned."""
    reg = state["devices"].get(dev.get("id")) or {}
    rid = dev.get("room_id") or reg.get("room_id") or ""
    name = next((r["name"] for r in ROOMS if r["id"] == rid), "")
    return rid, name


def candidate_object(dev: dict) -> dict:
    """BuildCandidateObject mirror (caller holds LOCK for already_added
    and detail_loaded)."""
    return {
        "id": dev["id"], "name": dev["name"], "category": dev["category"],
        "product_name": dev.get("product_name") or f"Mock {dev['category']}",
        "online": dev.get("online", True), "home_id": dev.get("home_id", ""),
        # Resolved from the registry like the firmware: top-level keys, never
        # DP codes (the scan-stage row itself carries no room field).
        "room_id": cand_room(dev)[0],
        "room_name": cand_room(dev)[1],
        "already_added": dev["id"] in state["devices"],
        "detail_loaded": dev["id"] in state["detail_loaded"],
        "entities": device_entity_rows(dev),
    }


def refresh_registry_device(existing: dict, cand: dict, ents) -> dict:
    """ApplyCommitEntry refresh mirror: model maps (status/status_range/
    function) come from the staged candidate; identity (name/category/online/
    home_id/support_local) and prior disabled-entity choices survive."""
    refreshed = json.loads(json.dumps(cand))
    for key in ("name", "category", "online", "home_id", "support_local"):
        if key in existing:
            refreshed[key] = existing[key]
    old_disabled = {e["code"] for e in existing.get("entities", [])
                    if not e.get("enabled", True)}
    codes = entity_code_union(refreshed)
    keep = set(ents) if ents is not None else codes
    refreshed["entities"] = [{"code": c, "enabled": c in keep and c not in old_disabled}
                             for c in sorted(codes)]
    return refreshed


def staged_model_refreshes(existing: dict, cand: dict) -> bool:
    """ApplyCommitEntry gate mirror: refresh only a richer AND non-shrinking
    model; the mock has no local_strategy map (support_local is a flag only)."""
    ev, cv = entity_code_union(existing), entity_code_union(cand)
    richer = (len(cv) > len(ev)
              or len(cand.get("status_range", {})) > len(existing.get("status_range", {}))
              or len(cand.get("function", ())) > len(existing.get("function", ())))
    no_shrink = (len(cand.get("status", {})) >= len(existing.get("status", {}))
                 and len(cand.get("status_range", {})) >= len(existing.get("status_range", {}))
                 and len(cand.get("function", ())) >= len(existing.get("function", ())))
    return richer and no_shrink


def begin_discovery_locked() -> bool:
    """DiscoveryService::Start mirror: False (=409) when already running;
    staging cleared HERE."""
    if state["discovery"]["active"]:
        return False
    # RunDiscovery sets progress_ = {} and stages from a clean slate; the
    # homes total is only known once the homes list answers.
    state["discovery"] = dict(EMPTY_DISCOVERY, active=True, percent=0, done=False,
                              failed=False, homes_total=len(HOMES),
                              devices_total=len(INITIAL_DEVICES))
    state["candidates"] = []
    state["detail_loaded"] = set()
    state["scan_generation"] += 1
    return True


def list_only_row(dev: dict) -> dict:
    """ParseDevice mirror: the list-stage row (status values only); the
    detail maps (status_range/function/support_local) need the lazy fetch."""
    row = json.loads(json.dumps(dev))
    for key in ("status_range", "function", "support_local"):
        row.pop(key, None)
    return row


# Parallel homes simulated by the mock scan (each staged device carries a
# home_id from HOMES; the hub lists the homes, then every home's devices).
SCAN_HOMES = len(HOMES)


def run_discovery_scan() -> None:
    """The 0->100 driver (~3s): one discovery_progress push per staged device
    row, then per home done (RunDiscovery order), then done + the staging."""
    homes_total = SCAN_HOMES
    devices_total = len(INITIAL_DEVICES)
    homes_of = [sum(1 for d in INITIAL_DEVICES if d["home_id"] == h["id"]) for h in HOMES]
    for home_idx in range(homes_total):
        # device rows (devices_done++) then the home-done tick (homes_done++).
        for i in range(homes_of[home_idx] + 1):
            time.sleep(0.2)
            with LOCK:
                d = state["discovery"]
                d["homes_done"] = home_idx
                d["devices_done"] = sum(homes_of[:home_idx]) + i
                d["homes_total"] = homes_total
                d["devices_total"] = devices_total
                total = d["homes_total"] + d["devices_total"]
                d["percent"] = ((d["homes_done"] + d["devices_done"]) * 100 // total
                                if total else 0)
                payload = discovery_progress_payload()
            ws_broadcast("discovery_progress", payload)
    with LOCK:
        # Finish(false, ""): done + percent 100; then the staging commit.
        state["discovery"] = dict(EMPTY_DISCOVERY, percent=100, done=True, failed=False,
                                  homes_done=homes_total, homes_total=homes_total,
                                  devices_done=devices_total, devices_total=devices_total)
        # Every cloud device is staged (firmware RunDiscovery has no registry
        # filter); the registry check happens at commit.
        state["candidates"] = [list_only_row(d) for d in INITIAL_DEVICES]
        payload = discovery_progress_payload()
    ws_broadcast("discovery_progress", payload)


# FetchCandidateDetail's single-flight flag (one detail fetch at a time).
DETAIL_FETCH_LOCK = threading.Lock()


def run_candidate_detail(dev_id: str) -> str:
    """FetchCandidateDetail mirror -> busy/scan_active/no_scan/not_found/
    already_loaded/loaded."""
    if not DETAIL_FETCH_LOCK.acquire(blocking=False):
        return "busy"
    try:
        with LOCK:
            d = state["discovery"]
            if d["active"]:
                return "scan_active"
            if not d["done"]:
                return "no_scan"
            if not any(c["id"] == dev_id for c in state["candidates"]):
                return "not_found"
            if dev_id in state["detail_loaded"]:
                return "already_loaded"
            full = next((x for x in INITIAL_DEVICES if x["id"] == dev_id), None)
            if full is None:
                return "not_found"
            # Generation of the staging this fetch validated against.
            generation = state["scan_generation"]
        time.sleep(DETAIL_DELAY)  # 3 simulated cloud calls; LOCK not held
        with LOCK:
            d = state["discovery"]
            if d["active"] or not d["done"] or state["scan_generation"] != generation:
                # A fetch that raced a rescan must not splat pre-scan fields
                # onto the new staging row — discard.
                return "scan_active"
            idx = next((i for i, c in enumerate(state["candidates"])
                        if c["id"] == dev_id), -1)
            if idx < 0:
                return "not_found"  # staging was consumed mid-fetch
            if dev_id in DEGRADED_DETAIL:
                # Degraded fetch: the row keeps its scan-stage model and
                # detail_loaded stays false (the commit fetches it again).
                print(f" [discovery] detail degraded: {dev_id}")
                return "degraded"
            state["candidates"][idx] = json.loads(json.dumps(full))
            state["detail_loaded"].add(dev_id)
        print(f" [discovery] detail loaded: {dev_id}")
        return "loaded"
    finally:
        DETAIL_FETCH_LOCK.release()


# FetchCandidateDetail's degraded set (MOCK_DEGRADED_DETAIL=<ids>): those
# fetches "fail", so detail_loaded stays false and the commit re-fetches.
DEGRADED_DETAIL = {x.strip() for x in os.environ.get("MOCK_DEGRADED_DETAIL", "").split(",")
                   if x.strip()}

# rest_api_internal.h: ReadBody(req, body, kMaxCommandsBody) bounds the
# per-device POST bodies; tuya_crypto::kMaxJsonDepth is BodyNestingOk's depth.
kMaxCommandsBody = 8192
kMaxJsonDepth = 32


def json_nesting_within_limit(raw: bytes) -> bool:
    """BodyNestingOk mirror (tuya_crypto::json_guard.cc): bracket depth <= 32,
    balanced, no unterminated string."""
    depth = 0
    in_string = False
    text = raw.decode("utf-8", "replace")
    i = 0
    while i < len(text):
        c = text[i]
        i += 1
        if in_string:
            if c == "\\":
                i += 1  # the escaped byte cannot close the string
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
        elif c in "{[":
            depth += 1
            if depth > kMaxJsonDepth:
                return False
        elif c in "}]":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0 and not in_string


def openapi_status_payload() -> dict:
    """GET /api/openapi/status — the merged snapshot
    {configured, has_token, login_in_flight, uid?, endpoint?, quota{...}}."""
    oa = state["openapi"]
    payload = {
        "configured": oa["configured"],
        "has_token": oa["has_token"],
        # True while the login task runs — the UI's failure check needs it.
        "login_in_flight": oa["login_in_flight"],
        "quota": dict(oa["quota"]),
    }
    if oa["uid"]:
        payload["uid"] = oa["uid"]
    if oa["endpoint"]:
        payload["endpoint"] = oa["endpoint"]
    return payload


def after_openapi_login(login_gen: int) -> None:
    """Login completes OPENAPI_TOKEN_DELAY after save; no WS push (the
    contract is polling). login_in_flight covers the whole wait, like the
    firmware's snapshot_login_in_flight flag (cleared when the task ends).
    `login_gen` identifies the attempt: a clear/refresh while sleeping
    cancels it."""
    time.sleep(OPENAPI_TOKEN_DELAY)
    with LOCK:
        if login_gen != OPENAPI_LOGIN_GEN[0] or not state["openapi"]["creds"]:
            return  # superseded by clear / a newer save
        state["openapi"]["login_in_flight"] = False
        state["openapi"]["has_token"] = True
        state["openapi"]["uid"] = "mock-uid-123"
        state["openapi"]["endpoint"] = "https://openapi.tuyaus.com"
    print(" [openapi] login complete (uid=mock-uid-123)")


# --- Mock log ring (GET /api/logs — the hub's captured LogBuffer) ---

# (lvl, tag, msg) — boot-ish traffic the feeder thread cycles through.
MOCK_LOG_LINES: list = [
    ("I", "Application", "Hub ready (state=Linked)"),
    ("I", "wifi", "WiFi connected: ssid=Mock-5G ip=192.168.4.1 rssi=-58"),
    ("I", "SystemInfo", "Firmware v0.1.0-mock (IDF v6.0.1, reset reason: POWERON)"),
    ("I", "CpuMonitor", "cpu ring: 32768 bytes (PSRAM)"),
    ("I", "LogBuffer", "log ring: 32768 bytes (PSRAM)"),
    ("I", "Application", "Sys self-test passed (55 checks)"),
    ("I", "TuyaMQTT", "MQTT connected"),
    ("I", "Discovery", "Discovery 10%: homes 1/2 devices 0/8"),
    ("I", "Discovery", "Discovery 40%: homes 2/2 devices 3/8"),
    ("I", "Discovery", "Discovery 100%: homes 2/2 devices 8/8"),
    ("I", "Registry", "Device added: light_rgb"),
    ("I", "Registry", "Device added: sensor_bed"),
    ("I", "Registry", "Registry restored from store: 2 home(s), 8 device(s)"),
    ("W", "MQTT", "connection lost (broker timeout)"),
    ("I", "MQTT", "reconnect in 5s"),
    ("I", "TuyaMQTT", "MQTT connected"),
    ("I", "Http", "GET /api/status 200 (192.168.4.23:51422)"),
    ("I", "Http", "GET /api/devices 200 (192.168.4.23:51423)"),
    ("I", "Http", "POST /api/devices/light_rgb/commands 200 (192.168.4.23:51424)"),
    ("W", "Http", "POST /api/devices/ghost99/commands 404 (device not found)"),
    ("E", "Http", "GET /api/scenes 502 (upstream timeout)"),
    ("I", "TokenManager", "token refreshed (expires in 7200s)"),
    ("W", "WifiStation", "rssi low: -79 dBm"),
    ("I", "Presence", "presence refresh: 7/8 device(s) online"),
    ("E", "Command", "light_dim command failed: device offline"),
    ("I", "Application", "tick: state=Linked uptime=600s heap_int=215040"),
]

LOG_RING: deque = deque(maxlen=500)
LOG_LOCK = threading.Lock()
LOG_TOTAL = [0]  # monotonic appended counter (boxed for the thread)


def seed_mock_logs() -> None:
    """Pre-fill the ring as if the first minute had already been logged."""
    with LOG_LOCK:
        for i, (lvl, tag, msg) in enumerate(MOCK_LOG_LINES):
            LOG_RING.append({"t": i * 2000, "lvl": lvl, "tag": tag, "msg": msg})
        LOG_TOTAL[0] = len(MOCK_LOG_LINES)


def log_feeder() -> None:
    """Appends one line every 2s (cycling the list, t = live uptime ms)."""
    i = 0
    while True:
        time.sleep(2)
        with LOCK:
            uptime_ms = int((time.time() - START_TIME) * 1000)
        lvl, tag, msg = MOCK_LOG_LINES[i % len(MOCK_LOG_LINES)]
        with LOG_LOCK:
            LOG_RING.append({"t": uptime_ms, "lvl": lvl, "tag": tag, "msg": msg})
            LOG_TOTAL[0] += 1
        i += 1


def logs_payload(lines: int) -> dict:
    """GET /api/logs — {"total", "lines": oldest->newest} like the hub."""
    with LOG_LOCK:
        lines_out = list(LOG_RING)[-lines:]
        total = LOG_TOTAL[0]
    return {"total": total, "lines": lines_out}


# --- WebSocket plumbing (raw sockets) ---

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ws_clients: list = []  # [ [sock, send_lock, rfile], ... ]
ws_clients_lock = threading.Lock()


def make_text_frame(payload: bytes) -> bytes:
    """Unmasked server text frame."""
    header = bytearray([0x81])
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack(">H", n)
    else:
        header.append(127)
        header += struct.pack(">Q", n)
    return bytes(header) + payload


def make_pong_frame(payload: bytes) -> bytes:
    return bytes([0x8A, len(payload)]) + payload


def ws_broadcast(event: str, data) -> None:
    frame = make_text_frame(json.dumps(
        {"event": event, "data": data, "timestamp": int(time.time() * 1000)}).encode())
    with ws_clients_lock:
        clients = list(ws_clients)
    for client in clients:
        sock, lk, _rfile = client
        try:
            with lk:
                sock.sendall(frame)
        except OSError:
            ws_drop(client)


def ws_drop(client) -> None:
    with ws_clients_lock:
        if client in ws_clients:
            ws_clients.remove(client)
    try:
        client[0].close()
    except OSError:
        pass
    print(f"[ws] client removed ({len(ws_clients)} left)")


def ws_evict_all() -> None:
    """Key rotation: drop every live session (firmware EvictAllSessions)."""
    with ws_clients_lock:
        clients = list(ws_clients)
        ws_clients.clear()
    for sock, lk, _rfile in clients:
        try:
            with lk:
                sock.sendall(bytes([0x88, 0x02]) + struct.pack(">H", 1008))  # policy violation
        except OSError:
            pass
        try:
            sock.shutdown(socket.SHUT_RDWR)  # wake the handler's blocked read
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass
    if clients:
        print(f"[ws] api key changed: evicted {len(clients)} session(s)")


def current_api_key() -> str:
    with LOCK:
        return state["api_key"]


def unauthorized(handler) -> None:
    """The 401: flat error shape + WWW-Authenticate (callers stall 0.05s
    first, mirroring the hub's anti-brute-force delay)."""
    body = json.dumps(
        {"success": False, "error": "unauthorized", "message": "Invalid API key"}
    ).encode()
    handler.send_response(401)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("WWW-Authenticate", "Bearer")
    handler.send_header("Cache-Control", "no-store")
    handler.send_header("Connection", "close")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)
    # close_connection tells BaseHTTPRequestHandler to drop the socket
    # after this response.
    handler.close_connection = True


def bearer_ok(handler) -> bool:
    """True when Bearer <key> matches the active mock key (like the hub)."""
    parts = (handler.headers.get("Authorization") or "").split(None, 1)
    if len(parts) != 2 or parts[0].lower() != "bearer":
        return False
    return parts[1].strip() == current_api_key()


def read_ws_frame(rfile):
    """Returns (opcode, payload) or None on EOF."""
    try:
        hdr = rfile.read(2)
    except OSError:
        return None
    if not hdr or len(hdr) < 2:
        return None
    b1, b2 = hdr[0], hdr[1]
    opcode = b1 & 0x0F
    masked = b2 & 0x80
    length = b2 & 0x7F
    try:
        if length == 126:
            ext = rfile.read(2)
            if len(ext) < 2:
                return None
            length = struct.unpack(">H", ext)[0]
        elif length == 127:
            ext = rfile.read(8)
            if len(ext) < 8:
                return None
            length = struct.unpack(">Q", ext)[0]
        mask = rfile.read(4) if masked else b""
        payload = rfile.read(length) if length else b""
    except OSError:
        return None
    if masked and len(mask) == 4:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def handle_ws_connection(handler: BaseHTTPRequestHandler) -> None:
    # ?token= must match the API key before the 101 goes out (a wrong
    # token gets 401 + close).
    qs = parse_qs(urlparse(handler.path).query)
    token = (qs.get("token") or [""])[0]
    if token != current_api_key():
        unauthorized(handler)
        print(" [ws] rejected: bad or missing token")
        return
    key = handler.headers.get("Sec-WebSocket-Key", "")
    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    sock = handler.connection
    rfile = handler.rfile
    try:
        sock.sendall(response.encode())
    except OSError:
        return
    client = [sock, threading.Lock(), rfile]
    with ws_clients_lock:
        ws_clients.append(client)
    print(f"[ws] client connected ({len(ws_clients)} total)")
    # Welcome snapshot so a freshly opened UI does not wait 10s.
    try:
        with client[1]:
            sock.sendall(make_text_frame(json.dumps(
                {"event": "status", "data": status_payload(),
                 "timestamp": int(time.time() * 1000)}).encode()))
    except OSError:
        ws_drop(client)
        return
    while True:
        frame = read_ws_frame(rfile)
        if frame is None:
            ws_drop(client)
            return
        opcode, payload = frame
        if opcode == 0x8:  # close
            try:
                with client[1]:
                    sock.sendall(bytes([0x88, 0]) + payload[:2])
            except OSError:
                pass
            ws_drop(client)
            return
        if opcode == 0x9:  # ping -> pong
            try:
                with client[1]:
                    sock.sendall(make_pong_frame(payload))
            except OSError:
                ws_drop(client)
                return
            continue
        if opcode == 0x1:  # text
            text = payload.decode("utf-8", "replace")
            if text in ("ping", '{"type":"ping"}'):
                try:
                    with client[1]:
                        sock.sendall(make_text_frame(b'{"event":"pong"}'))
                except OSError:
                    ws_drop(client)
                    return


def status_pusher() -> None:
    while True:
        time.sleep(10)
        with LOCK:
            payload = status_payload()
        ws_broadcast("status", payload)


# --- Auth / discovery state machine threads ---

def after_start_sequence(user_code: str) -> None:
    # QR_DELAY: the QR payload "arrives" (exercises the "Generating QR code" state).
    time.sleep(QR_DELAY)
    with LOCK:
        state["auth"]["qr_payload"] = "tuyaSmart--qrLogin?token=mock123"
        payload = auth_status_payload()
    ws_broadcast("auth_status", payload)
    refresh_wait = 0.0
    if QR_REFRESH:
        # MOCK_QR_REFRESH: rotate the token twice ~2 s apart before linking.
        for token in ("mock456", "mock789"):
            time.sleep(2.0)
            refresh_wait += 2.0
            with LOCK:
                state["auth"]["qr_payload"] = f"tuyaSmart--qrLogin?token={token}"
                payload = auth_status_payload()
            ws_broadcast("auth_status", payload)
    # LINK_DELAY: linked (the user "scanned" with the app).
    time.sleep(max(0.1, LINK_DELAY - QR_DELAY - refresh_wait))
    with LOCK:
        state["auth"]["state"] = "linked"
        state["auth"]["username"] = "mock_user"
        payload = auth_status_payload()
        st = status_payload()
    ws_broadcast("auth_status", payload)
    ws_broadcast("status", st)
    ws_broadcast("mqtt_status", {"connected": True, "error": ""})
    # Discovery simulation — the same driver as POST /api/discovery/scan.
    with LOCK:
        begin_discovery_locked()
    run_discovery_scan()
    # device_added nudges (the UI refetches /api/devices).
    ws_broadcast("device_added", {"devId": "light_rgb"})
    ws_broadcast("device_added", {"devId": "socket_2"})


def echo_device_update(dev_id: str, commands: list) -> None:
    """The MQTT confirm: apply + push device_update 1s after the command."""
    time.sleep(1.0)
    with LOCK:
        dev = state["devices"].get(dev_id)
        if not dev:
            return
        props = []
        for c in commands:
            code = c.get("code")
            value = c.get("value")
            if code is None:
                continue
            dev["status"][code] = value
            props.append({"code": code, "value": value, "t": int(time.time() * 1000)})
        payload = {"devId": dev_id, "name": dev["name"], "props": props}
    if props:
        ws_broadcast("device_update", payload)


def set_device_online(dev_id: str, online: bool) -> str:
    """kDeviceOnlineChanged mirror: flip dev["online"] and push
    device_online {devId, online} (the dashboard badge patch). Returns
    "ok" / "not_found" / "unchanged"."""
    with LOCK:
        dev = state["devices"].get(dev_id)
        if dev is None:
            return "not_found"
        if bool(dev.get("online", True)) == online:
            return "unchanged"
        dev["online"] = online
    ws_broadcast("device_online", {"devId": dev_id, "online": online})
    print(f" [presence] {dev_id} -> {'online' if online else 'offline'}")
    return "ok"


def presence_driver() -> None:
    """QA: the presence toggler thread (armed by MOCK_PRESENCE)."""
    flip = True
    rounds = 0
    while True:
        time.sleep(6.0)
        with LOCK:
            ids = list(state["devices"])
        for dev_id in ids:
            set_device_online(dev_id, flip)
        flip = not flip
        rounds += 1
        if PRESENCE_ROUNDS and rounds >= PRESENCE_ROUNDS:
            return


# --- Static serving from the WAS1 image (like the firmware) ---

# The assets partition size (partitions/tuya_16m.csv).
ASSET_PARTITION_SIZE = 0x100000

# The live in-memory image: loaded from dist/web_assets.bin (byte-identical
# to what the firmware flashes — same build_id), swapped on upload.
IMAGE_LOCK = threading.RLock()
ASSET_IMAGE = {"bytes": b"", "entries": {}, "info": None}


def load_asset_image() -> None:
    """Loads dist/web_assets.bin and keeps the parsed view — one data
    path, byte-identical to the firmware's flashed image."""
    bin_path = DIST_DIR / "web_assets.bin"
    if not bin_path.is_file():
        raise FileNotFoundError("missing dist/web_assets.bin — run scripts/build_web.py")
    image = bin_path.read_bytes()
    info, reason = web_asset_pack.validate(image, ASSET_PARTITION_SIZE)
    if info is None:
        raise ValueError(f"dist/web_assets.bin invalid: {reason}")
    with IMAGE_LOCK:
        ASSET_IMAGE["bytes"] = image
        ASSET_IMAGE["entries"] = info["entries"]
        ASSET_IMAGE["info"] = info


def asset_lookup(name: str):
    """FindAsset mirror: entry dict or None (caller holds IMAGE_LOCK)."""
    return ASSET_IMAGE["entries"].get(name)


def asset_slice(name: str) -> bytes:
    entry = ASSET_IMAGE["entries"][name]
    base = (web_asset_pack.HEADER_SIZE
            + web_asset_pack.TOC_ENTRY_SIZE * ASSET_IMAGE["info"]["file_count"])
    blob = ASSET_IMAGE["bytes"][base + entry["offset"]:base + entry["offset"] + entry["size"]]
    return blob


def asset_info_payload() -> dict:
    """GET /api/asset/info — the firmware's WebAssetInfo shape."""
    with IMAGE_LOCK:
        info = dict(ASSET_IMAGE["info"])
    return {
        "valid": True,
        "version": info["version"],
        "build_id": info["build_id"],
        "files": info["file_count"],
        "payload_len": info["payload_len"],
        "partition_size": ASSET_PARTITION_SIZE,
    }


def mock_mime(name: str) -> str:
    if name.endswith(".html"):
        return "text/html; charset=utf-8"
    if name.endswith(".css"):
        return "text/css"
    if name.endswith(".js"):
        return "text/javascript"
    if name.endswith(".json"):
        return "application/json"
    if name.endswith(".svg"):
        return "image/svg+xml"
    return "application/octet-stream"


# --- HTTP handler ---

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        """Quieter logging that survives every call site: log_request passes 3
        values for a 2-placeholder format, log_error passes 1 for 2."""
        try:
            text = fmt % args
        except (TypeError, ValueError):
            text = " ".join([str(fmt)] + [str(a) for a in args])
        print(" [http] " + text)

    # ---- responses ----

    def send_json(self, obj, status: int = 200) -> None:
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_api_error(self, status: int, code: str, message: str) -> None:
        self.send_json({"success": False, "error": {"code": code, "message": message}}, status)

    # Body filled by prepare_body() before routing (b"" for a bodyless POST).
    raw_body = b""

    def read_body(self) -> dict:
        # Explicit lenient parse: an empty body is {} (not a parse failure).
        if not self.raw_body:
            return {}
        try:
            return json.loads(self.raw_body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    def read_raw_body(self) -> bytes:
        """Binary body (asset .bin / OTA image) — Content-Length only."""
        return self.raw_body

    def drain_body(self) -> None:
        """Consume Content-Length bytes so a 401/404 cannot desync keep-alive."""
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length > 0:
            self.rfile.read(length)

    def prepare_body(self) -> None:
        """Read the whole request body ONCE; every handler works off it, so
        the snapshot/update branch and a rejecting response cannot leave
        unread bytes on a keep-alive connection."""
        length = int(self.headers.get("Content-Length", "0") or 0)
        self.raw_body = self.rfile.read(length) if length > 0 else b""

    # ---- routing ----

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        conn = (self.headers.get("Connection") or "").lower()
        if path == "/ws":
            ws_upgrade = "websocket" in (self.headers.get("Upgrade") or "").lower()
            if "upgrade" in conn and ws_upgrade:
                with LOCK:
                    down_until = state.get("hub_down_until", 0)
                if time.time() < down_until:
                    # /api/qa/hub-down: the simulated hub is off (503 on upgrade).
                    self.send_response(503)
                    self.send_header("Connection", "close")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    self.close_connection = True
                    return
                self.close_connection = True  # upgraded socket never returns to HTTP
                handle_ws_connection(self)
            else:
                # ws_handler.cc: a plain GET on /ws is a 400, never the SPA.
                body = b"WebSocket upgrade required"
                self.send_response(400)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True
            return
        if path == "/favicon.ico":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path.startswith("/api/"):
            self.api_get(path, parse_qs(parsed.query))
            return
        self.serve_static(path)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        self.prepare_body()  # drain first: every response below may keep-alive
        if not path.startswith("/api/"):
            self.send_api_error(404, "not_found", "Unknown path")
            return
        self.api_post(path)

    def method_not_allowed(self) -> None:
        """All mock routes are GET/POST: answer 405 JSON, never the 501 default."""
        self.prepare_body()  # keep-alive safe when the request carried a body
        body = json.dumps({"success": False, "error": {
            "code": "method_not_allowed", "message": "Method not allowed"}}).encode()
        self.send_response(405)
        self.send_header("Allow", "GET, POST")
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.method_not_allowed()

    def do_PUT(self):
        self.method_not_allowed()

    def do_DELETE(self):
        self.method_not_allowed()

    # ---- static (from the parsed WAS1 image, gzip like the ESP32 route) ----

    def serve_static(self, path: str) -> None:
        name = path[1:] if path != "/" else "index.html"
        data = None
        with IMAGE_LOCK:
            entry = asset_lookup(name)
            if entry:
                data = asset_slice(name)
            else:
                # HandleAsset mirror: SPA fallback first, recovery page
                # only when the image has no index.html.
                entry = asset_lookup("index.html")
                if entry:
                    name = "index.html"
                    data = asset_slice(name)
        if data is None:
            # No valid image index — the embedded recovery page.
            gz = DIST_DIR / "fallback.html.gz"
            if not gz.is_file():
                self.send_api_error(500, "internal",
                                    "missing dist/fallback.html.gz — run scripts/build_web.py")
                return
            data = gz.read_bytes()
            name = "fallback.html"
        self.send_response(200)
        self.send_header("Content-Type", mock_mime(name))
        self.send_header("Content-Encoding", "gzip")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("ETag", f'"{name}"')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ---- REST GET ----

    def api_get(self, path: str, query: dict) -> None:
        if not bearer_ok(self):  # every /api GET is Bearer-gated
            time.sleep(0.05)
            unauthorized(self)
            return
        if path == "/api/status":
            with LOCK:
                self.send_json(status_payload())
            return
        if path == "/api/logs":
            try:
                n = int((query.get("lines") or ["200"])[0])
            except ValueError:
                n = 200
            n = max(1, min(500, n))
            self.send_json(logs_payload(n))
            return
        if path == "/api/discovery/candidate/detail":
            # HandleDiscoveryCandidateDetailApi mirror (blocks ~DETAIL_DELAY).
            dev_id = (query.get("id") or [""])[0]
            if not dev_id:
                self.send_api_error(400, "invalid_param", "Missing 'id' query parameter")
                return
            status = run_candidate_detail(dev_id)
            if status == "no_scan":
                self.send_api_error(400, "no_scan",
                                    "No completed discovery scan — run a scan first")
                return
            if status == "scan_active":
                self.send_api_error(400, "discovery_active",
                                    "A discovery scan is running — wait for it to complete")
                return
            if status == "not_found":
                self.send_api_error(404, "not_found",
                                    "Unknown device id — not a staged candidate")
                return
            if status == "busy":
                self.send_api_error(409, "detail_fetch_running",
                                    "A candidate detail fetch is already running")
                return
            # loaded / already_loaded -> the fresh candidate row.
            with LOCK:
                cand = next((c for c in state["candidates"] if c["id"] == dev_id), None)
                obj = candidate_object(cand) if cand is not None else None
            if obj is None:
                self.send_api_error(404, "not_found",
                                    "Unknown device id — not a staged candidate")
                return
            self.send_json(obj)
            return
        with LOCK:
            if path == "/api/auth/status":
                self.send_json(auth_status_payload())
                return
            if path == "/api/discovery/candidates":
                # HandleDiscoveryCandidatesApi: {active,done,failed,candidates}.
                d = state["discovery"]
                self.send_json({
                    "active": d["active"], "done": d["done"], "failed": d["failed"],
                    "candidates": [candidate_object(c) for c in state["candidates"]],
                })
                return
            if path == "/api/homes":
                self.send_json(HOMES if linked() else [])
                return
            if path == "/api/devices":
                if not linked():
                    self.send_json([])
                    return
                home = (query.get("home_id") or [""])[0]
                devs = [device_api_view(d) for d in state["devices"].values()
                        if not home or d["home_id"] == home]
                self.send_json(devs)
                return
            if path == "/api/scenes":
                if not linked():
                    self.send_json([])
                    return
                home = (query.get("home_id") or [""])[0]
                self.send_json([s for s in SCENES if not home or s["home_id"] == home])
                return
            if path == "/api/rooms":
                # HandleRoomsApi mirror: [{id,name,sort,home_id,device_count}].
                self.send_json(rooms_with_counts(state["devices"]) if linked() else [])
                return
            if path == "/api/settings/language":
                self.send_json({"lang": state["lang"]})
                return
            if path == "/api/settings/apikey":
                # Only the initial flag — the key never leaves the hub.
                self.send_json({"initial": state["apikey_initial"]})
                return
            if path == "/api/openapi/status":
                self.send_json(openapi_status_payload())
                return
            if path == "/api/openapi/quota":
                # Legacy endpoint kept for compat (the UI uses /status).
                self.send_json({
                    "configured": state["openapi"]["configured"],
                    **state["openapi"]["quota"],
                })
                return
            if path == "/api/mock/commands":
                self.send_json(state["command_log"])
                return
            if path == "/api/asset/info":
                # HandleAssetInfoApi mirror (the store is always valid here).
                self.send_json({"success": True, "asset": asset_info_payload()})
                return
            if path == "/api/ota/info":
                # HandleOtaInfoApi mirror: slots + app description.
                self.send_json({"success": True, "ota": {
                    "running": state["ota"]["running"],
                    "next": state["ota"]["next"],
                    "slot_size": 0x3F0000,
                    "version": "0.1.0-mock",
                    "build_time": time.strftime("%b %d %Y %H:%M:%S"),
                }})
                return
        self.send_api_error(404, "not_found", "Unknown path")

    # ---- REST POST ----

    def api_post(self, path: str) -> None:
        if not bearer_ok(self):  # every /api POST is Bearer-gated
            time.sleep(0.05)
            unauthorized(self)  # the body was already drained by do_POST
            return

        # Binary bodies (asset .bin / OTA image) skip the JSON parse.
        if path == "/api/asset/upload":
            # HandleAssetUploadApi mirror: validate in memory, then swap
            # the image (no flash, no reboot).
            raw = self.raw_body
            if not raw:
                self.send_api_error(400, "invalid_param", "Content-Length required")
                return
            if len(raw) > ASSET_PARTITION_SIZE:
                self.send_api_error(400, "too_large", "Body exceeds the maximum size")
                return
            info, reason = web_asset_pack.validate(raw, ASSET_PARTITION_SIZE)
            if info is None:
                self.send_api_error(400, "invalid_asset", reason)
                return
            with IMAGE_LOCK:
                ASSET_IMAGE["bytes"] = raw
                ASSET_IMAGE["entries"] = info["entries"]
                ASSET_IMAGE["info"] = info
            print(f" [asset] image swapped: {info['file_count']} files, "
                  f"build_id={info['build_id']}")
            self.send_json({"success": True, "asset": asset_info_payload()})
            return

        if path == "/api/ota/upload":
            # HandleOtaUploadApi mirror: header check + size cap, then
            # "flash" = flip the slots.
            raw = self.raw_body
            if not raw:
                self.send_api_error(400, "invalid_param", "Content-Length required")
                return
            if len(raw) > 0x3F0000:
                self.send_api_error(400, "too_large", "Image exceeds the OTA slot size")
                return
            if len(raw) < 24:
                self.send_api_error(400, "invalid_param", "Image shorter than the 24-byte header")
                return
            magic = raw[0]
            # esp_image_header_t: chip_id at byte offset 12.
            chip_id = struct.unpack("<H", raw[12:14])[0]
            if magic != 0xE9:
                self.send_api_error(400, "invalid_param", "Bad image magic (not an ESP32 app image)")
                return
            if chip_id != 0x0009:
                self.send_api_error(400, "invalid_param", "Image is not for the ESP32-S3")
                return
            with LOCK:
                old_next = state["ota"]["next"]
                state["ota"]["running"] = old_next
                state["ota"]["next"] = "ota_0" if old_next == "ota_1" else "ota_1"
            print(f" [ota] image accepted: {len(raw)} B -> {old_next} (reboot pending)")
            self.send_json({"success": True})
            return

        # Device-scoped: parsed BEFORE the JSON read, like the firmware order
        # (ParseIdPath + GetDevice 404 run before ReadBody in every handler).
        dev_id = parse_id_path(path, "/api/devices/", "/entities")
        if dev_id:
            # HandleDeviceEntitiesApi: empty body = snapshot; {"entities":{code:bool}}
            # = update. Order, limits and messages mirror rest_api_devices.cc:120-195.
            raw_body = self.raw_body
            with LOCK:
                dev = state["devices"].get(dev_id)
            if dev is None:  # GetDevice runs BEFORE ReadBody in the firmware
                self.send_api_error(404, "not_found", "Unknown device id")
                return
            if len(raw_body) > kMaxCommandsBody:  # ReadBody(req, body, kMaxCommandsBody)
                self.send_api_error(400, "invalid_param", "Body too large")
                return
            # Body already drained by api_post: empty = snapshot, else update.
            if not raw_body.strip():
                # The const snapshot: {"id", "entities": [...]}.
                with LOCK:
                    self.send_json({"id": dev_id, "entities": device_entity_rows(dev)})
                return
            # BodyNestingOk(body) gates the parse.
            body = self.read_body()
            if body is None or not json_nesting_within_limit(raw_body):
                self.send_api_error(400, "invalid_param", "Invalid JSON")
                return
            entities = body.get("entities") if isinstance(body, dict) else None
            if not isinstance(entities, dict):
                self.send_api_error(400, "invalid_param",
                                    "'entities' must be an object of {code: bool}")
                return
            flags = {}
            for code, value in entities.items():
                # Same per-child rule; cJSON object keys are always non-empty.
                if not isinstance(code, str) or not code or not isinstance(value, bool):
                    self.send_api_error(
                        400, "invalid_param",
                        "Each entity needs a non-empty code and a bool value")
                    return
                flags[code] = value
            with LOCK:
                if flags:  # an empty object is a 200 no-op (no changes queued)
                    apply_entity_flags(dev, flags)
                rows = device_entity_rows(dev)
            print(f" [registry] entities {dev_id}: {json.dumps(flags)}")
            self.send_json({"success": True, "entities": rows})
            return

        dev_id = parse_id_path(path, "/api/devices/", "/remove")
        if dev_id:
            # HandleDeviceRemoveApi: 404 unknown, else drop + device_removed push.
            with LOCK:
                if dev_id not in state["devices"]:
                    self.send_api_error(404, "not_found", "Unknown device id")
                    return
                del state["devices"][dev_id]
            ws_broadcast("device_removed", {"devId": dev_id})
            print(f" [registry] removed {dev_id}")
            self.send_json({"success": True})
            return

        dev_id = parse_id_path(path, "/api/devices/", "/commands")
        if dev_id:
            # HandleDeviceCommandsApi: 404 before the body, 400 on the commands
            # shape, then 502 (MOCK_FAIL_COMMANDS) or the echoed update.
            with LOCK:
                if dev_id not in state["devices"]:
                    self.send_api_error(404, "not_found", "Unknown device id")
                    return
            body = self.read_body()
            if body is None:
                self.send_api_error(400, "invalid_param", "Invalid JSON")
                return
            commands = body.get("commands") if isinstance(body, dict) else None
            if not isinstance(commands, list) or not commands:
                self.send_api_error(400, "invalid_param", "'commands' must be a non-empty array")
                return
            if FAIL_COMMANDS:
                print(f" [cmd] {dev_id}: injected command_failed")
                self.send_api_error(502, "command_failed",
                                    "Device command failed (cloud error)")
                return
            with LOCK:
                state["command_log"].append({
                    "devId": dev_id, "commands": commands, "t": int(time.time() * 1000),
                })
            print(f" [cmd] {dev_id}: {json.dumps(commands)}")
            threading.Thread(target=echo_device_update, args=(dev_id, commands), daemon=True).start()
            self.send_json({"success": True})
            return

        body = self.read_body()
        if body is None:
            self.send_api_error(400, "invalid_param", "Invalid JSON")
            return

        if path == "/api/auth/start":
            user_code = str(body.get("user_code", "")).strip()
            if not user_code:
                self.send_api_error(400, "invalid_param", "'user_code' must be a non-empty string")
                return
            with LOCK:
                state["auth"] = {"state": "waiting_scan", "user_code": user_code,
                                 "qr_payload": "", "username": "", "error": ""}
            print(f" [auth] start user_code={user_code!r} -> waiting_scan")
            threading.Thread(target=after_start_sequence, args=(user_code,), daemon=True).start()
            self.send_json({"success": True})
            return

        if path == "/api/auth/unlink":
            with LOCK:
                state["auth"] = {"state": "idle", "user_code": "", "qr_payload": "", "username": "", "error": ""}
                state["discovery"] = dict(EMPTY_DISCOVERY)
                state["candidates"] = []  # staging resets with the link state
                state["detail_loaded"] = set()
                payload = auth_status_payload()
                st = status_payload()
            ws_broadcast("auth_status", payload)
            ws_broadcast("status", st)
            ws_broadcast("mqtt_status", {"connected": False, "error": ""})
            print(" [auth] unlinked -> idle")
            self.send_json({"success": True})
            return

        if path == "/api/discovery/scan":
            # HandleDiscoveryScanApi mirror: 409 while a scan is running.
            with LOCK:
                started = begin_discovery_locked()
            if not started:
                self.send_api_error(409, "discovery_running",
                                    "A discovery scan is already running")
                return
            print(" [discovery] manual scan started")
            threading.Thread(target=run_discovery_scan, daemon=True).start()
            self.send_json({"success": True})
            return

        if path == "/api/discovery/commit":
            # HandleDiscoveryCommitApi mirror: validate, move, push — staging
            # survives the commit (only a new scan replaces it).
            devices = body.get("devices") if isinstance(body, dict) else None
            if not isinstance(devices, list) or not devices:
                self.send_api_error(400, "invalid_param", "'devices' must be a non-empty array")
                return
            parsed = []
            for item in devices:
                ok = isinstance(item, dict) and isinstance(item.get("id"), str) and bool(item["id"])
                ents = item.get("entities") if isinstance(item, dict) else None
                if ok and ents is not None:
                    # PRESENT: must be a non-empty array of non-empty codes.
                    ok = (isinstance(ents, list) and bool(ents)
                          and all(isinstance(c, str) and c for c in ents))
                if not ok:
                    self.send_api_error(
                        400, "invalid_param",
                        "Each device needs a non-empty string 'id' and an optional "
                        "non-empty 'entities' array of non-empty codes")
                    return
                parsed.append((item["id"], ents))
            with LOCK:
                d = state["discovery"]
                if d["active"]:
                    self.send_api_error(400, "discovery_active",
                                        "A discovery scan is running — wait for it to complete")
                    return
                if not d["done"]:
                    self.send_api_error(400, "no_scan",
                                        "No completed discovery scan — run a scan first")
                    return
                staged = {c["id"]: c for c in state["candidates"]}
                added_ids, refreshed_ids, skipped, seen = [], [], 0, set()
                for dev_id, ents in parsed:
                    cand = staged.get(dev_id)
                    if cand is None:
                        self.send_api_error(
                            400, "invalid_param",
                            f"Unknown device id '{dev_id[:80]}' — not a staged candidate")
                        return
                    if dev_id in seen:
                        skipped += 1  # duplicate id in one request (idempotent)
                        continue
                    seen.add(dev_id)
                    if dev_id not in state["detail_loaded"] and dev_id not in DEGRADED_DETAIL:
                        # Stage 3 mirror: fetch-before-commit — unfetched
                        # rows commit from the full model (all entities). A
                        # DEGRADED fetch keeps the scan-stage row instead.
                        cand = next((x for x in INITIAL_DEVICES
                                     if x["id"] == dev_id), cand)
                    existing = state["devices"].get(dev_id)
                    if existing is not None:
                        # ApplyCommitEntry mirror: a richer, non-shrinking
                        # staged model refreshes (counts as added).
                        if not staged_model_refreshes(existing, cand):
                            skipped += 1
                            continue
                        state["devices"][dev_id] = refresh_registry_device(
                            existing, cand, ents)
                        refreshed_ids.append(dev_id)
                        continue
                    dev = json.loads(json.dumps(cand))
                    codes = entity_code_union(dev)
                    keep = set(ents) if ents is not None else codes
                    dev["entities"] = [{"code": c, "enabled": c in keep} for c in sorted(codes)]
                    state["devices"][dev_id] = dev
                    added_ids.append(dev_id)
            for dev_id in added_ids:
                ws_broadcast("device_added", {"devId": dev_id})
            # A refresh swaps the card's entity set: the add-path refetch
            # nudge, not the device_update props patch.
            for dev_id in refreshed_ids:
                ws_broadcast("device_added", {"devId": dev_id})
            print(f" [discovery] commit: {len(added_ids)} added, "
                  f"{len(refreshed_ids)} refreshed, {skipped} skipped")
            self.send_json({"success": True,
                            "added": len(added_ids) + len(refreshed_ids),
                            "skipped": skipped})
            return

        if path == "/api/settings/language":
            lang = body.get("lang")
            if lang not in ("vi", "en"):
                self.send_api_error(400, "invalid_param", "'lang' must be \"vi\" or \"en\"")
                return
            with LOCK:
                state["lang"] = lang
            print(f" [settings] language -> {lang}")
            self.send_json({"success": True})
            return

        if path == "/api/settings/apikey":
            # 8..64 printable, no leading/trailing whitespace (round-trips
            # the firmware's trimmed Bearer parse). Valid Bearer required.
            new = body.get("new")
            if (not isinstance(new, str) or not 8 <= len(new) <= 64
                    or not all(32 <= ord(c) < 127 for c in new)
                    or new != new.strip(" \t")):
                self.send_api_error(400, "invalid_param",
                                    "'new' must be 8..64 printable characters (no leading/trailing spaces)")
                return
            with LOCK:
                state["api_key"] = new
                # Mirrors ApiKeyStore: rotation ends the initial-key window.
                state["apikey_initial"] = False
            ws_evict_all()  # old-key WS sessions must not survive
            print(" [settings] api key changed (initial flag cleared)")
            self.send_json({"success": True})
            return

        if path == "/api/openapi/credentials":
            fields = ("access_id", "access_secret", "username", "password")
            for key in fields:
                value = body.get(key)
                if not isinstance(value, str) or not value.strip():
                    self.send_api_error(
                        400, "invalid_param",
                        f"'{key}' must be a non-empty string")
                    return
            creds = {key: body.get(key, "") for key in
                     ("access_id", "access_secret", "username", "password",
                      "country_code", "schema", "datacenter")}
            # Mirror OpenApiClient::Configure: a fresh configuration cancels
            # any running login task, then LoginInBackground starts one.
            with LOCK:
                OPENAPI_LOGIN_GEN[0] += 1
                login_gen = OPENAPI_LOGIN_GEN[0]
                state["openapi"]["configured"] = True
                state["openapi"]["has_token"] = False
                state["openapi"]["login_in_flight"] = True
                state["openapi"]["uid"] = ""
                state["openapi"]["endpoint"] = ""
                state["openapi"]["creds"] = creds
            print(f" [openapi] credentials saved (user={creds['username']!r}, "
                  f"dc={creds['datacenter']!r}) -> login scheduled")
            threading.Thread(target=after_openapi_login, args=(login_gen,),
                             daemon=True).start()
            self.send_json({"success": True})
            return

        if path == "/api/openapi/clear":
            with LOCK:
                # Clearing the store cancels the in-flight login attempt.
                OPENAPI_LOGIN_GEN[0] += 1
                state["openapi"]["configured"] = False
                state["openapi"]["has_token"] = False
                state["openapi"]["login_in_flight"] = False
                state["openapi"]["uid"] = ""
                state["openapi"]["endpoint"] = ""
                state["openapi"]["creds"] = None
                # quota deliberately kept (hub-namespace tracking)
            print(" [openapi] cleared -> not configured")
            self.send_json({"success": True})
            return

        if path == "/api/system/reboot":
            print(" [system] REBOOT requested (mock: no-op)")
            self.send_json({"success": True})
            return

        if path == "/api/qa/push":
            # QA hook: push one WS event ({"event": "...", "data": {...}}) so a
            # browser harness can observe a structural event out of band.
            # The frame carries the hub's timestamp envelope: the web client's
            # router ignores an event frame without it.
            frame = make_text_frame(json.dumps({
                "event": body.get("event", ""), "data": body.get("data") or {},
                "timestamp": int(time.time() * 1000)}).encode())
            with ws_clients_lock:
                clients = list(ws_clients)
            for sock_client in clients:
                try:
                    with sock_client[1]:
                        sock_client[0].sendall(frame)
                except OSError:
                    pass
            self.send_json({"success": True})
            return

        if path == "/api/rooms/refresh":
            # HandleRoomsRefreshApi mirror: one refresh at a time, nothing blocks.
            with LOCK:
                busy = state.get("rooms_refresh") is not None
                if not busy:
                    state["rooms_refresh"] = True
            if busy:
                self.send_json({"success": True, "already_running": True})
                return
            print(" [rooms] refresh started")
            threading.Thread(target=publish_rooms_refresh, daemon=True).start()
            self.send_json({"success": True}, status=202)
            return

        if path == "/api/qa/rooms":
            # QA hook: rewrite the room fixture (+ device room ids) before a
            # rooms_updated push; the UI cannot learn a rename any other way.
            rooms_mutation(body if isinstance(body, dict) else {})
            self.send_json({"success": True})
            return

        if path == "/api/qa/hub-down":            # QA hook: drop every WS client; {"dur": seconds > 0} keeps the
            # hub refusing new WS clients for that long (outage simulation).
            with LOCK:
                state["hub_down_until"] = time.time() + float(body.get("dur") or 0)
            ws_evict_all()
            self.send_json({"success": True})
            return

        if path == "/api/qa/hub-up":
            # QA hook: end the simulated outage now (WS upgrades accepted again).
            with LOCK:
                state["hub_down_until"] = 0.0
            self.send_json({"success": True})
            return

        if path == "/api/system/factory-reset":
            print(" [system] FACTORY RESET requested (mock: state reset)")
            with LOCK:
                state["auth"] = {"state": "idle", "user_code": "", "qr_payload": "", "username": "", "error": ""}
                state["lang"] = "en"
                state["discovery"] = dict(EMPTY_DISCOVERY)
                state["candidates"] = []  # devices restored -> staging empty
                state["detail_loaded"] = set()
                state["devices"] = seed_state_devices()
                state["command_log"].append({"action": "factory-reset", "t": int(time.time() * 1000)})
                # The hub erases the key state; the next boot generates a
                # fresh random initial key (mock: back to the test key).
                state["api_key"] = MOCK_API_KEY
                state["apikey_initial"] = True
                OPENAPI_LOGIN_GEN[0] += 1  # cancels a running login task
                state["openapi"] = {
                    "configured": False, "has_token": False, "login_in_flight": False,
                    "uid": "", "endpoint": "",
                    "quota": {"month": "2026-10", "devices": 3, "limit": 10, "warning": False},
                    "creds": None,
                }
                payload = auth_status_payload()
                st = status_payload()
            ws_broadcast("auth_status", payload)
            ws_broadcast("status", st)
            self.send_json({"success": True})
            return

        if path == "/api/ota/rollback":
            # HandleOtaRollbackApi mirror: the running slot is marked invalid,
            # the hub boots the other (previously valid) slot (no reboot here).
            with LOCK:
                running = state["ota"]["running"]
                rolled = "ota_0" if running == "ota_1" else "ota_1"
                state["ota"]["running"] = rolled
                state["ota"]["next"] = running
            print(" [ota] rollback -> %s (mock: no reboot)" % rolled)
            self.send_json({"success": True})
            return

        if path.startswith("/api/scenes/") and path.endswith("/trigger"):
            # Scene ids are literal fixture ids; the firmware decodes the same way.
            scene_id = unquote(path[len("/api/scenes/"):-len("/trigger")])
            if not any(s["id"] == scene_id for s in SCENES):
                self.send_api_error(404, "not_found", "Unknown scene id")
                return
            print(f" [scene] trigger {scene_id} (body={json.dumps(body)})")
            self.send_json({"success": True})
            return

        self.send_api_error(404, "not_found", "Unknown path")


def publish_rooms_refresh() -> None:
    """RoomsRefresh mirror: the sweep finishes ~300 ms later, then the event."""
    time.sleep(0.3)
    with LOCK:
        state["rooms_refresh"] = None
    ws_broadcast("rooms_updated", {"home_id": ""})


def rooms_mutation(body: dict) -> bool:
    """QA hook: mutate the room fixture so a rooms_updated push is observable.
    {"rooms": [{id,name,sort,home_id}], "set_room": {devId: room_id}}."""
    with LOCK:
        rows = body.get("rooms")
        if isinstance(rows, list):
            ROOMS[:] = [{**r, "device_count": 0} for r in rows]
        moved = body.get("set_room")
        if isinstance(moved, dict):
            for dev_id, room_id in moved.items():
                dev = state["devices"].get(dev_id)
                if dev is not None:
                    dev["room_id"] = room_id or ""
    return True


def main() -> int:
    try:
        load_asset_image()
    except (FileNotFoundError, ValueError) as e:
        print(f"error: {e}")
        print("run: python scripts/build_web.py")
        return 1
    seed_mock_logs()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    server.daemon_threads = True
    threading.Thread(target=status_pusher, daemon=True).start()
    threading.Thread(target=log_feeder, daemon=True).start()
    if PRESENCE_DRIVER:
        threading.Thread(target=presence_driver, daemon=True).start()
        print("QA: MOCK_PRESENCE armed (device_online flips every ~6s)")
    print(f"mock server: http://{HOST}:{PORT}/  (WS: ws://{HOST}:{PORT}/ws)")
    with IMAGE_LOCK:
        print(f"serving dist/web_assets.bin (build_id={ASSET_IMAGE['info']['build_id']})")
    print("QA helper: GET /api/mock/commands shows every command received")
    print("QA helper: GET /api/logs?lines=500 (mock ring, +1 line/2s)")
    print("QA helper: POST /api/asset/upload swaps the in-memory image")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
