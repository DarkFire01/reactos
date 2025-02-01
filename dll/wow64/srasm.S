.code

PUBLIC __readfsdword
__readfsdword:
    mov eax, fs:[rcx]
    ret
   
PUBLIC Enter32
Enter32:
    push r8 
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
    ret

PUBLIC SetupFs
SetupFs:
    mov fs, rcx
    ret
    
END