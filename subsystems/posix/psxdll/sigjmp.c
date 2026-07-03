/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL signal-aware setjmp support. sigsetjmp() expands to
 *              setjmp(_sigjmp_store_mask(env, savemask)); we save the current
 *              signal mask into the sigjmp_buf tail (jmp_buf[16]=flag,[17]=mask,
 *              per MSTOOLS SETJMP.H sigjmp_buf[_JBLEN+2]) and siglongjmp restores
 *              it before jumping. The jump itself is done with NtContinue so no
 *              inline assembly is needed (portable across the gcc/MSVC builds).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

// _JUMP_BUFFER (x86): Ebp,Ebx,Edi,Esi,Esp,Eip at ULONG indices 0..5.
#define JB_EBP 0
#define JB_EBX 1
#define JB_EDI 2
#define JB_ESI 3
#define JB_ESP 4
#define JB_EIP 5
#define JB_MASK_FLAG 16     // sigjmp_buf tail: was the mask saved?
#define JB_MASK      17     // the saved signal mask

#define PSX_SIG_SETMASK 3

//
// _sigjmp_store_mask(env, savemask) -- ordinal 19. Stashes the signal mask (when
// savemask != 0) and returns env so setjmp can save the registers into it.
//
void * __cdecl
_sigjmp_store_mask(void *Env, int SaveMask)
{
    PULONG Jb = (PULONG)Env;

    Jb[JB_MASK_FLAG] = (ULONG)SaveMask;
    if (SaveMask)
    {
        ULONG Mask = 0;
        sigprocmask(0, NULL, &Mask);        // query the current blocked mask
        Jb[JB_MASK] = Mask;
    }
    return Env;
}

//
// siglongjmp(env, val) -- ordinal 93. Restore the saved mask, then longjmp.
//
void __cdecl
siglongjmp(void *Env, int Value)
{
    PULONG Jb = (PULONG)Env;
    CONTEXT Context;

    if (Jb[JB_MASK_FLAG])
    {
        ULONG Mask = Jb[JB_MASK];
        sigprocmask(PSX_SIG_SETMASK, &Mask, NULL);
    }

    RtlCaptureContext(&Context);            // start from a valid context (segs/eflags)
    Context.Ebp = Jb[JB_EBP];
    Context.Ebx = Jb[JB_EBX];
    Context.Edi = Jb[JB_EDI];
    Context.Esi = Jb[JB_ESI];
    Context.Esp = Jb[JB_ESP];
    Context.Eip = Jb[JB_EIP];
    Context.Eax = Value ? (ULONG)Value : 1; // longjmp(env, 0) must return 1
    NtContinue(&Context, FALSE);
}
