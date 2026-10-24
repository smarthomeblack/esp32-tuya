#!/usr/bin/env python3
"""Release packager: factory merge-bin + OTA/asset bins + SHA256SUMS +
manifest, self-verified before the PASS line; also writes the
deterministic RELEASE_NOTES.md template."""

import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_ROOT / "build"
RELEASE_DIR = PROJECT_ROOT / "dist" / "release"
IDF_ROOT = os.environ.get("ESP_IDF")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import web_asset_pack  # noqa: E402  (same directory, pure stdlib)

# App image: 24 B header + 8 B first-segment header, then esp_app_desc_t
# (offsets verified against build/tuya_hub.bin).
ESP_IMAGE_MAGIC, CHIP_ID, CHIP_NAME = 0xE9, 0x0009, "esp32s3"
APP_DESC_OFFSET, APP_DESC_MAGIC, PROJECT_NAME = 32, 0xABCD5432, "tuya_hub"
DESC_VERSION, DESC_PROJECT, DESC_IDF_VER, STR_LEN = 16, 48, 112, 32
DESC_MIN_LEN = APP_DESC_OFFSET + 144  # through esp_app_desc.idf_ver

# Flash images expected in build/flash_args (basename -> role).
FLASH_PARTS = {"bootloader.bin": "bootloader", "partition-table.bin": "partition-table",
               "ota_data_initial.bin": "otadata", "tuya_hub.bin": "app",
               "web_assets.bin": "assets"}
PART_ORDER = ("bootloader", "partition-table", "otadata", "app", "assets")
MERGE_ORDER = PART_ORDER[:4]  # factory stops right after the app (firmware-only)


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, cwd=None):
    """Run a command, capture output; die with stderr on failure."""
    cmd = [str(c) for c in cmd]
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    except FileNotFoundError:
        die(f"command not found: {cmd[0]}")
    if r.returncode != 0:
        tail = (r.stderr or r.stdout or "").strip().splitlines() or [f"exit {r.returncode}"]
        die(f"command failed: {' '.join(cmd)}\n  {tail[-1]}")
    return r


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_version() -> str:
    try:
        ver = (PROJECT_ROOT / "VERSION").read_text(encoding="ascii").strip()
    except OSError as e:
        die(f"cannot read VERSION: {e}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", ver):
        die(f"VERSION must be x.y.z, got {ver!r}")
    return ver


def git_head() -> str:
    """Manifest traceability only (no clean-tree gate)."""
    return run(["git", "rev-parse", "HEAD"], cwd=PROJECT_ROOT).stdout.strip()


def esptool_python() -> str:
    """The running python must have esptool (IDF env or pip install)."""
    r = subprocess.run([sys.executable, "-m", "esptool", "version"], capture_output=True, text=True)
    if r.returncode != 0:
        at = f" at {IDF_ROOT}" if IDF_ROOT else ""
        die(f"this python cannot run `python -m esptool` -- open the ESP-IDF "
            f"environment{at} first (export.sh / ESP-IDF PowerShell), or `pip install esptool`")
    return sys.executable


def parse_flash_args():
    """build/flash_args -> (esptool flash flags, {role: (offset, path)})."""
    try:
        lines = (BUILD_DIR / "flash_args").read_text(encoding="ascii").splitlines()
    except OSError as e:
        die(f"cannot read build/flash_args (build the firmware first): {e}")
    if not lines or not lines[0].strip():
        die("build/flash_args is empty (build the firmware first)")
    flags = lines[0].split()
    parts = {}
    for line in lines[1:]:
        off_s, rel = line.split()
        path = (BUILD_DIR / rel).resolve()
        role = FLASH_PARTS.get(path.name)
        if role is None:
            die(f"build/flash_args has an unexpected image: {rel}")
        parts[role] = (int(off_s, 0), path)
    missing = sorted(set(PART_ORDER) - set(parts))
    if missing:
        die(f"build/flash_args is missing images: {', '.join(missing)}")
    return flags, parts


