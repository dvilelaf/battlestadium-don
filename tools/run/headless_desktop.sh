#!/usr/bin/env bash
# T-305: a private headless desktop (GNOME Shell as a Wayland compositor with one virtual monitor, on the real
# graphics card) and a command run inside it, so that a program gets a real window, a real swapchain and a real
# compositor without touching the user's own session.
#
#   tools/run/headless_desktop.sh [--size WxH] [--rate HZ] [--x11] -- COMMAND [ARGS...]
#
#   --size   virtual monitor size (default 1920x1080)
#   --rate   virtual monitor refresh rate (default 60); the compositor presents at this rate
#   --x11    also give COMMAND the compositor's own X server (DISPLAY set to it, SDL_VIDEODRIVER=x11); without it
#            COMMAND gets WAYLAND_DISPLAY only (SDL_VIDEODRIVER=wayland)
#
# Isolation (the rule of this script): DISPLAY and every session variable of the caller are removed, the bus is a
# new one (dbus-run-session), XDG_RUNTIME_DIR, HOME-style config folders and the GSettings backend are private
# (a fresh directory under /tmp, mode 700, deleted at the end). Nothing is shared with the user's desktop; only the
# processes started here are stopped, by their ids.
set -eu
size=1920x1080; rate=60; x11=0
while [ $# -gt 0 ]; do
  case "$1" in
    --size) size=$2; shift 2 ;;
    --rate) rate=$2; shift 2 ;;
    --x11) x11=1; shift ;;
    --) shift; break ;;
    *) echo "usage: $0 [--size WxH] [--rate HZ] [--x11] -- COMMAND..." >&2; exit 2 ;;
  esac
done
[ $# -gt 0 ] || { echo "no command given" >&2; exit 2; }

priv=$(mktemp -d /tmp/don-desktop.XXXXXX)
chmod 700 "$priv"
mkdir -p "$priv/run" "$priv/config" "$priv/data" "$priv/cache"
chmod 700 "$priv/run"
export priv size rate x11
export XDG_RUNTIME_DIR="$priv/run" XDG_CONFIG_HOME="$priv/config" XDG_DATA_HOME_PRIV="$priv/data" XDG_CACHE_HOME="$priv/cache"
export DON_USER_DIR="$priv/data/don"  # T-524: the program's log/cards/cache stay private
export GSETTINGS_BACKEND=memory
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_ID XDG_SESSION_TYPE XDG_SESSION_DESKTOP \
      XDG_CURRENT_DESKTOP DESKTOP_SESSION GNOME_SETUP_DISPLAY SESSION_MANAGER

inner() {
  sock=wayland-don
  gnome-shell --wayland --headless --sm-disable --virtual-monitor "${size}@${rate}" --wayland-display "$sock" \
    $([ "$x11" = 1 ] || echo --no-x11) > "$priv/shell.log" 2>&1 &
  shell_pid=$!
  trap 'kill $shell_pid 2>/dev/null || true; wait $shell_pid 2>/dev/null || true' EXIT
  for _ in $(seq 1 100); do
    [ -S "$XDG_RUNTIME_DIR/$sock" ] && break
    kill -0 $shell_pid 2>/dev/null || { echo "[desktop] gnome-shell exited; log:" >&2; cat "$priv/shell.log" >&2; exit 3; }
    sleep 0.2
  done
  [ -S "$XDG_RUNTIME_DIR/$sock" ] || { echo "[desktop] no wayland socket" >&2; exit 3; }
  export WAYLAND_DISPLAY=$sock SDL_VIDEODRIVER=wayland
  if [ "$x11" = 1 ]; then
    # Xwayland is started by the compositor on its own socket in the private folder of /tmp/.X11-unix; find it
    # from the shell's log or the new socket.
    for _ in $(seq 1 50); do
      d=$(grep -o -E ':[0-9]+' "$priv/shell.log" 2>/dev/null | head -n1 || true)
      [ -n "$d" ] && break; sleep 0.2
    done
    [ -n "${d:-}" ] || d=$(ls -t /tmp/.X11-unix 2>/dev/null | head -n1 | sed 's/^X/:/')
    export DISPLAY=$d SDL_VIDEODRIVER=x11
  fi
  echo "[desktop] pid $shell_pid, ${size}@${rate}, WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=${DISPLAY:-}" >&2
  sleep 2
  set +e
  "$@"
  code=$?
  exit $code
}
export -f inner
rc=0
dbus-run-session -- bash -c 'inner "$@"' inner "$@" || rc=$?
sleep 1
for m in "$priv/run/doc" "$priv/run/gvfs"; do fusermount -uz "$m" 2>/dev/null || true; done
rm -rf "$priv" 2>/dev/null || echo "[desktop] note: $priv not fully removed (a portal mount); harmless, under /tmp" >&2
exit $rc
