# 018 · The crash at 0x66F80: a build truncated a shared library a running game had mapped

- **Date:** 2026-10-13
- **Task:** T-304 (the crash and the stop with sound)
- **Status:** resolved

## Symptom

About one run in three of the port with `--sound on` (and, before the VU1 fix of journal 012, with
`DON_VU1=microvu`) died with SIGSEGV: a call through a pointer that holds a *guest-looking* value,
`rip = 0x66F80`, an address that is not mapped in the process. It only ever happened in long runs, and
never under gdb (four runs of the same route under gdb, 0 crashes) nor without `--shots` (agent C's runs:
no crash without them). The first one was blamed on the video set-up near vertical blank 1740 (journal
011), and the ten words of the game's own code that the MPEG stubs used to overwrite were blamed for it
(journal 015). The second was wrong: the MPEG overwrite was real and is fixed (patch 0036), but the crash
survived it unchanged.

## Cause

`tools/pipeline/build.sh` ended every build with

```
cp -fL /opt/deps/lib/libSDL3.so.0.4.12 "$dir/libSDL3.so.0"
```

`cp -f` does not replace the file: it opens the *existing* one with `O_TRUNC` and rewrites it **in place**
(same inode). A running game has that library mapped. When a mapped file is truncated, the kernel discards
the process's pages of it - the private, modified ones included, which for a shared library are its
*relocated* data: the table of pointers (GOT/PLT) that its public entry points jump through, written by the
dynamic linker at load time. The next access faults the new bytes in from the rewritten file, but nothing
re-applies the relocations: the pointers are link-time addresses without the library's load address, so the
next call to SDL jumps there. `0x66F80` is one of SDL's own permanent link-time addresses (its
`SDL_SavePNG` is at 0x60010 in this build), which is why the value is always the same.

It fits every observation: only runs that still call SDL die (that is, only with `--shots`), only long
runs (the build has to happen after the library was loaded), and only when somebody built meanwhile - agent
B relinked at 12:59 during run 1 and at 14:01 during agent C's, and the four gdb runs that never crashed
were four runs during which nobody built. The guest state is identical up to the crash, because the crash
is entirely in the host's own library.

## Proof

`work/orchestrator/t304/sdl-copy-proof.sh`, two phases, each a run of `to_main_menu.txt` with
`--shots --shot-every 60` (the route in `tools/run/scripts/`), the replacement made once three shots exist:

| phase | replacement | result |
|---|---|---|
| in-place | `cp -f "$LIB" /tmp/x.so && cp -f /tmp/x.so "$LIB"` (the same inode, truncated) | **died, exit 139**, and the core the script read: `rip = 0x66f80`, `#1 ScreenshotWriter::write(std::string const&)`, `#2 ScreenshotWriter::on_vsync(unsigned long)`, `#3 HostGs::on_vsync(unsigned long const*, int)`, `#4 PS2Runtime::notifyHostGsVsync()` |
| rename | `cp -f "$LIB" /tmp/x.so && mv -f /tmp/x.so "$LIB"` (a new inode) | **alive** after 25 s, and the shots kept coming (20 by then) |

So the crash is reproducible on demand in a few seconds, at the next screenshot, exactly where agent C saw
it, and `0x66F80` is the unrelocated pointer. The e2e test of the fix
(`work/orchestrator/t304/sdl-fix-e2e.sh`) makes the destination differ on purpose so the fix really copies:
the run survived (shots 5 -> 26, the destination's inode changed) - the old code would have killed it.

## Fix and rule

**The rule: a build never writes in place into a file a running program has mapped.** Copy it beside the
target under a temporary name and `mv -f` it over (a running process keeps its own inode), and do nothing
at all when the two files are already identical (`cmp -s`). `patchelf` rewrites a file in place too, so it
is applied to a copy that is then renamed.

* new `tools/pipeline/copy_safe.sh`: `copy_safe <src> <dst>` and `patchelf_safe <file> <rpath>`.
* `build.sh` (the SDL3 copy and the `don` rpath), `build_host.sh` (the same two), `build_gs.sh`
  (`libdon_gs.so`, `gsreplay`, `pcsx2-gsrunner`, the `/opt/deps` libraries and `libshaderc`, their
  rpaths), `build_spu2.sh` and `build_vu.sh` (`libdon_spu2.so`, `libdon_vu.so`, the selftests): all use it.
  Each build script says why in one line. `build_tools.sh` copies nothing; in `build_gs.sh` the one
  remaining `cp -r resources` copies a *directory* after `rm -rf` (an unlink, not a truncation), which
  does not rewrite a file another process has mapped.
* Two lines in `build.sh`'s header explain it.

## Corrections to other entries

* **Journal 011** ("the two crashes", "the crash of run 1", "after the fix"): the crash is not a defect of
  the sound configuration nor of the host VU1 - it is this build script writing into a mapped library while
  the run was taking screenshots. The sound and the VU1 configurations are *not* implicated by it, and the
  "seen once near blank 1740" reading was the video set-up's timing plus a build at the wrong moment.
* **Journal 015** (this session's own): it named the crash as the consequence of the MPEG overwrite. The
  overwrite was real, it was fixed and verified, but it is *not* what crashed the runs.
* **Journal 010's follow-up** and **journal 014**: the crash sentence/paragraph there is corrected with a
  dated note (014 is another agent's, so only a note at the end).

## Verification

`work/orchestrator/t304/sdl-copy-proof.sh` (both phases) and `work/orchestrator/t304/sdl-fix-e2e.sh` as
above; `bash -n` on the five build scripts and on `copy_safe.sh`. The port's own regression (the two md5 of
`to_main_menu.txt`) passes after the change (T-304's report).
