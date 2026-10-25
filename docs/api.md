# API Reference — REST + WebSocket

The hub's complete contract: **33 REST endpoints** (prefix `/api`) and a **WebSocket `/ws`** pushing state in realtime. Everything runs right on the ESP32 — no separate server.

## Overview

- **Base URL**: `http://tuyahub-<xxxx>.local` (mDNS — `<xxxx>` = the last 4 hex characters of the MAC, lowercase) or the hub's IP on your home WiFi (see the serial log). Default HTTP port: **80**.
- **Auth**: every `/api/*` endpoint requires an API key:
  - REST: header `Authorization: Bearer <API key>`
  - WebSocket: query `?token=<API key>` (URL-encode the key if it contains special characters)
  - **The initial API key is randomly generated per hub** (8 characters, created at first boot or after a factory reset) — shown on the WiFi setup portal page and in the serial log (the `Initial API key: ...` line). That serial line appears on the **boot that generates the key** only (first boot, or the boot after a factory reset); later boots and reboots never print it again, while the setup portal keeps showing the key as long as it has not been rotated. The hub checks the key against its stored SHA-256; the initial key's plaintext is also kept in the encrypted NVS (so the setup portal can display it) until the first rotation erases it. Rotate the key in the web UI (**Settings → Security**) or via `POST /api/settings/apikey`.
  - **Before the first key change** (`auth.initial = true` in `GET /api/status`): the web UI **forces a key change right on the login page** before letting you into the app. This is the normal state of a brand-new hub — no endpoint is blocked, only the UI redirects.
  - Wrong/missing key → `401`, answered immediately — the per-IP lockout below is the protection against key guessing.
  - **Key-guessing protection (rate limit + lockout)**: more than **10 wrong keys within 60 s**, counted per IP → that IP is locked out for **300 s**: every request from it gets a `401` immediately, **before** the key is checked (even a correct key is rejected). REST and `/ws` share this same counter; a successful login before lockout resets the counter.
- **Format**: JSON, UTF-8. Every response carries `Cache-Control: no-store`.
- **Body limits** (Content-Length is mandatory for POST; chunked upload is not supported):
  - `POST /api/devices/{id}/commands`: ≤ **8 KB**; `POST /api/discovery/commit`: ≤ **64 KB**
  - `POST /api/openapi/credentials`: ≤ **512 B**
  - All other small POSTs (`auth/start`, `scenes/*/trigger`, `settings/*`): ≤ **256 B**
  - `POST /api/asset/upload`: ≤ the assets partition size (1 MB); `POST /api/ota/upload`: ≤ the OTA slot size (~3.9 MB)
