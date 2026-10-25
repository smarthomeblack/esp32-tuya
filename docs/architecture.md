# Architecture

A document for contributors: a map of the hub's code, the rules for adding and changing each part, and the places where execution flows can trip over each other. Every description follows the current code — when the structure changes, this document must change with it.

## Overview

The hub runs on an ESP32-S3 (16 MB flash, octal PSRAM), built with ESP-IDF v6.0.1, C++26, `-fno-exceptions`, and warnings-as-errors (`-Werror`). The code splits into two parts: C++ firmware in `main/` + `components/`, and the web UI (vanilla JS) in `components/web_assets/src/`.

What the hub does:

- **Control Tuya devices through the cloud** (the only transport): receives device states over MQTT and sends commands through the Tuya IoT Platform HTTP API. Works with every device linked to the account — the hub talks to the Tuya cloud (MQTT + HTTP) and never to the devices directly.
- **A web UI for browsers on the home network**: REST for actions, WebSocket for realtime events, static files served from a dedicated partition. The web server announces itself over mDNS as `tuyahub-<last-4-hex-of-MAC>.local`.
- **OTA with rollback**: two app partitions (`ota_0`/`ota_1`); new firmware must pass the boot self-test before being confirmed valid, otherwise the bootloader automatically rolls back to the previous image.

