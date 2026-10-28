#!/usr/bin/env python3
"""Build the GitHub Pages tree in _site.

Pages serves the web folder. web/Resources is a symlink, and Pages does not
list directories, so this copies the real Resources tree and writes an
index.html in each folder. The page parser reads those <a href> links.
"""

import os
import shutil
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "web"
OUT = ROOT / "_site"


def link(name, directory):
    href = quote(name, safe="")
    if directory:
        href += "/"
    text = (
        name.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )
    return f'<a href="{href}">{text}</a>'


def write_listings(resources):
    for dirpath, dirnames, filenames in os.walk(resources):
        dirnames[:] = sorted(name for name in dirnames if not name.startswith("."))
        lines = ["<!DOCTYPE html>", "<html>", "<body>"]
        for name in dirnames:
            lines.append(link(name, True))
        for name in sorted(filenames):
            if name.startswith(".") or name == "index.html":
                continue
            lines.append(link(name, False))
        lines.extend(["</body>", "</html>", ""])
        index = Path(dirpath) / "index.html"
        if index.exists():
            raise SystemExit(f"refusing to replace {index}")
        index.write_text("\n".join(lines), encoding="utf-8")


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(
        SRC,
        OUT,
        symlinks=False,
        ignore=shutil.ignore_patterns("*.map", ".DS_Store"),
    )
    resources = OUT / "Resources"
    if not resources.is_dir() or resources.is_symlink():
        raise SystemExit("Resources did not copy as a real folder")
    write_listings(resources)


if __name__ == "__main__":
    main()
