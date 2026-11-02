#!/usr/bin/env bash
# Starts the reference PCSX2 with the game on a private virtual display (Xvfb), for unattended
# reference captures (see tools/ref/drive.py and tools/ref/route.txt).
#
# Usage: bash tools/ref/pcsx2_auto.sh start|stop|status
#   start   starts Xvfb and PCSX2, returns when the game window exists, prints the display
#           (for example ":101") on the last line of standard output.
#   stop    ends PCSX2 and its Xvfb. Leaves no process behind.
#   status  prints running/stopped and the display; exit 0 only if both processes are alive.
#
# It never uses the user's display: the DISPLAY of the caller is ignored, Xvfb picks its own
# display number, and every program is started with DISPLAY set to that one. PCSX2 has its own
# data folder (third_party/pcsx2-auto/, a hard link of the AppImage so that PCSX2 puts its data
# next to it) and the manual setup of tools/ref/run_pcsx2.sh is not touched.
#
# Settings: those of run_pcsx2.sh (native resolution, no patches, captures under
# work/ref/pcsx2/raw/), the Vulkan renderer on Mesa's software driver (the real GPUs cannot
# present to Xvfb), and no audio output.
# Optional environment: DON_AUTO_RAW=<folder for screenshots, savestates and GS dumps>
# (default work/ref/pcsx2/raw), DON_PS2_BIOS_FILE (default ps2-0200j-20040614.bin, the same as run_pcsx2.sh: PCSX2 2.8.2 with
# ps2-0210j-20040917.bin shows only a black picture here, also with fast boot off), DON_AUTO_FASTBOOT (default true), DON_AUTO_RENDERER (number of GSRendererType:
# 14 Vulkan, 12 OpenGL, 13 software; default 14), DON_AUTO_SCREEN (default 1280x960).
# DON_ISO and DON_PS2_BIOS_DIR come from local.env, as in run_pcsx2.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ -f "$ROOT/local.env" ]]; then
    # shellcheck source=/dev/null
    . "$ROOT/local.env"
fi

SRC_APPIMAGE="$ROOT/third_party/pcsx2-ref/pcsx2-v2.8.2-linux-appimage-x64-Qt.AppImage"
AUTO_DIR="$ROOT/third_party/pcsx2-auto"
APPIMAGE="$AUTO_DIR/pcsx2-auto.AppImage"
DATA_ROOT="$AUTO_DIR/PCSX2"
INI="$DATA_ROOT/inis/PCSX2.ini"
STATE="$ROOT/work/ref/pcsx2/auto/state"
RAW="${DON_AUTO_RAW:-$ROOT/work/ref/pcsx2/raw}"
BIOS_FILE="${DON_PS2_BIOS_FILE:-ps2-0200j-20040614.bin}"
RENDERER="${DON_AUTO_RENDERER:-14}"
SCREEN="${DON_AUTO_SCREEN:-1280x960}"
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"

log() { echo "[pcsx2_auto] $*" >&2; }
die() { echo "[pcsx2_auto] ERROR: $*" >&2; exit 1; }

