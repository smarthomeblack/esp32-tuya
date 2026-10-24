#!/usr/bin/env python3
"""Pre-build validation of the golden JSON literals in device_store_test.cc:
each literal is extracted, C-unescaped, and checked to be well-formed JSON
that round-trips byte-identically through a compact dump."""
import json
import re
import sys

SRC = r"components/hub_core/src/device_store_test.cc"

text = open(SRC, encoding="utf-8").read()


def extract(name: str) -> str:
    pattern = name + r"\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\"\s*)+);"
    m = re.search(pattern, text)
    if not m:
        sys.exit(f"constant {name} not found")
    raw = m.group(1)
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
    joined = "".join(parts)
    # Decode the C escapes that occur in these literals: \" and \\
    joined = joined.replace("\\\\", "\x00").replace('\\"', '"').replace("\x00", "\\")
    return joined


def check(name: str):
    s = extract(name)
    parsed = json.loads(s)
    redumped = json.dumps(parsed, separators=(",", ":"), ensure_ascii=False)
    ok = redumped == s
    print(f"{name}: {len(s)} bytes, well-formed={True}, round-trip-stable={ok}")
    if not ok:
        for i, (a, b) in enumerate(zip(s, redumped)):
            if a != b:
                print(f"  first diff at {i}: {s[i:i+40]!r} vs {redumped[i:i+40]!r}")
                break
        else:
            print(f"  length differs: {len(s)} vs {len(redumped)}")
    return ok


ok = True
for const in ("kGoldenDeviceJson", "kGoldenStoreJson", "kCorruptDeviceJson",
              "kForwardDeviceJson"):
    ok &= check(const)
sys.exit(0 if ok else 1)
