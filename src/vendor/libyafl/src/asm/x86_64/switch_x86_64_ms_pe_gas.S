/*
            Copyright Oliver Kowalke 2009.
            Copyright Thomas Sailer 2013.
            Copyright Kyle Hayes 2026.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE or copy at
            http://www.boost.org/LICENSE_1_0.txt)
*/

.file	"jump_x86_64_ms_pe_gas.S"
.text
.p2align 4,,15
.globl	yafl_switch
.def	yafl_switch;	.scl	2;	.type	32;	.endef
.seh_proc	yafl_switch
yafl_switch:
.seh_endprologue

    leaq  -0x118(%rsp), %rsp /* prepare stack */

#if !defined(BOOST_USE_TSX)
    /* save XMM storage */
    movaps  %xmm6, 0x0(%rsp)
    movaps  %xmm7, 0x10(%rsp)
    movaps  %xmm8, 0x20(%rsp)
    movaps  %xmm9, 0x30(%rsp)
    movaps  %xmm10, 0x40(%rsp)
    movaps  %xmm11, 0x50(%rsp)
    movaps  %xmm12, 0x60(%rsp)
    movaps  %xmm13, 0x70(%rsp)
    movaps  %xmm14, 0x80(%rsp)
    movaps  %xmm15, 0x90(%rsp)
    stmxcsr  0xa0(%rsp)  /* save MMX control- and status-word */
    fnstcw   0xa4(%rsp)  /* save x87 control-word */
#endif

    /* load NT_TIB */
    movq  %gs:(0x30), %r10
    /* save fiber local storage */
    movq  0x20(%r10), %rax
    movq  %rax, 0xb0(%rsp)
    /* save current deallocation stack */
    movq  0x1478(%r10), %rax
    movq  %rax, 0xb8(%rsp)
    /* save current stack limit */
    movq  0x10(%r10), %rax
    movq  %rax, 0xc0(%rsp)
    /* save current stack base */
    movq  0x08(%r10), %rax
    movq  %rax, 0xc8(%rsp)

    movq  %r12, 0xd0(%rsp)  /* save R12 */
    movq  %r13, 0xd8(%rsp)  /* save R13 */
    movq  %r14, 0xe0(%rsp)  /* save R14 */
    movq  %r15, 0xe8(%rsp)  /* save R15 */
    movq  %rdi, 0xf0(%rsp)  /* save RDI */
    movq  %rsi, 0xf8(%rsp)  /* save RSI */
    movq  %rbx, 0x100(%rsp)  /* save RBX */
    movq  %rbp, 0x108(%rsp)  /* save RBP */

    /* save current context through the first argument pointer */
    movq  %r8, %r9
    movq  %rsp, %rax
    movq  %rax, (%rcx)

    /* restore target context from the second argument */
    movq  %rdx, %rsp

#if !defined(BOOST_USE_TSX)
    /* restore XMM storage */
    movaps  0x0(%rsp), %xmm6
    movaps  0x10(%rsp), %xmm7
    movaps  0x20(%rsp), %xmm8
    movaps  0x30(%rsp), %xmm9
    movaps  0x40(%rsp), %xmm10
    movaps  0x50(%rsp), %xmm11
    movaps  0x60(%rsp), %xmm12
    movaps  0x70(%rsp), %xmm13
    movaps  0x80(%rsp), %xmm14
    movaps  0x90(%rsp), %xmm15
    ldmxcsr 0xa0(%rsp)  /* restore MMX control- and status-word */
    fldcw   0xa4(%rsp)  /* restore x87 control-word */
#endif

    /* load NT_TIB */
    movq  %gs:(0x30), %r10
    /* restore fiber local storage */
    movq  0xb0(%rsp), %rax
    movq  %rax, 0x20(%r10)
    /* restore current deallocation stack */
    movq  0xb8(%rsp), %rax
    movq  %rax, 0x1478(%r10)
    /* restore current stack limit */
    movq  0xc0(%rsp), %rax
    movq  %rax, 0x10(%r10)
    /* restore current stack base */
    movq  0xc8(%rsp), %rax
    movq  %rax, 0x08(%r10)

    movq  0xd0(%rsp),  %r12  /* restore R12 */
    movq  0xd8(%rsp),  %r13  /* restore R13 */
    movq  0xe0(%rsp),  %r14  /* restore R14 */
    movq  0xe8(%rsp),  %r15  /* restore R15 */
    movq  0xf0(%rsp),  %rdi  /* restore RDI */
    movq  0xf8(%rsp),  %rsi  /* restore RSI */
    movq  0x100(%rsp), %rbx  /* restore RBX */
    movq  0x108(%rsp), %rbp  /* restore RBP */

    leaq  0x118(%rsp), %rsp /* prepare stack */

    /* restore return-address */
    popq  %r10

    /* return and pass data directly */
    movq  %r9, %rax
    movq  %r9, %rcx

    /* indirect jump to context */
    jmp  *%r10
.seh_endproc

.section .drectve
.ascii " -export:\"yafl_switch\""
