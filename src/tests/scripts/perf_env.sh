#!/usr/bin/env bash
#
# perf_env.sh - run a performance command under checked, recorded conditions.
#
# Usage:
#   perf_env.sh <out_dir> <command...>
#
# Refuses to start on battery, in Low Power Mode, or under a thermal warning (macOS).
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
} > "$ENV_FILE"

"$@"
rc=$?

{
    echo "end=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "thermal_after=$(thermal_state)"
    echo "exit_code=$rc"
} >> "$ENV_FILE"

if [ "$(uname -s)" = "Darwin" ] && ! thermal_ok; then
    echo "perf_env: a thermal warning appeared during the run; the results are suspect." >&2
fi

exit $rc