def parse_partitions():
    """partitions CSV -> {name: (offset, size)} for ota_0 and assets."""
    out = {}
    try:
        rows = (PROJECT_ROOT / "partitions" / "tuya_16m.csv").read_text(encoding="utf-8").splitlines()
        for row in rows:
            row = row.split("#", 1)[0].strip()
            if not row:
                continue
            f = [x.strip() for x in row.split(",")]
            if f[0] in ("ota_0", "assets"):
                out[f[0]] = (int(f[3], 0), int(f[4], 0))
    except (OSError, ValueError, IndexError):
        print("warning: partitions/tuya_16m.csv unreadable -- default caps")
    return out


def check_app_image(blob: bytes, label: str, want_version: str) -> dict:
    """Verify image header + esp_app_desc; return the descriptor fields."""
    if len(blob) < DESC_MIN_LEN:
        die(f"{label}: too short for an app image ({len(blob)} B)")
    chip = struct.unpack_from("<H", blob, 12)[0]
    desc_magic = struct.unpack_from("<I", blob, APP_DESC_OFFSET)[0]
    version, project, idf_ver = (
        cstr(blob[APP_DESC_OFFSET + o:APP_DESC_OFFSET + o + STR_LEN])
        for o in (DESC_VERSION, DESC_PROJECT, DESC_IDF_VER))
    errs = []
    if blob[0] != ESP_IMAGE_MAGIC:
        errs.append(f"image magic 0x{blob[0]:02X} != 0xE9")
    if chip != CHIP_ID:
        errs.append(f"chip_id 0x{chip:04X} != 0x{CHIP_ID:04X}")
    if desc_magic != APP_DESC_MAGIC:
        errs.append(f"app desc magic 0x{desc_magic:08X} != 0xABCD5432")
    if version != want_version:
        errs.append(f"version {version!r} != VERSION {want_version!r}")
    if project != PROJECT_NAME:
        errs.append(f"project {project!r} != {PROJECT_NAME!r}")
    if errs:
        die(f"{label}: " + "; ".join(errs))
    return {"version": version, "project": project, "idf_ver": idf_ver}


def merge_factory(py, out, flags, parts):
    """esptool merge-bin: firmware images into one flashable 0x0 file."""
    cmd = [py, "-m", "esptool", "--chip", CHIP_NAME, "merge-bin", "-o", out, *flags]
    for role in MERGE_ORDER:
        off, path = parts[role]
        cmd += [hex(off), path]
    run(cmd, cwd=PROJECT_ROOT)


def write_sums(files):
    """GNU sha256sum format, sorted by name (deterministic)."""
    sums = "".join(f"{sha256_hex(p.read_bytes())}  {p.name}\n"
                   for p in sorted(files, key=lambda p: p.name))
    out = RELEASE_DIR / "SHA256SUMS"
    out.write_text(sums, encoding="ascii", newline="\n")
    return out


def write_manifest(ver, tag, head, desc, app_size, asset_info):
    """schema 1; no timestamps so the bytes are deterministic."""
    manifest = {
        "schema_version": 1,
        "version": ver,
        "tag": tag,
        "source_sha": head,
        "idf_version": desc["idf_ver"],
        "app_size": app_size,
        "asset": {"build_id": asset_info["build_id"], "files": asset_info["file_count"],
                  "payload_len": asset_info["payload_len"], "image_len": asset_info["image_len"]},
    }
    out = RELEASE_DIR / "release-manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n",
                   encoding="ascii", newline="\n")
    return out


