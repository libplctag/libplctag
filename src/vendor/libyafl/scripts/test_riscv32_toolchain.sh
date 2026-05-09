#!/bin/bash
# DISABLED - RISC-V 32-bit support is deferred (as of 2026-01-25)
# See: RISCV32_IMPLEMENTATION_PLAN.md and src/asm/riscv32/README.md
#
# Test script to verify RISC-V 32-bit toolchain availability
# This script tests if we can compile RV32 code with available toolchains
# Kept for future use when RV32 support is re-enabled.

set -e

echo "=== RISC-V 32-bit Toolchain Verification ==="
echo

# Test 1: Check if qemu-riscv32 is available
echo "Test 1: Checking for qemu-riscv32..."
if command -v qemu-riscv32 &> /dev/null; then
    echo "✓ qemu-riscv32 found: $(qemu-riscv32 --version | head -n1)"
else
    echo "✗ qemu-riscv32 not found"
    exit 1
fi
echo

# Test 2: Check if gcc-riscv64-linux-gnu is available
echo "Test 2: Checking for gcc-riscv64-linux-gnu..."
if command -v riscv64-linux-gnu-gcc &> /dev/null; then
    echo "✓ riscv64-linux-gnu-gcc found: $(riscv64-linux-gnu-gcc --version | head -n1)"
else
    echo "✗ riscv64-linux-gnu-gcc not found"
    exit 1
fi
echo

# Test 3: Try to compile a simple RV32 program
echo "Test 3: Attempting to compile RV32 program with riscv64-linux-gnu-gcc..."
cat > /tmp/test_rv32.c << 'EOF'
#include <stdio.h>
int main() {
#ifdef __riscv
    printf("RISC-V detected\n");
    printf("XLEN: %d\n", __riscv_xlen);
#ifdef __riscv_float_abi_soft
    printf("Float ABI: soft\n");
#elif defined(__riscv_float_abi_single)
    printf("Float ABI: single\n");
#elif defined(__riscv_float_abi_double)
    printf("Float ABI: double\n");
#else
    printf("Float ABI: unknown\n");
#endif
#ifdef __riscv_flen
    printf("FLEN: %d\n", __riscv_flen);
#else
    printf("No FPU\n");
#endif
#else
    printf("Not RISC-V\n");
#endif
    return 0;
}
EOF

# Try different RV32 configurations
CONFIGS=(
    "rv32gc -mabi=ilp32d"
    "rv32gc -mabi=ilp32"
    "rv32imac -mabi=ilp32"
)

SUCCESS=0
for config in "${CONFIGS[@]}"; do
    march=$(echo $config | cut -d' ' -f1)
    mabi=$(echo $config | cut -d' ' -f2)
    echo "  Trying: -march=$march $mabi -static"
    if riscv64-linux-gnu-gcc -march=$march $mabi -static /tmp/test_rv32.c -o /tmp/test_rv32 2>&1; then
        echo "  ✓ Compilation succeeded with $march $mabi"

        # Test 4: Try to run with qemu-riscv32
        echo "  Test 4: Running with qemu-riscv32..."
        if qemu-riscv32 /tmp/test_rv32 2>&1; then
            echo "  ✓ Execution succeeded"
            SUCCESS=1
            break
        else
            echo "  ✗ Execution failed"
        fi
    else
        echo "  ✗ Compilation failed with $march $mabi"
    fi
    echo
done

rm -f /tmp/test_rv32.c /tmp/test_rv32

if [ $SUCCESS -eq 1 ]; then
    echo
    echo "=== SUCCESS: RV32 toolchain is functional ==="
    echo "Recommended configuration: -march=$march $mabi -static"
    exit 0
else
    echo
    echo "=== FAILURE: Unable to compile/run RV32 code ==="
    echo "You may need to build a custom RV32 toolchain from source."
    exit 1
fi
