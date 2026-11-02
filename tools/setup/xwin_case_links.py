"""Adds Capitalised symlinks (winver.h -> Winver.h) next to every lowercase header of an xwin
sysroot, because third-party sources include Windows headers with the case used on Windows."""
import os, sys
root = sys.argv[1]
n = 0
for sub in ("sdk/include/um", "sdk/include/shared", "sdk/include/ucrt", "crt/include"):
    d = os.path.join(root, sub)
    names = set(os.listdir(d))
    for f in sorted(names):
        if not f.endswith(".h") or not f[0].islower():
            continue
        alt = f[0].upper() + f[1:]
        if alt not in names and alt.lower() != alt:
            os.symlink(f, os.path.join(d, alt)); n += 1
print("links added:", n)