The hub lifecycle is expressed through 7 states (`sys/device_state.h`): `Starting` at boot → `WifiConfig` (no stored WiFi, or the router unreachable at boot — enters the provisioning portal; exits only by reboot: the portal's own, or an automatic ~5-minute retry reboot when credentials are stored) or `NeedsSetup` (WiFi is up but no Tuya account linked) → `Idle` (running, cloud not up yet — retries on its own) → `CloudConnecting` (login/discovery/MQTT in progress) → `Linked` (fully running). `FatalError` is the final stop: an unrecoverable error, no network, fast-blinking LED. Every state change updates the LED, the `state` field of `/api/status`, and broadcasts over WS to the UI.

## Component map

The tree below reads straight from `REQUIRES`/`PRIV_REQUIRES` in each component's `CMakeLists.txt` — the arrows show the direction "this component needs that component":

```text
main                composition root: boot, wiring hook, main event loop
 │
 ├─ web_server      REST /api/ + /ws + static files + mDNS   ┐
 ├─ tuya_mqtt       cloud MQTT: receive device state         ┘
 │
 ├─ hub_services    cloud orchestration: command service,
 │                  discovery, enrichment
 │
 ├─ hub_core        pure domain core: device model, registry, store
 │
 ├─ tuya_cloud      HTTP cloud: auth, token, Tuya IoT client
 │
 ├─ sys             settings/NVS, WiFi provisioning, log, LED, self-test
 ├─ tuya_crypto     crypto (AES, HMAC, MD5) + JSON helper
 └─ web_assets      WAS1-packed SPA — standalone, depends on no one
```

| Component | Role | Project dependencies (`REQUIRES`) |
|---|---|---|
| `main` | composition root + boot | every component below |
| `web_server` | httpd: REST, WebSocket, static, mDNS | `hub_core`, `hub_services`, `tuya_cloud`, `tuya_mqtt`, `sys`, `web_assets` |
| `tuya_mqtt` | cloud MQTT client (state path) | `hub_core`, `tuya_cloud` |
| `hub_services` | orchestration: command routing, discovery, enrichment | `hub_core` (PRIV: `tuya_cloud`, `sys`, `tuya_crypto`) |
| `hub_core` | device model/registry/store — pure domain core | none (PRIV: `tuya_crypto`, `heap`) |
| `tuya_cloud` | HTTP cloud + auth + token | `tuya_crypto`, `sys` |
| `sys` | foundation: NVS, WiFi, log, self-test | none (IDF only) |
| `tuya_crypto` | crypto + JSON writer — the deepest layer | none (IDF only) |
| `web_assets` | WAS1 asset store + fallback page | none (IDF only) |

A few points worth noting:

- **Dependencies always flow downward**: entry points (`main`, `web_server`) → `hub_services` → `hub_core`/`tuya_cloud` → `sys`/`tuya_crypto`. `tuya_mqtt` holds `hub_core` directly because it only needs the domain model, not orchestration. No component "looks back up" at `main`. `tuya_crypto` is the deepest layer; `sys` also sits at the bottom but uses `tuya_crypto` privately in one place (src only, not exposed in a public header) as the JSON depth guard.
- The table above lists only `REQUIRES`; many components also have `PRIV_REQUIRES` on `tuya_crypto` to use `JsonWriter`/hash functions in their src, and `hub_services` keeps both `tuya_cloud`/`sys` privately (src only) — which creates no reverse dependency direction either.
- **`std::function` hooks are the deliberate way to avoid CMake cycles**. The clearest case:
  - *Scheduler of the web server*: `web_server` needs to call up into Application (push jobs to the main loop) but must not depend on `main`. `WebServer` only sees the `SetScheduler`/`SetCriticalScheduler` hooks — main registers them before the network comes up.

The principle when adding a new component: place it in the right layer of the tree above, declare the minimal `REQUIRES`, and if it needs to "call back up", expose a hook for main to register — never add a reverse dependency.

## Composition root

All of the hub's wiring lives in a single block of `Application::Start()` (`main/application.cc`), running after the self-test passes and before the network comes up:

- the listeners of `CloudAuthManager`, `DiscoveryService`, `DeviceRegistry`, `TuyaMqttClient` — every component event is funneled into the main event loop via `Schedule()` (see the [Threading model](#threading-model) section);
- `WebServer::SetScheduler()` + `WebServer::SetCriticalScheduler()` — REST marshalling back to the main loop; `WebServer::SetRoomRefreshTrigger()` — the room-refresh task spawn behind `POST /api/rooms/refresh`.

A new component plugging into the hub follows this exact pattern: the component exposes a `SetXxxListener`/`SetXxxHandler`, and the registration happens only in the wiring block inside `Application::Start()`. No importing components into each other, no scattering hook registrations across multiple places.

## Threading model

This is the most important part when contributing firmware — most bugs come from calling an API in the wrong context.

**The main event loop owns every state change.** The hub runs one `main_event_loop` task (12 KB stack): it drains two job queues, handles the 1 Hz clock tick, and is the only place allowed to change state, (re)start the MQTT transport, or save the registry. Any other task that wants to change state must push a job into the loop, never modify it directly.

**Two queues, two different policies:**

- `Schedule()` — a 512-slot queue that **drops the oldest job when full** (drop-oldest). Used for transient events: WebSocket broadcasts, online flag flips... Old jobs carry old state, so dropping them is harmless; when a job is dropped, it logs a WARN with the total drop count since the session started. Thanks to this policy the queue only ever holds the latest state and never bloats.
- `ScheduleCritical()` — a 16-slot queue that **never drops a job**. Used for must-run-to-completion events: saving the registry, rebooting, the MQTT resubscribe cycle. Whoever pushes a job must coalesce it themselves using a latch (one CAS flag per pending job); exceeding 16 slots means a latch is leaking — an ERROR is logged immediately and the job is still kept.

Each drain pass: transient jobs run first (the latest state gets out earliest), critical jobs run after (already coalesced, heavy). The loop registers a task watchdog and feeds it while draining — a job that hangs forever makes the watchdog PANIC and the hub reboots itself.

Besides draining, the main loop also keeps the 1 Hz clock tick — all periodic work anchors to this tick: a health log + a full snapshot broadcast over WS every 10 seconds, a stack headroom report every 60 seconds, a registry flush debounced to 60 seconds, the retry windows (cloud boot — up to 5 attempts every 30 s while parked in Idle, then one attempt every 10 minutes, the budget re-arms whenever WiFi reconnects — and MQTT self-heal), and a cloud token check every 10 minutes. To add periodic work, anchor it to the tick — don't spin up your own timer.

**APIs that may only be called from the main loop** — calling them from another context (especially an httpd handler) is a bug:

- `TuyaMqttClient::Stop()` — joins the task for up to ~5 seconds;
- `StartCloud()` — blocks for minutes (time-sync + HTTP retry inside);
- `SaveRegistryToStore()` — blocking flash write of 1–2 seconds.

The cloud HTTP leg has a 60-second timeout and no internal retry; a command chain (dedup → route → send) is capped at 30 seconds — past the deadline, the remaining legs are dropped.

**The copy-then-notify principle:**

- `DeviceRegistry` listeners are called **outside** the registry's mutex, but a listener must not call back into the registry (notify sits on the critical path of a mutation — a listener that self-Schedules is safe).
- `ForEachDevice()` is the opposite: the callback runs while the mutex **is held** — the callback must not call other registry APIs (deadlock) and must not do I/O. Copy the data out and process it afterwards. Collecting the MQTT device topics follows the same spirit: inside the callback, only copy; leave all I/O outside.

**REST handlers and marshalling** (see `components/web_server/src/rest_api_marshal.cc`):

- An httpd handler never runs heavy work itself. Light mutations are marshalled up to the main loop (`RunOnMainLoop`) and then wait on a semaphore — waiting at most 25 seconds (discovery commit: up to ~60 seconds — 30 seconds for the detail pre-fetch + 30 seconds for the registry write); blocking cloud I/O is pushed to a worker task (`RunOnWorkerAndWait`) so the main loop stays free.
- On timeout → return **504 Gateway Timeout**. Behind the scenes the job keeps running: the worker task finishes its work before releasing; a main-loop job stays in the queue — but if the queue overloads, that job may never run (drop-oldest). A retrying client sees the new state.
- A web-requested reboot also goes through a dedicated task, delayed by one tick so the response can reach the client — never `esp_restart()` right inside a handler.

**One-shot tasks** (spawn → run → self-delete): `tuya_presence` (fetches the cloud device list at boot, every 10 seconds while linked — 30/60/120-second backoff on failure — and five 1-second polls after a successful command; it heals both the online flags and the DP status, and its room sweep rides along at most every 5 minutes), `tuya_scenes` (fetches the scene list), `tuya_cloudboot` (cloud boot retry — blocks for minutes), `web_worker`, `hub_web_reboot`. Their results return to the main loop via a listener + `Schedule()`.

**Follow one control command end to end** — assembling all the rules above:

1. The UI calls `sendCommands` (`cards-commands.js`) → `POST /api/devices/{id}/commands`; the card goes into the *pending* state waiting for confirmation.
2. The httpd handler checks auth, parses the body, blocks commands aimed at a disabled entity — then pushes the send onto a worker task (`RunOnWorkerAndWait`), waiting at most 25 seconds.
3. `CommandService::SendDeviceCommands` (hub_services) runs on the worker: 10-second dedup (an exact duplicate within 10 seconds is swallowed, still returns success) → the list is split per command code by `local_strategy` (the DP-routing logic: OpenAPI-discovered DPs go to the Tuya IoT leg, the rest to the Device Sharing leg) → each leg goes through sharing/OpenAPI, the whole chain capped at 30 seconds.
4. The device receives the command → reports new state → MQTT pushes it into the registry → the registry fires an event → the main loop broadcasts `device_update` over WS.
5. The UI receives the event, patches the card in place and clears *pending*. If the cloud returns an error, REST returns 502 with details; past the 25-second wait, 504 — the command may still complete in the background.

A sharing session does not always deliver those pushes, so the receive path is **not MQTT-only**: `tuya_presence` polls the cloud device list (10 seconds while linked, five 1-second polls right after a successful command) and applies the same registry updates, so a card still converges when nothing is pushed.

Understanding this flow means understanding almost the entire runtime architecture of the hub.

## Self-test gate

Every component has its own self-test file (`self_test.cc`/`*_test.cc` in src). At boot, `main/boot_self_test.cc` runs the full gate chain in order: crypto → sys → cloud pipeline → Tuya IoT client → device model → device store → enrichment → MQTT message → command service → discovery → web assets. 949 checks in total, running **before** the hub touches the network.

- Any failed check → the hub enters the `FatalError` state, fast-blinking LED, **no WiFi/MQTT connection** — broken firmware never gets out onto the home network. The OTA rollback window stays open (the new app is not confirmed), so the next boot has the bootloader roll back to the previous image.
- All pass → only then is `esp_ota_mark_app_valid_cancel_rollback()` called — confirming the new app as valid and closing the rollback window.
- Log capture and the CPU monitor start **before** the self-test, so the failing line is already sitting in the log buffer — readable over serial, or via `GET /api/logs` on softer failed boots (the web server still comes up).

Consequences for contributors:

- A PR touching firmware must pass 949/949 on real hardware before being sent — see [docs/testing.md](testing.md) (a detailed table of each gate, how to read the log when it fails, the mock server).
- New logic means adding matching checks to that component's self-test; never lower the bar.

## Web UI

**Module structure.** The web UI is vanilla-JS ES modules, split into 3 layers under `components/web_assets/src/js/`:

- `core/` — state, event bus, i18n, utils, REST client, WS client; knows nothing about the layers above.
- `ui/` — shared widgets: toast, dialog, appbar, uploader.
- `features/` — one folder per screen/feature group: dashboard, cards, scan, onboarding, settings... The entry point is `app.js`.

Layer rules are enforced by `scripts/check_web_imports.py` in CI: every import must resolve; `core` must not import `features`; `features`→`features` is only valid within the same `cards-*`/`settings-*` family or inside the script's whitelist; cycles are only allowed within a family; an imported binding that goes unused and an export that no module imports are both flagged. The only library bundled into the UI is qrcode-generator (MIT).

**The card render pipeline** — the path a device takes to the UI:

1. When loading the device list, `app.js` calls `detectCapabilities(dev)` (`cards-caps.js`) and assigns the result to `dev._caps`. This function scans the **union** `status ∪ status_range` (every DP the device has ever reported or declared in metadata) and computes the descriptor: head switch, channels, countdown, light DPs (brightness/color), mode select, measured metrics...
2. `refineByCategory()` (`cards-refine.js`) refines by Tuya category (`kt` air conditioner, `fs` fan, `jsq` humidifier, `kj` purifier, `cs` dehumidifier, `sd` robot vacuum, `cl` curtain): adds extra DPs for a dedicated section, changes icons/labels. An unknown category → returns null, and the card continues with the default descriptor.
3. The `buildCard()` builder in `cards.js` assembles the card from the descriptor; each section builder registers its own value-updating closure by calling `registerUpdater(fn)` (`cards-registry.js`).
4. When WebSocket pushes a `device_update`, `dashboard.js` patches the card **in place** (`updateCardInPlace`): it runs the registered updaters and never replaces nodes — so CSS transitions and scroll position don't jump. Each device is debounced ~200 ms, so a burst of events paints only once. Only a structural card change (entity added/removed, descriptor changed) rebuilds the whole card. A card that throws during build is swapped for an error card — it doesn't take the whole grid down.

**The metadata render floor.** Every entity in `status ∪ status_range` must appear on the card. A DP without its own section falls to the generic floor: rendered exactly per its metadata (type, name, writable, values) — Boolean becomes a toggle, Integer with min/max becomes a stepper/slider, Enum with a range becomes a dropdown, everything else renders as a line of text. The consequence: a brand-new device type still shows all its entities in simple form — the UI works right away with no extra code; refine is just a "polish" layer on top of that floor.

**Mandatory UI rules** (CI can't catch these — review will):

- Every option list (enum) uses a `<select>`: it survives 3 or 100 values; never a row of buttons/chips.
- Every enabled entity must be visible on its card, even when it has no value yet (label with an empty slot).

**Checklist when adding a new card type:**

1. Add/change a field in the descriptor (`cards-caps.js`, or `cards-refine.js` if category-driven).
2. Write the builder + `registerUpdater` in `cards.js` (or `cards-light.js`/`cards-climate.js`/`cards-cover.js` for specialized sections).
3. Add the i18n key in **both** `en.json` and `vi.json` — CI (`check_web_i18n.py`) fails the moment one side is missing a key or placeholders diverge.
4. Register the new file in the `ASSETS` dict of `scripts/build_web.py` — CI has a completeness gate: any file in `src/` that isn't registered fails the build (a file that isn't packed is a runtime 404).
5. Run `python scripts/build_web.py`, QA on the mock server (see [docs/testing.md](testing.md)).

The build script gzips each file (deterministically) and packs them into a WAS1 image written to the `assets` partition (1 MB); the `web_assets` CMake invokes the script itself when building the firmware. A single ~2 KB fallback page is embedded directly into the firmware and served when the assets partition is missing or broken.

## Persist & boot

**Partition table** (`partitions/tuya_16m.csv`): `nvs` (settings, token) + `otadata` + two app slots `ota_0`/`ota_1` (dual OTA) + `storage` (littlefs — the registry snapshot) + `assets` (the web UI's WAS1 image, 1 MB) + `coredump` (post-mortem debug).

**Boot order** (read from `main/main.cc` → `main/application.cc`):

1. Create the default event loop (WiFi/netif/httpd all need it).
2. Init encrypted NVS (`main/nvs_secure_boot.cc`): HMAC scheme, key taken from eFuse. The first boot (no key yet) runs a one-shot migration: snapshot every plaintext NVS entry → burn the HMAC key into eFuse → wipe the partition → re-init as encrypted → write the values back. Every failure is fail-soft, no abort: a failed eFuse key burn leaves flash intact so the next boot retries the migration, while that boot's storage degrades (NVS writes are dropped, defaults are in use) and the hub continues into setup mode.
3. Init the at-rest key for the registry snapshot (a dedicated eFuse key block) — also fail-soft: it only disables sealing, loading the old file still works.
4. `Application::Start()`: PSRAM pool for the device model → LED/log/CPU monitor → create the main event loop → **the boot self-test gate** (section above) → WiFi (no SSID yet → provisioning portal; stored SSID but router unreachable → the portal too, with an automatic reboot after ~5 minutes that retries the station) → web server → cloud.

**Registry persistence.** The snapshot lives at `/littlefs/registry.json` (`storage` partition), stored in an `ENC1` container: the plaintext JSON is sealed with AES-256-GCM using a key derived from eFuse — plaintext never reaches flash. Atomic write: write a `.tmp` file first, flush, then rename over it.

When it saves: big events (device add/remove/rename, entity selection) save immediately; status churn just sets a dirty flag and flushes on a 60-second debounce. Only one save job exists at a time (coalesced via a latch), and the save job sits in the critical queue so it is never dropped. A factory reset raises a latch that blocks saves until reboot — a wiped registry can't be "resurrected" by a save job that happens to fire at that moment. The snapshot carries the account's uid stamp — on boot, if it belongs to a different account, the snapshot is wiped and a full scan starts from scratch. The same uid-mismatch policy applies mid-session: a re-link that changes the account wipes the registry and store on the way into `Linked`, so a device list from the previous account never survives the switch.

## Development

- **Build/lint commands and the PR workflow**: see [CONTRIBUTING.md](../CONTRIBUTING.md) (summary) and [docs/getting-started.md](getting-started.md) (per-platform details) — not repeated here.
- **CI runs on every PR into `master`** (feature branches don't build on their own — they get checked when the PR opens):
  - *Build job* (inside an ESP-IDF v6.0.1 container): build web assets → web UI import-graph lint → clang-format 23.1.3 → build firmware with `-Werror`.
  - *Web-smoke job* (runs in parallel on a regular runner, because the IDF container has no Node): loads the entire ES module graph with Node plus a validator that really runs (catches unbound import errors that `node --check` can't see), and checks i18n parity between `en.json`/`vi.json`.
- **Releases**: cut by the maintainer; the `VERSION` file and release tags are not part of pull requests — see [CONTRIBUTING.md](../CONTRIBUTING.md).
