#!/usr/bin/env bash
# Traces which runtime handlers the game calls: starts $DON_BUILD_DIR/don (default build/linux-dev)
# under gdb headless, puts a breakpoint on every function that matches each pattern (gdb regular
# expressions, `[clone .cold]` copies are skipped), prints a short backtrace at each hit and continues. At the time limit
# it stops the program, prints the last events and the backtrace of every host thread, and ends.
#
# Usage: tools/run/gdb_trace.sh [-t seconds=40] [-n frames=6] [-c cap=3] [-o out.log]
#                               [-f patterns.txt] [pattern ...]
#   -t  seconds the program runs, counted from its start (the run ends at that time)
#   -n  frames of backtrace printed at each hit (and per thread at the end)
#   -c  full backtraces printed per distinct event (breakpoint, expressions, first game frame);
#       later repeats are only counted and listed in the summary and the last-events list
#   -o  output file, default work/logs/gdbtrace-<date>.log (always under work/logs/)
#   -f  file with one pattern per line (blank lines and lines starting with # are ignored)
# Patterns are gdb regular expressions (`info functions`; alternation is \|, a literal
# parenthesis is [(]), for example '^ps2_syscalls::', '^EeScheduler::',
# 'CreateThread\|WaitSema'.
# A pattern can end in @@expr;expr[@@condition]: gdb expressions printed at each hit and a
# condition under which the breakpoint stops (evaluated by gdb, much cheaper than a hit that
# prints), for example '^ps2_syscalls::dispatchNumericSyscall@@$rdi@@$rdi!=0x2f' (the system
# call number is in rdi at entry). Guest registers: a handler gets (rdram, ctx, runtime) in
# rdi, rsi, rdx, and R5900Context holds register N at offset N*16 (a0 0x40, a1 0x50, v0 0x20,
# sp 0x1d0, ra 0x1f0), so '*(unsigned int*)($rsi+0x1f0)' is the guest return address. Anchor them: an unanchored pattern also matches library functions.
#
# The output has the game's text only if DON_TTY=1 is set by the caller; it is not set here.
# The log can hold game text: quote only short lines from it in documents (AGENTS.md, rule 1).
set -euo pipefail

SECONDS_TO_RUN=40
FRAMES=6
CAP=3
OUT=""
PATTERNS=()

while getopts "t:n:c:o:f:" opt; do
    case "$opt" in
        t) SECONDS_TO_RUN="$OPTARG" ;;
        n) FRAMES="$OPTARG" ;;
        c) CAP="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        f)
            while IFS= read -r line || [ -n "$line" ]; do
                case "$line" in ''|'#'*) ;; *) PATTERNS+=("$line") ;; esac
            done < "$OPTARG"
            ;;
        *) echo "usage: see the header of $0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
