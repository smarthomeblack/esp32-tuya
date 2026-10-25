# ESP32-Tuya Smart Home Hub

**A self-contained Tuya hub running on a single ESP32-S3 board.** Account authentication via Device Sharing (scan a QR code with the Smart Life app), commands sent through an AES-128-GCM + HMAC encrypted HTTPS pipeline, realtime state received via **MQTT-over-TLS push** — and because a sharing session does not always receive those pushes, the hub also reconciles the state from the Tuya cloud every 10 seconds and right after every command — everything happens in a **mobile-first bilingual VI/EN web UI** running on the ESP32 itself: no app to install, no separate server.

> Firmware — the latest release is always on the [Releases](https://github.com/smarthomeblack/esp32-tuya/releases) page · ESP-IDF v6.0.1 · factory bin ~1.5 MB / OTA slot 3.9 MB · embedded UI ~102 KB gzip

## ✨ Features

- 🔐 **QR login (Device Sharing)** — enter your User Code → the web UI shows a QR code → scan it with the Smart Life/Tuya app; no password or API key needed.
- 🏠 **8 device card types** — multi-channel switch/socket, lights (toggle + brightness + color temp + HSV color wheel), temperature/humidity sensors, air conditioners (mode dropdown + target temp), curtains (open/close/stop + position), fans (speed dropdown), robot vacuums (start/pause + suction dropdown), and a generic fallback card.
- 🎬 **Scenes** — the Tuya scene list for each home, run with one tap.
- ⚡ **Realtime WebSocket** — every state change is pushed to the UI instantly over MQTT → WS; automatic reconnect (backoff 1s→30s) plus `/api/status` polling fallback every 5 s.
- 🌐 **Bilingual VI/EN** — all web UI text runs through an i18n dictionary; switch language in Settings (saved on the hub and in the browser, kept across restarts). The WiFi setup portal is English-only.
- 📡 **mDNS** — reach the hub at `http://tuyahub-<4hex>.local`, no need to remember an IP.
- 📶 **First-time AP provisioning** — the hub broadcasts a `TuyaHub-XXXX` setup AP + captive portal (DNS hijack) for entering WiFi, no screen or cable needed.
- ♻️ **Factory reset / reboot** — from the Settings page (wipes all saved settings — WiFi including the setup-AP password, account link, device list, API key — then restarts).
- 🧪 **Boot self-test** — 953 checks run before the hub goes online; a failure keeps it offline.
- 💾 **Coredump** — when the firmware crashes, the crash details are stored in a dedicated memory region (64 KB) for later debugging.

## 🧭 Architecture overview

8 independent components, no circular dependencies.

```
  📱 Mobile browser
        │  http://tuyahub-xxxx.local (mDNS)
        ▼
┌──────────────────── ESP32-S3 (16MB + PSRAM) ────────────────────┐
│  web_server  REST /api/* + WebSocket /ws + UI (web_assets gz)   │
│      │  (Schedule + semaphore 25s)                              │
│  main — Application + MainEventLoop + state machine             │
│      ├── sys        WiFi STA/AP provisioning, saved settings, LED, DNS  │
│      ├── tuya_cloud CustomerApi (AES-GCM+HMAC, token, QR login) │
│      ├── tuya_mqtt  MQTT push (TLS, sub batch 20)               │
│      ├── hub_services command service, discovery, enrichment    │
│      └── hub_core   pure domain: device model, registry, store  │
└────────┬──────────────────────────────┬──────────────────────────┘
         │ HTTPS (TLS, crt-bundle)      │ MQTT over TLS :8883
         ▼                              ▼
   Tuya Cloud (endpoint per       Tuya MQTT broker
   account, QR via apigw)        (topic assigned by server)
```

## 🔧 Requirements

**Hardware**
- An **ESP32-S3** board with 16 MB flash + PSRAM (build target `esp32s3`, octal PSRAM at 80 MHz)
- Optional status LED — Kconfig `SYS_LED_GPIO` (default `-1` = headless)

**Software**
- **ESP-IDF v6.0.1** — install it following the [official Espressif documentation](https://docs.espressif.com/projects/esp-idf/) (Linux/macOS: `./install.sh` then `. ./export.sh`; Windows: the installer's "ESP-IDF 6.x PowerShell" shortcut)
- A mobile browser (the UI is a PWA-lite: manifest + theme-color)

**Account**
- A **Smart Life** (or Tuya) app account that already has Tuya devices.

## 🚀 Build & Flash

Open the IDF environment (`. ./export.sh` or the ESP-IDF PowerShell shortcut), then from the repo directory:

```powershell
idf.py set-target esp32s3      # first time — run this only once
idf.py build                   # first build needs internet (downloads managed components)
idf.py -p <PORT> flash monitor # replace <PORT> with your real port (COMx / /dev/ttyUSB0)
```

The full guide — installing ESP-IDF, flashing from a release bin, troubleshooting: **[docs/getting-started.md](docs/getting-started.md)**.

## 📦 Firmware updates

No cable needed: download `tuya_hub_vX.Y.Z_ota.bin` from [GitHub Releases](https://github.com/smarthomeblack/esp32-tuya/releases) → web UI → **Settings → Firmware (OTA)** → pick the file → the hub updates its firmware and reboots.

Fresh install / reflash from scratch — the 3 bin files in every release:

| File | Use for |
|---|---|
| `tuya_hub_vX.Y.Z_factory.bin` | Fresh install / blank board — a single file, flashed from the start of flash (~1.5 MB: bootloader + partition table + firmware) |
| `tuya_hub_vX.Y.Z_ota.bin` | Update the firmware of a running hub, no cable needed (Settings → Firmware OTA) |
| `web_assets_vX.Y.Z.bin` | The web UI — upload it through the web UI (Settings → Web UI) or from the recovery page; or flash it alongside with the command below |

Flash the firmware + web UI in one command (needs an environment with `esptool`, e.g. ESP-IDF):

```powershell
python -m esptool --chip esp32s3 -p <PORT> write-flash 0x0 <factory.bin> 0xEE8000 <web_assets.bin>
```

⚠️ Flashing the factory image is a fresh install: the hub **loses its saved WiFi and account link** and re-opens its `TuyaHub-XXXX` setup AP so you can set it up again like the first time. The list of added devices is kept. Without `web_assets.bin`, the hub still boots into its embedded recovery page to upload the web UI right there.

Step-by-step details: [docs/getting-started.md](docs/getting-started.md).

## 📱 First use

1. **Power it up** for the first time (no WiFi set up yet): the hub broadcasts the AP **`TuyaHub-XXXX`** (WPA2) — XXXX is the last 4 hex characters of the MAC. **The AP password is printed on the serial log** right when the hub boots (line `Access point up: ssid=... pass=...`) — open a serial monitor to read it.
2. Connect your phone **to the AP with the password you just read** → the captive portal opens automatically (or browse to `http://192.168.4.1`) → pick your home WiFi and enter its password → the hub saves it → **reboots by itself**. The portal page also shows the hub's **initial API key** (the same code is printed on the serial log until the key is first changed).
3. Reconnect to your home WiFi and open **`http://tuyahub-xxxx.local`** (mDNS; xxxx = the last 4 hex characters of the MAC, lowercase) or the IP shown on the serial log. Sign in with the **initial API key** (from the setup portal or the serial log — every hub generates its own random code) — the hub **forces you to rotate the key right on the login screen** before letting you in.
4. The UI is in the **NeedsSetup** state → get the **User Code** from the Smart Life app → enter it in the wizard → the hub requests a QR code from the cloud and displays it (each QR code is valid for ~180 seconds while the hub polls the cloud every 3 s; if you don't scan it in time, the hub refreshes it — at most 2 times, ~540 seconds in total — and the web UI repaints the QR and restarts its countdown whenever it changes).
5. **Open the Smart Life app → scan the QR code** (the scan icon) → approve the device share.
6. The hub switches to **Linked** → the wizard asks about the **optional Tuya IoT Platform** (to show the full datapoint set on some devices such as power-metering switches): fill in your IoT Platform credentials or skip it.
7. The hub **scans for devices** (progress bar) → a **checklist** appears: tick the devices you want to add, expand each device to **untick the entities you don't need** → "Add N devices" → the dashboard shows the selected devices + scenes.
8. **From the next boot on: no re-scanning** — the device list is already saved on the hub, so a restart brings realtime state + scenes back right away. The **"+"** button on the dashboard (or the empty state) **adds new devices** (re-scan, only devices that are new can be ticked); the **gear** icon on a card **edits entities** (tick/untick) or **deletes the device**.
9. Control every device: flip a switch, drag the brightness, pick a color... The UI shows a *pending* spinner until the hub confirms the new state (an MQTT push, or the cloud check right after the command).

## 🌐 API & WebSocket

REST (prefix `/api`, JSON UTF-8) + WebSocket `/ws` pushing realtime state — every endpoint is protected by a **Bearer API key** (`Authorization` header; WS uses `?token=`). 33 REST endpoints, 9 WS event types.

The full contract — each endpoint with request/response examples, error tables, the event table and its frequency: **[docs/api.md](docs/api.md)**.

## 📁 Project structure

```
esp32-tuya/
├── main/                      # orchestration: Application, MainEventLoop, state machine
├── components/
│   ├── sys/                   # Settings (persisted config), WiFi STA, AP provisioning + captive portal, LED, DNS
│   ├── tuya_crypto/           # AES-128-GCM, HMAC-SHA256, MD5, base64, JsonWriter + self-test
│   ├── tuya_cloud/            # CustomerApi pipeline, token manager, QR login, cloud auth + self-tests
│   ├── hub_services/          # Command service (dedup filter), discovery, OpenAPI enrichment + self-tests
│   ├── hub_core/              # Device model + Registry, DeviceStore (LittleFS), strategies, parse + self-tests
│   ├── tuya_mqtt/             # MQTT push channel (TLS, protocol 4/20) + self-test
│   ├── web_server/            # REST /api + WS broadcast + static gzip + mDNS
│   └── web_assets/            # SPA UI (ES modules, gzip)
├── scripts/                   # build & test tooling — see CONTRIBUTING.md
├── partitions/tuya_16m.csv    # 2 OTA slots 3.9MB + nvs + storage + assets + coredump
├── docs/                      # 4 public docs: api.md · getting-started.md · testing.md · architecture.md
├── sdkconfig.defaults(.esp32s3)
└── VERSION
```

## ⚠️ Current limitations

- **OTA is manual, via the web UI only** — Settings → *Firmware (OTA)*, pick the `.bin` file and reboot; there is no automatic OTA from the Internet.
- **No Tuya cameras** (too heavy for an ESP32 to proxy).
- **WiFi passwords, Tuya tokens and the API key are encrypted at rest** (NVS with an eFuse HMAC key; the registry snapshot with AES-256-GCM) — someone who only dumps the flash cannot read them; but this does not stop an attacker who solders a JTAG debugger directly onto the board and reads RAM while it runs.
- **The cloud path processes requests sequentially** — one request at a time; concurrent requests wait for their turn.
- **Adjusting brightness while a light is in color mode** makes it jump to white mode (the Tuya DP model treats brightness as a channel of white mode).
- **Static assets don't support 304** (an ETag is sent but `If-None-Match` is not handled).
- **A stalled client on your network can hold the web server for up to 30 seconds** — a connection that opens but sends slow or partial headers occupies the server's single worker until its 30 s timeout; the timeout is deliberate so that slow OTA uploads from far-away devices keep working.

## 🔒 Security

- Every cloud call goes over **HTTPS + esp_crt_bundle** (full certificate verification — there is no insecure option).
- The request pipeline uses **AES-128-GCM + HMAC-SHA256** exactly as the Tuya protocol requires (a fresh `rid`/nonce/sign per request).
- The **hub's API key** is generated randomly per device (shown on the setup portal + serial log, must be rotated at first sign-in) and stored as a SHA-256 hash (the initial key is also kept in encrypted storage until first rotation, so the setup portal can display it); every REST/WS call must carry a valid key; rotating the key drops every open session.
- **AP provisioning has its own WPA2 password** (generated randomly per hub, printed on the serial log) — used only to set up WiFi the first time; you leave the portal by rebooting, and it never runs alongside a working WiFi connection (if the router is unreachable at boot, the hub falls back to the AP and reboots itself after ~5 minutes to retry); the DNS hijack only works in this mode.
- Tokens refresh themselves before expiring (a 60 s margin); a `"sign invalid"` error wipes the stored credentials and returns the UI to the re-link wizard.
- If the cloud refuses the hub's MQTT login (an authentication error), the hub fetches fresh connection credentials and retries right away; any other refusal (protocol or server trouble) waits out the connection's regular 10-second retry, and if the connection never comes up the hub still fetches fresh credentials about every 30 seconds, so it never spins in a fast retry loop.

## 📜 License & credits

- **qrcode-generator** (Kazuhiko Arase) — [MIT](http://www.opensource.org/licenses/mit-license.php) — QR rendering on the web UI (`web_assets/src/qrcode.min.js`).
- The source code is released under the **MIT license** — see [LICENSE](LICENSE).

Want to contribute to the project? The process is in [CONTRIBUTING.md](CONTRIBUTING.md).
