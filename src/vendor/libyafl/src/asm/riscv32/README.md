# RISC-V 32-bit Assembly - DEFERRED

## Status

RISC-V 32-bit (RV32) support is currently **DEFERRED** as of 2026-01-25.

## Why Deferred

The Debian package `gcc-riscv64-linux-gnu` does not include RV32 (ilp32) multilib support. Building a custom RV32 toolchain from source would add 30-60 minutes to Docker build times.

Toolchain verification test results (2026-01-25):
- ✅ qemu-riscv32 available (Debian Bookworm)
- ✅ gcc-riscv64-linux-gnu available
- ❌ RV32 multilib libraries missing (gnu/stubs-ilp32d.h, gnu/stubs-ilp32.h)

See `scripts/test_riscv32_toolchain.sh` and `scripts/run_colima_riscv32_test.sh` for verification details.

## To Re-enable in the Future

1. **Build Custom Toolchain:** Modify `docker/Dockerfile.riscv32` to build from [riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain) source with RV32 support
2. **Implement Assembly:** Create `make_riscv32_sysv_elf_gas.S` and `jump_riscv32_sysv_elf_gas.S` from the implementation plan
3. **Configure Build:** Create `toolchains/riscv32-unknown-linux-gnu.cmake`
4. **Add CI:** Create `.github/workflows/riscv32-unknown-linux-gnu.yml`

See `RISCV32_IMPLEMENTATION_PLAN.md` for complete implementation details (Phases 1-4).

## Files in This Directory

Currently empty. Files will be created when support is re-enabled.

## References

- Implementation Plan: `RISCV32_IMPLEMENTATION_PLAN.md`
- Test Script: `scripts/test_riscv32_toolchain.sh` (DISABLED)
- Colima Test Runner: `scripts/run_colima_riscv32_test.sh` (DISABLED)
- RISC-V GNU Toolchain: https://github.com/riscv-collab/riscv-gnu-toolchain
- Debian RV32 Status: https://wiki.debian.org/RISC-V/32
