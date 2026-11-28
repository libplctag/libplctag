#!/usr/bin/env bash
#
# perf_env.sh - run a performance command under checked, recorded conditions.
#
# Usage:
#   perf_env.sh <out_dir> <command...>
#
# Refuses to start on battery, in Low Power Mode, or under a thermal warning (macOS),
# or with deep CPU idle states allowed (Linux).
# Sets ulimit -n 1024 for the command.  Writes <out_dir>/perf_env.txt with the host
# details, Wi-Fi state and thermal state before and after the run.  Exits with the
# command's exit code, or 2 if the conditions are not met.

set -u

if [ $# -lt 2 ]; then
    echo "Usage: $0 <out_dir> <command...>" >&2
    exit 2
fi

OUT_DIR=$1
shift
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$OUT_DIR/perf_env.txt"

mkdir -p "$OUT_DIR" || exit 2

# ponytail: pmset -g therm only reports warning levels, not powermetrics' finer pressure
# levels (Moderate, Heavy).  It needs no sudo.  Switch to powermetrics if runs throttle
# without a warning.
thermal_state() {
    pmset -g therm 2>/dev/null | tr '\n' ' '
}

thermal_ok() {
    case "$(thermal_state)" in
        *"No thermal warning level has been recorded"*"No performance warning level has been recorded"*) return 0 ;;
        *) return 1 ;;
    esac
}

# Deep idle states add 50-200 us to every wake.  That favors a client that spins over one
# that blocks, so Linux runs need them blocked (docs/performance.md, "Deep idle states").
IDLE_HOLD_CMD="sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf \"\\0\\0\\0\\0\" >&3; exec sleep 86400' &"

deep_idle_entries() {
    local n=0 s
    for s in /sys/devices/system/cpu/cpu*/cpuidle/state*; do
        [ "$(cat "$s/latency")" -gt 10 ] && n=$((n + $(cat "$s/usage")))
    done
    echo "$n"
}

# blocked, or none to block (no cpuidle driver, e.g. some VMs).
deep_idle_state() {
    local before
    if [ ! -d /sys/devices/system/cpu/cpu0/cpuidle ]; then
        echo "none (no cpuidle)"
        return 0
    fi
    before=$(deep_idle_entries)
    sleep 1
    if [ "$(deep_idle_entries)" -eq "$before" ]; then
        echo "blocked"
        return 0
    fi
    echo "allowed"
    return 1
}

if [ "$(uname -s)" = "Darwin" ]; then
    if ! pmset -g batt | grep -q "'AC Power'"; then
        echo "perf_env: on battery; connect mains power." >&2
        exit 2
    fi
    if pmset -g | grep -Eq 'lowpowermode[[:space:]]+1'; then
        echo "perf_env: Low Power Mode is on; turn it off." >&2
        exit 2
    fi
    if ! thermal_ok; then
        echo "perf_env: thermal warning active: $(thermal_state)" >&2
        exit 2
    fi
else
    echo "perf_env: power and thermal state are only checked on macOS." >&2
    if ! deep_idle_state > /dev/null; then
        echo "perf_env: deep CPU idle states are allowed.  Block them for 24 h with:" >&2
        echo "  $IDLE_HOLD_CMD" >&2
        exit 2
    fi
fi

if ! ulimit -n 1024; then
    echo "perf_env: cannot set ulimit -n 1024." >&2
    exit 2
fi

{
    echo "command=$*"
    echo "start=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "commit=$(git -C "$SCRIPT_DIR" describe --always --dirty 2>/dev/null || echo unknown)"
    "$SCRIPT_DIR/host_info.sh" 2>/dev/null
    echo "ulimit_n=$(ulimit -n)"
    [ "$(uname -s)" = "Darwin" ] && echo "wifi=$(networksetup -getairportpower en0 2>/dev/null)"
    echo "thermal_before=$(thermal_state)"
    [ "$(uname -s)" = "Darwin" ] || echo "deep_idle_before=$(deep_idle_state)"
} > "$ENV_FILE"

"$@"
rc=$?

{
    echo "end=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "thermal_after=$(thermal_state)"
    [ "$(uname -s)" = "Darwin" ] || echo "deep_idle_after=$(deep_idle_state)"
    echo "exit_code=$rc"
} >> "$ENV_FILE"

if [ "$(uname -s)" != "Darwin" ] && ! grep -q '^deep_idle_after=\(blocked\|none\)' "$ENV_FILE"; then
    echo "perf_env: deep idle states were allowed again by the end of the run; the results are suspect." >&2
fi

if [ "$(uname -s)" = "Darwin" ] && ! thermal_ok; then
    echo "perf_env: a thermal warning appeared during the run; the results are suspect." >&2
fi

exit $rc
