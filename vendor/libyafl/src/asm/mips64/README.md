# MIPS64 Assembly - Changes and Notes

This directory contains the MIPS64 N64 ABI context switching implementation for fcontext.

## Changes from Boost.Context

### API Refactoring (2026)

The original Boost.Context API required fiber functions to explicitly call `yafl_switch()` at the end to return control. The fcontext library has been refactored to allow fiber functions to return naturally with a `void` return type, eliminating the need for explicit context switches at function end.

This required a key fix in `make_context_mips64_n64_elf_gas.S`:

#### Fix in `finish` Routine - Context Pointer Preservation

**Original Problem:** Early versions tried to reconstruct the context pointer from $sp using fixed offset calculations (`$sp - 160`). This was unreliable because:

1. `yafl_switch` adjusts $sp by 160 bytes after restoring the context
2. The fiber function then allocates its own stack space, moving $sp even further
3. By the time `finish` is reached, $sp is no longer near the context data
4. This caused segmentation faults when fiber functions did substantial work

**Solution:** Use a callee-saved register (S1) to preserve the context pointer.

In `make_context_mips64_n64_elf_gas.S`:
```asm
# Save context pointer in S1 slot within the context
# S1 is callee-saved, so the fiber will preserve it across calls
sd  $v0, 72($v0)
```

In `finish` routine:
```asm
finish:
    # S1 contains the context pointer (saved during yafl_make_context and preserved by fiber)
    # S1 is callee-saved, so it survives the entire fiber execution
    ld $gp, 136($s1)
    ...
```

**Why This Works:**
- The MIPS64 ABI requires S1 to be callee-saved
- We store the context pointer in the S1 register slot at offset 72 during initialization
- When `yafl_switch` restores the context, it loads S1 from this slot
- The fiber function must preserve S1 (ABI requirement), so it's still available in `finish`
- By using S1 directly, we avoid relying on $sp which has been moved during fiber execution

## Stack Pointer Alignment

Stack operations ensure 16-byte alignment compliance required by the MIPS64 ABI. The context structure and stack metadata are positioned to maintain proper alignment for all function calls within fiber execution.

## Files

- `make_context_mips64_n64_elf_gas.S` - Context initialization
- `switch_mips64_n64_elf_gas.S` - Context switching/jumping

## Updating from Boost.Context

When updating MIPS64 assembly files from Boost.Context, be sure to reapply the modifications documented in this file and in `../../MODIFICATIONS.md`. The key change is removal of per-context $gp save/restore since $gp is shared across the entire program.
