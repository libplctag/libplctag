#!/bin/bash
#
# Download musl.cc cross-compiler toolchains
#
# This script downloads pre-built cross-compiler toolchains from musl.cc
# and stores them in the cross-compilers/ directory for Git LFS tracking.
#
# Usage: ./scripts/download-musl-cc.sh
#
# Before running this script, ensure Git LFS is installed:
#   brew install git-lfs  (macOS)
#   apt-get install git-lfs  (Ubuntu/Debian)
#   choco install git-lfs  (Windows)
#
# Then initialize Git LFS in this repository:
#   git lfs install
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CROSS_COMPILERS_DIR="$REPO_ROOT/cross-compilers"

# Create cross-compilers directory if it doesn't exist
mkdir -p "$CROSS_COMPILERS_DIR"

# Array of architectures to download
declare -a ARCHITECTURES=(
    "arm-linux-musleabihf"
    "riscv64-linux-musl"
    "s390x-linux-musl"
    "powerpc64le-linux-musl"
)

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}Downloading musl.cc cross-compiler toolchains${NC}"
echo "Destination: $CROSS_COMPILERS_DIR"
echo ""

# Check if Git LFS is configured
if ! git lfs version &>/dev/null; then
    echo -e "${RED}Error: Git LFS is not installed${NC}"
    echo ""
    echo "Install Git LFS first:"
    echo "  macOS: brew install git-lfs"
    echo "  Linux: apt-get install git-lfs"
    echo "  Windows: choco install git-lfs"
    echo ""
    echo "Then initialize it in this repository:"
    echo "  git lfs install"
    exit 1
fi

# Download each toolchain
for arch in "${ARCHITECTURES[@]}"; do
    filename="${arch}-cross.tgz"
    filepath="$CROSS_COMPILERS_DIR/$filename"
    url="https://musl.cc/${filename}"

    # Skip if already exists
    if [ -f "$filepath" ]; then
        echo -e "${GREEN}✓${NC} $filename already exists, skipping"
        continue
    fi

    echo -e "${BLUE}Downloading${NC} $filename..."

    # Download with retries
    for attempt in 1 2 3; do
        if wget -O "$filepath" "$url"; then
            echo -e "${GREEN}✓${NC} Downloaded: $filename"
            break
        else
            if [ $attempt -lt 3 ]; then
                echo "  Attempt $attempt failed, retrying in 15 seconds..."
                sleep 15
            else
                echo -e "${RED}✗${NC} Failed to download $filename after 3 attempts"
                rm -f "$filepath"
                exit 1
            fi
        fi
    done
done

echo ""
echo -e "${GREEN}All musl.cc cross-compilers downloaded successfully!${NC}"
echo ""
echo "Next steps:"
echo "  1. Add these files to Git LFS tracking:"
echo "     git lfs track 'cross-compilers/*.tgz'"
echo "  2. Commit and push:"
echo "     git add cross-compilers/ .gitattributes"
echo "     git commit -m 'Add musl.cc cross-compiler toolchains'"
echo "     git push"
