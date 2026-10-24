#!/usr/bin/env python3
"""Web asset builder: gzips web_assets/src into dist/*.gz and packs
dist/web_assets.bin (WAS1) for the "assets" partition. Deterministic,
idempotent. Run: python scripts/build_web.py"""

from __future__ import annotations

import gzip
import hashlib
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = PROJECT_ROOT / "components" / "web_assets" / "src"
DIST_DIR = PROJECT_ROOT / "components" / "web_assets" / "dist"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import web_asset_pack  # noqa: E402  (same directory, pure stdlib)

# Everything the ESP32 serves. Keys = source paths (relative to src/),
# values = flat dist names (the EMBED symbol base; '/' -> '_').
ASSETS = {
    "index.html": "index.html",
    "css/base.css": "css_base.css",
    "css/dashboard.css": "css_dashboard.css",
    "css/rooms.css": "css_rooms.css",
    "css/scenes.css": "css_scenes.css",
    "css/settings.css": "css_settings.css",
    "css/onboarding.css": "css_onboarding.css",
    "css/scan.css": "css_scan.css",
    "css/logs.css": "css_logs.css",
    "css/ui.css": "css_ui.css",
    "qrcode.min.js": "qrcode.min.js",
    "vi.json": "vi.json",
    "en.json": "en.json",
    "manifest.json": "manifest.json",
    "icon.svg": "icon.svg",
    # ES modules (served at the src path, embedded flat).
    "js/app.js": "js_app.js",
    "js/core/state.js": "js_core_state.js",
    "js/core/events.js": "js_core_events.js",
    "js/core/i18n.js": "js_core_i18n.js",
    "js/core/utils.js": "js_core_utils.js",
    "js/core/api.js": "js_core_api.js",
    "js/core/ws.js": "js_core_ws.js",
    "js/ui/toast.js": "js_ui_toast.js",
    "js/ui/dialogs.js": "js_ui_dialogs.js",
    "js/ui/appbar.js": "js_ui_appbar.js",
    "js/ui/uploader.js": "js_ui_uploader.js",
    "js/features/gate.js": "js_features_gate.js",
    "js/features/onboarding.js": "js_features_onboarding.js",
    "js/features/tuya-iot.js": "js_features_tuya_iot.js",
    "js/features/scan.js": "js_features_scan.js",
    "js/features/dashboard.js": "js_features_dashboard.js",
    "js/features/rooms.js": "js_features_rooms.js",
    "js/features/cards.js": "js_features_cards.js",
    "js/features/cards-registry.js": "js_features_cards_registry.js",
    "js/features/cards-caps.js": "js_features_cards_caps.js",
    "js/features/cards-refine.js": "js_features_cards_refine.js",
    "js/features/cards-commands.js": "js_features_cards_commands.js",
    "js/features/cards-light.js": "js_features_cards_light.js",
    "js/features/cards-climate.js": "js_features_cards_climate.js",
    "js/features/cards-cover.js": "js_features_cards_cover.js",
    "js/features/entity-editor.js": "js_features_entity_editor.js",
    "js/features/scenes.js": "js_features_scenes.js",
    "js/features/settings.js": "js_features_settings.js",
    "js/features/settings-security.js": "js_features_settings_security.js",
    "js/features/settings-logs.js": "js_features_settings_logs.js",
    "js/features/settings-asset.js": "js_features_settings_asset.js",
    "js/features/settings-ota.js": "js_features_settings_ota.js",
}

# src/ files intentionally not in ASSETS: the firmware-embedded fallback
# page and this component's C++ sources.
ASSETS_EXEMPT = {
    "fallback.html",
    "asset_format.h",
    "asset_parse.h",
    "asset_self_test.cc",
    "asset_store.cc",
}

BUDGET_BYTES = 250 * 1024
# The "assets" flash partition (partitions/tuya_16m.csv).
ASSET_BIN_CAP = 0x100000


