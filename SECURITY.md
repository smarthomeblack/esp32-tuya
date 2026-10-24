# Security policy

This document spells out what the hub protects itself against, which risks have been accepted, and how to report a vulnerability.

## Supported versions

Security support covers only the official firmware releases on the [Releases](https://github.com/smarthomeblack/esp32-tuya/releases) page. Self-built firmware from source is out of scope.

## Reporting a vulnerability

Found a security vulnerability? Go to the repo's **Security** tab and click **"Report a vulnerability"** (GitHub private vulnerability reporting) — the report goes through GitHub's private channel, readable only by maintainers.

**Do not open a public issue for a vulnerability**, even when it looks critical — that gives maintainers time to fix it before it can be exploited.

## What the hub protects

- Sensitive data (WiFi, Tuya tokens) is encrypted before being written to flash: NVS uses an eFuse key, the registry snapshot uses AES-256-GCM. The key is generated on the chip itself and never leaves it.
- API key checks compare a stored SHA-256 hash in constant time; the un-rotated initial key's plaintext also lives in the encrypted NVS (displayed by the WiFi setup portal) until the first rotation erases it.
- The initial API key and the setup-AP passphrase are generated from the bootloader entropy source (`bootloader_random_enable`) before Wi-Fi starts — the same source as the at-rest eFuse key.
- Brute-force key guessing is blocked: more than 10 wrong attempts within 60 seconds and that IP is banned outright for 5 minutes (even a correct attempt).
- Every REST and WebSocket endpoint of the hub's own API server must carry a valid key (REST uses the `Authorization: Bearer ...` header, WS uses `?token=`). Scope: the API server on port 80 that runs while the hub is connected to your home WiFi. The WiFi setup portal (the next bullet) is a separate server with its own rules.
- OTA rollback: if new firmware fails to boot, the hub reverts to the previous version — no brick. (With rollback enabled, a second consecutive rollback leaves both slots INVALID and the bootloader falls back to `ota_0`: it still boots, but that may be the old image.)
- All input is validated; every string displayed on the web UI is escaped.

## Accepted risks

The hub's design trades away a few security points, stated plainly so you can weigh them before using it:

- TLS on the local web UI is deliberately not implemented — a design decision, not a missing feature: the hub is designed for a home network behind a router, the hub API is gated by the API key with per-IP lockout, and a self-signed certificate would provide no real trust anchor (browsers warn and users click through) while costing mbedTLS RAM and certificate management on a headless device. An attacker who already controls the home network is a different threat class (see the physical-access risk).
- The physical boundary (the USB/serial port) is trusted: secure boot and flash encryption are not enabled. Anyone who plugs a cable straight into the board can reflash it (and can read the setup-AP passphrase off the serial log during setup).
- **The WiFi setup portal is intentionally unauthenticated.** While the hub is in setup mode — no WiFi stored yet, or the saved network was unreachable at boot — it runs an HTTP server at `192.168.4.1` that needs no API key: it shows the un-rotated initial API key, scans for networks, saves the SSID/password the hub will use on its next boot, and can reboot the hub. Anyone who can join its WPA2 AP can therefore change which WiFi the hub boots into and read the API key shown there. In the unreachable-at-boot case the portal reboots the hub after ~5 minutes to retry the saved network (a fresh setup stays up until saved). The AP passphrase is random per hub, printed on the serial log while the AP is up (line `Access point up: ssid=... pass=...`), and a factory reset replaces it with a fresh one. Treat a hub in setup mode like an unconfigured device on the bench: finish the setup, then rotate the API key.
- **A rotated API key cannot be recovered.** Rotation stores only the SHA-256 hash: no endpoint returns the key, and the hub cannot print it again (the serial `Initial API key: ...` line appears once, on the boot that generated it, and the setup portal stops showing it after the first rotation). If a rotated key is lost, the only way back in is a **factory reset**, which also clears the WiFi credentials, the Tuya link, the device registry and the at-rest encryption key — see [api.md](docs/api.md#post-apisystemfactory-reset). Keep the key somewhere safe before you rely on the hub unattended.
- OTA firmware is not signed — the hub's API key is the root of trust for flashing new firmware.
- An attacker attaching JTAG to the board to read RAM while the hub runs: out of scope.

## Notes

- The ESP-IDF crash dump region is **not encrypted** (unlike NVS and the registry) — someone who dumps the raw flash can still read that part. A factory reset erases the dump and the registry store (and, if the registry delete fails and the storage partition can be located, the whole storage partition) rather than shredding the chip: the erased flash blocks (an earlier registry snapshot may still be carved out of them) and the eFuse at-rest key survive a reset, and the firmware images in the OTA slots are not erased at all.
- The WiFi driver keeps its own encrypted copy of the saved home WiFi SSID and password in the NVS namespace `nvs.net80211`. A factory reset calls `esp_wifi_restore()`, which resets the driver's configuration to defaults but leaves the encrypted copy in place until something overwrites it, so a flash dump plus the eFuse key can still carve out the old network — the same class of survivor as the entries above.
