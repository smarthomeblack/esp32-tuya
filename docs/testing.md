# Testing

A guide for contributors — anyone who wants to change the code/web UI and verify it themselves before sending a PR (contribution process: [CONTRIBUTING.md](../CONTRIBUTING.md)).

## Boot self-test

Every component has a self-test suite that runs **before** the hub joins the network. Any failed check → the hub enters `FatalError`, the LED blinks fast, **no WiFi/MQTT connection** — broken firmware never gets out onto the home network.

| Self-test | Checks | What it protects |
|---|---|---|
| `tuya_crypto` | 75 | MD5, HMAC, AES-128-GCM, at-rest AES-256-GCM, JSON escape, base64, UUID — checked against golden vectors generated from the real Python SDK |
| `sys` log/cpu | 56 | log ring reassembly/eviction, capture scrub, line parse, CPU usage vector table, Settings NVS round-trip, NVS enumeration |
| `tuya_cloud` pipeline | 26 | URL, body, 6 headers + sign of the request pipeline |
| `tuya_cloud` OpenAPI | 28 | HMAC-SHA256 sign + login body for the Tuya IoT Platform (golden vectors) + home/room path templates |
| `hub_core` model | 172 | response parsing + default/enum strategies + registry, room and presence-merge semantics |
| `hub_core` device store | 64 | serialize/parse `registry.json` byte-exact golden + round-trip + corrupt tolerance + legacy (no rooms) snapshot |
| `hub_services` enrichment | 87 | Thing Model double-encoding + shadow + sharing-wins merge |
| `tuya_mqtt` message | 114 | protocol 4/20, topic `/pen`/`/sta`, code/dpId dispatch, reassembly |
| `hub_services` command | 219 | 10 s dedup, byte-exact body, scenes parse, disabled-entity blocking, post-send hook |
| `hub_services` discovery | 90 | degraded vs loaded detail staging (uncached retries), commit add/refresh/skip, entity selection, no-scan guards |
| `web_assets` | 22 | WAS1 image validate (magic/version/TOC bounds/CRC) + fallback page |

**Total: 953 checks** — all must PASS before joining the network.

Every firmware change must keep this coverage level intact: when you add new code, add matching checks to that component's self-test — never lower the bar.

## No hub / hardware needed

### Mock server

```bash
python scripts/build_web.py     # if components/web_assets/dist/web_assets.bin doesn't exist yet
python scripts/mock_server.py
# → http://127.0.0.1:8787  ·  WS: ws://127.0.0.1:8787/ws?token=<key>
```