def gzip_bytes(data: bytes) -> bytes:
    """Deterministic gzip (level 9, mtime 0, no filename)."""
    import io

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as gz:
        gz.write(data)
    return buf.getvalue()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def main() -> int:
    if not SRC_DIR.is_dir():
        print(f"error: missing source directory {SRC_DIR}", file=sys.stderr)
        return 1
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    # Dist names must be the flat sanitized form of the src path ("/", "-"
    # -> "_"; mirrors the CMake EMBED symbol derivation).
    import re
    for src_name, dist_name in ASSETS.items():
        if dist_name != re.sub(r"[^A-Za-z0-9.]", "_", src_name):
            print(f"error: dist name '{dist_name}' is not the sanitized form "
                  f"of '{src_name}'", file=sys.stderr)
            return 1

    total_src = 0
    total_gz = 0
    changed = []
    packed_blobs = {}  # served URL path -> gzip bytes (feeds the WAS1 pack)
    print(f"{'source':<34} {'raw':>8} {'gzip':>8}  {'ratio':>6}  hash")
    print("-" * 78)

    for src_name, dist_name in ASSETS.items():
        src = SRC_DIR / src_name
        if not src.is_file():
            print(f"error: missing source file {src}", file=sys.stderr)
            return 1
        raw = src.read_bytes()
        packed = gzip_bytes(raw)
        packed_blobs[src_name] = packed
        out = DIST_DIR / (dist_name + ".gz")
        if not out.is_file() or out.read_bytes() != packed:
            out.write_bytes(packed)
            changed.append(out.name)
        total_src += len(raw)
        total_gz += len(packed)
        print(f"{src_name:<34} {len(raw):>8} {len(packed):>8}  "
              f"{len(packed) / max(1, len(raw)) * 100:>5.0f}%  {sha(packed)}")

    # Reverse completeness: an unregistered src/ file is never packed
    # and 404s at runtime.
    on_disk = {p.relative_to(SRC_DIR).as_posix()
               for p in SRC_DIR.rglob("*") if p.is_file()}
    unregistered = sorted((on_disk - ASSETS_EXEMPT) - set(ASSETS))
    if unregistered:
        for name in unregistered:
            print(f"error: src file not registered in ASSETS: {name}",
                  file=sys.stderr)
        print(f"ASSETS completeness: FAIL ({len(unregistered)} unregistered)",
              file=sys.stderr)
        return 1
    print(f"ASSETS completeness: {len(set(ASSETS))}/"
          f"{len(on_disk - ASSETS_EXEMPT)} files registered")

    # The firmware-embedded recovery page — NOT part of the packed bin
    # (serves only when the assets partition is missing/invalid).
    fallback_src = SRC_DIR / "fallback.html"
    if not fallback_src.is_file():
        print(f"error: missing source file {fallback_src}", file=sys.stderr)
        return 1
    fallback_gz = gzip_bytes(fallback_src.read_bytes())
    fallback_out = DIST_DIR / "fallback.html.gz"
    if not fallback_out.is_file() or fallback_out.read_bytes() != fallback_gz:
        fallback_out.write_bytes(fallback_gz)
        changed.append(fallback_out.name)
    print(f"{'fallback.html (embedded)':<34} {fallback_src.stat().st_size:>8} "
          f"{len(fallback_gz):>8}  {len(fallback_gz) / max(1, fallback_src.stat().st_size) * 100:>5.0f}%  "
          f"{sha(fallback_gz)}")

    # Remove stale .gz outputs no longer mapped to a source (keep
    # ASSETS + the embedded fallback page).
    expected = {v + ".gz" for v in ASSETS.values()} | {"fallback.html.gz"}
    for stale in DIST_DIR.glob("*.gz"):
        if stale.name not in expected:
            stale.unlink()
            print(f"removed stale {stale.name}")

    # Legacy placeholder must not linger in dist.
    legacy = DIST_DIR / "index.html"
    if legacy.is_file():
        legacy.unlink()
        print("removed legacy dist/index.html placeholder")

    print("-" * 78)
    print(f"total: {total_src} raw -> {total_gz} gzip "
          f"({total_gz / 1024:.1f} KB of {BUDGET_BYTES / 1024:.0f} KB budget, "
          f"{total_gz / BUDGET_BYTES * 100:.1f}%)")
    if changed:
        print(f"rewritten ({len(changed)}): {', '.join(changed)}")
    else:
        print("all outputs up to date (idempotent)")
    if total_gz > BUDGET_BYTES:
        print("error: gzip total exceeds the 250 KB budget", file=sys.stderr)
        return 1

    # ---- the standalone WAS1 image for the "assets" partition ----
    # Keys = served URL paths (gzip -> FLAG_GZIP); sorted => deterministic.
    image = web_asset_pack.pack(
        {src_name: (out_bytes, web_asset_pack.FLAG_GZIP)
         for src_name, out_bytes in packed_blobs.items()})
    info, reason = web_asset_pack.validate(image, ASSET_BIN_CAP)
    if info is None:
        print(f"error: packed image failed validation: {reason}", file=sys.stderr)
        return 1
    bin_path = DIST_DIR / "web_assets.bin"
    if not bin_path.is_file() or bin_path.read_bytes() != image:
        bin_path.write_bytes(image)
    print(f"web_assets.bin: {len(image)} B of {ASSET_BIN_CAP} B partition cap "
          f"({len(image) / 1024:.1f} KB, build_id={info['build_id']}, "
          f"{info['file_count']} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
