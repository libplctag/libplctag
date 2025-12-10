#!/usr/bin/env bash
set -euo pipefail

# This script spins up a Colima-managed ARM64 Alpine container, copies the current
# libplctag workspace into it, builds the project, and runs the simulator tests.
# Used to reproduce and debug the segmentation faults and issues specific to Alpine/musl.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"

COLIMA_CPUS="${COLIMA_CPUS:-4}"
COLIMA_MEMORY="${COLIMA_MEMORY:-6}"
COLIMA_DISK="${COLIMA_DISK:-40}"
COLIMA_VM_TYPE="${COLIMA_VM_TYPE:-vz}"
ALPINE_IMAGE="${ALPINE_IMAGE:-alpine:latest}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/arm64}"
HOST_UID="${HOST_UID:-$(id -u)}"
HOST_GID="${HOST_GID:-$(id -g)}"
HOST_BIN_DIST_DIR="${HOST_BIN_DIST_DIR:-${REPO_ROOT}/build/bin_dist}"
HOST_LOG_DIR="${HOST_LOG_DIR:-${REPO_ROOT}/out/colima-alpine-arm64}"
INTERACTIVE_DEBUG="${INTERACTIVE_DEBUG:-0}"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug|-d)
            INTERACTIVE_DEBUG=1
            echo "Interactive debug mode enabled - will drop to shell on test failure"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--debug|-d]"
            exit 1
            ;;
    esac
done

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
    echo "Starting Colima with ARM64 Alpine configuration..."
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
set -e

echo "Updating apk cache..."
apk update >/dev/null 2>&1

echo "Installing build dependencies (Alpine)..."
apk add --no-cache \
    build-base \
    cmake \
    ninja \
    pkgconfig \
    openssl-dev \
    git \
    ca-certificates \
    rsync \
    psmisc \
    gdb \
    valgrind \
    bash >/dev/null 2>&1

# Enable coredump generation for debugging segfaults
echo "Setting up core dump support..."
ulimit -c unlimited
# In Docker, we also set core_uses_pid to ensure unique core files
if [ -w /proc/sys/kernel/core_uses_pid ]; then
    echo 1 > /proc/sys/kernel/core_uses_pid 2>/dev/null || true
fi

COREDUMP_DIR="/tmp/coredumps"
mkdir -p "${COREDUMP_DIR}"
export COREDUMP_DIR

# Configure where core dumps go (if writable)
if [ -w /proc/sys/kernel/core_pattern ]; then
    echo "${COREDUMP_DIR}/core.%e.%p.%t" > /proc/sys/kernel/core_pattern 2>/dev/null || true
fi

mkdir -p "${WORKSPACE}"
rsync -a --delete \
    --exclude 'build/' \
    --exclude 'out/' \
    "${SOURCE_MOUNT}/" "${WORKSPACE}/"

cd "${WORKSPACE}"
git config --global --add safe.directory "${WORKSPACE}" >/dev/null 2>&1 || true

# Ensure test scripts are executable (rsync may not preserve permissions across volumes)
echo "Verifying test scripts were copied..."
if [ -f "src/tests/scripts/run_simulator_tests.sh" ]; then
    echo "  ✓ run_simulator_tests.sh found"
    chmod +x src/tests/scripts/run_simulator_tests.sh
else
    echo "  ✗ run_simulator_tests.sh NOT found!"
    echo "  Current directory: $(pwd)"
    echo "  Contents of src/tests/scripts/:"
    ls -la src/tests/scripts/ 2>/dev/null || echo "  src/tests/scripts/ directory does not exist!"
    exit 1
fi

