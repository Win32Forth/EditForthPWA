#!/usr/bin/env python3
"""Write build/prim_lines.inc: each primitive name and the line of its C function.

The table in kernel/ef32.c names the function. VIEW opens that function, not the
table row. Rebuild this file whenever ef32.c changes.
"""

import re
import sys
from pathlib import Path


def unescape(src):
    out = []
    i = 0
    while i < len(src):
        if src[i] == "\\" and i + 1 < len(src):
            nxt = src[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "t":
                out.append("\t")
            else:
                out.append(nxt)
            i += 2
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def c_string(text):
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def main():
    src = Path(sys.argv[1])
    dest = Path(sys.argv[2])
    lines = src.read_text(encoding="utf-8").splitlines()
    funcs = {}
    for n, line in enumerate(lines, 1):
        match = re.match(r"static int (p_\w+)\s*\(", line)
        if match and match.group(1) not in funcs:
            funcs[match.group(1)] = n
    text = "\n".join(lines)
    start = text.find("k_prims[]")
    if start < 0:
        raise SystemExit("k_prims[] not found")
    end = text.find("};", start)
    table = text[start:end]
    entries = re.findall(
        r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*(p_\w+)\s*,',
        table,
    )
    if not entries:
        raise SystemExit("no primitive entries")
    rows = []
    seen = set()
    for name, fn in entries:
        if name in seen:
            continue
        seen.add(name)
        if fn not in funcs:
            raise SystemExit(f"no function line for {name} ({fn})")
        rows.append(f"    {{ {c_string(unescape(name))}, {funcs[fn]} }},")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"{dest}: {len(rows)} primitives")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: prim_lines.py kernel/ef32.c build/prim_lines.inc")
    main()
