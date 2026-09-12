#!/usr/bin/env python3
"""Dump twinvq_tables.cpp arrays to a compact binary for the browser encoder."""
from __future__ import annotations

import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "twinvq_tables.cpp"
OUT = Path("/workspace/public/twinvq-tables.bin")

TYPE_MAP = {
    "uint16_t": (0, "<H"),
    "int16_t": (1, "<h"),
    "float": (2, "<f"),
    "uint8_t": (3, "<B"),
}

ARRAY_RE = re.compile(
    r"const\s+(uint16_t|int16_t|float|uint8_t)\s+(\w+)\s*\[\s*\d+\s*\]\s*=\s*\{",
    re.M,
)


def parse_values(body: str, ctype: str) -> list:
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//.*?$", "", body, flags=re.M)
    vals = []
    for tok in body.replace("{", " ").replace("}", " ").split(","):
        tok = tok.strip().rstrip("fF")
        if not tok:
            continue
        vals.append(float(tok) if ctype == "float" else int(tok, 0))
    return vals


def extract():
    text = SRC.read_text()
    tables = []
    for m in ARRAY_RE.finditer(text):
        ctype, name = m.group(1), m.group(2)
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
        vals = parse_values(body, ctype)
        tables.append((ctype, name, vals))
        print(f"{name:24} {ctype:8} {len(vals)}")
    magic = b"TVQT"
    blob = bytearray(magic)
    blob += struct.pack("<I", len(tables))
    for ctype, name, vals in tables:
        tcode, fmt = TYPE_MAP[ctype]
        nb = name.encode()
        blob += struct.pack("<BHI", tcode, len(nb), len(vals))
        blob += nb
        for v in vals:
            blob += struct.pack(fmt, v)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    print(f"wrote {OUT} ({len(blob)} bytes, {len(tables)} tables)")


if __name__ == "__main__":
    extract()
