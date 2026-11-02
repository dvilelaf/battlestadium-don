# 012 · The VU1 table that died with the block that built it (crash A)

- **Date:** 2026-10-13
- **Task:** T-304 (follow-up: the two crashes with sound on)
- **Status:** resolved

## Symptom

`DON_VU1=microvu --sound on` crashes with SIGSEGV a fraction of a second after the guest boots
(about 0.85 s of emulated time); each option alone runs.

```
$ DON_VU1=microvu build/linux-dev/don --headless --renderer software --sound on \
      --wav /tmp/x.wav --seconds 5 disc/SLPS_256.75
exit 139
```

Under gdb (a call through a null pointer, and always the same place):

```
Thread 2 "GameThread" received signal SIGSEGV
#0  0x0000000000000000 in ?? ()
#1  0x000055555c0ffe56 in PS2Memory::vu1CodeWritten(unsigned int, unsigned int)
#2  0x000055555c12abc8 in PS2Memory::processVIF1Data(unsigned char const*, unsigned int)
#3  0x000055555c10383a in PS2Memory::processPendingTransfers()
#4  0x000055555c101004 in PS2Memory::writeIORegister(unsigned int, unsigned int)
#5  0x000055555c11da20 in PS2Runtime::Store32(...)
#6  0x0000555559580d29 in FUN_002d3a60_0x2d3a60(...)   (a guest store to the VU1 code window)
```

## Investigation

The faulting instruction is `call *0x3c8(%rbx)` at `PS2Memory::vu1CodeWritten+64`, with `rbx` the
`PS2Memory`. `m_hostVu1` starts at `+0x3a8` (user first, `write_micro` at `+0x20`), so the call is
the VU1 table's `write_micro`. That field was **not** null (`0x55555c0e74e0`, the host trampoline) and
`m_hostVu1Bound` was true, so the call left the runtime with correct arguments: the null call is one
level deeper, inside the trampoline.

`HostVu::write_micro` (src/host/vu_glue.cpp):

```
movzbl 0x74(%rdi),%eax   ; the host object's open flag
test   $0x1,%al
jne    ...+9
ret
mov    (%rdi),%rax       ; the DonVuApi the object was built with
mov    0x58(%rax),%rax   ; don_vu1_write_micro
jmp    *%rax             ; rax = 0 -> address 0
```

At the crash `rax` was 0 and `(%rdi)` was `0x7fffffffb5a0` — **a main-thread stack address**: the
`DonVuApi` the table was built from. `0x58(%rax)` read as zero because that stack slot had been
overwritten since.

`src/host/host.cpp` declares it inside the block that loads the library:

```cpp
std::unique_ptr<HostVu> vu;
{
    const int selection = don_vu1_selection_from_env();
    if (selection != DON_VU1_RUNTIME)
    {
        DonVuApi vu_api;                 // <-- on this function's stack, and this block ends below
        ...
        vu = std::make_unique<HostVu>(vu_api, don_vu1_engine_for_selection(selection));
        runtime->setHostVu1(&vu->iface());
    }
}                                        // vu_api's lifetime ends here
```

and `HostVu` held it by reference (`const DonVuApi &m_api`). The object therefore read its function
pointers from a dead stack slot. With `DON_VU1=microvu` alone nothing rewrites that slot, so the stale
values still happened to read correctly — the reason each option alone "worked". With `--sound on` the
main thread's audio path (the chip pull and the WAV writer) reuses that stack region and zeroes it, the
next VU1 microcode upload calls through the table and jumps to 0. It also explains why the crash was
flaky under gdb: the added breakpoint/watchpoint changes when the stack is reused (one instrumented run
did not crash at all; four plain gdb runs did, always at the same instruction).

Ruled out, with the checks that refute them:

- **Symbol sharing between `libdon_vu.so` and `libdon_spu2.so`.** `nm -D --defined-only` of both: no
  type-`u` (unique) symbol in either, and no dynamic definition in common beyond each library's own
  `don_vu1_*` / `don_spu2_*` API. SDL loads both with `dlopen(..., RTLD_LOCAL)`, so even plain globals
  in the two objects cannot interpose on each other. (The host and the VU1 library are separate: the
  crashed code is in the runtime and the host program.)
- **A second `setHostVu1`.** A breakpoint on `PS2Memory::setHostVu1` counts exactly one call, from
  `don_host_run`, with the host's complete table; the fields are all non-null at `beginHostVu1`.
- **The library failing to open.** `[don] VU1 library: .../build/vu/libdon_vu.so (microvu)` is the
  success line; a refused open prints its own `[don-vu] ... could not be opened` line and `data()`
  answers null, which leaves the runtime on its own interpreter instead of crashing.

## Cause

`HostVu` stored a **reference** to the caller's `DonVuApi`, and the host program built it from a local
of a block that ends before the game thread starts. The VU1 table outlived its storage; the first
reuse of that stack turned its function pointers to zero.

## Fix

`src/host/vu_glue.h`: `HostVu` now **owns a copy** of the table (`DonVuApi m_api;`, one word; the
constructor already takes `const DonVuApi &` and copies). The library handle in the copy is borrowed,
exactly as before; `don_vu_load` and `HostVu` are otherwise untouched, and `host.cpp` needs no change —
which is also why the other agents' pending edits in that file are not touched.

## Verification

The regression test is `tests/vuglue/test_vu_glue_lifetime.cpp` (built by `tests/vuglue/CMakeLists.txt`
next to `test_vu_glue`; the runner picks it up). It builds a fake table, constructs `HostVu` from it,
ends the table's lifetime and reuses its exact storage for a second table whose stubs answer different
values, then drives the whole `Ps2HostVu1` sequence. Against the old shape it fails cleanly:

```
$ cmake --build build/vuglue-tests -j4 && build/vuglue-tests/test_vu_glue_lifetime   # with const DonVuApi &m_api
test_vu_glue_lifetime.cpp:156: CHECK_EQ failed: original_open (0) != 1 (1)
... 12 check(s) failed
exit 1
```

and with the fix:

```
$ build/vuglue-tests/test_vu_glue_lifetime
test_vu_glue_lifetime: all checks passed
exit 0
```

In the port, with the fixed `build/linux-dev` (25 s of emulated time, no crash, and the vector unit is
actually used):

```
$ DON_VU_LOG=1 DON_VU1=microvu build/linux-dev/don --headless --renderer software --sound on \
      --wav work/orchestrator/t304/fixA.wav --seconds 25 disc/SLPS_256.75
exit 0
[don-vu] VU1 opened on microVU
[don-vu] VU1 (microvu): 61420 programs started, 0 continuations, 4606500 cycles,
         61420 XGKICK packets, 20637120 bytes
```

and the full route (title and menu) with microVU and sound: 81 s, exit 0, 831 411 XGKICK packets
(1.2 GB) — see journal 011, "After the fix".

## Temporary diagnostics

None added. The gdb scripts and logs used here are under `work/orchestrator/t304/` (crashA*.gdb,
t304A*.log); no core file was produced (the crash is cheap to reproduce under gdb).
