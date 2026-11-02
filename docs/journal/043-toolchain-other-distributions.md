# 043 · Pinned toolchain on other distributions (T-619)

## Classification
Toolchain reproducibility (M6 public release). No precedent in `docs/knowledge/08-external-references.md`.

## Investigation
- Toolchain: LLVM 17.0.6 `clang+llvm-17.0.6-x86_64-linux-gnu-ubuntu-22.04.tar.xz`, fetched by
  `tools/pipeline/fetch_toolchain.py` into `build/toolchain/linux/` (tree hash
  `98373bd4a3d582cc82c37f7034b3223f49ee1a37b207f785379ab32326ebac53`, `--check` OK). The main checkout had no
  toolchain cache, so the archive was downloaded into this clone's `build/toolchain/cache/`.
- Test unit: `work/t603/mod.cpp` (a 161-byte stub exporting `don_mod`, no game code), copied to `work/t619/`.
  Flags are the A1 set of `docs/tasks/M6-public-release.md:76-131`, as in `work/t603/run.sh`, plus `-O1 -fPIC`,
  `-funwind-tables`, `-nostdinc -isystem <resource dir>/include`, `-ffile-prefix-map`, no `-g`.
- Compile: `clang++ $FLAGS -c mod.cpp -o mod.o` (identical command line on the host and in both containers, only
  the toolchain path differs).
- Link: the default link (`clang++ -shared`) needs the C runtime start files and `-lc`/`-lgcc`, which are development
  packages not present in the containers. The comparable link is `-shared -nostdlib` with lld; the default link is not
  compared because its hash depends on the host C runtime.

## Reproduction
Run `tools/verify/toolchain_cross_check.sh` (default toolchain `build/toolchain/linux`, output `work/t619/out`). It
writes the stub, builds it on the host, in `debian:12` and in `fedora:40` (toolchain mounted read-only at the same
path), and prints the hashes. Every flag is commented there with its source (A1, `work/t603/run.sh` or the script).
`-std=gnu++20` comes from `work/t603/run.sh:2` and is in the script's compile line.
Link mode: A1 makes the module freestanding with no imports and T-604 measured `nm -D -u` empty, so the player build
links `-shared -nostdlib` with lld.
`ld.lld` needs `libxml2.so.2`, absent in `debian:12`; the script supplies the host's copy and its ICU libraries
through `LD_LIBRARY_PATH` in that container only (no network, nothing installed). `fedora:40` gets no extra library and
built without one (the run log has no `not found` line).

## Cause
Not applicable: no failure. The object file is byte-identical across the three environments.

## Fix
None. Nothing in the repository's sources or patches changed.

## Verification
Output of the one run of the script (`work/t619/run.log`, last 12 lines, literal; exit code 0):

```
== debian:12
== fedora:40
<clone>/build/toolchain/linux/bin/clang++: /lib64/libtinfo.so.6: no version information available (required by <clone>/build/toolchain/linux/bin/clang++)
<clone>/build/toolchain/linux/bin/clang++: /lib64/libtinfo.so.6: no version information available (required by <clone>/build/toolchain/linux/bin/clang++)
<clone>/build/toolchain/linux/bin/ld.lld: /lib64/libtinfo.so.6: no version information available (required by <clone>/build/toolchain/linux/bin/ld.lld)
== sha256
8f1b60941be62e8544d022785a5ac7eaf7987bd4beeb9aefb056abfe1e82fc45  host/mod.o (host)
8192b65c8684e7149e5114f6d35c37fabdb4ff862290b90dd06a18cafc8e6650  host/mod-nostdlib.so (host)
8f1b60941be62e8544d022785a5ac7eaf7987bd4beeb9aefb056abfe1e82fc45  debian12/mod.o (debian12)
8192b65c8684e7149e5114f6d35c37fabdb4ff862290b90dd06a18cafc8e6650  debian12/mod-nostdlib.so (debian12)
8f1b60941be62e8544d022785a5ac7eaf7987bd4beeb9aefb056abfe1e82fc45  fedora40/mod.o (fedora40)
8192b65c8684e7149e5114f6d35c37fabdb4ff862290b90dd06a18cafc8e6650  fedora40/mod-nostdlib.so (fedora40)
```

The default link (`clang++ -shared`) needs the distribution's C runtime, so it is not compared.

Verdict: the pinned LLVM 17.0.6 compiles the test unit to the same object bytes on Ubuntu 22.04 (host), Debian 12 and
Fedora 40, and the `-nostdlib` shared object is also identical. The default link needs the distribution's C runtime
and is not reproducible across systems without it. This measurement covers the compile step and the `-nostdlib` link.

## Temporary diagnostics
None. Scripts were one-off shell commands; no diagnostic was added to the repository.
