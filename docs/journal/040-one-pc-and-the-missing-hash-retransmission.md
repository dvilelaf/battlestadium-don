# 040 · More than one instance of the port on one PC: the two crashes and the one lost HASH

- **Date:** 2026-10-15
- **Task:** T-509, T-510 (M5, wave 2)
- **Status:** open (both causes are outside the two cards: one is PCSX2's, one is `src/host/net/netplay.cpp`'s)

## Symptom

Three observations, all with **more than one instance** of the port on this PC at once, all `--headless`, all with the
English variant and `DON_TRACE_STATE` set (the instrument of journal 039; it is not the cause, see below).

1. **Vulkan (`--renderer vulkan`, the default): both instances of a pair die at the same guest blank with SIGSEGV**
   (exit code 139), around blank 360-420, i.e. still in the menus. Reproduced on ports 7778 and 7781:

   ```
   [t509] host rc=139 client rc=139, last blank=blank=360
   ```

   The backtrace (gdb on the host, `work/np-gdb/host-gdb.log`) is in the GS, not in the session:

   ```
   Thread 3 "GameThread" received signal SIGSEGV, Segmentation fault.
   #0 .. #4  libvulkan_radeon.so
   #5  Create() at .../GS/Renderers/Vulkan/VKBuilders.cpp:286
   #6  CreateTFXPipeline() at .../GS/Renderers/Vulkan/GSDeviceVK.cpp:5138
   #8  BindDrawPipeline() ... #9 RenderHW()
   #17 don_gs_gif_transfer() at src/gsbridge/don_gs.cpp:291
   #20 PS2Memory::submitGifPacket() ... #28 EeScheduler::run()
   ```

2. **`--renderer software`, four instances: one sometimes aborts at startup** with SIGABRT (exit code -6):

   ```
   Failed to open /GS.mem due to File exists
   third_party/pcsx2/pcsx2/GS/GSLocalMemory.cpp:55: assertion failed in function
     GSLocalMemory::GSLocalMemory(): Failed to allocate GS memory storage.
   ```

3. **With the simulated network (`DON_NET_SIM_*`), the session ends after a few minutes** on
   `[don-net] timed out waiting for the clients' hashes` (host) and `[don-net] timed out waiting for frame N`
   (every client). All three stalls seen were at `N` = a multiple of `hash_every` (120) plus the session's delay:

   | Run | Players | Stalls at frame | Host | Clients |
   |---|---|---|---|---|
   | `work/netplay/4p-sim` (1st) | 4 + sim | 8 | `player 3 stopped sending (30000 ms)` | `timed out waiting for frame 8` |
   | `work/netplay/4p-sim` (2nd) | 4 + sim | 1808 (= 1800 + 8) | `timed out waiting for the clients' hashes` | `timed out waiting for frame 1808` |
   | `work/netplay/4p-sim` (3rd) | 4 + sim | 848 (= 840 + 8) | the same | the same |
   | `work/np-sim2` | 2 + sim | 2888 (= 2880 + 8) | the same | `timed out waiting for frame 2888` |

## Classification

1 and 2 are `CRASH` (`tools/run/classify_log.py`: a signal status other than 137, 141 and 143). 3 is not a guest-log
category: it is printed by the port's netplay layer.

## Investigation

- 1: one instance with the same environment and the same trace ran 90 s without a crash (blank 5280, ended by
  `timeout`). A pair **without** `DON_TRACE_STATE` also crashed (exit 139 both, `work/np-notrace`), so the trace is
  not the trigger, and neither is the input script. T-509's first smoke run (40 s, port 7777) had survived, which is
  what made the crash look intermittent at first. The GS of this port runs on the game thread (`src/gsbridge/`), so
  there is no GS thread of the port to blame; the frames below `don_gs_gif_transfer` are driver frames. Two
  `GSDeviceVK`s in two processes at once is the combination that fails; with `--renderer software` (what
  `tests/regression/harness.py` uses) 2, 3 and 4 instances ran for minutes without a crash.