def write_release_notes(ver, tag, head, desc, ota_slot, app_end, asset_off,
                        files, asset_info):
    """dist/release/RELEASE_NOTES.md — deterministic template for
    `gh release create --notes-file` (release.yml appends the changelog)."""
    rows = "\n".join(
        f"| `{p.name}` | {p.stat().st_size:,} | `{sha256_hex(p.read_bytes())}` |"
        for p in files)
    text = f"""# tuya_hub {tag}

| Item | Value |
|---|---|
| Version | {ver} |
| Tag | {tag} |
| Source SHA | `{head}` |
| ESP-IDF | {desc['idf_ver']} (app descriptor) |
| App | {files[1].stat().st_size:,} B / OTA slot {ota_slot:,} B |
| Web assets | {asset_info['image_len']:,} B · {asset_info['file_count']} file · build_id `{asset_info['build_id']}` |

## Artifacts

| File | Size (B) | SHA-256 |
|---|---|---|
{rows}

## Flash / update

- **Factory (blank board)** — flash both images with one command (replace `COMx`):

  ```
  python -m esptool --chip esp32s3 -p COMx write-flash 0x0 {files[0].name} 0x{asset_off:X} {files[2].name}
  ```

- **OTA (hub already running)** — Settings → **Firmware (OTA)** → pick `{files[1].name}` → the hub reboots into the new slot on its own.
- **Web UI** — Settings → **Web UI** → upload `{files[2].name}` (applied immediately, no reboot needed); a board flashed with factory only boots into the **embedded recovery page**, where you can upload the assets right there.
- **First boot after a factory flash** — NVS is empty: the hub broadcasts the `TuyaHub-XXXX` AP (captive portal) → enter WiFi → the Tuya linking wizard (User Code + QR).

## Factory flash warning

⚠️ The factory image covers `0x0 → 0x{app_end:X}` end to end (0xFF padding between the images), so it **erases NVS** — the saved WiFi + Tuya token are lost and the hub re-opens its setup AP for a from-scratch setup. The device registry (LittleFS `storage`), the assets partition (`0x{asset_off:X}`) and the `ota_1` slot are **untouched** (each byte region is verified at packaging time).

## Changes

<!-- release-notes-changelog: release.yml replaces this line with the auto-generated changelog -->
"""
    out = RELEASE_DIR / "RELEASE_NOTES.md"
    out.write_text(text, encoding="utf-8", newline="\n")
    return out


