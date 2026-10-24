# Getting started — build, flash, and use

A guide for every user: build the firmware from source or flash a ready-made bin from the Releases page, then set the hub up for the first time.

## Requirements

**Hardware**
- An **ESP32-S3** board with **16 MB flash + PSRAM** (build target `esp32s3`, octal PSRAM at 80 MHz).
- Optional status LED — pick the pin with the Kconfig `SYS_LED_GPIO` (default `-1` = headless, no LED needed).

**Software**
- **ESP-IDF v6.0.1** — install it following the [official Espressif documentation](https://docs.espressif.com/projects/esp-idf/):
  - Linux/macOS: download the ESP-IDF v6.0.1 release, then in that directory run `./install.sh` followed by `. ./export.sh` before each working session.
  - Windows: use the [ESP-IDF Windows Installer](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/windows-setup.html), then open the **"ESP-IDF 6.x PowerShell"** shortcut from the Start Menu (the environment is already set up).
- Python 3 (bundled with ESP-IDF; used for `esptool` and the repo's scripts).
- A mobile browser — the hub's web UI is a PWA-lite (manifest + theme-color) that runs well on mobile Chrome/Safari.

**Account**
- A **Smart Life** (or Tuya) app account on your phone, with your Tuya devices already in it.

## Build from source

```bash
git clone https://github.com/smarthomeblack/esp32-tuya.git
cd esp32-tuya

# Open the IDF environment (each new terminal):
#   Linux/macOS:  . ./export.sh
#   Windows:      open the "ESP-IDF 6.x PowerShell" shortcut, cd into the repo

idf.py set-target esp32s3      # first time — run this only once
idf.py build                   # build the firmware
idf.py -p <PORT> flash monitor # flash + watch the log (Ctrl+] exits the monitor)
```

- Replace `<PORT>` with your real port: Windows `COMx`, Linux `/dev/ttyUSB0`, macOS `/dev/cu.usbmodem*`.
- **The first build needs internet** — the IDF component manager downloads the managed components (`espressif/cjson`, `espressif/mqtt`, `espressif/mdns`, `joltwallet/littlefs`) once, then caches them.
- Editing the web UI (`components/web_assets/src/`): run `python scripts/build_web.py` (re-gzips it into the assets image) **before** `idf.py build` so the firmware embeds the new version.
- A clean build: exit code 0, the `Project build complete` line, no warnings (IDF v6 enables warnings-as-errors).

## Flash from a bin file (no build needed)

Download the bin files from [GitHub Releases](https://github.com/smarthomeblack/esp32-tuya/releases).

| File | Use for |
|---|---|
| `tuya_hub_vX.Y.Z_factory.bin` | **Fresh install / blank board** (~1.5 MB, the merge stops right after the app): bootloader + partition table + otadata + app — flash at `0x0` |
| `tuya_hub_vX.Y.Z_ota.bin` | **Update the firmware of a running hub**, no cable needed: web UI → **Settings → Firmware (OTA)** → pick the `.bin` file → the hub writes it into the next slot and reboots |
| `web_assets_vX.Y.Z.bin` | **The web UI** — flash at offset `0xEE8000`, or upload it directly through the web UI: **Settings → Web UI** (applied immediately, no reboot) |

Flash the firmware + UI in one command (needs an environment with `esptool` — e.g. the ESP-IDF environment, or `pip install esptool`):

```bash
python -m esptool --chip esp32s3 -p <PORT> write-flash 0x0 <factory.bin> 0xEE8000 <web_assets.bin>
```

Verify the integrity: compare each file's SHA-256 with the `SHA256SUMS` file attached to the release (`sha256sum -c` on Linux/macOS; Windows: `Get-FileHash`).

⚠️ **Notes on flashing the factory image**:
- The factory image covers `0x0 → 0x17B770` (padded with `0xFF` between the images), so it **erases NVS** (WiFi, the Tuya token — the hub re-opens its `TuyaHub-XXXX` setup AP, with a fresh password printed on the serial log, for a from-scratch setup) and overwrites the firmware. The device registry (LittleFS), the assets partition and the spare OTA slot are **untouched**.
- Flashing the factory image without `web_assets.bin` still boots — into the **embedded recovery page** (small, built into the app): connect WiFi from that page and upload `web_assets_vX.Y.Z.bin` right there, or flash it separately over a cable with `write-flash 0xEE8000 <web_assets.bin>`.

**Sensitive data encryption**: credentials (WiFi, the Tuya token, the API key) are encrypted by the hub before being written to flash. The encryption key is generated randomly and burnt into the chip's eFuse on the very first boot, and never leaves the chip — anyone who extracts the flash contents (esptool) only gets encrypted data (the crash dump region is the one exception, it is not encrypted — details in [SECURITY.md](../SECURITY.md)). Reflashing the firmware (OTA or factory) still works normally; but the stored data can only be read on the exact chip the key was written to — on any other chip it counts as blank, and the hub re-opens its setup AP for a from-scratch setup. On the first boot of the encryption-capable firmware, the hub converts the old data to encrypted form in a single pass — if power is lost at exactly that moment, the hub may fall back to the initial setup mode, and you simply configure it again (rare, and it can only happen that one time).

## First use

1. **Power it up** for the first time (no WiFi in NVS yet): the hub broadcasts the AP **`TuyaHub-XXXX`** (WPA2) — XXXX is the last 4 hex characters of the MAC. **The AP password is printed on the serial log** right when the hub boots (line `Access point up: ssid=... pass=...`) — open a serial monitor to read it. The AP stays up until you configure WiFi; a hub that already has stored credentials only opens it when the router is unreachable at boot — and then reboots by itself after ~5 minutes to retry the connection (see Troubleshooting).
2. Connect your phone **to the AP with the password you just read from the log** → the captive portal opens by itself (or browse to `http://192.168.4.1`) → pick your home WiFi and enter its password → the hub saves it to NVS → **reboots on its own**. The portal page also shows the hub's **initial API key** — write it down. The same key appears on the serial log as the line `Initial API key: ...`, but **only on the boot that generated it** (the first boot, or the boot after a factory reset): a later reboot does not print it again, while the setup portal keeps showing it until you rotate the key (step 3).
3. Reconnect to your home WiFi and open **`http://tuyahub-xxxx.local`** (mDNS; xxxx = the last 4 hex characters of the MAC, lowercase) or the IP shown on the serial log. The UI asks you to **sign in with the initial API key** (from the setup portal or the serial log — each hub has its own random code). On first sign-in, the hub **forces you to rotate the key right on the login screen** — use the new key from then on.
4. The UI is in the **NeedsSetup** state → get the **User Code** from the Smart Life app → enter it in the wizard → the hub requests a QR code from the cloud and displays it (polls every 3 s, automatically refreshing the QR twice within ~180 seconds).
5. **Open the Smart Life app → scan the QR code** (the scan icon) → approve the device share.
6. The hub switches to **Linked** → the wizard asks about the **optional Tuya IoT Platform** (to display the full datapoint set on some devices such as power-metering switches): fill in your IoT Platform credentials or skip it.
7. The hub **scans for devices** (progress bar) → a **checklist** appears: tick the devices you want to add, expand each device to **untick the entities you don't need** → "Add N devices" → the dashboard shows the selected devices + scenes.
8. **From the next boot: no re-scan** — the registry is stored on LittleFS (partition `storage`) and restored automatically at boot → MQTT + scenes work right away. The **"+"** button on the dashboard (or the empty state) **adds new devices** (re-scan: new devices are pre-ticked, and already-added ones can be ticked too — re-committing an already-added device updates its data points when the hub holds a poorer copy, otherwise the request is harmlessly skipped); the **gear** icon on a card **edits entities** (tick/untick) or **deletes the device**.
9. Control every device: flip a switch, drag the brightness, pick a color... The UI shows a *pending* spinner until MQTT confirms the state back.

Quick notes:
- To reset everything: **Settings → Factory reset** (clears the WiFi credentials, the Tuya link and the device registry, and re-opens the setup AP with a newly generated password and a newly generated API key). The firmware itself stays on the hub — the reset does not erase flash and does not touch the OTA slots.
- External integrations (scripts, home automation): read [api.md](api.md) — the full REST + WebSocket reference.
- More: [Testing](testing.md).

## Troubleshooting

- **No serial port showing up** — check in this order:
  - Install the driver for the board's USB-to-serial chip (CP210x and CH34x usually need their own driver — download it from the manufacturer's site).
  - Try another cable — some cables only charge and don't carry data.
  - On Linux: add your user to the `dialout` group, then reopen the terminal.
  - Check whether the computer sees the port: Device Manager → Ports (Windows), `ls /dev/ttyUSB*` (Linux), `ls /dev/cu.usbmodem*` (macOS).
- **The `TuyaHub-XXXX` AP doesn't show up** — wait about 30 seconds after powering up, then scan for WiFi again; the hub needs a few seconds to finish booting before it starts the AP. Open a serial monitor to be sure: the line `Access point up: ssid=... pass=...` means the AP is up. A hub that already has WiFi stored connects straight to your network and doesn't open the AP — with one exception: if the router is unreachable at boot, the hub opens the AP (so you can still reach it) and automatically reboots after about 5 minutes to retry the connection by itself. To set the hub up from scratch again, go to **Settings → Factory reset**.
- **The hub never goes online, or reboots over and over** — read the last line of the serial log: if the boot self-test fails, the log prints exactly where it failed and the hub stops in an error state, deliberately refusing to connect to WiFi (broken firmware is not allowed onto your home network). Save that log line and attach it when you open an issue. A hub that reboots roughly every 5–6 minutes while its setup AP is up is a different, healthy case: that is the WiFi recovery retry — the router is unreachable; bring the router back and the hub connects on its next retry.
- **`tuyahub-xxxx.local` won't open** — some routers/phones block multicast, which breaks mDNS: take the hub's IP from the serial log and open `http://<IP>` directly, or give the hub a static IP on your router.
- **Need a reset to defaults** — reflash `tuya_hub_vX.Y.Z_factory.bin` (wipes WiFi and the token, keeps the device list); or, if the hub breaks after an OTA to a new version, go to **Settings → Firmware (OTA)** and click **"Roll back to the previous firmware"** to return to the earlier version.