PATTERNS+=("$@")
[ "${#PATTERNS[@]}" -gt 0 ] || { echo "no breakpoint pattern given" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [ -f local.env ]; then
    set -a
    # shellcheck disable=SC1091
    . ./local.env
    set +a
fi

EXE="${DON_BUILD_DIR:-build/linux-dev}/don"
ELF="disc/SLPS_256.75"
[ -x "$EXE" ] || { echo "missing $EXE" >&2; exit 2; }
[ -f "$ELF" ] || { echo "missing $ELF" >&2; exit 2; }
for tool in gdb; do
    command -v "$tool" >/dev/null || { echo "$tool not found" >&2; exit 2; }
done

# Never the user's display: --headless asks the host program for no window at all.
unset DISPLAY WAYLAND_DISPLAY
export LC_ALL=C LANG=C
# T-524: the program writes its log/cards/cache under its user folder; keep them in work/ (never the owner's).
export DON_USER_DIR="${DON_USER_DIR:-$PWD/work/logs/user-dir}"
ulimit -c 0

mkdir -p work/logs
[ -n "$OUT" ] || OUT="work/logs/gdbtrace-$(date +%Y%m%d-%H%M%S).log"
case "$OUT" in work/logs/*) ;; *) echo "output must be under work/logs/" >&2; exit 2 ;; esac
WORK="$(mktemp -d work/logs/gdbtrace-tmp.XXXXXX)"
PIDFILE="$WORK/inferior.pid"
PYFILE="$WORK/trace.py"
CMDFILE="$WORK/cmds.gdb"

# The runtime writes with std::cout, which is block-buffered into a file: without this the
# last lines are lost when the program is stopped. libstdbuf is what `stdbuf` preloads.
STDBUF_LIB=""
for dir in /usr/libexec/coreutils /usr/lib/coreutils /usr/lib/x86_64-linux-gnu/coreutils; do
    [ -f "$dir/libstdbuf.so" ] && { STDBUF_LIB="$dir/libstdbuf.so"; break; }
done

# gdb's own Python script: the pattern list is written by bash, the rest is fixed.
{
    echo "PATTERNS = ["
    for p in "${PATTERNS[@]}"; do
        esc="${p//\\/\\\\}"; esc="${esc//\"/\\\"}"
        echo "    \"$esc\","
    done
    echo "]"
    echo "FRAMES = $FRAMES"
    echo "CAP = $CAP"
    echo "PIDFILE = \"$PIDFILE\""
    cat <<'PYEOF'
import gdb
import collections
import re

GAME_FRAME = re.compile(r"^(\w+_0x[0-9a-f]{5,8})\(")
seq = 0
sig_counts = collections.Counter()
ring = collections.deque(maxlen=300)

last_game = {}
lookups = collections.Counter()

def game_frame(key):
    # Walking the stack costs time; after a few lookups per key the last answer is reused.
    lookups[key] += 1
    if lookups[key] > 20 and key in last_game:
        return last_game[key]
    f = gdb.newest_frame()
    for _ in range(40):
        if f is None:
            break
        n = f.name() or ""
        m = GAME_FRAME.match(n)
        if m:
            last_game[key] = m.group(1)
            return m.group(1)
        f = f.older()
    return last_game.get(key, "-")

def trace_hit(name, exprs):
    global seq
    seq += 1
    vals = []
    for e in exprs:
        try:
            vals.append("%s=0x%x" % (e, int(gdb.parse_and_eval(e)) & 0xFFFFFFFFFFFFFFFF))
        except gdb.error:
            vals.append("%s=?" % e)
    gf = game_frame((name, tuple(vals)))
    thr = gdb.selected_thread().name or "?"
    short = name.split("(")[0]
    ring.append("[trace] #%d (%s) %s %s game=%s" % (seq, thr, short, " ".join(vals), gf))
    sig = (name, tuple(vals), gf)
    sig_counts[sig] += 1
    if sig_counts[sig] > CAP:
        if sig_counts[sig] == CAP + 1:
            print("[trace] CAP: %s %s game=%s: full backtrace not printed again (seq %d)" % (short, " ".join(vals), gf, seq))
        return
    try:
        bt = gdb.execute("bt %d" % FRAMES, to_string=True).strip().splitlines()
    except gdb.error as e:
        bt = ["<bt failed: %s>" % e]
    bt = [re.sub(r"0x[0-9a-f]+ in ", "", l) for l in bt]
    print("[trace] #%d (%s) %s %s game=%s\n    %s" % (seq, thr, name, " ".join(vals), gf, "\n    ".join(bt)))

def setup():
    inf = gdb.selected_inferior()
    with open(PIDFILE, "w") as f:
        f.write(str(inf.pid))
    for spec in PATTERNS:
        pat, _, rest = spec.partition("@@")
        ex, _, cond = rest.partition("@@")
        exprs = [x for x in ex.split(";") if x]
        try:
            listing = gdb.execute("info functions %s" % pat, to_string=True)
        except gdb.error as e:
            print("[trace] pattern %r: %s" % (pat, e))
            continue
        n = 0
        seen = set()
        for line in listing.splitlines():
            m = re.match(r"^(0x[0-9a-f]+)\s+(\S.*)$", line)
            if not m or "[clone .cold]" in m.group(2) or m.group(1) in seen:
                continue
            seen.add(m.group(1))
            b = gdb.Breakpoint("*" + m.group(1), internal=True)
            if cond:
                b.condition = cond
            b.commands = "silent\npython trace_hit(%r, %r)\ncontinue" % (m.group(2), exprs)
            n += 1
        print("[trace] pattern %r: %d breakpoints" % (spec, n))

def finish():
    print("\n[trace] ===== stopped after %d events; distinct signatures by count (top 60) =====" % seq)
    for (name, vals, gf), n in sig_counts.most_common(60):
        print("[trace] %8d  %s %s game=%s" % (n, name.split("(")[0], " ".join(vals), gf))
    print("[trace] ===== last %d events (one line each) =====" % len(ring))
    for t in ring:
        print(t)
PYEOF
} > "$PYFILE"

{
    echo "set pagination off"
    echo "set language c++"
    echo "set confirm off"
    echo "set breakpoint pending off"
    echo "handle SIGPIPE nostop noprint pass"
    echo "handle SIGINT stop print nopass"
    [ -z "$STDBUF_LIB" ] || {
        echo "set environment LD_PRELOAD $STDBUF_LIB"
        echo "set environment _STDBUF_O 0"
        echo "set environment _STDBUF_E 0"
    }
    echo "source $PYFILE"
    echo "starti"
    echo "python setup()"
    echo "continue"
    echo "python finish()"
    echo "echo \\n[trace] ===== backtrace of every host thread =====\\n"
    echo "thread apply all bt $((FRAMES + 12))"
    echo "kill"
} > "$CMDFILE"

GDB_PID=""
cleanup() {
    # Only what this script started: the inferior by the pid gdb wrote, then gdb itself.
    if [ -s "$PIDFILE" ]; then
        local ipid
        ipid="$(cat "$PIDFILE")"
        kill -0 "$ipid" 2>/dev/null && kill -KILL "$ipid" 2>/dev/null || true
    fi
    if [ -n "$GDB_PID" ] && kill -0 "$GDB_PID" 2>/dev/null; then
        kill -TERM "$GDB_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

# The program has a headless mode, so no virtual display is needed. Its --seconds is well beyond
# the trace window: what ends the run is the SIGINT below (which prints the last events and the
# backtraces of every host thread), not the program's own end after --seconds.
timeout --kill-after=10 $((SECONDS_TO_RUN + 120)) \
    gdb -q -batch -x "$CMDFILE" --args "$EXE" --headless --sound off --seconds $((SECONDS_TO_RUN + 60)) "$ELF" > "$OUT" 2>&1 &
GDB_PID=$!

# Wait for gdb to write the program's pid (up to 60 s: breakpoints are set after that, which
# can take a while with many matches), then let it run for the time limit and interrupt it.
for _ in $(seq 1 600); do
    [ -s "$PIDFILE" ] && break
    kill -0 "$GDB_PID" 2>/dev/null || break
    sleep 0.1
done
if [ -s "$PIDFILE" ]; then
    # Count the time from when the program starts running (after the breakpoints are set).
    for _ in $(seq 1 1200); do
        grep -q "pattern .*: [0-9]* breakpoints" "$OUT" 2>/dev/null && \
            [ "$(grep -c "\[trace\] pattern" "$OUT")" -ge "${#PATTERNS[@]}" ] && break
        kill -0 "$GDB_PID" 2>/dev/null || break
        sleep 0.1
    done
    sleep "$SECONDS_TO_RUN"
    IPID="$(cat "$PIDFILE")"
    kill -0 "$IPID" 2>/dev/null && kill -INT "$IPID" 2>/dev/null || true
fi
wait "$GDB_PID" || true
GDB_PID=""
echo "gdb_trace: output $OUT ($(wc -c < "$OUT") bytes)"
