#!/usr/bin/env python3
"""Write the ninja build of the game module (libdon_game.so) for a tree emitted with general.emit_module_abi = true.

usage: make_ninja.py --gen <folder with the emitted sources> --out <build folder> [--target linux|windows]

--target windows writes the twin build of don_game.dll: clang --target=x86_64-pc-windows-msvc with the same freestanding
flags, linked by lld-link as a DLL with no entry point and no default libraries (so no imports), plus
src/game_module/win_runtime.cpp for __chkstk and _fltused. It has no shell substitution: the compiler's resource
folder is asked of `clang++ -print-resource-dir` here, while writing the file (clang++ must be on PATH).

Run ninja in <build folder> inside the build image (clang and ld.lld live there): tools/container/run.sh ninja -C <out>.
The emitted code includes only src/game_module/don_guest_*.h: no shim folder is on any include path. The runtime's
macro header is derived into <out>/inc by tools/pipeline/gen_shim_macros.py (nothing from the runtime is copied into git).
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIG_BYTES = 200000  # same split as tools/pipeline/generate.sh: a file this big is its own translation unit
BATCH = 32          # small files per translation unit
MACROS = ROOT / "third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h"

FLAGS = (
    "-std=gnu++20 -nostdinc -nostdinc++ -isystem {resource}/include -ffreestanding -fno-rtti "
    "-fno-threadsafe-statics -fvisibility=hidden -fno-stack-protector -ffp-contract=off -march=x86-64-v3 "
    "-fno-crash-diagnostics -DNDEBUG -DDON_GAME_MODULE_BUILD -DDON_MODULE_IOP "
    "-I {gen} -I {iop} -I {inc} -I {guest} -I {host} -ffile-prefix-map={root}=."
)

RULES = """flags = {flags}
rule cc
  command = clang++ {target_flag}$flags $opt -MD -MF $out.d -c $in -o $out 2> $out.err
  depfile = $out.d
  deps = gcc
  description = CC $out
rule link
  command = {link} @$out.rsp
  rspfile = $out.rsp
  rspfile_content = $in
  description = LINK $out
"""

GAME_OPT = "-O2"           # optimisation level of every game unit; the measurements are next to kGameOpt in src/setup/module_build.cpp (T-752)
REGISTRATION_OPT = "-O1"   # the unit holding register_functions.cpp: a 7 MB registration table that runs no game instruction
REGISTRATION_UNIT = "big_register_functions"

LINK_LINUX = "ld.lld -shared --hash-style=gnu -z now --no-undefined-version --eh-frame-hdr -o $out"
# lld-link flavour (picked from the program name): a DLL, no entry point, no default libraries -> no imports; /Brepro keeps
# the header free of a timestamp. don_game_module_v1 is exported by __declspec(dllexport) in module_entry.cpp.
LINK_WINDOWS = "lld-link /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /Brepro /OUT:$out"
WINDOWS_TARGET = "--target=x86_64-pc-windows-msvc "
LINUX_RESOURCE = "$$(clang++ -print-resource-dir)"  # ninja expands $$ to $; the shell substitutes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gen", required=True, type=Path, help="folder with the emitted sources")
    ap.add_argument("--out", required=True, type=Path, help="build folder (build.ninja, unity/, obj/, libdon_game.so or don_game.dll)")
    ap.add_argument("--target", choices=("linux", "windows"), default="linux", help="platform of the module (default linux)")
    ap.add_argument("--opt", default=GAME_OPT, help=f"optimisation flag of the game units (default {GAME_OPT})")
    args = ap.parse_args()
    gen, out = args.gen.resolve(), args.out.resolve()
    win = args.target == "windows"
    module = "don_game.dll" if win else "libdon_game.so"
    resource = subprocess.run(["clang++", "-print-resource-dir"], check=True, capture_output=True, text=True).stdout.strip() if win else LINUX_RESOURCE
    (out / "unity").mkdir(parents=True, exist_ok=True)
    (out / "inc").mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/pipeline/gen_shim_macros.py"), str(MACROS),
                    str(out / "inc/don_guest_macros.h")], check=True)

    sources = sorted(p for p in gen.glob("*.cpp"))
    big = [p for p in sources if p.stat().st_size >= BIG_BYTES]
    small = [p for p in sources if p.stat().st_size < BIG_BYTES]
    units: list[tuple[str, list[Path]]] = [(f"batch_{i // BATCH:05d}", small[i:i + BATCH]) for i in range(0, len(small), BATCH)]
    units += [("big_" + p.stem, [p]) for p in big]

    lines = [RULES.format(target_flag=WINDOWS_TARGET if win else "", link=LINK_WINDOWS if win else LINK_LINUX,
                                  flags=FLAGS.format(resource=resource, gen=gen, iop=ROOT / "src/game_module/iop_shim", inc=out / "inc",
                                             guest=ROOT / "src/game_module", host=ROOT / "src/host", root=ROOT))]
    objs = []
    for name, files in units:
        (out / "unity" / f"{name}.cpp").write_text("".join(f'#include "{f}"\n' for f in files))
        lines.append(f"build obj/{name}.o: cc unity/{name}.cpp\n  opt = {REGISTRATION_OPT if name == REGISTRATION_UNIT else args.opt}\n")
        objs.append(f"obj/{name}.o")
    lines.append(f"build obj/entry.o: cc {ROOT / 'src/game_module/module_entry.cpp'}\n  opt = {args.opt}\n")
    if win:
        lines.append(f"build obj/win_runtime.o: cc {ROOT / 'src/game_module/win_runtime.cpp'}\n  opt = {args.opt}\n")
        objs.append("obj/win_runtime.o")
    lines.append(f"build {module}: link obj/entry.o " + " ".join(objs) + "\n")
    lines.append(f"default {module}\n")
    (out / "build.ninja").write_text("".join(lines))
    print(f"{len(units)} units from {len(sources)} sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
