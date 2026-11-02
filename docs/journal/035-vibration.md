# 035 - Vibration (T-308)

Date: 2026-10-14. Binary of the measurements: `6629efd834a8` (linux-dev, English variant, headless, software renderer).

## What the game does

- Only `scePadSetActDirect` (`0x1194C8`) is called, once per frame, port 0 slot 0 in a one-player fight, with
  `{small, large, 0, 0, 0, 0}`. `scePadSetActAlign` (`0x119590`, a call site at `0x139760`) was never called
  (a trace of refused calls too: none). So the runtime's table starts as `{0, 1, FF...}` (inference, not seen in
  the SDK: it is the only mapping consistent with what the game sends).
- Practice fight (`tools/run/scripts/fight_practice.txt`), setting ON, changes of the motors per blank
  (`DON_TRACE_PAD_ACT=1`): small on 6391-6397, 6778-6789, 6854-6857, 6878-6888 (3 to 11 blanks, hits taken);
  small + large 255 on 6957-6978 and 7349-7370 (21 blanks, twice: probably the ultra move). Calls: 7137 zeros, 63 `01 ff`, 30 `01 00`
  in one run.
- Setting OFF (Triangle on the practice character select, Right, Circle: the panel shows OFF; script `work/t308/off.txt`,
  not versioned): 7229 calls, all zeros; no change reaches the host.

## What was built

- Patch 0066 (runtime): align table and data per (port, slot), motors derived, `Ps2HostPad::actuator` callback on the
  game thread only on a change; closed port, `scePadInit` and `scePadEnd` send 0, 0; both calls answer 0 for a pad not
  open or not plugged in. Test `tests/runtime/pad_actuator_test`.
- Host: `PadTable` stores the motors (atomics); `drive_rumble` in the main loop gives them to
  `SdlInput::set_rumble` (player -> gamepad; none for the keyboard); `RumbleSchedule` renews every 400 ms
  (SDL duration 1000 ms); shutdown stops the motors. `--rumble on|off` (default on) is the only switch besides the
  game's own setting: some pads misbehave under SDL's rumble. Tests: `test_input_rumble` and, in the container,
  `test_input_sdl` with an SDL virtual joystick (no real device is ever driven).
- `DON_TRACE_PAD_ACT=1`: `[don-pad]` lines per runtime call and `[don]` lines per change per blank. Off by default.

## Not verified

A real gamepad's motors (feel, strength: the small motor goes to full high frequency, the large one is scaled
linearly); four-player vibration (the mapping uses the same layout code as the buttons); whether the game ever calls
`SetActAlign` in other modes.
