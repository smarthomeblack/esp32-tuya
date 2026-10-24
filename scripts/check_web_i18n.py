#!/usr/bin/env python3
"""i18n guard: en.json/vi.json key + {placeholder} parity, and every en.json
key is accounted for as a static t('literal') (inline ternary included), a
key-shaped table literal in a module that t()s a variable, a data-i18n
attribute, a dynamic runtime family, or an explicit known-unused whitelist
row. Run: python scripts/check_web_i18n.py"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = PROJECT_ROOT / "components" / "web_assets" / "src"
JS_DIR = SRC_DIR / "js"

# t('prefix_' + var) builds the key at runtime: keys with these prefixes are
# covered as a FAMILY. Each family must also prove it can still produce a key
# (the base expression must exist in that module) and must own at least one
# dict key, so a renamed or dead family fails instead of widening a hole.
DYNAMIC_PREFIXES = {
    "auth_state_": ("features/settings.js", "auth_state_"),
}
# Keys each family can build: <prefix><suffix>.
FAMILY_KEY_RE = {
    "auth_state_": re.compile(r"auth_state_[a-z_]+"),
}

# Keys no code path t()s (known unused). Add a row only after confirming the
# key is genuinely dead in the UI; never to silence a real miss.
KNOWN_UNUSED: tuple[str, ...] = ()

# Static t('literal') / t("literal") (group 2); the optional prefix also
# matches t(cond ? 'a' : 'b') (the key after the '?').
T_KEY_RE = re.compile(
    r"(?<![\w$])t\s*\(\s*[^()?'\"]*?\??\s*(?P<q>['\"])(?P<key>[^'\"]+)(?P=q)")
# t(var) call sites, or a helper that forwards its argument to t()
# (`function modeLabel(v) { return t(key); }`): that module's code tables
# can hand t() any key-shaped literal they carry.
# Any indexed lookup (`TABLE[String(v)]`, `MAP[x]`) feeds t() in these
# accessor modules (modeLabel/tuyaIotHint do exactly that), so their
# key-shaped literals are candidates.
KEY_TABLE_RE = re.compile(r"\[[^\]\n]*\]")


def has_key_tables(text: str) -> bool:
    return text.count("t(") > 0 and KEY_TABLE_RE.search(text) is not None
# Quoted key-shaped literals (table values and inline branches).
LITERAL_RE = re.compile(r"""(?P<q>['"])(?P<key>[a-z][a-z0-9]*(?:_[a-z0-9]+)+)(?P=q)""")
# DP codes that look like keys but live in code tables, never t()'d.
KNOWN_CODES = {
    "c_f", "clean_time", "dehumidify_set_value", "charge_done",
    "fan_speed_enum", "fan_speed_percent", "filter_reset", "goto_charge",
    "humidity_set", "mach_operate", "percent_control", "percent_state",
    "power_go", "spray_mode", "standby_st", "switch_horizontal",
    "switch_vertical", "to_charge", "work_mode", "work_status",
    "invalid_asset", "invalid_param",
}
# Document-language lookups that never reach t() (static shell attributes).
HTML_T_KEY_RE = re.compile(r"""data-i18n(?:-aria|-title)?=["']([a-z0-9_]+)["']""")

PLACEHOLDER_RE = re.compile(r"\{[a-z0-9_]+\}")


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def ph_set(value: object) -> set[str]:
    # Dict values are plain strings; anything else has no placeholders.
    return set(PLACEHOLDER_RE.findall(value)) if isinstance(value, str) else set()


def ph_fmt(s: set[str]) -> str:
    return ("{" + ", ".join(sorted(s)) + "}") if s else "(none)"


def main() -> int:
    errors: list[str] = []
    dicts: dict[str, dict] = {}
    for lang in ("en", "vi"):
        path = SRC_DIR / f"{lang}.json"
        try:
            dicts[lang] = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"error: cannot load {path}: {e}", file=sys.stderr)
            return 1
    en, vi = dicts["en"], dicts["vi"]
    en_keys, vi_keys = set(en), set(vi)

    for key in sorted(en_keys - vi_keys):
        errors.append(f"vi.json is missing key '{key}' (en.json has it)")
    for key in sorted(vi_keys - en_keys):
        errors.append(f"en.json is missing key '{key}' (vi.json has it)")
    for key in sorted(en_keys & vi_keys):
        pe, pv = ph_set(en[key]), ph_set(vi[key])
        if pe != pv:
            errors.append(f"placeholder mismatch '{key}': "
                          f"en {ph_fmt(pe)}, vi {ph_fmt(pv)}")

    files = sorted(JS_DIR.rglob("*.js"))
    if not files:
        print(f"error: no modules under {JS_DIR}", file=sys.stderr)
        return 1
    sources = {f.relative_to(JS_DIR).as_posix(): f.read_text(encoding="utf-8")
               for f in files}

    used: dict[str, list[str]] = {}
    calls = 0
    skipped = 0
    for rel, text in sources.items():
        for m in T_KEY_RE.finditer(text):
            calls += 1
            key = m.group("key")
            if any(key.startswith(p) for p in DYNAMIC_PREFIXES):
                skipped += 1
                continue
            used.setdefault(key, []).append(f"{rel}:{line_of(text, m.start())}")
        if has_key_tables(text):
            for lit in LITERAL_RE.finditer(text):
                key = lit.group("key")
                if any(key.startswith(p) for p in DYNAMIC_PREFIXES):
                    continue
                used.setdefault(key, []).append(
                    f"{rel}:{line_of(text, lit.start())} (table)")

    html_sources = {f.name: f.read_text(encoding="utf-8")
                    for f in sorted(SRC_DIR.rglob("*.html"))}
    html_calls = 0
    for rel, text in html_sources.items():
        for m in HTML_T_KEY_RE.finditer(text):
            html_calls += 1
            used.setdefault(m.group(1), []).append(f"{rel}:{line_of(text, m.start())}")

    # An unknown key is an error only at a real t() site; a code table's DP
    # codes that merely look like keys are ignored.
    for key, locs in sorted(used.items()):
        if key in en_keys or key in KNOWN_CODES:
            continue
        for loc in locs:
            if loc.endswith("(table)"):
                continue
            errors.append(f"{loc}: t() key '{key}' not in en.json")

    members = {p: {k for k in en_keys if k.startswith(p)} for p in DYNAMIC_PREFIXES}
    for prefix, found in members.items():
        module, base = DYNAMIC_PREFIXES[prefix]
        for key in sorted(found):
            if not FAMILY_KEY_RE[prefix].fullmatch(key):
                errors.append(f"'{key}' starts with the dynamic family '{prefix}' "
                              f"but is not a key that family can build")
        if not found:
            errors.append(f"dynamic prefix '{prefix}' owns no en.json key (dead family)")
        if module not in sources:
            errors.append(f"family '{prefix}': {module} is gone")
        elif base not in sources[module]:
            errors.append(f"family '{prefix}': '{base}' no longer appears in {module}")

    via_static = en_keys & set(used)
    via_prefix = set().union(*members.values()) if members else set()
    via_whitelist = {k for k in en_keys - via_static - via_prefix if k in KNOWN_UNUSED}
    accounted = via_static | via_prefix | via_whitelist
    for key in sorted(en_keys - accounted):
        errors.append(f"en.json key '{key}' is neither t()'d (directly, via a "
                      f"lookup table or data-i18n), nor under a dynamic prefix, "
                      f"nor whitelisted")
    for key in sorted(set(KNOWN_UNUSED) - en_keys):
        errors.append(f"whitelisted key '{key}' is not in en.json (stale row)")
    for key in sorted(set(KNOWN_UNUSED) & (via_static | via_prefix)):
        errors.append(f"whitelisted key '{key}' is now used — drop the whitelist row")
    for code in sorted(KNOWN_CODES & en_keys):
        errors.append(f"'{code}' is both a known DP code and a dict key — "
                      f"drop the KNOWN_CODES row")

    print(f"en: {len(en_keys)} keys, vi: {len(vi_keys)} keys")
    print(f"t(): {calls} literal calls + table literals, {len(used)} distinct "
          f"candidate keys ({skipped} skipped: dynamic prefixes) in {len(files)} "
          f"modules; {html_calls} data-i18n attributes in {len(html_sources)} html files")
    print(f"accounted: {len(accounted)}/{len(en_keys)} "
          f"(static/table/html {len(via_static)}, dynamic-prefix {len(via_prefix)}, "
          f"whitelist {len(via_whitelist)})")
    print("dynamic families: " + ", ".join(
        f"{p}({len(members[p])})" for p in DYNAMIC_PREFIXES))

    if errors:
        for e in errors:
            print("error: " + e, file=sys.stderr)
        print(f"i18n parity: FAIL ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(f"i18n parity: PASS ({len(en_keys)} keys, {len(accounted)} accounted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
