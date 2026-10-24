#!/usr/bin/env python3
"""WAS1 web asset image: pack / parse / validate (pure stdlib).
Little-endian, zero-padded to 4 bytes — mirrored by asset_format.h;
emitted by build_web.py, served by mock_server.py."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

WAS1_MAGIC = b"WAS1"
WAS1_VERSION = 1

HEADER_SIZE = 40
TOC_ENTRY_SIZE = 76
NAME_SIZE = 64
BUILD_ID_SIZE = 16
CRC_OFFSET = 16  # image_crc32 field position in the header

# TOC flags
FLAG_GZIP = 0x1

_HEADER = struct.Struct("<4sHHIIII16s")
_TOC = struct.Struct("<64sIII")


def crc32(data: bytes, prev: int = 0) -> int:
    """CRC-32/IEEE — byte-identical to esp_rom_crc32_le(0, buf, len)."""
    return zlib.crc32(data, prev) & 0xFFFFFFFF


def image_crc(image: bytes) -> int:
    """CRC-32 over the whole image minus the 4-byte crc field itself."""
    return crc32(image[:CRC_OFFSET] + image[CRC_OFFSET + 4:])


def build_id(payload: bytes) -> bytes:
    """"w" + sha256(payload)[:11], NUL-padded to 16 bytes."""
    raw = ("w" + hashlib.sha256(payload).hexdigest()[:11]).encode("ascii")
    return raw.ljust(BUILD_ID_SIZE, b"\0")


def pack(entries: dict) -> bytes:
    """Packs {name: data | (data, flags)} into a deterministic WAS1 image
    (entries sorted by name; 4-byte-aligned total length)."""
    names = sorted(entries)
    if len(names) > 0xFFFF:
        raise ValueError(f"too many entries ({len(names)} > 65535)")
    toc_rows = []
    blobs = []
    offset = 0
    for name in names:
        if not name or len(name) >= NAME_SIZE:
            raise ValueError(f"bad asset name (len must be 1..{NAME_SIZE - 1}): {name!r}")
        if "/" in name and name.startswith("/"):
            raise ValueError(f"asset name must not start with '/': {name!r}")
        item = entries[name]
        data, flags = item if isinstance(item, tuple) else (item, 0)
        if not isinstance(data, (bytes, bytearray)):
            raise TypeError(f"asset data for {name!r} must be bytes")
        data = bytes(data)
        if len(data) > 0xFFFFFFFF or offset > 0xFFFFFFFF:
            raise ValueError("asset image exceeds the u32 offset/size fields")
        toc_rows.append((name.encode("utf-8"), offset, len(data), int(flags)))
        blobs.append(data)
        offset += len(data)
    payload = b"".join(blobs)
    image_len = HEADER_SIZE + TOC_ENTRY_SIZE * len(names) + len(payload)
    pad = (-image_len) % 4
    image_len += pad
    header = _HEADER.pack(
        WAS1_MAGIC, WAS1_VERSION, 0, len(names), len(payload),
        0, image_len, build_id(payload),
    )
    out = [header]
    for name, off, size, flags in toc_rows:
        out.append(_toc_row(name, off, size, flags))
    out.append(payload)
    out.append(b"\0" * pad)
    image = bytearray(b"".join(out))
    struct.pack_into("<I", image, CRC_OFFSET, image_crc(bytes(image)))
    return bytes(image)


def _toc_row(name: bytes, off: int, size: int, flags: int) -> bytes:
    if len(name) >= NAME_SIZE:
        raise ValueError(f"asset name too long: {name!r}")
    return _TOC.pack(name.ljust(NAME_SIZE, b"\0"), off, size, flags)


def parse(image: bytes) -> dict:
    """Parses a structurally valid image -> {"version", "file_count",
    "payload_len", "image_len", "build_id", "entries"}; raises ValueError
    with a reason otherwise."""
    if len(image) < HEADER_SIZE:
        raise ValueError("shorter than the 40-byte header")
    magic, version, _res, file_count, payload_len, _crc, image_len, build_id = (
        _HEADER.unpack_from(image, 0))
    if magic != WAS1_MAGIC:
        raise ValueError("bad magic (not WAS1)")
    if version != WAS1_VERSION:
        raise ValueError(f"unsupported version {version} (want {WAS1_VERSION})")
    if image_len != len(image):
        raise ValueError(f"image_len {image_len} != buffer length {len(image)}")
    toc_len = TOC_ENTRY_SIZE * file_count
    if HEADER_SIZE + toc_len > image_len:
        raise ValueError("TOC overruns the image")
    body = HEADER_SIZE + toc_len + payload_len
    if body > image_len or image_len - body >= 4:
        # 0..3 zero tail-pad bytes close the image (word alignment).
        raise ValueError("payload_len does not close the image")
    entries = {}
    for i in range(file_count):
        raw_name, off, size, flags = _TOC.unpack_from(image, HEADER_SIZE + i * TOC_ENTRY_SIZE)
        name = raw_name.split(b"\0", 1)[0]
        if not name:
            raise ValueError(f"TOC entry {i}: empty name")
        if len(name) >= NAME_SIZE:
            raise ValueError(f"TOC entry {i}: name not NUL-terminated within 64 bytes")
        if off > payload_len or size > payload_len - off:
            raise ValueError(f"TOC entry {i} ({name.decode('utf-8', 'replace')}): "
                             "offset+size overruns the payload")
        entries[name.decode("utf-8")] = {"offset": off, "size": size, "flags": flags}
    return {
        "version": version,
        "file_count": file_count,
        "payload_len": payload_len,
        "image_len": image_len,
        "build_id": build_id.split(b"\0", 1)[0].decode("ascii", "replace"),
        "entries": entries,
    }


def validate(image: bytes, max_size: int = 0) -> tuple:
    """Full validation (structure + image CRC + optional size cap);
    (info_dict, None) when valid, else (None, reason_string)."""
    try:
        info = parse(image)
    except ValueError as e:
        return None, str(e)
    if max_size and len(image) > max_size:
        return None, f"image ({len(image)} B) exceeds the {max_size} B cap"
    if image_crc(image) != _HEADER.unpack_from(image, 0)[5]:
        return None, "image CRC32 mismatch"
    return info, None


def payload_of(image: bytes, name: str) -> bytes:
    """Slice one entry's blob out of a parsed-valid image (QA helper)."""
    info = parse(image)
    entry = info["entries"][name]
    base = HEADER_SIZE + TOC_ENTRY_SIZE * info["file_count"]
    return image[base + entry["offset"]:base + entry["offset"] + entry["size"]]


def _main() -> int:
    ap = argparse.ArgumentParser(description="WAS1 web asset image tool")
    ap.add_argument("--info", metavar="FILE", help="parse and print the TOC")
    ap.add_argument("--verify", metavar="FILE", help="validate (structure + CRC)")
    ap.add_argument("--max-size", type=int, default=0, help="size cap for --verify")
    args = ap.parse_args()
    if not args.info and not args.verify:
        ap.print_help()
        return 1
    path = Path(args.info or args.verify)
    try:
        image = path.read_bytes()
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if args.info:
        try:
            info = parse(image)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        print(f"{path}: version={info['version']} files={info['file_count']} "
              f"payload={info['payload_len']} image={info['image_len']} "
              f"build_id={info['build_id']}")
        for name in sorted(info["entries"]):
            e = info["entries"][name]
            print(f"  {name:<34} {e['offset']:>8} {e['size']:>8} flags={e['flags']}")
        return 0
    info, reason = validate(image, args.max_size)
    if info is None:
        print(f"FAIL {path}: {reason}", file=sys.stderr)
        return 1
    print(f"OK {path}: {info['file_count']} files, {info['payload_len']} B payload, "
          f"build_id={info['build_id']}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
