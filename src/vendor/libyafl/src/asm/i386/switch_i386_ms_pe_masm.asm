
;           Copyright Oliver Kowalke 2009.
;           Copyright Kyle Hayes 2026.
;  Distributed under the Boost Software License, Version 1.0.
;     (See accompanying file LICENSE or copy at
;           http://www.boost.org/LICENSE_1_0.txt)
;
;  Changes from Boost.Context jump_i386_ms_pe_masm.asm:
;  - Function renamed from jump_fcontext to yafl_switch
;    (.model flat, c prepends the required underscore for the linker)
;  - Signature changed from jump_fcontext(to, data) to
;    yafl_switch(from*, to, data) — three __cdecl arguments:
;      arg1 [esp+030h]: from*  pointer where current context is stored
;      arg2 [esp+034h]: to     target context to switch to
;      arg3 [esp+038h]: data   value passed to the resumed context
;  - data (arg3) is saved in EDX before the stack switch and returned
;    in EAX after; Boost read data from the old frame after switching,
;    but yafl carries it in a register to avoid the extra indirection
;  - NT_TIB save uses EDX for the pointer; NT_TIB restore uses EAX
;    (EDX must be free to hold data through the switch)

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
;  |   ESI   |   EBX   |   EBP   |   EIP   |  from*  |    to   |   data  |         |
;  ---------------------------------------------------------------------------------

.386
.XMM
.model flat, c
.code

yafl_switch PROC
    ; prepare stack: grow frame by 02ch (44 bytes)
    lea  esp, [esp-02ch]

IFNDEF BOOST_USE_TSX
    ; save MMX control- and status-word
    stmxcsr  [esp]
    ; save x87 control-word
    fnstcw  [esp+04h]
ENDIF

    assume  fs:nothing
    ; load NT_TIB
    mov  edx, fs:[018h]
    assume  fs:error
    ; save fiber local storage
    mov  eax, [edx+010h]
    mov  [esp+08h], eax
    ; save current deallocation stack
    mov  eax, [edx+0e0ch]
    mov  [esp+0ch], eax
    ; save current stack limit
    mov  eax, [edx+08h]
    mov  [esp+010h], eax
    ; save current stack base
    mov  eax, [edx+04h]
    mov  [esp+014h], eax
    ; save current SEH exception list
    mov  eax, [edx]
    mov  [esp+018h], eax

    mov  [esp+01ch], edi  ; save EDI
    mov  [esp+020h], esi  ; save ESI
    mov  [esp+024h], ebx  ; save EBX
    mov  [esp+028h], ebp  ; save EBP

    ; save current context pointer into *from (arg1)
    mov  eax, esp              ; eax = current context
    mov  edx, [esp+038h]      ; edx = data (arg3, carried through switch)
    mov  ecx, [esp+030h]      ; ecx = from* (arg1)
    mov  [ecx], eax            ; *from = current context

    ; switch to target context (arg2)
    mov  eax, [esp+034h]      ; eax = to (arg2)
    mov  esp, eax              ; switch stack; edx still holds data

IFNDEF BOOST_USE_TSX
    ; restore MMX control- and status-word
    ldmxcsr  [esp]
    ; restore x87 control-word
    fldcw  [esp+04h]
ENDIF

    assume  fs:nothing
    ; restore NT_TIB; use eax (edx must not be touched -- it holds data)
    mov  eax, fs:[018h]
    assume  fs:error
    ; restore fiber local storage
    mov  ecx, [esp+08h]
    mov  [eax+010h], ecx
    ; restore current deallocation stack
    mov  ecx, [esp+0ch]
    mov  [eax+0e0ch], ecx
    ; restore current stack limit
    mov  ecx, [esp+010h]
    mov  [eax+08h], ecx
    ; restore current stack base
    mov  ecx, [esp+014h]
    mov  [eax+04h], ecx
    ; restore current SEH exception list
    mov  ecx, [esp+018h]
    mov  [eax], ecx

    mov  ecx, [esp+02ch]  ; restore EIP

    mov  edi, [esp+01ch]  ; restore EDI
    mov  esi, [esp+020h]  ; restore ESI
    mov  ebx, [esp+024h]  ; restore EBX
    mov  ebp, [esp+028h]  ; restore EBP

    ; pop frame (02ch) + EIP slot (04h)
    lea  esp, [esp+030h]

    ; return data in EAX
    mov  eax, edx

    ; jump to restored context
    jmp  ecx
yafl_switch ENDP
END
