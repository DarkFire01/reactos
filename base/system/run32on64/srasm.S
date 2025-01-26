.code

PUBLIC __readfsdword
__readfsdword:
    mov eax, fs:[rcx]
    ret
   
PUBLIC Enter32
Enter32:
    // Push KGDT64_R3_CODE and exchange it with the return address
    sub rsp, 8
    mov rax, [rsp + 8]
    mov [rsp], rax
    mov rax, 0033h
    mov [rsp + 8], rax
    
    // Hardcoded hacks for ntdll32!LdrInitializeThunk
    push 0
    push rdx
    
    // Push the return address for the 32 bit procedure
    mov rax, retpoint
    push rax
    // KGDT64_R3_CMCODE
    push 0023h
    // Address of the 32 bit procedure to be called
    push rcx
    
    xor eax, eax
    
    // REX.W prefix - the default size for far returns is 32 bit
    db 48h
    retf
retpoint:
    push 00401010h
    mov eax, 7C656FE0h
    
    db 0ffh
    db 0d0h

PUBLIC SetupFs
SetupFs:
    mov fs, rcx
    ret
    
END