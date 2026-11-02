#!/usr/bin/env bash
# Starts the reference PCSX2 (third_party/pcsx2-ref/, see tools/setup/fetch_pcsx2_ref.sh) with the
# game, in portable mode and with the configuration needed for reference captures:
# Vulkan, native resolution (1x), no widescreen/no-interlacing patches, no cheats, and
# screenshots, savestates and GS dumps all saved under work/ref/pcsx2/raw/.
#
# Usage: bash tools/ref/run_pcsx2.sh [iso]
# The disc image is the argument if given, otherwise DON_ISO. DON_ISO and DON_PS2_BIOS_DIR (the
# folder with the PS2 BIOS) normally come from local.env at the repository root, which this script
# reads by itself (see local.env.example); a value already set in the environment wins.
# Optional: DON_PS2_BIOS_FILE=<bios file name> (default ps2-0200j-20040614.bin: PCSX2 2.8.2 with ps2-0210j-20040917.bin
# shows only a black picture on this machine, with or without fast boot and with a fresh NVRAM file)
# The BIOS is read from where it is. It is never copied into the repository.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ -f "$ROOT/local.env" ]]; then
    # shellcheck source=/dev/null
    . "$ROOT/local.env"
fi

APPIMAGE="$ROOT/third_party/pcsx2-ref/pcsx2-v2.8.2-linux-appimage-x64-Qt.AppImage"
DATA_ROOT="$ROOT/third_party/pcsx2-ref/PCSX2"    # chosen by PCSX2: "PCSX2" next to the AppImage
INI="$DATA_ROOT/inis/PCSX2.ini"
RAW="$ROOT/work/ref/pcsx2/raw"
BIOS_FILE="${DON_PS2_BIOS_FILE:-ps2-0200j-20040614.bin}"

log() { echo "[run_pcsx2] $*"; }
die() { echo "[run_pcsx2] ERROR: $*" >&2; exit 1; }

BIOS_DIR="${DON_PS2_BIOS_DIR:-}"
if [[ -z "$BIOS_DIR" ]]; then
    cat >&2 <<EOF
[run_pcsx2] ERROR: the environment variable DON_PS2_BIOS_DIR is not set.
PCSX2 needs a PS2 BIOS dumped from your own console. This project does not include one and
never copies one into the repository. Set DON_PS2_BIOS_DIR to the folder that contains it. The
usual way is to copy local.env.example to local.env (git ignores it) and fill in the paths there.
One-off alternative:

    DON_PS2_BIOS_DIR=/path/to/bios bash tools/ref/run_pcsx2.sh

The folder must contain $BIOS_FILE (change the name with DON_PS2_BIOS_FILE).
EOF
    exit 1
fi
BIOS_DIR="$(cd "$BIOS_DIR" 2>/dev/null && pwd)" || die "DON_PS2_BIOS_DIR does not exist: ${DON_PS2_BIOS_DIR}"
[[ -f "$BIOS_DIR/$BIOS_FILE" ]] || die "BIOS file not found: $BIOS_DIR/$BIOS_FILE"

ISO="${1:-${DON_ISO:-}}"
if [[ -z "$ISO" ]]; then
    cat >&2 <<EOF
[run_pcsx2] ERROR: no disc image given.
Pass the disc image as the first argument, or set DON_ISO to its path. The usual way is to copy
local.env.example to local.env (git ignores it) and fill in DON_ISO and DON_PS2_BIOS_DIR there.
EOF
    exit 1
fi
[[ -f "$ISO" ]] || die "disc image not found: $ISO (check DON_ISO in local.env, see local.env.example)"
[[ -x "$APPIMAGE" ]] || die "PCSX2 is not installed; run: bash tools/setup/fetch_pcsx2_ref.sh"

mkdir -p "$RAW"

# First run: let PCSX2 write its own default configuration (it includes the default keyboard
# pad and hotkeys). It is started on a virtual display so that no window appears, and stopped
# as soon as PCSX2.ini exists.
if [[ ! -f "$INI" ]]; then
    command -v xvfb-run >/dev/null || die "xvfb-run is needed for the first run (creates the default configuration)"
    log "first run: creating the default configuration in $DATA_ROOT"
    setsid timeout 60 xvfb-run -a "$APPIMAGE" -portable >/dev/null 2>&1 &
    boot_pid=$!
    for _ in $(seq 1 60); do
        [[ -f "$INI" ]] && break
        sleep 1
    done
    sleep 2
    kill -- "-$boot_pid" 2>/dev/null || kill "$boot_pid" 2>/dev/null || true
    wait "$boot_pid" 2>/dev/null || true
    pkill -f "$APPIMAGE" 2>/dev/null || true
    [[ -f "$INI" ]] || die "PCSX2 did not create $INI"
fi

# set_ini SECTION KEY VALUE: replaces KEY in [SECTION] of PCSX2.ini, or adds it
set_ini() {
    local section="$1" key="$2" value="$3"
    awk -v sec="$section" -v key="$key" -v val="$value" '
        function flush() { if (insec && !done) { print key " = " val; done = 1 } }
        /^\[.*\]$/ { flush(); insec = ($0 == "[" sec "]"); if (insec) seen = 1 }
        insec && index($0, key " = ") == 1 { print key " = " val; done = 1; next }
        { print }
        END { flush(); if (!seen) { print ""; print "[" sec "]"; print key " = " val } }
    ' "$INI" > "$INI.tmp"
    mv "$INI.tmp" "$INI"
}

# Keys and sections checked against pcsx2/Pcsx2Config.cpp at tag v2.8.2
set_ini UI SetupWizardIncomplete false      # no first-run wizard
set_ini UI ConfirmShutdown false
set_ini UI StartFullscreen false
set_ini Folders Bios "$BIOS_DIR"
set_ini Folders Snapshots "$RAW"            # screenshots and GS dumps
set_ini Folders Savestates "$RAW"
set_ini Filenames BIOS "$BIOS_FILE"
set_ini EmuCore EnablePatches false         # no game patches
set_ini EmuCore EnableCheats false
set_ini EmuCore EnableWideScreenPatches false
set_ini EmuCore EnableNoInterlacingPatches false
set_ini EmuCore EnableGameFixes true        # only PCSX2's automatic per-game fixes
set_ini EmuCore SaveStateOnShutdown false
set_ini EmuCore/GS Renderer 14              # Vulkan
set_ini EmuCore/GS upscale_multiplier 1     # native resolution
set_ini EmuCore/GS AspectRatio "4:3"
set_ini EmuCore/GS ScreenshotSize 1         # screenshots at the internal (native) resolution
set_ini EmuCore/GS ScreenshotFormat 0       # PNG
set_ini EmuCore/GS OrganizeScreenshotsByGame false
set_ini EmuCore/GS GSDumpCompression 2      # Zstandard: .gs.zst

log "BIOS:  $BIOS_DIR/$BIOS_FILE"
log "disc:  $ISO"
log "data:  $DATA_ROOT"
log "raw captures go to: $RAW"
log "log file: $DATA_ROOT/logs/emulog.txt"
exec "$APPIMAGE" -portable -nofullscreen -batch -- "$ISO"
