#!/usr/bin/env python3
"""Derive a manifest that builds the app module from this working tree."""
import re
import sys

src, dst = sys.argv[1:3]
s = open(src).read()
s, n = re.subn(
    r"    sources:\n      - type: git\n        url: https://github.com/oh2gba/otd.git\n        tag: v[0-9.]+\n",
    "    sources:\n      - type: dir\n        path: .\n"
    "        skip: [.flatpak, .flatpak-builder, .git, build, build-flatpak, build-appimage, "
    "build-windows, dist, data-local, repo]\n",
    s)
if n != 1:
    sys.exit("git source of the app module not found")
open(dst, "w").write(s)