- **25 s timeout**: mutation POSTs (entities, remove, discovery, auth/start) run as marshalled jobs on the main event loop, while commands that wait on the cloud (commands, scene trigger, auth/unlink) run on a worker task — the hub waits at most **25 s** (discovery commit alone: up to **~60 s** — **30 s** for the detail pre-fetch + **30 s** for the registry write); past the deadline it returns `504` (a job on the worker task still completes after the `504`; a job on the main event loop's drop-oldest queue is rarely dropped even under sustained queue overload).
- **Unknown `/api/*` path** → JSON `404`; non-API paths → the SPA `index.html` is served (web UI).

### Error schema

Standard errors (400/404/408/409/500/502/504):

```json
{"success": false, "error": {"code": "not_found", "message": "Unknown device id"}}
```

408 is a special case: it returns **plain text** (not JSON) when the socket times out mid-way while the hub is reading the request body.

The 401 error (special — flat, `error` is not nested):

```json
{"success": false, "error": "unauthorized", "message": "Invalid API key"}
```

### Table of contents

- [1. Auth & account linking](#1-auth--account-linking)
- [2. Devices & Scenes](#2-devices--scenes)
- [3. Discovery — scanning & adding devices](#3-discovery--scanning--adding-devices)
- [4. Settings](#4-settings)
- [5. Tuya IoT Platform (optional)](#5-tuya-iot-platform-optional)
- [6. Asset & OTA firmware](#6-asset--ota-firmware)
- [7. System](#7-system)
- [8. WebSocket `/ws`](#8-websocket-ws)
- [9. Error code table](#9-error-code-table)

---

## 1. Auth & account linking

QR login (Device Sharing) and API key management.

### POST /api/auth/start

Start a QR login with a User Code from the Smart Life app. **Asynchronous**: the handler replies immediately; the UI polls `GET /api/auth/status` to pick up the QR payload.

```json
// Request (≤256 B)
{"user_code": "1A2B3C"}
```

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | body is not JSON / `user_code` empty or longer than 64 characters |

### GET /api/auth/status

A snapshot of the login engine. During the onboarding wizard, the UI polls this endpoint every ~1 s. Each QR is valid for ~180 s (the hub itself polls the Tuya cloud every ~3 s); if it is never scanned, the hub refreshes it — at most 2 times, so the whole login window lasts up to ~540 s. The web UI repaints the QR and restarts its countdown whenever `qr_payload` changes.

```json
// Response 200
{
  "state": "waiting_scan",
  "user_code": "1A2B3C",
  "qr_payload": "tuyaSmart--qrLogin?token=...",
  "username": "",
  "error": ""
}
```

- `state`: `idle` · `waiting_scan` · `linked` · `failed`
- `qr_payload`: the string encoded into the QR (empty when not available yet); scan it with the Smart Life app.

### POST /api/auth/unlink

Unlink the account: revoke the token on the cloud side + delete the credentials in the NVS namespace `tuya` (the device registry is **kept intact**). The revoke runs on a worker task; HTTP blocks for at most 25 s.

```json
// Response 200
{"success": true}
```

### GET /api/settings/apikey

API key status — the key is never returned by any API endpoint (the hub stores its SHA-256, plus the initial key's plaintext in encrypted NVS until first rotation).

```json
// Response 200
{"initial": true}
```

`initial: true` while the hub still uses the randomly generated initial API key (never rotated, or right after a factory reset).

### POST /api/settings/apikey

Rotate the API key (≤256 B). You must authenticate with the current valid key. After the change, **every open HTTP/WS session is closed** — including WebSocket sessions that authenticated with the old key but never sent a frame. The hub sends no follow-up reload: a browser dashboard reconnects its socket (no full `/api/devices` reload follows — see the note at the end of this section), and REST clients simply use the new key for their next request.

```json
// Request
{"new": "my-new-key-123"}
```

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | `new` is not a printable string of 8–64 characters, or starts/ends with a space/tab |

### Initial API key and first login (mandatory key change)

There is no factory-default key — each hub generates **one random 8-character initial API key** at first boot (or after a factory reset). While this key is in effect:

- `GET /api/status` returns `auth.initial: true`; `GET /api/settings/apikey` returns `{"initial": true}`.
- The initial API key is shown on the **WiFi setup portal page** (192.168.4.1 — displayed whenever the hub enters setup mode) for as long as the key has not been rotated. In the **serial log** the `Initial API key: ...` line appears once, on the boot that generated the key (first boot or the boot after a factory reset) — a later boot or reboot does not print it again (rotate the key after the first login). Write it down: if it is lost, the hub still accepts it, but no API endpoint returns it — the setup portal is the only place left to read it.
- Every endpoint still works normally with the initial API key — there is no blocklist. **Protecting the "everyone-knows-this-key" window** relies on the key's randomness: only someone who can read the portal/log (WPA2 + serial) can know the key.
- The web UI uses the `initial` flag to **force a key change on the login page the first time**; once the key is rotated the flag returns to `false` and the initial API key no longer works.

After a successful key change, the new key takes effect immediately — no reboot needed.

**What a browser sees after the change.** The hub closes every open HTTP/WS session. The `200 {"success":true}` write-back and the close requests for the rotation's own connection compete on the same socket, so that connection normally receives the answer first (ordering is typical, not guaranteed — a client should treat a closed socket after a rotation as success and switch to the new key). The shipped web UI already knows the new key it just submitted, so it keeps working: its socket reconnects automatically (with backoff), and its data refreshes again on the next event-driven refresh (`device_added`/`device_removed`, a device-delete from the UI, or a finished scan). Nothing forces a `/api/devices` reload on reconnect, so give the dashboard a manual refresh (the browser's reload button) or open another tab if you want an immediate repaint. A client that does not know the new key starts collecting `401`s — the shipped UI answers that by clearing its stored key and showing the login gate.

---

## 2. Devices & Scenes

### GET /api/homes

The account's list of homes after linking.

```json
// Response 200
[{"id": "40213", "name": "My home"}]
```

### GET /api/devices?home_id=

Devices by home (omit the query = all). The response is **chunked**, with a budget of ~512 KB (past it the list is truncated but still valid JSON). Disabled entities (`enabled=false`) **do not appear** in `status`/`status_range`.

```json
// Response 200
[
  {
    "id": "vdev23180903",
    "name": "Living room light",
    "category": "dj",
    "online": true,
    "support_local": false,
    "home_id": "40213",
    "room_id": "178870394",
    "room_name": "Living room",
    "status": {"switch_led": true, "bright_value": 255},
    "status_range": {
      "switch_led": {"type": "Boolean", "values": "{}", "name": "Light", "writable": true},
      "bright_value": {"type": "Integer", "values": "{\"min\":10,\"max\":255}", "name": "Brightness", "writable": true}
    }
  }
]
```

- `status`: current values keyed by datapoint code (bool/number/string).
- `room_id` / `room_name`: the room the device sits in, as reported by the **Tuya IoT Platform** room lists (`""` when the device is unassigned, or when rooms were never fetched — the hub only knows rooms while Tuya IoT is connected; see `GET /api/rooms`). `room_name` is resolved per response and never stored, so a renamed room shows up on the next request.
- `support_local`: `true` for devices that report raw datapoint IDs over MQTT (`/pen` topic form) and need per-dpId conversion — a cloud-path flag describing how the hub talks to the Tuya cloud, not a local-transport capability. The shipped dashboard does not render it; the discovery commit request carries it back on staged rows (see `POST /api/discovery/commit`).
- `status_range`: metadata for **every enabled entity** — the hub merges the model's function map and status_range map, so a code that appears only in the function map is still listed here. Each entry:
  - `type`: `Boolean` / `Integer` / `Enum` / `String` / `Json` / `Raw` / `Bitmap`.
  - `values`: the **raw JSON string** from the server (`{"range":[...]}` or `{"min":...,"max":...}`) — the client parses it itself.
  - `name`: the display name taken from the model; if the model has no name, the code itself is used — never empty.
  - `writable`: `true` if the code is in the function map (it can be commanded), `false` if the code only reports state.

Per-entity rows as objects (`{code, name, type, writable, value, enabled}` — including disabled entities, which `status`/`status_range` omit) are only served by `POST /api/devices/{id}/entities` (empty body = snapshot); `GET /api/devices` never returns them.

A device added via a Device Sharing push (`bindUser` — someone shares a device to your account in the app while the hub is linked) gets its `home_id` assigned right away when the account has a single home. Accounts with several homes see the new device with an empty `home_id` first; it is back-filled when the device's model refreshes on a later scan (a scan that finds nothing new does not rewrite the device).

### POST /api/devices/{id}/commands

Send control commands (≤8 KB). A command aimed at a disabled entity is blocked **before** being sent to the cloud.

```json
// Request
{"commands": [{"code": "switch_led", "value": true},
              {"code": "bright_value", "value": 200}]}
```

- `value`: bool / number (integer or real) / string — must not be `null`.
- Commands are deduped for 10 s: a duplicate within 10 s still returns `success` (nothing is sent again).

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `404 not_found` | id is not in the registry / wrong path |
| `400 invalid_param` | bad JSON, empty `commands`, missing `code`/`value` |
| `400 entity_disabled` | command aimed at a disabled entity |
| `502 command_failed` | the cloud rejected it |
| `504 timeout` | the command job didn't finish within 25 s (the command may still complete) |

### POST /api/devices/{id}/entities

View / change the displayed entity (datapoint) list of a device.

- **Empty body** (Content-Length: 0) = read the snapshot:

```json
// Response 200
{
  "id": "vdev23180903",
  "entities": [
    {"code": "switch_led", "name": "Switch LED", "type": "Boolean", "writable": true, "value": true, "enabled": true},
    {"code": "work_mode", "name": "Mode", "type": "Enum", "writable": true, "enabled": false}
  ]
}
```

- **Body `{"entities": {code: bool}}`** = enable/disable entities; the response returns the **new** list:

```json
// Request (≤8 KB)
{"entities": {"switch_led": true, "work_mode": false}}

// Response 200
{"success": true, "entities": [ ... ]}
```

| Error | Meaning |
|---|---|
| `404 not_found` | id is not in the registry |
| `400 invalid_param` | bad JSON / a value that is not a bool |

### POST /api/devices/{id}/remove

Remove a device from the registry + save the store (LittleFS partition `storage`). The mutation runs on the main event loop.

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `404 not_found` | id is not in the registry |
| `500 internal` | race (the device was deleted mid-flight by another request) |

### GET /api/rooms

The rooms the hub currently knows, across all homes (chunked). Rooms are **not** part of the Device Sharing API: they come from the **Tuya IoT Platform** room lists (`GET /v1.0/homes/{home_id}/rooms` + `.../rooms/{room_id}/devices`), so the list is filled once Tuya IoT is connected (boot, or `POST /api/rooms/refresh`). An **empty array means "rooms not fetched yet"**, not "this account has no rooms" — a client that has no rooms must render its flat device list.

```json
// Response 200
[{"id": "178870394", "name": "Living room", "sort": 1, "home_id": "40213", "device_count": 3}]
```

- `id` / `name`: cloud room id and name (`name` may be empty).
- `sort`: the cloud display order (`0` = unsorted).
- `home_id`: the home the room belongs to — room ids are only unique **within** a home, so key any client-side grouping on `home_id + id`.
- `device_count`: devices the hub currently places in that room (from the last complete room sweep).

### POST /api/rooms/refresh

Ask the hub to re-read the rooms and their devices from the Tuya IoT Platform (one request per home + one per room, runs on its own task). The result arrives as the `rooms_updated` (rooms) and `device_added` (each device whose room changed) WS events.

```json
// Response 202
{"success": true}
```

| Response | Meaning |
|---|---|
| `202 Accepted` | a refresh task was started |
| `200 OK` + `{"success": true, "already_running": true}` | one is already in flight (single-flight) |
| `500 internal` | the refresh hook is not wired |

The list is also refreshed automatically at boot, at most once every 5 minutes while linked (it rides along with the presence refresh, which runs at boot, every 10 seconds while linked, after a successful command and after an MQTT (re)connect), and on demand. The refresh is skipped silently (log line only, `202` still returned) while Tuya IoT is not connected, and a home whose room call fails keeps its previous assignments — only a complete, successful home sweep may clear a device's room.

### GET /api/scenes?home_id=

Scenes by home from the **cache** (no cloud fetch; omit the query = all). If a scene fetch fails for only some homes, the hub keeps the successful homes' scenes and retries the fetch — up to 3 times, 30 s apart — before giving up with a log message.

```json
// Response 200
[{"id": "scene_883", "name": "Go to sleep", "enabled": true, "home_id": "40213"}]
```

### POST /api/scenes/{id}/trigger

Run a scene. The body is **optional**: `{"home_id": "..."}`; if omitted, the hub looks up the scene's home from the cache itself.

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `404 not_found` | unknown scene id and no `home_id` attached |
| `502 command_failed` | the cloud refused to run the scene |

---

## 3. Discovery — scanning & adding devices

Flow: `scan` → poll `candidates` → expand each device where you need to pick entities → `commit`. The scan only **stages** list rows (id, name, current status) — the full model of **each** device (specifications, status strategy, dp-report-types + Tuya IoT enrich) is fetched lazily when the detail endpoint is called, or right before commit.

### POST /api/discovery/scan

Start a scan (no body needed) — it only fetches the list of homes + the devices in each home, so the list shows up within ~2–3.5 s. The scan runs on its own task; follow its progress via `GET /api/discovery/candidates` or the WS event `discovery_progress`.

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `409 discovery_running` | a scan is already running |

### GET /api/discovery/candidates

The result of the **most recently completed** scan (chunked, budget ~512 KB).

```json
// Response 200
{
  "active": false,
  "done": true,
  "failed": false,
  "candidates": [
    {
      "id": "vdev23180903",
      "name": "Living room light",
      "category": "dj",
      "product_name": "Smart Bulb",
      "online": true,
      "home_id": "40213",
      "room_id": "",
      "room_name": "",
      "already_added": false,
      "detail_loaded": false,
      "entities": [
        {"code": "switch_led", "name": "", "type": "", "writable": false, "value": true, "enabled": true}
      ]
    }
  ]
}
```

- `already_added: true` — the id is already in the registry (ticking it again is harmless; at commit it counts as skipped — or gets refreshed when the staged detail is fully loaded, strictly richer than the registry's device, and no model map would shrink: `status`, `status_range`, `function`, and `local_strategy` when the candidate reports local support).
- `detail_loaded: false` — the full model has not been fetched yet; `entities` then only contains the codes already present in the list row's `status` (incomplete). It becomes `true` only after a fully loaded fetch (detail endpoint or commit); a degraded fetch keeps it `false`.

### GET /api/discovery/candidate/detail?id=<dev>

Fetch the full model of **one** staged candidate, then update it into staging (idempotent — calling again once loaded is a no-op, no HTTP call). Takes ~1.2 s; it runs serially, so two concurrent calls → the later one gets `409`. The response is exactly the candidate object as in `candidates`, with `detail_loaded: true` — or `false` when the fetch came back degraded (see below).

```json
// Response 200
{
  "id": "vdev23180903",
  "name": "Living room light",
  "category": "dj",
  "product_name": "Smart Bulb",
  "online": true,
  "home_id": "40213",
  "room_id": "",
  "room_name": "",
  "already_added": false,
  "detail_loaded": true,
  "entities": [
    {"code": "switch_led", "name": "Switch LED", "type": "Boolean", "writable": true, "value": true, "enabled": true}
  ]
}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | missing query `id` |
| `400 no_scan` | no completed scan yet |
| `400 discovery_active` | a scan is running — wait for it to finish |
| `404 not_found` | id is not a staged candidate |
| `409 detail_fetch_running` | another detail call is running — retry later |
| `504 timeout` | past 15 s (the hub still finishes the fetch in the background — a fully fetched detail is cached, a degraded one is retried) |

If any cloud call fails → the degraded model (missing the corresponding map) is still returned with `detail_loaded: false`, along with a warning in the log — the client can render the parts it has. A degraded fetch is not cached as loaded: the next detail call (or the commit pre-fetch) retries the full fetch.

### POST /api/discovery/commit

Add the selected devices (≤64 KB). Omitting the `entities` field = enable **all** entities of that device; if `entities` is present, it must be a **non-empty** array of codes. An id already added, or duplicated within the same request → `skipped` (idempotent). Exception: when the staged detail is fully loaded, strictly richer than the registry's device and no model map would shrink (`status`, `status_range`, `function`, and `local_strategy` when the candidate reports local support), the commit refreshes it with the staged data instead of skipping — a degraded/partial detail never replaces the registry model.

Before writing the registry, the hub fetches the full model of every selected device whose detail was never loaded (serially, ~1.2 s/device). There are two waiting steps — the detail pre-fetch and the registry-write job — each capped at 30 s, so the hub can block for up to ~60 s in the worst case (the web UI only waits 30 s and then gives up). Past the pre-fetch step → `504` and **not a single device has been added** (the fetch still completes on the hub; a re-commit is fast when the detail finished loading — a degraded result is not cached, it is retried). Past the registry-write step → `504`, but the job usually still completes afterwards (the drop-oldest queue rarely drops the job even under sustained overload) — the devices are still added, and a re-commit only sees `skipped`. A total detail-fetch failure → the device is still added with a degraded model when the failure lands within the 30 s pre-fetch budget (the degraded model stays uncached — a later commit retries it); a slower failure instead exhausts the budget → the pre-fetch `504` above, nothing added.

```json
// Request
{"devices": [
  {"id": "vdev23180903", "entities": ["switch_led", "bright_value"]},
  {"id": "vdev23180904"}
]}
```

```json
// Response 200
{"success": true, "added": 2, "skipped": 0}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | bad JSON, empty `devices`, unknown id (not a staged candidate), empty `entities` |
| `400 discovery_active` | a scan is running — wait for it to finish |
| `400 no_scan` | no completed scan yet |
| `504 timeout` | the detail pre-fetch or the registry-write job didn't finish within 30 s |

---

## 4. Settings

### GET /api/settings/language

```json
// Response 200
{"lang": "vi"}
```

### POST /api/settings/language

```json
// Request (≤256 B)
{"lang": "en"}

// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | `lang` is not `"vi"` or `"en"` |

Stored in the NVS namespace `hub`; persists across reboots.

---

## 5. Tuya IoT Platform (optional)

Endpoints for the Tuya IoT Platform integration (adds datapoints for some devices, such as power-measuring switches). Entirely optional — the hub still runs fully if you skip it.

### GET /api/openapi/status

A merged snapshot for the UI: configuration status + token + quota. The first query after boot reloads the credentials/token from NVS; if configured but there is no token yet, the hub spawns a background login on its own.

```json
// Response 200
{
  "configured": true,
  "has_token": true,
  "login_in_flight": false,
  "uid": "eu1582xxxx",
  "endpoint": "https://openapi.tuyaus.com",
  "quota": {"month": "2025-06", "devices": 5, "limit": 10, "warning": false}
}
```

- `month`: `""` when not tracked yet (clock not synced).
- `quota.warning`: past 80% of the managed-device quota.

### GET /api/openapi/quota

A slim variant of quota (the new UI should use `/openapi/status`).

```json
// Response 200
{"configured": true, "month": "2025-06", "devices": 8, "limit": 10, "warning": true}
```

### POST /api/openapi/credentials

Save credentials (≤512 B), then **spawn a background login** — returns 200 immediately; poll `/api/openapi/status` for the outcome. Credentials are saved to NVS **before** the login (a failed login doesn't lose the form).

```json
// Request
{
  "access_id": "xxxx",
  "access_secret": "yyyy",
  "username": "me@example.com",
  "password": "secret",
  "country_code": "1",
  "schema": "smartlife",
  "datacenter": "https://openapi.tuyaus.com"
}
```

- Required: `access_id`, `access_secret`, `username`, `password`. Optional: `country_code`, `schema`, `datacenter`.

| Error | Meaning |
|---|---|
| `400 invalid_param` | one of the 4 required fields missing / a field is not a string / `datacenter` is not one of the known Tuya IoT hosts |

### POST /api/openapi/clear

Clear the whole domain: credentials + token in NVS and the in-memory client. Afterwards the state is back to "not configured".

The wipe runs on a worker task. While a Tuya IoT login is in flight (it can hold the client's lock for up to ~2 minutes), this request may time out with `504 timeout` (`"Tuya IoT clear timed out"`) — the clear still completes on the hub afterwards.

```json
// Response 200
{"success": true}
```

---

## 6. Asset & OTA firmware

Updating the web UI (the `assets` partition) and the firmware (the OTA slot) over HTTP.

### GET /api/asset/info

Snapshot of the WAS1 image currently running in the `assets` partition.

```json
// Response 200
{
  "success": true,
  "asset": {
    "valid": true,
    "version": 1,
    "build_id": "a1b2c3",
    "files": 12,
    "payload_len": 104296,
    "partition_size": 1048576
  }
}
```

### POST /api/asset/upload

Upload a new web UI image (the body is a **binary .bin file**, Content-Length required, ≤ the assets partition size ~1 MB). Validated in RAM (magic/version/TOC/CRC32) before being written to flash. **No reboot** — the new image takes effect immediately (the store re-mounts and serves).

```json
// Response 200
{"success": true, "asset": { ... }}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | no Content-Length / empty body |
| `400 too_large` | body larger than the maximum size (here: the assets partition, ~1 MB) — `"Body exceeds the maximum size"` |
| `400 invalid_asset` | not a valid WAS1 image — `"Not a valid WAS1 asset image"` (magic/version/TOC/CRC32 check failed) |
| `500 internal` | the image could not be buffered, or the flash write failed — `"Out of memory"` / `"Asset flash write failed"` |

### GET /api/ota/info

The running slot / next slot + app description.

```json
// Response 200
{
  "success": true,
  "ota": {"running": "ota_0", "next": "ota_1", "slot_size": 4128768, "version": "0.1.0", "build_time": "Jun  1 2025 10:20:30"}
}
```

### POST /api/ota/upload

Upload new firmware (binary body, ≤ OTA slot ~3.9 MB). The hub reads the whole body into RAM, then validates the 24-byte header (magic 0xE9 + ESP32-S3 chip) **before** writing; it writes to the OTA slot in 16 KB chunks; `esp_ota_end` verifies (checksum + SHA) and then sets it as the boot partition. **Does not reboot by itself** — the client calls `POST /api/system/reboot` when ready.

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `400 invalid_param` | missing Content-Length / empty body / shorter than the 24-byte header / wrong magic / not an ESP32-S3 image |
| `400 too_large` | the image exceeds the OTA slot |
| `400 invalid_asset` | final verify failed (corrupt image) |
| `500 ota_unavailable` | no OTA slot available — `"No OTA slot available"` (also: OTA begin/write failed, or setting the boot partition failed) |
| `500 internal` | out of memory — `"Out of memory"` (the image or the 16 KB write buffer could not be allocated) |

### POST /api/ota/rollback

Mark the running image as INVALID in `otadata` (no erase) → on the next boot the bootloader will enter **another still-valid OTA slot** (the previously working image), which is not always `ota_0`. It replies first and reboots after ~800 ms. If no valid slot is left to roll back to → `500 ota_unavailable` and the hub **does not reboot** (a pending-verify window is the exception: the running slot is marked VALID first, so that rollback can still succeed).

```json
// Response 200
{"success": true}
```

| Error | Meaning |
|---|---|
| `500 ota_unavailable` | rollback not possible (no valid slot left / running from factory / otadata write error) |

---

## 7. System

### GET /api/status

The hub's overall snapshot — the dashboard's primary data source.

```json
// Response 200
{
  "version": "0.1.0",
  "language": "vi",
  "state": "linked",
  "wifi": {"connected": true, "ssid": "MyWiFi", "ip": "192.168.1.20", "rssi": -52},
  "heap": {
    "internal_free": 185000, "internal_min_free": 96000,
    "internal_total": 327680, "internal_largest": 110592,
    "psram_free": 7810000, "psram_total": 8388608, "psram_largest": 8126464
  },
  "cpu": {"usage": 12, "core0": 10, "core1": 14},
  "uptime_ms": 654321,
  "auth": {"state": "linked", "username": "me@example.com", "qr_payload": "", "initial": false},
  "mqtt": {"connected": true, "error": ""},
  "discovery": {"active": false, "percent": 100, "done": true, "failed": false, "candidates": 0},
  "home_count": 1,
  "device_count": 8,
  "scene_count": 3
}
```

- `state`: `starting` · `wifi_config` · `needs_setup` · `idle` · `cloud_connecting` · `linked` · `fatal_error`
- `auth.initial`: `true` while the hub still uses the unchanged initial API key (see [Initial API key](#initial-api-key-and-first-login-mandatory-key-change)).
- `cpu`: **absent** for the first ~5 s after boot (waiting for the monitor's first sample).
- `mqtt`: the live MQTT connection state (`connected: false` while the hub is offline/reconnecting).

### GET /api/logs?lines=N

The hub's log as shown on the web (an in-RAM ring buffer). `lines`: 1–500, default 200; a non-numeric value → the default is kept. Chunked response.

```json
// Response 200
{"total": 1200, "lines": [
  {"t": 12345, "lvl": "I", "tag": "RestApi", "msg": "register /api/status: esp_ok"}
]}
```

- `t`: ms since boot; `lvl`: 1 character (`I/W/E/D`); continuation lines keep the previous line's level.

### POST /api/system/reboot

Restart the hub. **Replies first**, reboots after 800 ms (so the response reaches the client).

```json
// Response 200
{"success": true}
```

### POST /api/system/factory-reset

Clear the hub's credentials and device registry: NVS namespaces `wifi`/`tuya`/`hub`/`openapi` (WiFi including the setup-AP passphrase, Tuya token, language, API key — on the next boot the hub generates a **new, random initial API key** and a fresh setup-AP passphrase) + the WiFi driver's own stored settings (its `nvs.net80211` namespace — the saved home WiFi credentials, best effort via `esp_wifi_restore()`) + the registry store on LittleFS + the coredump partition (a dump left by an earlier crash is erased, best effort), then reboots. Other namespaces are untouched. The registry wipe is best-effort: if deleting it fails, the storage partition is erased instead, and if that fails too (or the partition is missing) the reset still reports success — the old registry can survive the reboot.

The reset does **not** shred anything: the registry store and the coredump partition are erased with a `0xFF` overwrite (the old bytes remain recoverable in principle by carving the flash chip; the whole storage partition is erased only when the registry delete fails). **Both OTA slots keep their firmware images** — the reset endpoint never erases the app partitions — so after the reboot the hub runs the slot `otadata` points at. One `POST /api/ota/rollback` after the reset still switches slots (the running slot is marked INVALID); after that reboot both slots are INVALID, the bootloader falls back to `ota_0` no matter which image that is, and a second consecutive rollback returns `500 ota_unavailable` in that state (inside a pending-verify window it can still succeed, because the running slot is marked VALID before the other one is checked).

The registry snapshot is sealed with the at-rest key in the eFuse, which survives the reset: on a hub whose snapshot was written through the normal load path (any hub that has or had devices) the surviving bytes cannot be read without that key. Edge case: a snapshot left at first boot without a single device is never re-sealed, so if it survives the reset it stays plaintext.

```json
// Response 200
{"success": true}
```

After the reset: the hub broadcasts the `TuyaHub-XXXX` AP again; set it up from scratch (see [getting-started.md](getting-started.md)).

---

## 8. WebSocket `/ws`

The realtime push channel from the hub down to the UI/integrator.

### Connection

```
ws://tuyahub-<xxxx>.local/ws?token=<API key>
```

- Auth via the `?token=` query — checked **before** the handshake completes; a wrong/missing token → HTTP `401` (body like REST), answered immediately, and the upgrade never completes. A wrong token also counts toward the per-IP lockout counter (see the Auth section above): a locked-out IP is rejected before the token is checked.
- **Right after connecting, the client must send one text frame `"ping"`** to register the session (ESP-IDF completes the WS handshake without invoking the handler — the hub only sees the client once the first frame arrives). The server replies `"pong"`.
- Alternative heartbeat: send `{"type":"ping"}` → receive `{"event":"pong"}`.
- WS PING control frame (RFC 6455) → the hub answers with a PONG echo on its own.
- A client frame > **256 bytes** → dropped + disconnected.
- Every broadcast is gated per-send: the hub re-checks that each tracked socket is still a live WebSocket before writing to it. A socket the web server has since reused for a plain HTTP session is skipped and untracked — WS bytes never splice into an HTTP response.
- At most 10 sockets open at the same time (the cap leaves room for one live WS plus a burst of REST calls; an older idle socket is closed first, longest-unused wins, so raise this if you run many parallel connections).

### Server → client event format

```json
{"event": "device_update", "data": {"devId": "vdev23180903", "props": [{"code": "switch_led", "value": true, "t": 654321}]}, "timestamp": 654321}
```

- `timestamp`: ms since the hub booted.
- `data` is always an object (an event without a payload → `{}`).

### Event table

| Event | `data` | When it is pushed |
|---|---|---|
| `status` | identical to the `GET /api/status` response | to every **registered** WS session (one that has sent its `ping`) **every 10 s** + **immediately** when the hub's `state` changes. The shipped web UI polls `GET /api/status` itself (every 5 s) only while its socket is down |
| `auth_status` | identical to `GET /api/auth/status` (`state, user_code, qr_payload, username, error`) | each step of the QR login (idle → waiting_scan → linked/failed, unlink) |
| `discovery_progress` | `{active, percent, done, failed, homes_done, homes_total, devices_done, devices_total, error}` | continuously during the scan (on each step of progress) + the terminal done/failed event |
| `mqtt_status` | `{connected, error}` | when the MQTT broker connects/disconnects/reconnects |
| `device_added` | `{devId}` | discovery commit, registry restore, or a model/rename refresh of an already-added device — fired only when something actually changed (a no-op save with the same name and the same entity selection sends no event and does not re-save the registry snapshot). The payload is `{devId}` alone: the shipped web UI cannot build a card from it, so a known id triggers one debounced silent refresh (~300 ms) of `/api/homes`, `/api/devices`, `/api/scenes` and `/api/status`, which patches only the structurally changed panels |
| `device_removed` | `{devId}` | `POST /api/devices/{id}/remove`, or a cloud MQTT push reporting the device was deleted — the shipped web UI drops the device from its model and removes exactly that one card node, no REST call |
| `device_update` | `{devId, name, props: [{code, value, t}]}` | MQTT push reporting a datapoint change (dedup; `t` = report timestamp, `0` if never reported) |
| `device_online` | `{devId, online}` | online/offline change (MQTT push, or a presence refresh: at boot, every 10 seconds while linked, after a successful command, and after an MQTT (re)connect) |
| `rooms_updated` | `{home_id}` | the room list of a home changed (a room refresh after boot/`POST /api/rooms/refresh`, a rename, a reorder). Clients refetch `GET /api/rooms` (and `GET /api/devices` for the new `room_id`/`room_name`) — the payload carries no room data |

Minimal client example (JS):

```js
const ws = new WebSocket(`ws://${location.host}/ws?token=${encodeURIComponent(key)}`);
ws.onopen = () => ws.send('ping');          // SESSION REGISTRATION — mandatory
ws.onmessage = (e) => {
  const {event, data, timestamp} = JSON.parse(e.data);
  if (event === 'pong') return;             // heartbeat reply
  console.log(event, data);
};
```

---

## 9. Error code table

| HTTP | `error.code` | Context |
|---|---|---|
| 400 | `invalid_param` | bad JSON, missing/mistyped field, empty/over-limit body, wrong OTA image magic |
| 400 | `entity_disabled` | command to a disabled entity |
| 400 | `discovery_active` | a scan is running (candidate detail / commit cannot run during a scan) |
| 400 | `no_scan` | commit (or candidate detail) with no completed scan |
| 409 | `discovery_running` | scan while a scan is already running |
| 409 | `detail_fetch_running` | GET candidate detail while another fetch is running |
| 400 | `too_large` | body over the limit (asset/OTA) — `"Body exceeds the maximum size"` |
| 400 | `invalid_param` | `"Empty body"` — a zero-length body on a POST that requires one: `/api/auth/start`, `/api/openapi/credentials`, `/api/settings/language`, `/api/settings/apikey`, `/api/discovery/commit`, `/api/devices/{id}/commands`. `POST /api/devices/{id}/entities` accepts an empty body and answers with that device's snapshot instead; `POST /api/scenes/{id}/trigger` treats it as no `home_id`; `/api/system/reboot`, `/api/system/factory-reset`, `/api/auth/unlink`, `/api/openapi/clear` and `/api/discovery/scan` never read the body (anything sent is ignored) |
| 400 | `invalid_asset` | WAS1/OTA image that fails verification (asset: `"Not a valid WAS1 asset image"`) |
| 401 | `unauthorized` *(flat)* | wrong/missing bearer token (API key) or WS `?token=` (a locked-out IP also returns this 401 — it fails before the key check) |
| 404 | `not_found` | id/path does not exist |
| 500 | `internal` | out of memory (`"Out of memory"` on asset/OTA upload), rare race, scheduler not wired |
| 500 | `ota_unavailable` | no OTA partition/slot (`"No OTA slot available"`), OTA begin/write/boot-partition failure, or rollback not possible (no valid slot left) |
| 502 | `command_failed` | the cloud rejected a command or scene |
| 504 | `timeout` | a job (main event loop or worker task) didn't finish within 25 s (commit: 2 steps × 30 s) — a worker-task job still completes after the 504; a job on the drop-oldest main queue is rarely dropped even under sustained overload |

---

*Code cross-check: `components/web_server/src/rest_api.cc` (31 httpd routes — the 2 sub-route wildcards expand into 4 paths → 33 logical endpoints; with `/ws` and the static asset wildcard that fills the 33-handler httpd cap exactly), `ws_handler.cc` (/ws), `api_auth.cc` (Bearer + `?token=`), `web_server.cc` (broadcast). Code is the source of truth — when an endpoint changes, update this doc.*