def main() -> int:
    if len(sys.argv) > 1:
        die(f"no options expected -- run: python scripts/release.py (got: {' '.join(sys.argv[1:])})")
    ver = read_version()
    tag = f"v{ver}"
    head = git_head()
    print(f"tuya_hub release {ver} (tag {tag})")

    print(f"[1/6] prerequisites\n      git HEAD {head}")

    print("[2/6] inputs (build/flash_args)")
    flags, parts = parse_flash_args()
    missing = []
    for role in PART_ORDER:
        off, path = parts[role]
        ok = path.is_file()
        print(f"      {role:<15} @0x{off:06X}  {path.name}{' ok' if ok else ' MISSING'}")
        if not ok:
            missing.append(str(path))
    if missing:
        print("build the firmware first (open the ESP-IDF environment, then):")
        print(f"  Set-Location {PROJECT_ROOT}; idf.py -B build build")
        die(f"missing build outputs: {', '.join(missing)}")
    app_off, app_path = parts["app"]
    asset_off, asset_path = parts["assets"]
    app_bytes = app_path.read_bytes()
    asset_bytes = asset_path.read_bytes()

    # Cross-check the flash layout and sizes against the partition table.
    csv = parse_partitions()
    ota_slot = csv.get("ota_0", (None, 0x3F0000))[1]
    asset_cap = csv.get("assets", (None, 0x100000))[1]
    if csv.get("ota_0", (app_off,))[0] != app_off:
        die(f"app offset 0x{app_off:X} != partitions ota_0 0x{csv['ota_0'][0]:X}")
    if csv.get("assets", (asset_off,))[0] != asset_off:
        die(f"assets offset 0x{asset_off:X} != partitions assets 0x{csv['assets'][0]:X}")
    if len(app_bytes) > ota_slot:
        die(f"app {len(app_bytes):,} B exceeds the ota_0 slot {ota_slot:,} B")
    desc = check_app_image(app_bytes, "build/tuya_hub.bin", ver)
    asset_info, reason = web_asset_pack.validate(asset_bytes, asset_cap)
    if asset_info is None:
        die(f"web_assets.bin: {reason}")
    print(f"      app {len(app_bytes):,} B / slot {ota_slot:,} B, "
          f"version {desc['version']}, idf {desc['idf_ver']}")
    print(f"      assets {len(asset_bytes):,} B / {asset_cap:,} B cap, "
          f"{asset_info['file_count']} files, build_id {asset_info['build_id']}")

    print("[3/6] esptool merge-bin -> factory image (firmware only)")
    RELEASE_DIR.mkdir(parents=True, exist_ok=True)
    factory = RELEASE_DIR / f"tuya_hub_v{ver}_factory.bin"
    ota_bin = RELEASE_DIR / f"tuya_hub_v{ver}_ota.bin"
    asset_bin = RELEASE_DIR / f"web_assets_v{ver}.bin"
    py = esptool_python()
    merge_factory(py, factory, flags, parts)
    factory_bytes = factory.read_bytes()
    print(f"      {factory.name} ({len(factory_bytes):,} B) via {py}; "
          f"web assets flash separately at 0x{asset_off:X}")

    print("[4/6] copy OTA + asset artifacts")
    ota_bin.write_bytes(app_bytes)
    asset_bin.write_bytes(asset_bytes)
    print(f"      {ota_bin.name} ({len(app_bytes):,} B), {asset_bin.name} ({len(asset_bytes):,} B)")

    print("[5/6] self-verify")
    for role in MERGE_ORDER:
        off, path = parts[role]
        src = app_bytes if role == "app" else path.read_bytes()
        if factory_bytes[off:off + len(src)] != src:
            die(f"factory {role} region @0x{off:X} != {path.name} bytes")
    if len(factory_bytes) != app_off + len(app_bytes):
        die(f"factory size {len(factory_bytes):,} B != app end 0x{app_off:X}+{len(app_bytes):,}")
    check_app_image(factory_bytes[app_off:app_off + DESC_MIN_LEN],
                    f"factory app header @0x{app_off:X}", ver)
    print(f"      factory: {len(MERGE_ORDER)} regions byte-identical, ends at app "
          f"end ({len(factory_bytes):,} B), descriptor ok (magic 0xE9, chip "
          f"0x{CHIP_ID:04X}, desc 0xABCD5432, version {ver}, project "
          f"{PROJECT_NAME}, idf {desc['idf_ver']})")
    ota_bytes = ota_bin.read_bytes()
    if ota_bytes != app_bytes:
        die(f"{ota_bin.name} != {app_path} bytes")
    check_app_image(ota_bytes, ota_bin.name, ver)
    print(f"      ota:    header ok (magic 0xE9, chip 0x{CHIP_ID:04X}, desc "
          f"0xABCD5432, version {ver}); bytes == {app_path.name}")
    shipped = asset_bin.read_bytes()
    if shipped != asset_bytes:
        die(f"{asset_bin.name} != {asset_path.name} bytes")
    ship_info, reason = web_asset_pack.validate(shipped, asset_cap)
    if ship_info is None:
        die(f"{asset_bin.name}: {reason}")
    print(f"      assets: WAS1 ok ({ship_info['file_count']} files); "
          f"bytes == {asset_path.name}")

    print("[6/6] SHA256SUMS + release-manifest.json")
    files = [factory, ota_bin, asset_bin]
    sums = write_sums(files)
    manifest = write_manifest(ver, tag, head, desc, len(app_bytes), asset_info)
    print(f"      {sums.name} ({len(files)} entries), {manifest.name} (schema 1, source {head[:10]})")
    notes = write_release_notes(ver, tag, head, desc, ota_slot, len(factory_bytes),
                                asset_off, [factory, ota_bin, asset_bin, sums, manifest],
                                asset_info)
    print(f"      {notes.name} ({notes.stat().st_size:,} B, deterministic, no timestamps)")

    print("artifacts (dist/release):")
    for p in [factory, ota_bin, asset_bin, sums, manifest, notes]:
        sha = f"  sha256 {sha256_hex(p.read_bytes())}" if p.suffix == ".bin" else ""
        print(f"  {p.name:<32} {p.stat().st_size:>12,}{sha}")
    print("PASS -- 3 verified release artifacts + RELEASE_NOTES.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
