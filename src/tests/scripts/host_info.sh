#!/usr/bin/env bash
#
# host_info.sh - Detect and print host hardware/OS details.
#
# Output: key=value pairs suitable for embedding in CSV comments or logs.
# Also prints a human-readable summary to stderr.
#
# Usage:
#   source host_info.sh   # sets HOST_* variables
#   ./host_info.sh        # prints to stdout

detect_host_info() {
    HOST_OS=$(uname -s)
    HOST_ARCH=$(uname -m)
    HOST_KERNEL=$(uname -r)

    HOST_CPU_MODEL="unknown"
    HOST_CPU_FREQ="unknown"
    HOST_CPU_COUNT="unknown"
    HOST_MANUFACTURER="unknown"
    HOST_PRODUCT="unknown"
    HOST_RAM_GB="unknown"

    case "$HOST_OS" in
        Darwin)
            HOST_CPU_MODEL=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
            HOST_CPU_COUNT=$(sysctl -n hw.ncpu 2>/dev/null || echo "unknown")

            # CPU frequency: try Intel path first, then Apple Silicon.
            cpu_freq=$(sysctl -n hw.cpufrequency 2>/dev/null || echo "")
            if [ -n "$cpu_freq" ] && [ "$cpu_freq" != "0" ]; then
                HOST_CPU_FREQ="$((cpu_freq / 1000000)) MHz"
            else
                # Apple Silicon: try powermetrics or known frequencies.
                # ioreg cluster-core info (varies by macOS version).
                p_freq=$(ioreg -rl -n pmgr 2>/dev/null | grep -o '"max-clock-frequency"=<[^>]*>' | head -1 | grep -o '<[^>]*>' | tr -d '<>' 2>/dev/null)
                if [ -n "$p_freq" ]; then
                    # Value is little-endian hex bytes.
                    dec=$(echo "$p_freq" | fold -w2 | tac | tr -d '\n' | xargs -I{} printf '%d' "0x{}" 2>/dev/null || echo "")
                    if [ -n "$dec" ] && [ "$dec" != "0" ]; then
                        HOST_CPU_FREQ="$((dec / 1000000)) MHz"
                    fi
                fi
                # Fallback: extract from sysctl brand string if it contains GHz.
                if [ "$HOST_CPU_FREQ" = "unknown" ]; then
                    ghz=$(echo "$HOST_CPU_MODEL" | grep -oE '[0-9]+\.[0-9]+ ?GHz' | head -1)
                    if [ -n "$ghz" ]; then
                        HOST_CPU_FREQ="$ghz"
                    fi
                fi
                # Apple Silicon doesn't expose frequency; use known specs.
                if [ "$HOST_CPU_FREQ" = "unknown" ]; then
                    case "$HOST_CPU_MODEL" in
                        *M1\ Pro*|*M1\ Max*) HOST_CPU_FREQ="3228 MHz (P-core)" ;;
                        *M1*)                HOST_CPU_FREQ="3200 MHz (P-core)" ;;
                        *M2\ Pro*|*M2\ Max*) HOST_CPU_FREQ="3490 MHz (P-core)" ;;
                        *M2*)                HOST_CPU_FREQ="3490 MHz (P-core)" ;;
                        *M3\ Pro*|*M3\ Max*) HOST_CPU_FREQ="4050 MHz (P-core)" ;;
                        *M3*)                HOST_CPU_FREQ="4050 MHz (P-core)" ;;
                        *M4\ Pro*|*M4\ Max*) HOST_CPU_FREQ="4510 MHz (P-core)" ;;
                        *M4*)                HOST_CPU_FREQ="4410 MHz (P-core)" ;;
                    esac
                fi
            fi

            ram_bytes=$(sysctl -n hw.memsize 2>/dev/null || echo "")
            if [ -n "$ram_bytes" ]; then
                HOST_RAM_GB="$((ram_bytes / 1073741824)) GB"
            fi

            # System model.
            HOST_MANUFACTURER="Apple"
            HOST_PRODUCT=$(sysctl -n hw.model 2>/dev/null || echo "unknown")
            # Try to get the marketing name (e.g. "MacBook Air (M1, 2020)").
            marketing=$(/usr/libexec/PlistBuddy -c "print 0:product-name" /dev/stdin 2>/dev/null \
                <<< "$(ioreg -r -d 1 -c IOPlatformDevice 2>/dev/null | grep -A1 'product-name' | tail -1 | sed 's/.*= //')" 2>/dev/null || true)
            if [ -n "$marketing" ] && [ "$marketing" != "" ]; then
                HOST_PRODUCT="$HOST_PRODUCT ($marketing)"
            fi
            ;;

        Linux)
            if [ -f /proc/cpuinfo ]; then
                HOST_CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')
                HOST_CPU_COUNT=$(grep -c '^processor' /proc/cpuinfo)
                freq=$(grep -m1 'cpu MHz' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')
                if [ -n "$freq" ]; then
                    HOST_CPU_FREQ="${freq} MHz"
                fi
            fi
            # Try lscpu as fallback for frequency.
            if [ "$HOST_CPU_FREQ" = "unknown" ] && command -v lscpu >/dev/null 2>&1; then
                freq=$(lscpu 2>/dev/null | grep -i 'max mhz' | awk '{print $NF}')
                if [ -n "$freq" ]; then
                    HOST_CPU_FREQ="${freq} MHz"
                fi
            fi

            ram_kb=$(grep -m1 'MemTotal' /proc/meminfo 2>/dev/null | awk '{print $2}')
            if [ -n "$ram_kb" ]; then
                HOST_RAM_GB="$((ram_kb / 1048576)) GB"
            fi

            # System manufacturer/model.
            if [ -f /sys/class/dmi/id/sys_vendor ]; then
                HOST_MANUFACTURER=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || echo "unknown")
            fi
            if [ -f /sys/class/dmi/id/product_name ]; then
                HOST_PRODUCT=$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo "unknown")
            fi
            ;;

        MINGW*|MSYS*|CYGWIN*)
            HOST_OS="Windows"
            if command -v wmic >/dev/null 2>&1; then
                HOST_CPU_MODEL=$(wmic cpu get name 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                HOST_CPU_COUNT=$(wmic cpu get NumberOfLogicalProcessors 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                freq=$(wmic cpu get MaxClockSpeed 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                if [ -n "$freq" ]; then
                    HOST_CPU_FREQ="${freq} MHz"
                fi
                HOST_MANUFACTURER=$(wmic computersystem get manufacturer 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                HOST_PRODUCT=$(wmic computersystem get model 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                ram=$(wmic computersystem get TotalPhysicalMemory 2>/dev/null | sed -n '2p' | sed 's/\r//g; s/[[:space:]]*$//')
                if [ -n "$ram" ]; then
                    HOST_RAM_GB="$((ram / 1073741824)) GB"
                fi
            fi
            ;;
    esac

    HOST_HOSTNAME=$(hostname -s 2>/dev/null || hostname 2>/dev/null || echo "unknown")
}

print_host_info() {
    echo "os=$HOST_OS"
    echo "arch=$HOST_ARCH"
    echo "kernel=$HOST_KERNEL"
    echo "hostname=$HOST_HOSTNAME"
    echo "manufacturer=$HOST_MANUFACTURER"
    echo "product=$HOST_PRODUCT"
    echo "cpu_model=$HOST_CPU_MODEL"
    echo "cpu_count=$HOST_CPU_COUNT"
    echo "cpu_freq=$HOST_CPU_FREQ"
    echo "ram=$HOST_RAM_GB"
}

print_host_summary() {
    >&2 echo "Host: $HOST_MANUFACTURER $HOST_PRODUCT"
    >&2 echo "CPU:  $HOST_CPU_MODEL ($HOST_CPU_COUNT cores, $HOST_CPU_FREQ)"
    >&2 echo "RAM:  $HOST_RAM_GB"
    >&2 echo "OS:   $HOST_OS $HOST_ARCH ($HOST_KERNEL)"
}

detect_host_info

# If run directly (not sourced), print output.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    print_host_summary
    echo "---"
    print_host_info
fi
