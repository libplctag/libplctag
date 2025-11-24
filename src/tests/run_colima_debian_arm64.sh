#!/usr/bin/env bash
set -euo pipefail

# This script spins up a Colima-managed ARM64 Debian container, copies the current
# libplctag workspace into it, builds the project, and runs the simulator tests.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"

COLIMA_CPUS="${COLIMA_CPUS:-4}"
COLIMA_MEMORY="${COLIMA_MEMORY:-6}"
COLIMA_DISK="${COLIMA_DISK:-40}"
COLIMA_VM_TYPE="${COLIMA_VM_TYPE:-vz}"
DEBIAN_IMAGE="${DEBIAN_IMAGE:-debian:bookworm}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/arm64}"
HOST_UID="${HOST_UID:-$(id -u)}"
HOST_GID="${HOST_GID:-$(id -g)}"
HOST_BIN_DIST_DIR="${HOST_BIN_DIST_DIR:-${REPO_ROOT}/build/bin_dist}"
HOST_LOG_DIR="${HOST_LOG_DIR:-${REPO_ROOT}/out/colima-debian-arm64}"

if ! command -v colima >/dev/null 2>&1; then
    echo "colima is required. Install it with 'brew install colima'." >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker (CLI) is required. Install it with 'brew install docker'." >&2
    exit 1
fi

mkdir -p "${HOST_BIN_DIST_DIR}" "${HOST_LOG_DIR}"

realpath_dir() {
    local dir="$1"
    (cd "${dir}" && pwd -P)
}

HOST_BIN_DIST_DIR="$(realpath_dir "${HOST_BIN_DIST_DIR}")"
HOST_LOG_DIR="$(realpath_dir "${HOST_LOG_DIR}")"

SOURCE_MOUNT="/mnt/libplctag-src"

if [[ "${HOST_BIN_DIST_DIR}" != "${REPO_ROOT}"* ]]; then
    echo "HOST_BIN_DIST_DIR (${HOST_BIN_DIST_DIR}) must reside under the repository root (${REPO_ROOT})." >&2
    exit 1
fi

if [[ "${HOST_BIN_DIST_DIR}" == "${REPO_ROOT}" ]]; then
    echo "HOST_BIN_DIST_DIR may not be the repository root." >&2
    exit 1
fi

if [[ "${HOST_LOG_DIR}" != "${REPO_ROOT}"* ]]; then
    echo "HOST_LOG_DIR (${HOST_LOG_DIR}) must reside under the repository root (${REPO_ROOT})." >&2
    exit 1
fi

if [[ "${HOST_LOG_DIR}" == "${REPO_ROOT}" ]]; then
    echo "HOST_LOG_DIR may not be the repository root." >&2
    exit 1
fi

REL_BIN_DIST="${HOST_BIN_DIST_DIR:${#REPO_ROOT}}"
REL_LOG_DIR="${HOST_LOG_DIR:${#REPO_ROOT}}"
HOST_BIN_DIST_MOUNT="${SOURCE_MOUNT}${REL_BIN_DIST}"
HOST_LOG_DIR_MOUNT="${SOURCE_MOUNT}${REL_LOG_DIR}"

COLIMA_ARGS=(
    start
    --arch aarch64
    --cpu "${COLIMA_CPUS}"
    --memory "${COLIMA_MEMORY}"
    --disk "${COLIMA_DISK}"
    --vm-type "${COLIMA_VM_TYPE}"
    --mount "${REPO_ROOT}:w"
)

if ! colima status >/dev/null 2>&1; then
    echo "Starting Colima with ARM64 Debian configuration..."
    colima "${COLIMA_ARGS[@]}"
else
    echo "Colima already running; ensure it was started with ARM64 support if this is the first run."
fi

DOCKER_CONTEXT_ARGS=()
if docker context inspect colima >/dev/null 2>&1; then
    DOCKER_CONTEXT_ARGS=(--context colima)
fi

WORKSPACE="/workspace/libplctag"

BUILD_COMMAND=$(cat <<'EOF'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "Updating apt metadata..."
apt-get update >/dev/null
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    libssl-dev \
    git \
    ca-certificates \
    rsync \
    psmisc \
    gdb >/dev/null
rm -rf /var/lib/apt/lists/*

# Enable coredump generation for debugging segfaults
ulimit -c unlimited
echo "core" > /proc/sys/kernel/core_pattern

mkdir -p "${WORKSPACE}"
rsync -a --delete \
    --exclude 'build/' \
    --exclude 'out/' \
    "${SOURCE_MOUNT}/" "${WORKSPACE}/"

cd "${WORKSPACE}"
git config --global --add safe.directory "${WORKSPACE}" >/dev/null 2>&1 || true

echo "Building libplctag (ARM64)..."
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build

cd "${WORKSPACE}"
WORKSPACE_LOG_DIR="${WORKSPACE}/test_logs"
mkdir -p "${WORKSPACE_LOG_DIR}"
echo "Running simulator tests..."
./src/tests/run_simulator_tests.sh build/bin_dist "${WORKSPACE_LOG_DIR}" || TEST_FAILED=1

echo "Syncing build artifacts back to host..."
mkdir -p "${HOST_BIN_DIST}"
rsync -a --delete --chown="${HOST_UID}:${HOST_GID}" build/bin_dist/ "${HOST_BIN_DIST}/"

echo "Collecting logs..."
mkdir -p "${HOST_LOG_DIR}"
find "${HOST_LOG_DIR}" -type f \( -name '*.log' -o -name 'core' \) -delete 2>/dev/null || true
rsync -a --chown="${HOST_UID}:${HOST_GID}" "${WORKSPACE_LOG_DIR}/" "${HOST_LOG_DIR}/" 2>/dev/null || true
find "${WORKSPACE}" -maxdepth 1 -name '*.log' -exec rsync -a --chown="${HOST_UID}:${HOST_GID}" {} "${HOST_LOG_DIR}/" \; 2>/dev/null || true
find . -maxdepth 1 -name 'core' -exec rsync -a --chown="${HOST_UID}:${HOST_GID}" {} "${HOST_LOG_DIR}/" \; 2>/dev/null || true
chown "${HOST_UID}:${HOST_GID}" "${HOST_BIN_DIST}" "${HOST_LOG_DIR}" >/dev/null 2>&1 || true

if [ "${TEST_FAILED:-0}" = "1" ]; then
    echo "Tests failed, but logs have been collected for analysis."
    exit 1
fi
EOF
)

echo "Launching Debian ${DEBIAN_IMAGE} container on ${DOCKER_PLATFORM}..."
docker "${DOCKER_CONTEXT_ARGS[@]}" run --rm \
    --platform "${DOCKER_PLATFORM}" \
    --ulimit core=-1 \
    -v "${REPO_ROOT}:${SOURCE_MOUNT}:rw" \
    -e SOURCE_MOUNT="${SOURCE_MOUNT}" \
    -e WORKSPACE="${WORKSPACE}" \
    -e HOST_BIN_DIST="${HOST_BIN_DIST_MOUNT}" \
    -e HOST_LOG_DIR="${HOST_LOG_DIR_MOUNT}" \
    -e HOST_UID="${HOST_UID}" \
    -e HOST_GID="${HOST_GID}" \
    "${DEBIAN_IMAGE}" \
    bash -lc "${BUILD_COMMAND}"

echo "Done. Build outputs are in ${HOST_BIN_DIST_DIR}. Logs are in ${HOST_LOG_DIR}."
