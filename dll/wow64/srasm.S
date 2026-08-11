#include <asm.inc>

.code

PUBLIC __readfsdword
__readfsdword:
    mov eax, fs:[rcx]
    ret
   
PUBLIC EnterApc32
EnterApc32:
    push r12
    mov r12, rsp
    
    sub rsp, rdx
    
    push rdi
    push rsi
    
    mov rsi, rcx
    mov rcx, rdx
    lea rdi, [rsp + 16]
    cld
    rep movsb
    
    pop rsi
    pop rdi
    
    lea rcx, [rip + return32]
    mov [rsp + 16], rcx
    
    .byte HEX(48) /* REX.W */
    .byte HEX(CB) /* retf */
return32:
    call l
l:
    pop rax
    add eax, return64 - l
    push HEX(33)
    push rax
    .byte HEX(CB) /* retf */
return64:
    mov rsp, r12
    pop r12
    ret

PUBLIC SetupFs
SetupFs:
    mov fs, rcx
    ret
    
PUBLIC FarReturn32
FarReturn32:
    .byte HEX(CB) /* retf */

PUBLIC FarReturn64
FarReturn64:
    .byte HEX(48) /* REX.W */
    .byte HEX(CB) /* retf */

PUBLIC Enter32
Enter32:
    .byte HEX(48) /* mov rsp, rcx */
    .byte HEX(89)
    .byte HEX(CC)
    .byte HEX(48) /* REX.W */
    .byte HEX(CB) /* retf*/

END