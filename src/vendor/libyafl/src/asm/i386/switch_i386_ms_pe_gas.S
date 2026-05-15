/*
            Copyright Oliver Kowalke 2009.
            Copyright Thomas Sailer 2013.
            Copyright Kyle Hayes 2026.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE or copy at
            http://www.boost.org/LICENSE_1_0.txt)

   Changes from Boost.Context jump_i386_ms_pe_gas.asm:
   - Function renamed from _jump_fcontext to _yafl_switch
   - Signature changed from jump_fcontext(to, data) to
     yafl_switch(from*, to, data) — three __cdecl arguments:
       arg1 [esp+0x30]: from*  pointer where current context is stored
       arg2 [esp+0x34]: to     target context to switch to
       arg3 [esp+0x38]: data   value passed to the resumed context
   - data (arg3) is saved in EDX before the stack switch and returned
     in EAX after; Boost read data from the old frame after switching,
     but yafl carries it in a register to avoid the extra indirection
   - NT_TIB save uses EDX for the pointer; NT_TIB restore uses EAX
     (EDX must be free to hold data through the switch)
*/

/*************************************************************************************
*  --------------------------------------------------------------------------------- *
*  |    0    |    1    |    2    |    3    |    4    |    5    |    6    |    7    | *
*  --------------------------------------------------------------------------------- *
*  |    0h   |   04h   |   08h   |   0ch   |   010h  |   014h  |   018h  |   01ch  | *
*  --------------------------------------------------------------------------------- *
*  | fc_mxcsr|fc_x87_cw| fc_strg |fc_deallo|  limit  |   base  |  fc_seh |   EDI   | *
*  --------------------------------------------------------------------------------- *
*  --------------------------------------------------------------------------------- *
*  |    8    |    9    |   10    |    11   |    12   |    13   |    14   |    15   | *
*  --------------------------------------------------------------------------------- *
*  |   020h  |  024h   |  028h   |   02ch  |   030h  |   034h  |   038h  |   03ch  | *
*  --------------------------------------------------------------------------------- *
*  |   ESI   |   EBX   |   EBP   |   EIP   |  from*  |    to   |   data  |         | *
*  --------------------------------------------------------------------------------- *
**************************************************************************************/

.file	"switch_i386_ms_pe_gas.S"
.text
.p2align 4,,15

/* mark as using no unregistered SEH handlers */
.globl	@feat.00
.def	@feat.00;	.scl	3;	.type	0;	.endef
.set    @feat.00,   1

.globl	_yafl_switch
.def	_yafl_switch;	.scl	2;	.type	32;	.endef
_yafl_switch:
    /* prepare stack: grow frame by 0x2c (44 bytes) */
    leal  -0x2c(%esp), %esp

#if !defined(BOOST_USE_TSX)
    /* save MMX control- and status-word */
    stmxcsr  (%esp)
    /* save x87 control-word */
    fnstcw  0x4(%esp)
#endif

    /* load NT_TIB via fs:0x18 */
    movl  %fs:(0x18), %edx
    /* save fiber local storage */
    movl  0x10(%edx), %eax
    movl  %eax, 0x8(%esp)
    /* save current deallocation stack */
    movl  0xe0c(%edx), %eax
    movl  %eax, 0xc(%esp)
    /* save current stack limit */
    movl  0x8(%edx), %eax
    movl  %eax, 0x10(%esp)
    /* save current stack base */
    movl  0x4(%edx), %eax
    movl  %eax, 0x14(%esp)
    /* save current SEH exception list */
    movl  (%edx), %eax
    movl  %eax, 0x18(%esp)

    movl  %edi, 0x1c(%esp)  /* save EDI */
    movl  %esi, 0x20(%esp)  /* save ESI */
    movl  %ebx, 0x24(%esp)  /* save EBX */
    movl  %ebp, 0x28(%esp)  /* save EBP */

    /* save current context pointer into *from (arg1) */
    movl  %esp, %eax          /* eax = current context */
    movl  0x38(%esp), %edx   /* edx = data (arg3, carried through switch) */
    movl  0x30(%esp), %ecx   /* ecx = from* (arg1) */
    movl  %eax, (%ecx)        /* *from = current context */

    /* switch to target context (arg2) */
    movl  0x34(%esp), %eax   /* eax = to (arg2) */
    movl  %eax, %esp          /* switch stack; edx still holds data */

#if !defined(BOOST_USE_TSX)
    /* restore MMX control- and status-word */
    ldmxcsr  (%esp)
    /* restore x87 control-word */
    fldcw  0x4(%esp)
#endif

    /* restore NT_TIB; use eax (edx must not be touched — it holds data) */
    movl  %fs:(0x18), %eax
    /* restore fiber local storage */
    movl  0x8(%esp), %ecx
    movl  %ecx, 0x10(%eax)
    /* restore current deallocation stack */
    movl  0xc(%esp), %ecx
    movl  %ecx, 0xe0c(%eax)
    /* restore current stack limit */
    movl  0x10(%esp), %ecx
    movl  %ecx, 0x8(%eax)
    /* restore current stack base */
    movl  0x14(%esp), %ecx
    movl  %ecx, 0x4(%eax)
    /* restore current SEH exception list */
    movl  0x18(%esp), %ecx
    movl  %ecx, (%eax)

    movl  0x2c(%esp), %ecx  /* restore EIP */

    movl  0x1c(%esp), %edi  /* restore EDI */
    movl  0x20(%esp), %esi  /* restore ESI */
    movl  0x24(%esp), %ebx  /* restore EBX */
    movl  0x28(%esp), %ebp  /* restore EBP */

    /* pop frame (0x2c) + EIP slot (0x4) */
    leal  0x30(%esp), %esp

    /* return data in EAX */
    movl  %edx, %eax

    /* jump to restored context */
    jmp  *%ecx

.section .drectve
.ascii " -export:\"yafl_switch\""
