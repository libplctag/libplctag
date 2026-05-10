
;           Copyright Oliver Kowalke 2009.
;           Copyright Kyle Hayes 2026.
;  Distributed under the Boost Software License, Version 1.0.
;     (See accompanying file LICENSE or copy at
;           http://www.boost.org/LICENSE_1_0.txt)
;
;  Changes from Boost.Context make_i386_ms_pe_masm.asm:
;  - Function renamed from make_fcontext to yafl_make_context
;    (.model flat, c prepends the required underscore for the linker)
;  - EBX (fn) slot at 024h, EBP (finish) at 028h, EIP at 02ch
;    to match the NT_TIB-extended frame used by switch_i386_ms_pe_masm.asm
;  - No to/data slots; data is passed through EAX by yafl_switch directly
;  - Trampoline puts data (EAX) on stack as the single context-function argument

;  ---------------------------------------------------------------------------------
;  |    0    |    1    |    2    |    3    |    4    |    5    |    6    |    7    |
;  ---------------------------------------------------------------------------------
;  |    0h   |   04h   |   08h   |   0ch   |   010h  |   014h  |   018h  |   01ch  |
;  ---------------------------------------------------------------------------------
;  | fc_mxcsr|fc_x87_cw| fc_strg |fc_deallo|  limit  |   base  |  fc_seh |   EDI   |
;  ---------------------------------------------------------------------------------
;  ---------------------------------------------------------------------------------
;  |    8    |    9    |   10    |    11   |    12   |    13   |    14   |    15   |
;  ---------------------------------------------------------------------------------
;  |   020h  |  024h   |  028h   |   02ch  |   030h  |   034h  |   038h  |   03ch  |
;  ---------------------------------------------------------------------------------
;  |   ESI   |   EBX   |   EBP   |   EIP   |  (data) |         |  EH NXT |SEH HNDLR|
;  ---------------------------------------------------------------------------------

.386
.XMM
.model flat, c
; standard C library function (_exit in C becomes __exit in the object)
_exit PROTO, value:SDWORD
.code

yafl_make_context PROC
    ; first arg of yafl_make_context() == top of context-stack
    mov  eax, [esp+04h]

    ; reserve space for first argument of context-function
    ; EAX might already point to a 16byte border
    lea  eax, [eax-08h]

    ; shift address in EAX to lower 16 byte boundary
    and  eax, -16

    ; reserve space for context-data on context-stack
    ; on context-function entry: (ESP -0x4) % 8 == 0
    ; additional space is required for SEH entries at 038h/03ch
    lea  eax, [eax-040h]

    ; save MMX control- and status-word
    stmxcsr  [eax]
    ; save x87 control-word
    fnstcw  [eax+04h]

    ; first arg of yafl_make_context() == top of context-stack
    mov  ecx, [esp+04h]
    ; save top address of context stack as 'base'
    mov  [eax+014h], ecx
    ; second arg of yafl_make_context() == size of context-stack
    mov  edx, [esp+08h]
    ; negate stack size for LEA instruction (== subtraction)
    neg  edx
    ; compute bottom address of context stack (limit)
    lea  ecx, [ecx+edx]
    ; save bottom address of context-stack as 'limit'
    mov  [eax+010h], ecx
    ; save bottom address of context-stack as 'deallocation stack'
    mov  [eax+0ch], ecx
    ; set fiber-storage to zero
    xor  ecx, ecx
    mov  [eax+08h], ecx

    ; third arg of yafl_make_context() == address of context-function
    ; stored in EBX slot
    mov  ecx, [esp+0ch]
    mov  [eax+024h], ecx

    ; store address of trampoline as EIP
    ; will be entered after calling yafl_switch() first time
    mov  ecx, trampoline
    mov  [eax+02ch], ecx

    ; store address of finish as EBP
    ; will be entered after context-function returns
    mov  ecx, finish
    mov  [eax+028h], ecx

    ; traverse current SEH chain to get the last exception handler installed by Windows
    ; required for SEHOP (SEH Overwrite Protection) on Windows Server 2008+
    assume  fs:nothing
    mov  ecx, fs:[0h]
    assume  fs:error

walk:
    mov  edx, [ecx]
    inc  edx
    jz  found
    dec  edx
    xchg  edx, ecx
    jmp  walk

found:
    ; load handler from last SEH node
    mov  ecx, [ecx+04h]
    ; save as SEH handler for this context
    mov  [eax+03ch], ecx
    ; set next to -1 (end of chain)
    mov  ecx, 0ffffffffh
    mov  [eax+038h], ecx
    ; store address of SEH chain node as fc_seh
    lea  ecx, [eax+038h]
    mov  [eax+018h], ecx

    ret ; return pointer to context-data

trampoline:
    ; EAX holds data passed by yafl_switch
    mov  [esp], eax   ; store as first argument of context-function
    push ebp          ; push finish as return-address
    jmp  ebx          ; jump to context-function

finish:
    xor  eax, eax
    mov  [esp], eax
    call  _exit
    hlt
yafl_make_context ENDP
END