- 2: `third_party/pcsx2/pcsx2/GS/GS.cpp:1004` does `shm_open("/GS.mem", O_RDWR | O_CREAT | O_EXCL, 0600)` and, on
  success, `shm_unlink`s it at once. The name is fixed for the whole machine, so two processes that reach the line
  in the same instant collide: the loser gets `EEXIST`, `GSAllocateWrappedMemory` returns `nullptr` and
  `GSLocalMemory` asserts. `/dev/shm` held no leftover `GS.mem` between runs, so it is a live collision, not a stale
  file. In net mode the boots are synchronized (the host starts everyone's game thread with one `START`), which is
  what puts four `shm_open`s together.
- 3: the clients' HASH is sent **once**. `Netplay::report_hash` (`src/host/net/netplay.cpp`, client branch, line
  1085) sends the message and returns; the host branch (lines 1090-1121) waits for every client's HASH until
  `abort_after_ms`, calling `recv_process` and `maybe_ping` in the loop and never asking for the HASH again. The
  client ignores `MsgType::Waiting` (`default: break;`, line 601) and never resends a HASH; it only resends its
  *inputs* (`send_input`, line 507: every unacked frame), which is why a lost frame does not stall a session but a
  lost HASH does. `host.cpp` calls the hash every `net_hash_every` frames (`host.cpp:564`, default 120), and
  `abort_after_ms` is 30000 (`netplay.h:29`). So one dropped datagram out of every 120-frame round costs 30 s and the
  session. Unit tests do not see it: `tests/net/test_net_netplay.cpp:281-301` (the `with_simulated_network` case)
  calls `run_peer(..., 600, 0, ...)`, and a `hash_every` of 0 means it never calls `report_hash`.
  `DON_NET_SIM_LOSS=0.05` over the 5 minutes of a T-510 case is ~450 HASH messages for 4 players: the session cannot
  survive it. `DON_NET_SIM_SEED` defaults to 1 (`host.cpp:367`), but the stalls land on different rounds between runs
  (1800, 840), so the drop pattern depends on the message order, not only on the seed.

## Cause

1. Not determined. A SIGSEGV inside `libvulkan_radeon.so` while a TFX pipeline is created, with two instances of the
   port (two `GSDeviceVK`s) on this GPU. Whether the port, the PCSX2 renderer or RADV is at fault was not
   established; the crash site is in the driver.
2. PCSX2's GS memory file has one name for the whole machine.
3. The HASH exchange has no retransmission and no re-request, so it is not tolerant of a lossy transport, while the
   input exchange is.

## Fix

- 1. Workaround used by T-509/T-510: `--renderer software`. T-509's card allows no code change, and the fault is not
  in `src/host/net/`.
- 2. None in place. The fix belongs to PCSX2 (a name per process, e.g. `/GS.mem.<pid>`), i.e. a patch in
  `patches/pcsx2/`.
- 3. None in place; T-509/T-510 do not allow editing `src/host/net/netplay.cpp`. The shape of a fix: have the client
  keep resending its HASH until the next round (or until the host's next `FRAMES` mentioning that round), and/or have
  the host re-request it while it waits, instead of waiting 30 s for a datagram that is not coming.

## Verification

- 2 instances, `--renderer software`, `DON_TRACE_STATE=60`, 150 s: `host rc=0 client rc=0`, both at blank 8580, 143
  state lines identical (`work/np-sw`).
- 4 instances, `--renderer software`, T-510 case `4p`: all four logs hold the same 76 state lines (blanks 240 to
  18240; one md5 for the four extracted sets, `d8faba1d6491d95ff942b7f96bb8cea1`).
- T-509's 5 minutes and T-510's cases: see their report.

## Temporary diagnostics

None added. `DON_TRACE_STATE` already existed (journal 014) and `DON_NET_SIM_*` are the card's own. No file of the
port, the runtime or the tests was changed by this entry.
