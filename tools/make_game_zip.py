"""Packs an installed copy of Pac-Man World Rally (PC) into the .zip the web page asks for.

Usage: python tools/make_game_zip.py [game_folder] [output.zip]

The folder must contain PMR.exe, the .xpc archives and streams/. Any zip of the install folder
works as well (Windows: right-click the folder > Send to > Compressed (zipped) folder)."""
import os, sys, zipfile

src = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\Pac-Man World Rally")
out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "pmwr-game.zip")
if not os.path.isfile(os.path.join(src, "PMR.exe")):
    sys.exit("PMR.exe not found in " + src)

n = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for dp, dn, fn in os.walk(src):
        dn[:] = [d for d in dn if not d.startswith(".") and d.lower() != "dont need a crack"]
        for f in fn:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            if full == out:
                continue
            comp = zipfile.ZIP_STORED if f.lower().endswith(".ogg") else zipfile.ZIP_DEFLATED
            z.write(full, rel, compress_type=comp)
            n += 1
print("%s: %d files, %.0f MB" % (out, n, os.path.getsize(out) / 1e6))
