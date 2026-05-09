/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/****************************************************************************************
 *                                                                                      *
 *  ----------------------------------------------------------------------------------  *
 *  |    0    |    1    |    2    |    3    |    4     |    5    |    6    |    7    |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x0   |   0x4   |   0x8   |   0xc   |   0x10   |   0x14  |   0x18  |   0x1c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  | fc_mxcsr|fc_x87_cw|   EDI   |   ESI   |   EBX    |   EBP   |   EIP   |    to   |  *
 *  ----------------------------------------------------------------------------------  *
 *  ----------------------------------------------------------------------------------  *
 *  |    8    |    9    |    10   |    11   |    12    |    13   |    14   |    15   |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x20  |                                                                      |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   data  |                                                                      |  *
 *  ----------------------------------------------------------------------------------  *
 *                                                                                      *
 ****************************************************************************************/

.text
.private_extern _yafl_switch
.globl _yafl_switch
.align 2
_yafl_switch:
    leal  -0x18(%esp), %esp  /* prepare stack */

#if !defined(BOOST_USE_TSX)
    stmxcsr  (%esp)     /* save MMX control- and status-word */
    fnstcw   0x4(%esp)  /* save x87 control-word */
#endif

    movl  %edi, 0x8(%esp)  /* save EDI */
    movl  %esi, 0xc(%esp)  /* save ESI */
    movl  %ebx, 0x10(%esp)  /* save EBX */
    movl  %ebp, 0x14(%esp)  /* save EBP */

    /* save current context through the first argument pointer */
    movl  %esp, %ecx
    movl  0x1c(%esp), %eax
    movl  %ecx, (%eax)

    /* target context and switch data */
    movl  0x20(%esp), %eax
    movl  0x24(%esp), %edx

    /* restore ESP (pointing to context-data) from target */
    movl  %eax, %esp

    movl  0x18(%esp), %ecx  /* restore EIP */

#if !defined(BOOST_USE_TSX)
    ldmxcsr  (%esp)     /* restore MMX control- and status-word */
    fldcw    0x4(%esp)  /* restore x87 control-word */
#endif

    movl  0x8(%esp), %edi  /* restore EDI */
    movl  0xc(%esp), %esi  /* restore ESI */
    movl  0x10(%esp), %ebx  /* restore EBX */
    movl  0x14(%esp), %ebp  /* restore EBP */

    leal  0x1c(%esp), %esp  /* prepare stack */

    /* return and pass data directly */
    movl  %edx, %eax

    /* jump to context */
    jmp *%ecx