- Serves the **real SPA** (the exact WAS1 image the firmware flashes) + a full-contract mock REST (see [api.md](api.md)) + WebSocket RFC6455 in pure stdlib.
- Auth works like the firmware: every `/api` route needs `Authorization: Bearer <key>`, `/ws` needs `?token=<key>` — the mock's initial API key comes from the `MOCK_API_KEY` variable (8 digits by default, declared in `scripts/mock_server.py`; you can also change it via `POST /api/settings/apikey`).
- A state machine simulates the hub: `needs_setup/idle` → `POST /api/auth/start` → `waiting_scan` (QR payload after `MOCK_QR_DELAY`) → after `MOCK_LINK_DELAY` it switches to `linked` + discovery progress + `device_added`; `POST /api/auth/unlink` returns to idle. It also covers domain discovery (scan/candidates/commit), the Tuya IoT Platform (credentials → token), asset/OTA.
- Device-scoped routes mirror the firmware: ids are percent-decoded and rejected when empty, over 64 chars or containing `/`; `POST /api/devices/<id>/{entities,remove,commands}` answer `404 not_found` for an unknown id *before* reading the body; `entities` and `commands` then read it (`entities` allows an empty body and answers with that device's snapshot), while `remove` never reads one. A successful `remove` drops the device and pushes `device_removed`.
- Commands sent down are echoed back as a `device_update` event after ~1 s — exactly the "MQTT confirm" behavior the UI's *pending* spinner waits for. `POST /api/devices/<id>/remove` (`{"success":true}`) removes the device from the mock state and pushes `device_removed {devId}`, like the firmware.
- The device set includes deliberate edge-case fixtures: a curtain whose only datapoint is `situation_set` (no cover section mounts) and an AC whose `mode` declares an empty range (no dropdown mounts) — both must still render as plain floor rows, never invisible. The main curtains (`cover_lr`) and fan (`mockfan_barecd`) fixtures carry the values the UI edits (`control`, `countdown`), so every rendered row exercises a real control path.
- Environment variables (all optional, read once at startup):

  | Variable | Semantics | Default |
  | --- | --- | --- |
  | `MOCK_PORT` | Listener port of the mock | `8787` |
  | `MOCK_API_KEY` | Initial API key (Bearer for `/api`, `?token=` for `/ws`); `POST /api/settings/apikey` rotates it | `12345678` |
  | `MOCK_LINK_DELAY` | Seconds from `POST /api/auth/start` to `linked` (stretches the QR screen) | `8` |
  | `MOCK_QR_DELAY` | Seconds until the QR payload arrives (the "generating QR" state) | `1.2` |
  | `MOCK_QR_REFRESH` | `=1`: the QR payload rotates twice (~2 s apart) before linking — three payloads in total, for the UI repaint | off |
  | `MOCK_OPENAPI_TOKEN_DELAY` | Seconds from saving Tuya IoT credentials to the token; `login_in_flight` covers the whole wait (`=4` to watch the connecting poll) | `1.5` |
  | `MOCK_DETAIL_DELAY` | Seconds a candidate detail fetch takes (the simulated 3-call cloud round-trip) | `0.6` |
  | `MOCK_DEGRADED_DETAIL` | Comma-separated candidate ids whose detail fetch "fails": `detail_loaded` stays false and the commit re-fetches them | none |
  | `MOCK_PRESENCE` | `=1`: flip every device's `online` flag every ~6 s and push `device_online` | off |
  | `MOCK_PRESENCE_ROUNDS` | Stop the presence flipper after N rounds (only with `MOCK_PRESENCE=1`) | `0` (run forever) |
  | `MOCK_FAIL_COMMANDS` | `=1`: every `POST /api/devices/<id>/commands` answers `502 command_failed` and is not applied (echo `device_update` never fires) | off |

Use it to develop and debug the UI in a browser — no real hub needed.

### Golden vectors

```bash
pip install -r scripts/requirements.txt    # if not installed yet
python scripts/gen_test_vectors.py
```

Regenerates the golden vectors for the crypto + pipeline self-tests: `components/tuya_crypto/src/test_vectors.h` and `components/tuya_cloud/src/pipeline_vectors.h` (generated files — don't edit by hand). The script is **deterministic, idempotent**: every input is hard-coded, no timestamps — rerunning it produces byte-for-byte identical output; different output means the logic changed somewhere.

### Web import-graph lint

```bash
python scripts/check_web_imports.py
```

Statically checks the web UI's ES modules (CI runs this as the "Lint web import graph" step): every import resolves correctly, no layer-rule violations (core doesn't import features, features->features must stay in the same `cards-*`/`settings-*` family or be in the script's whitelist), no cycles outside a family; an imported binding the file never uses and an export that no module imports are both flagged as dead code.

## Host-built self-tests (CI)

The same boot-gate self-test sources the hub runs on device, compiled for the host with plain gcc/g++/cmake — no ESP-IDF, no board. ESP-IDF headers are shadowed by minimal shims in `host_tests/` (first on the include path): an `esp_log` that captures log lines in memory, an in-memory NVS, FreeRTOS no-ops, and a software PSA crypto implementation (SHA-256, MD5, HMAC, AES) validated by the repo's own golden vectors. The runner executes each covered suite in the on-device boot-gate order and requires the exact `... SELFTEST PASS (N checks)` line — so both a failing check and a suite silently gaining or losing checks fail the run.

Coverage: 8 of the 11 device suites — 800 of the 953 checks.

| Self-test | Checks |
|---|---|
| `tuya_cloud` pipeline | 26 |
| `tuya_cloud` OpenAPI | 28 |
| `hub_core` model | 172 |
| `hub_core` device store | 64 |
| `hub_services` enrichment | 87 |
| `tuya_mqtt` message | 114 |
| `hub_services` command | 219 |
| `hub_services` discovery | 90 |

The three remaining suites are hardware-bound and stay device-only (the runner prints this gap on every run): `tuya_crypto` (75 — at-rest key material comes from eFuse), `sys` (56 — the settings roundtrip and log buffer need NVS/heap introspection), `web_assets` (22 — `asset_store` links ESP-IDF flash/mmap; the vectors are RAM-only).

CI runs this as the "Self-tests (host build)" job on every push and pull request (ubuntu-latest). The job first fetches the pinned upstream cJSON source into `managed_components/` — that directory is gitignored, so a checkout alone doesn't contain it; the pinned commit is recorded in the workflow file.

To run the same suite locally (from the repo root; `build-host-tests/` is gitignored):

```bash
cmake -S host_tests -B build-host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-host-tests -j
./build-host-tests/selftest_runner
```

A successful run ends with:

```
SELFTEST RUNNER PASS (8 suites, 800 checks)
```

One limitation: host crypto is that software PSA shim validated by golden vectors — test-level parity with the device, not binary parity with its mbedtls/hardware AES.

## On real hardware

1. Build + flash following [getting-started.md](getting-started.md).
2. Open a serial monitor (`idf.py -p <PORT> monitor`): watch the boot log — every component's self-test lines must PASS, a total of 953/953, **and only then** does the hub start connecting to WiFi.
3. Any failed check → `FatalError` in the log, fast-blinking LED, no network. Find the failed check in the last log line and match it to the corresponding component.

Note: the mock + golden vectors cover protocol/logic; real hardware behavior (flash, PSRAM, TLS, MQTT broker, real Tuya devices) can only be acceptance-tested on the board — the mock doesn't replace this step.
