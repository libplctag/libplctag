/*
       Copyright Claudio Jeker 2024
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*
 * typedef void*	fcontext_t;
 *
 * void *yafl_switch(fcontext_t *save, fcontext_t target, void *data);
 */
#define CC64FSZ 176
#define BIAS 2047
#define SP 128
#define I7 136

.file "jump_sparc64_sysv_elf_gas.S"
.text
.align  4
.global yafl_switch
.type   yafl_switch, %function
yafl_switch:
	# prepare stack
	save	%sp, -CC64FSZ, %sp

	# store framepointer and return address in slots reserved
	# for arguments
	stx %fp, [%sp + BIAS + SP]
	stx %i7, [%sp + BIAS + I7]
	mov %sp, %l0
	# force flush register windows to stack and with that save context
	flushw
	# save current context through the first argument pointer
	stx %l0, [%i0]
	# get SP (pointing to new context-data) from %i1 param
	mov %i1, %sp
	# load framepointer and return address from context
	ldx [%sp + BIAS + SP], %fp
	ldx [%sp + BIAS + I7], %i7

	ret
	 restore %i2, %g0, %o0
.size	yafl_switch,.-yafl_switch
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