chmod +x src/tests/*.sh src/tests/scripts/*.sh 2>/dev/null || true

echo "Building libplctag (ARM64 Alpine with musl)..."
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUSE_SANITIZERS=OFF
echo "Compiling..."
cmake --build build 2>&1 | tail -100

cd "${WORKSPACE}"
WORKSPACE_LOG_DIR="${WORKSPACE}/test_logs"
mkdir -p "${WORKSPACE_LOG_DIR}"

echo "Running simulator tests (with core dump collection)..."
echo "Core dumps will be written to: ${COREDUMP_DIR}"
if ./src/tests/scripts/run_simulator_tests.sh build/bin_dist "${WORKSPACE_LOG_DIR}"; then
    TEST_RESULT=0
else
    TEST_RESULT=$?
    echo "Tests exited with code $TEST_RESULT"
fi

echo "Syncing build artifacts back to host..."
mkdir -p "${HOST_BIN_DIST}"
rsync -a --delete --chown="${HOST_UID}:${HOST_GID}" build/bin_dist/ "${HOST_BIN_DIST}/"

echo "Collecting logs and debug info..."
mkdir -p "${HOST_LOG_DIR}"
find "${HOST_LOG_DIR}" -type f \( -name '*.log' -o -name 'core*' \) -delete 2>/dev/null || true

# Collect test logs
rsync -a --chown="${HOST_UID}:${HOST_GID}" "${WORKSPACE_LOG_DIR}/" "${HOST_LOG_DIR}/" 2>/dev/null || true

# Collect logs from workspace root
find "${WORKSPACE}" -maxdepth 1 -name '*.log' -type f -exec rsync -a --chown="${HOST_UID}:${HOST_GID}" {} "${HOST_LOG_DIR}/" \; 2>/dev/null || true

# Collect core dumps from multiple possible locations
echo "Searching for core dumps..."
CORE_COUNT=0
if [ -d "${COREDUMP_DIR}" ] && [ "$(ls -A ${COREDUMP_DIR} 2>/dev/null)" ]; then
    echo "Found cores in ${COREDUMP_DIR}:"
    ls -lh "${COREDUMP_DIR}/"
    rsync -a --chown="${HOST_UID}:${HOST_GID}" "${COREDUMP_DIR}/" "${HOST_LOG_DIR}/coredumps/" 2>/dev/null || true
    CORE_COUNT=$(find "${COREDUMP_DIR}" -type f | wc -l)
fi

# Also search in current directory and workspace
find . "${WORKSPACE}" -maxdepth 2 -name 'core*' -type f 2>/dev/null | while read core_file; do
    echo "Found core: $core_file"
    rsync -a --chown="${HOST_UID}:${HOST_GID}" "$core_file" "${HOST_LOG_DIR}/" 2>/dev/null || true
    CORE_COUNT=$((CORE_COUNT + 1))
done

echo "Core dumps collected: ${CORE_COUNT}"
chown "${HOST_UID}:${HOST_GID}" "${HOST_BIN_DIST}" "${HOST_LOG_DIR}" >/dev/null 2>&1 || true

echo "Logs collected in ${HOST_LOG_DIR}"
if [ "${TEST_RESULT:-0}" != "0" ]; then
    echo "=================================================="
    echo "Tests failed with exit code $TEST_RESULT"
    echo "=================================================="
    echo ""
    echo "Debug Information:"
    echo "  Test logs: ${WORKSPACE_LOG_DIR}/"
    echo "  Core dumps: ${COREDUMP_DIR}/"
    echo "  Build artifacts: ${WORKSPACE}/build/bin_dist/"
    echo ""

    if [ "${INTERACTIVE_DEBUG}" = "1" ]; then
        echo "Interactive debug mode enabled!"
        echo "Available tools: gdb, valgrind, build tools"
        echo ""
        echo "Useful commands:"
        echo "  cd ${WORKSPACE}"
        echo "  gdb ./build/bin_dist/ab_server ${COREDUMP_DIR}/core.ab_server.*"
        echo "  cat ${WORKSPACE_LOG_DIR}/logix_fast_emulator.log"
        echo "  ls -lh ${COREDUMP_DIR}/"
        echo ""
        echo "Type 'exit' to leave the shell"
        echo "=================================================="
        /bin/sh
    else
        echo ""
        echo "To debug interactively, run this script with --debug flag:"
        echo "  $0 --debug"
        echo ""
        exit 1
    fi
fi
EOF
)

echo "Launching Alpine ${ALPINE_IMAGE} container on ${DOCKER_PLATFORM}..."
echo "Core dumps will be captured for debugging crashes..."
if [ "${INTERACTIVE_DEBUG}" = "1" ]; then
    echo "Interactive debug mode: on test failure, you'll drop into a shell"
fi
echo ""

# Build docker run arguments
DOCKER_RUN_ARGS=("${DOCKER_CONTEXT_ARGS[@]}" run)

# Add interactive flags if debugging, always clean up with --rm
if [ "${INTERACTIVE_DEBUG}" = "1" ]; then
    DOCKER_RUN_ARGS+=(--rm -it)
else
    DOCKER_RUN_ARGS+=(--rm)
fi

DOCKER_RUN_ARGS+=(
    --platform "${DOCKER_PLATFORM}"
    --ulimit core=-1
    --cap-add=SYS_PTRACE
    -v "${REPO_ROOT}:${SOURCE_MOUNT}:rw"
    -e SOURCE_MOUNT="${SOURCE_MOUNT}"
    -e WORKSPACE="${WORKSPACE}"
    -e HOST_BIN_DIST="${HOST_BIN_DIST_MOUNT}"
    -e HOST_LOG_DIR="${HOST_LOG_DIR_MOUNT}"
    -e HOST_UID="${HOST_UID}"
    -e HOST_GID="${HOST_GID}"
    -e INTERACTIVE_DEBUG="${INTERACTIVE_DEBUG}"
    "${ALPINE_IMAGE}"
    sh -lc "${BUILD_COMMAND}"
)

docker "${DOCKER_RUN_ARGS[@]}"

echo "Done. Build outputs are in ${HOST_BIN_DIST_DIR}. Logs are in ${HOST_LOG_DIR}."
