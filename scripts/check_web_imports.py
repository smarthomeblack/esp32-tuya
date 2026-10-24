#!/usr/bin/env python3
"""Static import-graph check for the web UI ES modules: every import
resolves; core/ never imports features/; features->features edges stay
in-family or whitelisted; imported bindings are used; every export has
an importer. Run: python scripts/check_web_imports.py"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
JS_DIR = PROJECT_ROOT / "components" / "web_assets" / "src" / "js"

IMPORT_RE = re.compile(
    r"import\s+(?:[\w$*\s{},]+from\s*)?['\"]([^'\"]+)['\"]"
)

# Sanctioned feature->feature edges; the whitelist itself is the spec
# (all one-way).
SANCTIONED = {
    "features/dashboard.js -> features/cards.js",
    "features/dashboard.js -> features/cards-commands.js",
    "features/dashboard.js -> features/cards-registry.js",
    "features/dashboard.js -> features/rooms.js",
    "features/dashboard.js -> features/scan.js",
    "features/gate.js -> features/onboarding.js",
    "features/gate.js -> features/tuya-iot.js",
    "features/onboarding.js -> features/scan.js",
    "features/onboarding.js -> features/tuya-iot.js",
    "features/settings.js -> features/onboarding.js",
    "features/settings.js -> features/tuya-iot.js",
}


def module_of(path: Path) -> str:
    return path.relative_to(JS_DIR).as_posix()


def family(name: str) -> str | None:
    base = name.rsplit("/", 1)[-1]
    if base == "cards.js" or base.startswith("cards-"):
        return "cards"
    if base == "settings.js" or base.startswith("settings-"):
        return "settings"
    return None


def imports_of(text: str) -> list[str]:
    # Matches "import ... from 'x'" and side-effect "import 'x'";
    # specifiers never appear inside template literals here.
    return [m.group(1) for m in IMPORT_RE.finditer(text)]


# Import statements with the full clause (named lists may wrap across
# lines); clause=None is a side-effect import (no bindings to check).
IMPORT_STMT_RE = re.compile(
    r"(?<![.\w$])import\s+(?:(?P<clause>[\w$*\s{},]+?)\s*from\s*)?"
    r"['\"](?P<spec>[^'\"]+)['\"]"
)

# Export statements: declarations, default, brace lists, star re-exports.
EXPORT_STMT_RE = re.compile(
    r"(?<![.\w$])export\s+(?:"
    r"default\b"
    r"|(?:async\s+)?function\s*\*?\s*(?P<fn>[\w$]+)"
    r"|(?:const|let|var)\s+(?P<decl>[\w$]+)"
    r"|class\s+(?P<cls>[\w$]+)"
    r"|\{(?P<braced>[^{}]*)\}"
    r"(?:\s*from\s*['\"](?P<braced_spec>[^'\"]+)['\"])?"
    r"|\*(?:\s*as\s+(?P<ns>[\w$]+))?\s*from\s*['\"](?P<star_spec>[^'\"]+)['\"]"
    r")"
)


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def import_stmts_of(text: str) -> list[tuple[int, int, str, str | None]]:
    """(first_line, last_line, spec, clause) for each import statement."""
    return [(line_of(text, m.start()), line_of(text, m.end()),
             m.group("spec"), m.group("clause"))
            for m in IMPORT_STMT_RE.finditer(text)]


def split_clause(clause: str) -> tuple[str | None, str | None, list[tuple[str, str]]]:
    """(default, namespace, [(imported, local)]); for aliases the LOCAL
    name is the one the importing file must actually use."""
    m = re.search(r"\{([^{}]*)\}", clause)
    if m:
        braced, outside = m.group(1), clause[:m.start()] + clause[m.end():]
    else:
        braced, outside = "", clause
    named = []
    for item in braced.split(","):
        words = [w for w in item.split() if w and w != "as"]
        if words:
            named.append((words[0], words[-1]))
    ns = re.search(r"\*\s*as\s+([\w$]+)", outside)
    namespace = ns.group(1) if ns else None
    if ns:
        outside = outside[:ns.start()] + outside[ns.end():]
    words = outside.replace(",", " ").replace("*", " ").split()
    return (words[0] if words else None), namespace, named


def export_stmts_of(text: str) -> list[tuple]:
    """Normalized records: ("name", export_name, line) | ("default", line)
    | ("braced", [(imported, exported)], line, spec|None) | ("star",
    ns|None, line, spec)."""
    out = []
    for m in EXPORT_STMT_RE.finditer(text):
        line = line_of(text, m.start())
        name = m.group("fn") or m.group("decl") or m.group("cls")
        if name:
            out.append(("name", name, line))
        elif m.group("braced") is not None:
            items = []
            for item in m.group("braced").split(","):
                words = [w for w in item.split() if w and w != "as"]
                if words:
                    items.append((words[0], words[-1]))
            out.append(("braced", items, line, m.group("braced_spec")))
        elif m.group("star_spec") is not None:
            out.append(("star", m.group("ns"), line, m.group("star_spec")))
        else:
            out.append(("default", line))
    return out


def binding_used(name: str, body: str) -> bool:
    # $ is an identifier char in JS, so plain \b would miss $ / $$ names.
    return re.search(r"(?<![\w$])" + re.escape(name) + r"(?![\w$])", body) is not None


def main() -> int:
    files = sorted(JS_DIR.rglob("*.js"))
    if not files:
        print(f"error: no modules under {JS_DIR}", file=sys.stderr)
        return 1

    graph: dict[str, list[str]] = {}
    errors: list[str] = []
    infos: list[str] = []
    unused: list[str] = []   # findings: unused import bindings
    dead: list[str] = []     # findings: exports with no importers
    paths: dict[str, Path] = {}
    imports_by_mod: dict[str, list[tuple]] = {}
    exports_by_mod: dict[str, list[tuple[str, int]]] = {}
    reexports: list[tuple[str, str, list[str] | None, bool]] = []

    for f in files:
        mod = module_of(f)
        paths[mod] = f
        text = f.read_text(encoding="utf-8")
        imports_by_mod[mod] = []
        exports_by_mod[mod] = []
        deps = []
        for spec in imports_of(text):
            target = (f.parent / spec).resolve()
            if not target.is_file():
                errors.append(f"{mod}: import '{spec}' does not resolve to a file")
                continue
            if not target.is_relative_to(JS_DIR):
                errors.append(f"{mod}: import '{spec}' escapes the js/ tree")
                continue
            deps.append(target.relative_to(JS_DIR).as_posix())
        graph[mod] = deps

        # Binding-level data, same single pass over the text. Usage bodies
        # exclude import-statement lines; "export {x} from" lines still
        # count (re-exporting IS using the binding).
        stmts = import_stmts_of(text)
        excluded = set()
        for first, last, _, _ in stmts:
            excluded.update(range(first, last + 1))
        body = "\n".join(
            line for i, line in enumerate(text.split("\n"), 1)
            if i not in excluded)
        for first, last, spec, clause in stmts:
            if clause is None:
                continue  # side-effect import: no bindings, consumes none
            default, namespace, named = split_clause(clause)
            imports_by_mod[mod].append((spec, default, namespace, named))
            local_names = [n for n in (default, namespace) if n]
            local_names += [loc for _, loc in named]
            for name in local_names:
                if not binding_used(name, body):
                    unused.append(
                        f"{mod}:{first}: unused import binding '{name}' from '{spec}'")

        for rec in export_stmts_of(text):
            if rec[0] == "name":
                exports_by_mod[mod].append((rec[1], rec[2]))
            elif rec[0] == "default":
                exports_by_mod[mod].append(("default", rec[1]))
            elif rec[0] == "braced":
                _, items, line, spec = rec
                for _, exported in items:
                    exports_by_mod[mod].append((exported, line))
                if spec is not None:
                    reexports.append((mod, spec, [imp for imp, _ in items], False))
            else:  # star re-export: consumes every current export of the target
                _, ns, line, spec = rec
                if ns:
                    exports_by_mod[mod].append((ns, line))
                reexports.append((mod, spec, None, True))

    def layer_of(mod: str) -> str:
        if mod == "app.js":
            return "entry"
        top = mod.split("/", 1)[0]
        return top if top in ("core", "ui", "features") else "?"

    for mod, deps in sorted(graph.items()):
        for dep in deps:
            src_l, dst_l = layer_of(mod), layer_of(dep)
            if src_l == "core" and dst_l == "features":
                errors.append(f"core must not import features: {mod} -> {dep}")
            if src_l == "features" and dst_l == "features":
                if family(mod) is not None and family(mod) == family(dep):
                    continue  # intra cards-*/settings-* family
                if f"{mod} -> {dep}" in SANCTIONED:
                    continue
                errors.append(
                    f"features -> features outside cards-*/settings-* families: {mod} -> {dep}")
            if src_l == "ui" and dst_l == "features":
                infos.append(f"ui -> features edge (appbar composes tab content): {mod} -> {dep}")

    # Cycle detection (DFS); a cycle is allowed only when every module on
    # it belongs to the same cards-*/settings-* family.
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {m: WHITE for m in graph}
    stack: list[str] = []

    def dfs(mod: str) -> None:
        color[mod] = GRAY
        stack.append(mod)
        for dep in graph.get(mod, []):
            if color[dep] == GRAY:
                cyc = stack[stack.index(dep):] + [dep]
                fams = {family(m) for m in cyc}
                if len(fams) == 1 and fams != {None}:
                    infos.append("allowed intra-family cycle: " + " -> ".join(cyc))
                else:
                    errors.append("import cycle: " + " -> ".join(cyc))
            elif color[dep] == WHITE:
                dfs(dep)
        stack.pop()
        color[mod] = BLACK

    for m in sorted(graph):
        if color[m] == WHITE:
            dfs(m)

    # Dead exports: consumed = every binding any module imports or
    # re-exports. Side-effect imports consume nothing; namespace imports
    # and star re-exports consume all current exports of the target.
    consumed: set[tuple[str, str]] = set()

    def resolve_spec(mod: str, spec: str) -> str | None:
        target = (paths[mod].parent / spec).resolve()
        if not target.is_file() or not target.is_relative_to(JS_DIR):
            return None  # already reported by the resolution check
        return target.relative_to(JS_DIR).as_posix()

    for mod, records in imports_by_mod.items():
        for spec, default, namespace, named in records:
            tgt = resolve_spec(mod, spec)
            if tgt is None:
                continue
            if default:
                consumed.add((tgt, "default"))
            if namespace:
                consumed.update((tgt, n) for n, _ in exports_by_mod.get(tgt, []))
            for imported, _ in named:
                consumed.add((tgt, imported))
    for mod, spec, names, is_star in reexports:
        tgt = resolve_spec(mod, spec)
        if tgt is None:
            continue
        if is_star:
            consumed.update((tgt, n) for n, _ in exports_by_mod.get(tgt, []))
        else:
            for name in names or []:
                consumed.add((tgt, name))

    for mod in sorted(exports_by_mod):
        for name, line in exports_by_mod[mod]:
            if (mod, name) not in consumed:
                dead.append(f"{mod}:{line}: export '{name}' has no importers")

    errors.extend(unused)
    errors.extend(dead)

    print(f"modules: {len(graph)}")
    print("import graph (file names):")
    for mod in sorted(graph):
        deps = sorted(d.rsplit("/", 1)[-1] for d in graph[mod])
        print(f"  {mod:<36} -> {', '.join(deps) if deps else '(leaf)'}")

    print()
    for i in infos:
        print("note: " + i)

    if errors:
        print()
        for e in errors:
            print("error: " + e, file=sys.stderr)
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        return 1
    print("\nPASS: all imports resolve; no unused import bindings; no dead "
          "exports; no core->features edges; no forbidden cycles.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
