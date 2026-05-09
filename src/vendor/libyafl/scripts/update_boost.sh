#!/bin/bash
set -e

BOOST_VERSION=$1

if [ -z "$BOOST_VERSION" ]; then
    echo "Usage: $0 <boost_version>"
    echo "Example: $0 1.81.0"
    exit 1
fi

echo "Updating Boost.Context assembly files to version ${BOOST_VERSION}..."

# Create temp directory
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# Clone Boost.Context
echo "Cloning Boost.Context..."
git clone --depth 1 --branch "boost-${BOOST_VERSION}" https://github.com/boostorg/context.git "$TEMP_DIR/context"

# Source directory in Boost.Context
SRC_ASM="$TEMP_DIR/context/src/asm"

# Destination directory in yafl
DEST_ASM="src/asm"

# Clear existing assembly files (but keep directories)
echo "Cleaning existing assembly files..."
find "$DEST_ASM" -name "*.S" -type f -delete
find "$DEST_ASM" -name "*.asm" -type f -delete

# Helper function to copy files
copy_arch_files() {
    local arch_src=$1
    local arch_dest=$2
    
    echo "Copying $arch_src to $arch_dest..."
    mkdir -p "$DEST_ASM/$arch_dest"
    
    # Copy files matching pattern
    # Boost files are flat in src/asm, named like make_x86_64_sysv_elf_gas.S
    cp "$SRC_ASM"/*_"$arch_src"_*.S "$DEST_ASM/$arch_dest/" 2>/dev/null || true
    cp "$SRC_ASM"/*_"$arch_src"_*.asm "$DEST_ASM/$arch_dest/" 2>/dev/null || true
}

# Map Boost architectures to our directory structure
copy_arch_files "x86_64" "x86_64"
copy_arch_files "i386"   "i386"
copy_arch_files "arm"    "arm"
copy_arch_files "arm64"  "arm64"
copy_arch_files "mips32" "mips"
copy_arch_files "mips64" "mips64"
copy_arch_files "ppc32"  "ppc32"
copy_arch_files "ppc64"  "ppc64"
copy_arch_files "riscv64" "riscv64"
copy_arch_files "s390x"  "s390x"
copy_arch_files "sparc64" "sparc64"
copy_arch_files "combined" "xtensa" # Xtensa might need manual handling if not standard in Boost

# Update VERSION file
echo "${BOOST_VERSION}-0" > VERSION

echo "------------------------------------------------"
echo "Update complete."
echo "Version reset to: ${BOOST_VERSION}-0"
echo "Please verify the changes and commit."
echo "------------------------------------------------"