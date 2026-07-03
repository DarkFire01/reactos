/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Signal mask aware setjmp support (sigsetjmp/siglongjmp)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/* x86 jump buffer ULONG indices; the signal mask is stored after the registers */
#define JB_EBP          0
#define JB_EBX          1
#define JB_EDI          2
#define JB_ESI          3
#define JB_ESP          4
#define JB_EIP          5
#define JB_MASK_FLAG    16
#define JB_MASK         17

#define PSX_SIG_SETMASK 3

/**
 * @brief Saves the current signal mask into the jump buffer when SaveMask is
 * nonzero. sigsetjmp() expands to setjmp(_sigjmp_store_mask(Env, SaveMask)).
 *
 * @return Env, for setjmp to store the registers into.
 */
void *
__cdecl
_sigjmp_store_mask(
    _Inout_ void *Env,
    _In_ int SaveMask)
{
    PULONG JumpBuffer = (PULONG)Env;

    JumpBuffer[JB_MASK_FLAG] = (ULONG)SaveMask;
    if (SaveMask)
    {
        ULONG Mask = 0;

        sigprocmask(0, NULL, &Mask);
        JumpBuffer[JB_MASK] = Mask;
    }

    return Env;
}

/**
 * @brief Restores the saved signal mask and resumes at the setjmp site with NtContinue.
 */
void
__cdecl
siglongjmp(
    _In_ void *Env,
    _In_ int Value)
{
    PULONG JumpBuffer = (PULONG)Env;
    CONTEXT Context;

    if (JumpBuffer[JB_MASK_FLAG])
    {
        ULONG Mask = JumpBuffer[JB_MASK];

        sigprocmask(PSX_SIG_SETMASK, &Mask, NULL);
    }

    /* Start from a valid context so segments and flags are sane */
    RtlCaptureContext(&Context);
    Context.Ebp = JumpBuffer[JB_EBP];
    Context.Ebx = JumpBuffer[JB_EBX];
    Context.Edi = JumpBuffer[JB_EDI];
    Context.Esi = JumpBuffer[JB_ESI];
    Context.Esp = JumpBuffer[JB_ESP];
    Context.Eip = JumpBuffer[JB_EIP];

    /* A zero value must make setjmp return 1 */
    Context.Eax = Value ? (ULONG)Value : 1;
    NtContinue(&Context, FALSE);
}
