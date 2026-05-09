/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  | 0x0 | 0x4 | 0x8 | 0xc | 0x10| 0x14| 0x18| 0x1c|  *
 *  -------------------------------------------------  *
 *  | s16 | s17 | s18 | s19 | s20 | s21 | s22 | s23 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  | 0x20| 0x24| 0x28| 0x2c| 0x30| 0x34| 0x38| 0x3c|  *
 *  -------------------------------------------------  *
 *  | s24 | s25 | s26 | s27 | s28 | s29 | s30 | s31 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  | 0x0 | 0x4 | 0x8 | 0xc | 0x10| 0x14| 0x18| 0x1c|  *
 *  -------------------------------------------------  *
 *  | sjlj|hiddn|  v1 |  v2 |  v3 |  v4 |  v5 |  v6 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 |  29 |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  | 0x20| 0x24| 0x28| 0x2c| 0x30| 0x34| 0x38| 0x3c|  *
 *  -------------------------------------------------  *
 *  |  v7 |  v8 |  lr |  pc | FCTX| DATA|           |  *
 *  -------------------------------------------------  *
 *                                                     *
 *******************************************************/

.text
.private_extern _yafl_switch
.globl _yafl_switch
.align 2
_yafl_switch:
    @ save LR as PC
    push {lr}
    @ save V1-V8,LR and preserve layout slot for the old hidden pointer
    push {a1,v1-v8,lr}

    @ locate TLS to save/restore SjLj handler
    mrc  p15, 0, v2, c13, c0, #3
    bic  v2, v2, #3

    @ load TLS[__PTK_LIBC_DYLD_Unwind_SjLj_Key]
    ldr  v1, [v2, #72]
    @ save SjLj handler
    push  {v1}

    @ prepare stack for FPU
    sub  sp, sp, #64
#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
    @ save S16-S31
    vstmia sp, {d8-d15}
#endif

    @ save current context through the first argument pointer
    str  sp, [a1]

    @ restore target context from the second argument
    mov  sp, a2

#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
    @ restore S16-S31
    vldmia  sp, {d8-d15}
#endif
    @ prepare stack for FPU
    add  sp, sp, #64

    @ r#estore SjLj handler
    pop  {v1}
    @ store SjLj handler in TLS
    str  v1, [v2, #72]

    @ restore V1-V8,LR
    pop {a4,v1-v8,lr}

    @ return and pass data directly
    mov  a1, a3

    @ restore PC
    pop {pc}
