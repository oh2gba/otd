#!/usr/bin/env python3
"""Write the NSIS include files for the installer from the packaged folder.

    nsis-lists.py <pkgdir> <files.nsh> <delete.nsh>

files.nsh gets a SetOutPath/File line per packaged file, delete.nsh the
matching Delete lines and RMDir lines for the folders, deepest first, so
the uninstaller removes exactly what the installer put there.
"""
import os
import sys

pkg, files_out, delete_out = sys.argv[1:4]
pkg = os.path.abspath(pkg)

files = []
dirs = []
for root, dnames, fnames in os.walk(pkg):
    rel = os.path.relpath(root, pkg)
    if rel != ".":
        dirs.append(rel)
    for f in fnames:
        files.append(os.path.normpath(os.path.join(rel, f)))
files.sort()
dirs.sort()

def win(p):
    return p.replace("/", "\\")

with open(files_out, "w") as out:
    current = None
    for f in files:
        d = os.path.dirname(f)
        if d != current:
            out.write('SetOutPath "$INSTDIR%s"\n' % ("\\" + win(d) if d else ""))
            current = d
        out.write('File "%s"\n' % os.path.join(pkg, f))

with open(delete_out, "w") as out:
    for f in files:
        out.write('Delete "$INSTDIR\\%s"\n' % win(f))
    for d in sorted(dirs, key=lambda s: -s.count("/")):
        out.write('RMDir "$INSTDIR\\%s"\n' % win(d))

print("%d files, %d folders" % (len(files), len(dirs)))