alive() { [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null; }
read_state() { [[ -f "$STATE/$1" ]] && cat "$STATE/$1" || true; }

# Environment for everything that runs on the virtual display. DISPLAY is always replaced.
virtual_env() {
    local disp="$1"
    [[ -n "$disp" && "$disp" != ":0" && "$disp" == :* ]] || die "refusing display '$disp'"
    export DISPLAY="$disp"
    unset WAYLAND_DISPLAY XAUTHORITY
    export QT_QPA_PLATFORM=xcb
    export VK_ICD_FILENAMES="$LVP_ICD"
    export MESA_VK_VERSION_OVERRIDE=
    unset MESA_VK_VERSION_OVERRIDE
}

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

start_xvfb() {
    # -displayfd makes Xvfb pick a free display number and report it; nothing is guessed.
    local fifo
    fifo="$(mktemp -u "$STATE/dfd.XXXXXX")"
    mkfifo "$fifo"
    env -u DISPLAY setsid Xvfb -displayfd 3 -screen 0 "${SCREEN}x24" -nolisten tcp \
        3>"$fifo" >"$STATE/xvfb.log" 2>&1 &
    echo $! > "$STATE/xvfb.pid"
    local n=""
    read -r -t 20 n < "$fifo" || true
    rm -f "$fifo"
    [[ -n "$n" ]] || die "Xvfb did not report a display (see $STATE/xvfb.log)"
    echo ":$n" > "$STATE/display"
}

prepare_data() {
    [[ -x "$SRC_APPIMAGE" ]] || die "PCSX2 is not installed; run: bash tools/setup/fetch_pcsx2_ref.sh"
    mkdir -p "$AUTO_DIR"
    if [[ ! -x "$APPIMAGE" ]]; then
        ln -f "$SRC_APPIMAGE" "$APPIMAGE" 2>/dev/null || cp -p "$SRC_APPIMAGE" "$APPIMAGE"
    fi
    if [[ ! -f "$INI" ]]; then
        log "first run: creating the default configuration in $DATA_ROOT"
        local disp
        disp="$(cat "$STATE/display")"
        (
            virtual_env "$disp"
            setsid timeout 60 "$APPIMAGE" -portable >/dev/null 2>&1 &
            for _ in $(seq 1 60); do [[ -f "$INI" ]] && break; sleep 1; done
            sleep 2
            pkill -f "$APPIMAGE" 2>/dev/null || true
        )
        sleep 2
        [[ -f "$INI" ]] || die "PCSX2 did not create $INI"
    fi
    local bios_dir
    bios_dir="$(cd "${DON_PS2_BIOS_DIR:-/nonexistent}" 2>/dev/null && pwd)" \
        || die "DON_PS2_BIOS_DIR is not set or does not exist (see local.env.example)"
    [[ -f "$bios_dir/$BIOS_FILE" ]] || die "BIOS file not found: $bios_dir/$BIOS_FILE"
    mkdir -p "$RAW"
    set_ini UI SetupWizardIncomplete false
    set_ini UI ConfirmShutdown false
    set_ini UI StartFullscreen false
    set_ini UI HideMouseCursor true
    set_ini Folders Bios "$bios_dir"
    set_ini Folders Snapshots "$RAW"
    set_ini Folders Savestates "$RAW"
    set_ini Filenames BIOS "$BIOS_FILE"
    set_ini EmuCore EnablePatches false
    set_ini EmuCore EnableCheats false
    set_ini EmuCore EnableWideScreenPatches false
    set_ini EmuCore EnableNoInterlacingPatches false
    set_ini EmuCore EnableGameFixes true
    set_ini EmuCore SaveStateOnShutdown false
    set_ini EmuCore/GS Renderer "$RENDERER"
    set_ini EmuCore/GS upscale_multiplier 1
    set_ini EmuCore/GS AspectRatio "4:3"
    set_ini EmuCore/GS ScreenshotSize 1
    set_ini EmuCore/GS ScreenshotFormat 0
    set_ini EmuCore/GS OrganizeScreenshotsByGame false
    set_ini EmuCore/GS GSDumpCompression 2
    set_ini SPU2/Output Backend "${DON_AUTO_AUDIO:-Null}"   # no sound device
    set_ini EmuCore EnableFastBoot "${DON_AUTO_FASTBOOT:-true}"
    set_ini Logging EnableSystemConsole false
    set_ini Logging EnableEEConsole false
    set_ini Logging EnableIOPConsole false
}

cmd_status() {
    local xp pp disp
    xp="$(read_state xvfb.pid)"; pp="$(read_state pcsx2.pid)"; disp="$(read_state display)"
    if alive "$xp" && alive "$pp"; then
        echo "running display=$disp xvfb=$xp pcsx2=$pp"
        return 0
    fi
    echo "stopped"
    return 1
}

cmd_stop() {
    local pid
    for f in pcsx2.pid xvfb.pid; do
        pid="$(read_state "$f")"
        if alive "$pid"; then
            kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
            for _ in $(seq 1 100); do alive "$pid" || break; sleep 0.1; done
            alive "$pid" && { kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true; }
        fi
        rm -f "$STATE/$f"
    done
    rm -f "$STATE/display" "$STATE/window"
    log "stopped"
}

cmd_start() {
    command -v Xvfb >/dev/null || die "Xvfb is not installed"
    command -v xdotool >/dev/null || die "xdotool is not installed"
    [[ -f "$LVP_ICD" ]] || die "Mesa's software Vulkan driver not found: $LVP_ICD"
    local iso="${DON_ISO:-}"
    [[ -n "$iso" ]] || die "DON_ISO is not set (see local.env.example)"
    [[ -f "$iso" ]] || die "disc image not found: $iso"
    mkdir -p "$STATE"
    if cmd_status >/dev/null 2>&1; then die "already running: $(cmd_status)"; fi
    cmd_stop >/dev/null 2>&1 || true

    start_xvfb
    local disp
    disp="$(cat "$STATE/display")"
    # Refuse to go on if the display could be the user's one.
    [[ "$disp" != ":0" && "$disp" != "${DISPLAY:-}" ]] || { cmd_stop; die "virtual display collides with the user's display $disp"; }
    log "virtual display $disp ($SCREEN)"
    prepare_data || { cmd_stop; exit 1; }

    rm -f "$DATA_ROOT/logs/emulog.txt"
    (
        virtual_env "$disp"
        exec setsid "$APPIMAGE" -portable -nofullscreen -batch -- "$iso" \
            >"$STATE/pcsx2.out" 2>&1
    ) &
    echo $! > "$STATE/pcsx2.pid"
    log "waiting for the game window"
    local wid=""
    for _ in $(seq 1 180); do
        alive "$(read_state pcsx2.pid)" || { cmd_stop; die "PCSX2 exited (see $STATE/pcsx2.out and $DATA_ROOT/logs/emulog.txt)"; }
        if grep -q 'VM subsystems initialized' "$DATA_ROOT/logs/emulog.txt" 2>/dev/null; then
            wid="$(DISPLAY="$disp" xdotool search --onlyvisible --pid "$(read_state pcsx2.pid)" 2>/dev/null | head -n1 || true)"
            [[ -n "$wid" ]] && break
        fi
        sleep 1
    done
    [[ -n "$wid" ]] || { cmd_stop; die "no game window after 180 s (see $STATE/pcsx2.out)"; }
    echo "$wid" > "$STATE/window"
    log "game window $wid"
    echo "$disp"
}

case "${1:-}" in
    start) cmd_start ;;
    stop) cmd_stop ;;
    status) cmd_status ;;
    *) echo "usage: bash tools/ref/pcsx2_auto.sh start|stop|status" >&2; exit 1 ;;
esac
