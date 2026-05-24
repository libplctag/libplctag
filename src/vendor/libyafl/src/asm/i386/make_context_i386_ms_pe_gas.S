/*
            Copyright Oliver Kowalke 2009.
            Copyright Thomas Sailer 2013.
            Copyright Kyle Hayes 2026.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE or copy at
            http://www.boost.org/LICENSE_1_0.txt)

   Changes from Boost.Context make_i386_ms_pe_gas.asm:
   - Function renamed from make_fcontext to _yafl_make_context
   - EBX (fn) slot moved to 0x24, EBP (finish) to 0x28, EIP to 0x2c
     to match the NT_TIB-extended frame used by switch_i386_ms_pe_gas.S
   - No to/data slots; data is passed through EAX by yafl_switch directly
   - Trampoline puts data (EAX) on stack as the single context-function argument
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
*  |   ESI   |   EBX   |   EBP   |   EIP   |  (data) |         |  EH NXT |SEH HNDLR| *
*  --------------------------------------------------------------------------------- *
**************************************************************************************/

.file	"make_context_i386_ms_pe_gas.S"
.text
.p2align 4,,15

/* mark as using no unregistered SEH handlers */
.globl	@feat.00
.def	@feat.00;	.scl	3;	.type	0;	.endef
.set    @feat.00,   1

.globl	_yafl_make_context
.def	_yafl_make_context;	.scl	2;	.type	32;	.endef
_yafl_make_context:
    /* first arg of yafl_make_context() == top of context-stack */
    movl  0x04(%esp), %eax

    /* reserve space for first argument of context-function */
    /* EAX might already point to a 16byte border */
    leal  -0x8(%eax), %eax

    /* shift address in EAX to lower 16 byte boundary */
    andl  $-16, %eax

    /* reserve space for context-data on context-stack */
    /* on context-function entry: (ESP -0x4) % 8 == 0 */
    /* additional space is required for SEH entries at 0x38/0x3c */
    leal  -0x40(%eax), %eax

    /* save MMX control- and status-word */
    stmxcsr  (%eax)
    /* save x87 control-word */
    fnstcw  0x4(%eax)

    /* first arg of yafl_make_context() == top of context-stack */
    movl  0x4(%esp), %ecx
    /* save top address of context stack as 'base' */
    movl  %ecx, 0x14(%eax)
    /* second arg of yafl_make_context() == size of context-stack */
    movl  0x8(%esp), %edx
    /* negate stack size for LEA instruction (== subtraction) */
    negl  %edx
    /* compute bottom address of context stack (limit) */
    leal  (%ecx,%edx), %ecx
    /* save bottom address of context-stack as 'limit' */
    movl  %ecx, 0x10(%eax)
    /* save bottom address of context-stack as 'deallocation stack' */
    movl  %ecx, 0xc(%eax)
    /* set fiber-storage to zero */
    xorl  %ecx, %ecx
    movl  %ecx, 0x8(%eax)

    /* third arg of yafl_make_context() == address of context-function */
    /* stored in EBX slot */
    movl  0xc(%esp), %ecx
    movl  %ecx, 0x24(%eax)

    /* store address of trampoline as EIP */
    /* will be entered after calling yafl_switch() first time */
    movl  $trampoline, %ecx
    movl  %ecx, 0x2c(%eax)

    /* store address of finish as EBP */
    /* will be entered after context-function returns */
    movl  $finish, %ecx
    movl  %ecx, 0x28(%eax)

    /* traverse current SEH chain to get the last exception handler installed by Windows */
    /* required for SEHOP (SEH Overwrite Protection) on Windows Server 2008+ */
    movl  %fs:(0x0), %ecx

walk:
    movl  (%ecx), %edx
    incl  %edx
    jz  found
    decl  %edx
    xchgl  %ecx, %edx
    jmp  walk

found:
    /* load handler from last SEH node */
    movl  0x04(%ecx), %ecx
    /* save as SEH handler for this context */
    movl  %ecx, 0x3c(%eax)
    /* set next to -1 (end of chain) */
    movl  $0xffffffff, %ecx
    movl  %ecx, 0x38(%eax)
    /* store address of SEH chain node as fc_seh */
    leal  0x38(%eax), %ecx
    movl  %ecx, 0x18(%eax)

    ret /* return pointer to context-data */

trampoline:
    /* EAX holds data passed by yafl_switch */
    movl  %eax, (%esp)    /* store as first argument of context-function */
    pushl %ebp             /* push finish as return-address */
    jmp  *%ebx            /* jump to context-function */

finish:
    xorl  %eax, %eax
    movl  %eax, (%esp)
    call  __exit
    hlt

.def	__exit;	.scl	2;	.type	32;	.endef

.section .drectve
.ascii " -export:\"yafl_make_context\""
