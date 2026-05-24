#!/usr/bin/env bash
# DISABLED - RISC-V 32-bit support is deferred (as of 2026-01-25)
# See: RISCV32_IMPLEMENTATION_PLAN.md and src/asm/riscv32/README.md
# Kept for future use when RV32 support is re-enabled.

set -euo pipefail

# This script spins up a Colima-managed Debian container and tests
# whether RISC-V 32-bit toolchain support is available.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"

COLIMA_CPUS="${COLIMA_CPUS:-4}"
COLIMA_MEMORY="${COLIMA_MEMORY:-6}"
COLIMA_DISK="${COLIMA_DISK:-40}"
COLIMA_VM_TYPE="${COLIMA_VM_TYPE:-vz}"
DEBIAN_IMAGE="${DEBIAN_IMAGE:-debian:bookworm}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/arm64}"
HOST_UID="${HOST_UID:-$(id -u)}"
HOST_GID="${HOST_GID:-$(id -g)}"
HOST_LOG_DIR="${HOST_LOG_DIR:-${REPO_ROOT}/out/riscv32-test}"

if ! command -v colima >/dev/null 2>&1; then
    echo "colima is required. Install it with 'brew install colima'." >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker (CLI) is required. Install it with 'brew install docker'." >&2
    exit 1
fi

mkdir -p "${HOST_LOG_DIR}"

realpath_dir() {
    local dir="$1"
    (cd "${dir}" && pwd -P)
}

HOST_LOG_DIR="$(realpath_dir "${HOST_LOG_DIR}")"

SOURCE_MOUNT="/mnt/yafl-src"

if [[ "${HOST_LOG_DIR}" != "${REPO_ROOT}"* ]]; then
    echo "HOST_LOG_DIR (${HOST_LOG_DIR}) must reside under the repository root (${REPO_ROOT})." >&2
    exit 1
fi

if [[ "${HOST_LOG_DIR}" == "${REPO_ROOT}" ]]; then
    echo "HOST_LOG_DIR may not be the repository root." >&2
    exit 1
fi

REL_LOG_DIR="${HOST_LOG_DIR:${#REPO_ROOT}}"
HOST_LOG_DIR_MOUNT="${SOURCE_MOUNT}${REL_LOG_DIR}"

COLIMA_PROFILE="${COLIMA_PROFILE:-riscv32-test}"

COLIMA_ARGS=(
    start
    --profile "${COLIMA_PROFILE}"
    --arch aarch64
    --cpu "${COLIMA_CPUS}"
    --memory "${COLIMA_MEMORY}"
    --disk "${COLIMA_DISK}"
    --vm-type "${COLIMA_VM_TYPE}"
    --mount "${REPO_ROOT}:w"
)

if ! colima status "${COLIMA_PROFILE}" >/dev/null 2>&1; then
    echo "Starting Colima profile '${COLIMA_PROFILE}' with ARM64 Debian configuration..."
    colima "${COLIMA_ARGS[@]}"
else
    echo "Colima profile '${COLIMA_PROFILE}' already running; using existing instance."
fi

DOCKER_CONTEXT_ARGS=()
if docker context inspect "colima-${COLIMA_PROFILE}" >/dev/null 2>&1; then
    DOCKER_CONTEXT_ARGS=(--context "colima-${COLIMA_PROFILE}")
fi

TEST_COMMAND=$(cat <<'EOF'
set -e
export DEBIAN_FRONTEND=noninteractive

echo "=== RISC-V 32-bit Toolchain Verification ==="
echo "Updating apt metadata..."
apt-get update -qq >/dev/null 2>&1

echo "Installing RISC-V toolchain and QEMU..."
apt-get install -y -qq \
    gcc-riscv64-linux-gnu \
    qemu-user \
    qemu-user-static >/dev/null 2>&1

echo ""
echo "Running toolchain verification script..."
bash "${SOURCE_MOUNT}/scripts/test_riscv32_toolchain.sh" 2>&1 | tee "${HOST_LOG_DIR}/riscv32_test.log"
TEST_RESULT=${PIPESTATUS[0]}

# Fix ownership of log file
chown "${HOST_UID}:${HOST_GID}" "${HOST_LOG_DIR}/riscv32_test.log" >/dev/null 2>&1 || true

if [ "${TEST_RESULT}" = "0" ]; then
    echo ""
    echo "✅ SUCCESS: RISC-V 32-bit toolchain is functional!"
    echo "    You can proceed with Phase 1 implementation."
    exit 0
else
    echo ""
    echo "❌ FAILURE: RISC-V 32-bit toolchain is not available."
    echo "    See ${HOST_LOG_DIR}/riscv32_test.log for details."
    echo "    Consider building a custom toolchain (see RISCV32_IMPLEMENTATION_PLAN.md Phase 0, Option A)."
    exit 1
fi
EOF
)

echo "Launching Debian ${DEBIAN_IMAGE} container on ${DOCKER_PLATFORM}..."
docker "${DOCKER_CONTEXT_ARGS[@]}" run --rm \
    --platform "${DOCKER_PLATFORM}" \
    -v "${REPO_ROOT}:${SOURCE_MOUNT}:rw" \
    -e SOURCE_MOUNT="${SOURCE_MOUNT}" \
    -e HOST_LOG_DIR="${HOST_LOG_DIR_MOUNT}" \
    -e HOST_UID="${HOST_UID}" \
    -e HOST_GID="${HOST_GID}" \
    "${DEBIAN_IMAGE}" \
    bash -lc "${TEST_COMMAND}"

echo ""
echo "Test log saved to: ${HOST_LOG_DIR}/riscv32_test.log"
