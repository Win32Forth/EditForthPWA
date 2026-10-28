#!/usr/bin/env python3
"""Turn Forth sources into one C string for both the native and WASM builds."""
import pathlib
import sys

dst = sys.argv[1]
parts = []
for src in sys.argv[2:]:
    parts.append(pathlib.Path(src).read_text(encoding="utf-8"))
text = "\n".join(part.rstrip("\n") for part in parts) + "\n"
lines = ["static const char boot_fth[] ="]
for line in text.splitlines():
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append(f'"{esc}\\n"')
lines.append(";")
pathlib.Path(dst).write_text("\n".join(lines) + "\n", encoding="utf-8")
