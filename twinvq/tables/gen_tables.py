#!/usr/bin/env python3
"""Extract TwinVQ numerical codebooks into our own C++ tables.

Source data: public TwinVQ codebook constants (same numbers used by
independent reverse-engineered decoders). Output is original C++ in
namespace twinvq::tables — not a copy of FFmpeg source files.
"""
from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT_H = ROOT / "src" / "twinvq_tables.hpp"
OUT_C = ROOT / "src" / "twinvq_tables.cpp"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*?$", "", text, flags=re.M)
    return text


def parse_brace_array(body: str) -> list:
    """Parse a comma-separated C initializer into a nested list of numbers."""
    body = body.strip()
    if body.endswith(","):
        body = body[:-1]

    def parse(s: str, i: int):
        while i < len(s) and s[i] in " \t\n\r":
            i += 1
        if i >= len(s):
            return [], i
        if s[i] == "{":
            items = []
            i += 1
            while True:
                while i < len(s) and s[i] in " \t\n\r,":
                    i += 1
                if i < len(s) and s[i] == "}":
                    return items, i + 1
                val, i = parse(s, i)
                items.append(val)
            return items, i
        j = i
        while j < len(s) and s[j] not in ",}":
            j += 1
        tok = s[i:j].strip()
        if not tok:
            return 0, j
        if "." in tok or "e" in tok or "E" in tok:
            return float(tok), j
        return int(tok, 0), j

    vals, _ = parse("{" + body + "}", 0)
    return vals


def flatten(vals):
    out = []
    for v in vals:
        if isinstance(v, list):
            out.extend(flatten(v))
        else:
            out.append(v)
    return out


def emit_array(name: str, ctype: str, values, per_line: int = 12) -> str:
    lines = [f"const {ctype} {name}[{len(values)}] = {{"]
    row = []
    for i, v in enumerate(values):
        if ctype == "float":
            s = f"{float(v):.9g}"
            if "." not in s and "e" not in s and "E" not in s:
                s += ".0"
            row.append(s + "f")
        else:
            row.append(str(int(v)))
        if len(row) == per_line or i == len(values) - 1:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def parse_named_arrays(text: str, typename: str) -> dict:
    """Parse standalone `static const T name[N] = { ... };` arrays."""
    out = {}
    pat = re.compile(
        rf"static const {typename} (\w+)\s*(?:\[[^\]]*\])+\s*=\s*\{{",
        re.M,
    )
    for m in pat.finditer(text):
        name = m.group(1)
        start = m.end()
        depth = 1
        i = start
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body = text[start : i - 1]
        out[name] = flatten(parse_brace_array(body))
    return out


def parse_struct_tab(text: str) -> dict:
    m = re.search(r"static const struct twinvq_data\s*\{(.*?)\}\s*tab\s*=\s*\{", text, re.S)
    if not m:
        raise SystemExit("twinvq_data struct not found")
    fields_src = m.group(1)
    fields = {name: (ctype, int(n)) for ctype, name, n in re.findall(r"(int16_t|float)\s+(\w+)\[(\d+)\];", fields_src)}
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    body = text[start : i - 1]
    out = {}
    for dm in re.finditer(r"\.(\w+)\s*=\s*\{", body):
        name = dm.group(1)
        j = dm.end()
        d = 1
        k = j
        while k < len(body) and d:
            if body[k] == "{":
                d += 1
            elif body[k] == "}":
                d -= 1
            k += 1
        vals = flatten(parse_brace_array(body[j : k - 1]))
        if name not in fields:
            raise SystemExit(f"unknown tab field {name}")
        ctype, n = fields[name]
        if len(vals) != n:
            raise SystemExit(f"{name}: expected {n} values, got {len(vals)}")
        out[name] = (ctype, vals)
    missing = set(fields) - set(out)
    if missing:
        raise SystemExit(f"missing tab fields: {sorted(missing)}")
    return out


def parse_lsp(text: str) -> dict:
    out = {}
    pat = re.compile(r"const float (ff_metasound_lsp\d+)\s*\[\s*\]\s*=\s*\{", re.M)
    for m in pat.finditer(text):
        name = m.group(1)
        start = m.end()
        depth = 1
        i = start
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body = text[start : i - 1]
        out[name] = flatten(parse_brace_array(body))
    return out


def main() -> None:
    data = strip_comments((HERE / "twinvq_data.h").read_text(encoding="utf-8", errors="replace"))
    meta = strip_comments((HERE / "metasound_twinvq_data.h").read_text(encoding="utf-8", errors="replace"))

    bark = parse_named_arrays(data, "uint16_t")
    broken = parse_named_arrays(data, "uint8_t")
    tab = parse_struct_tab(data)
    lsp = parse_lsp(meta)

    decls = []
    defs = []
    defs.append('#include "twinvq_tables.hpp"\n\nnamespace twinvq::tables {\n')

    for name, vals in sorted(bark.items()):
        decls.append(f"extern const uint16_t {name}[{len(vals)}];")
        defs.append(emit_array(name, "uint16_t", vals))

    for name, (ctype, vals) in tab.items():
        ct = "float" if ctype == "float" else "int16_t"
        decls.append(f"extern const {ct} {name}[{len(vals)}];")
        defs.append(emit_array(name, ct, vals, 8 if ct == "float" else 12))

    for name, vals in sorted(lsp.items()):
        decls.append(f"extern const float {name}[{len(vals)}];")
        defs.append(emit_array(name, "float", vals, 8))

    for name, vals in sorted(broken.items()):
        decls.append(f"extern const uint8_t {name}[{len(vals)}];")
        defs.append(emit_array(name, "uint8_t", vals, 16))

    # very_broken_op lookup: index by b/5, size + pointer into tabN
    # tabs[] = { {0,NULL}, {5,tab8}, {5,tab8}, {15,tab12}, {5,tab8}, {25,tab10},
    #            {15,tab12}, {35,tab7}, {5,tab8}, {45,tab9}, {25,tab10}, {55,tab11}, {15,tab12} }
    tab_map = {
        0: ("nullptr", 0),
        1: ("tab8", 5),
        2: ("tab8", 5),
        3: ("tab12", 15),
        4: ("tab8", 5),
        5: ("tab10", 25),
        6: ("tab12", 15),
        7: ("tab7", 35),
        8: ("tab8", 5),
        9: ("tab9", 45),
        10: ("tab10", 25),
        11: ("tab11", 55),
        12: ("tab12", 15),
    }
    decls.append("struct BrokenTab { int size; const uint8_t *tab; };")
    decls.append("extern const BrokenTab broken_tabs[13];")
    defs.append("const BrokenTab broken_tabs[13] = {")
    for i in range(13):
        ptr, size = tab_map[i]
        defs.append(f"    {{ {size}, {ptr} }},")
    defs.append("};\n")
    defs.append("} // namespace twinvq::tables\n")

    header = """#pragma once
#include <cstdint>

// TwinVQ codebook constants (numerical codec tables).
namespace twinvq::tables {
"""
    header += "\n".join(decls) + "\n}\n"
    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(header, encoding="utf-8")
    OUT_C.write_text("\n".join(defs), encoding="utf-8")
    print(f"wrote {OUT_H} ({OUT_H.stat().st_size} bytes)")
    print(f"wrote {OUT_C} ({OUT_C.stat().st_size} bytes)")
    print(f"bark={len(bark)} tab_fields={len(tab)} lsp={len(lsp)} broken={len(broken)}")


if __name__ == "__main__":
    main()
