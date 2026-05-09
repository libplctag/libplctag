#!/bin/bash
#
# Download and install musl-cross pre-built toolchains
#
# This script downloads pre-built cross-compiler toolchains from the
# cross-tools/musl-cross GitHub releases and installs them to the
# cross-compilers/x-tools/ directory for use with CMake builds.
#
# Usage: ./scripts/download-musl-cross.sh [optional: specific architecture]
#        ./scripts/download-musl-cross.sh                    # Download all
#        ./scripts/download-musl-cross.sh arm-unknown-linux-musleabihf  # Download one
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
X_TOOLS_DIR="$REPO_ROOT/cross-compilers/x-tools"

# musl-cross release version
RELEASE="20250929"

# Array of architectures we need
declare -a ARCHITECTURES=(
    "arm-unknown-linux-musleabihf"
    "riscv64-unknown-linux-musl"
    "s390x-ibm-linux-musl"
    "powerpc64le-unknown-linux-musl"
)

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}Downloading musl-cross toolchains${NC}"
echo "Release: ${RELEASE}"
echo "Destination: $X_TOOLS_DIR"
echo ""

# Create x-tools directory if it doesn't exist
mkdir -p "$X_TOOLS_DIR"

# If specific architecture requested, override the array
if [ $# -gt 0 ]; then
    ARCHITECTURES=("$1")
    echo "Downloading only: $1"
    echo ""
fi

# Download each toolchain
for arch in "${ARCHITECTURES[@]}"; do
    filename="${arch}.tar.xz"
    filepath="$X_TOOLS_DIR/$filename"
    extract_dir="$X_TOOLS_DIR/$arch"
    url="https://github.com/cross-tools/musl-cross/releases/download/${RELEASE}/${filename}"

    # Skip if already exists
    if [ -d "$extract_dir" ]; then
        echo -e "${GREEN}✓${NC} $arch already installed, skipping"
        continue
    fi

    echo -e "${BLUE}Downloading${NC} $filename..."

    # Download with retries
    for attempt in 1 2 3; do
        if wget -O "$filepath" "$url"; then
            echo -e "${BLUE}Extracting${NC} $filename..."
            tar xf "$filepath" -C "$X_TOOLS_DIR"
            rm "$filepath"
            echo -e "${GREEN}✓${NC} Installed: $arch"
            break
        else
            if [ $attempt -lt 3 ]; then
                echo "  Attempt $attempt failed, retrying in 10 seconds..."
                sleep 10
            else
                echo -e "${RED}✗${NC} Failed to download $filename after 3 attempts"
                rm -f "$filepath"
                exit 1
            fi
        fi
    done
done

echo ""
echo -e "${GREEN}All musl-cross toolchains ready!${NC}"
echo ""
echo "You can now build with musl targets:"
echo "  cmake -DTARGET=arm-unknown-linux-musleabihf .."
echo "  cmake -DTARGET=riscv64-unknown-linux-musl .."
echo "  cmake -DTARGET=s390x-ibm-linux-musl .."
echo "  cmake -DTARGET=powerpc64le-unknown-linux-musl .."
