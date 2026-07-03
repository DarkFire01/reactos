/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL signal syscalls. Signal-set manipulation is pure
 *              client-side bit-twiddling (sigset_t == unsigned long, bit
 *              1<<(sig-1)); delivery/disposition state lives in the server.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_SIGMASK_ALL 0x0007FFFF      // 19 signals (bits 0..18)

//
// Client-side sigset_t ops (no server round-trip).
//
int __cdecl sigemptyset(unsigned long *Set) { *Set = 0; return 0; }
int __cdecl sigfillset(unsigned long *Set)  { *Set = PSX_SIGMASK_ALL; return 0; }

int __cdecl
sigaddset(unsigned long *Set, int Signal)
{
    if (Signal < 1 || Signal > 19) { PsxSetErrno(22 /* EINVAL */); return -1; }
    *Set |= (1UL << (Signal - 1));
    return 0;
}

int __cdecl
sigdelset(unsigned long *Set, int Signal)
{
    if (Signal < 1 || Signal > 19) { PsxSetErrno(22 /* EINVAL */); return -1; }
    *Set &= ~(1UL << (Signal - 1));
    return 0;
}

int __cdecl
sigismember(const unsigned long *Set, int Signal)
{
    if (Signal < 1 || Signal > 19) { PsxSetErrno(22 /* EINVAL */); return -1; }
    return (int)((*Set >> (Signal - 1)) & 1);
}

//
// kill(pid, sig) -- ApiNumber 0x04.
//
int __cdecl
kill(int Pid, int Signal)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiKill, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;      // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Signal;   // +0x34
    return (int)PsxCallServer(&Message);
}

//
// alarm(seconds) -- ApiNumber 0x09. Returns the previously scheduled remainder.
//
unsigned int __cdecl
alarm(unsigned int Seconds)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiAlarm, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = Seconds;
    return (unsigned int)PsxCallServer(&Message);
}

//
// sigaction(sig, act, oldact) -- ApiNumber 0x05. struct sigaction is
// {handler, mask, flags}; old disposition is returned at Args[6..8].
//
int __cdecl
sigaction(int Signal, const void *Action, void *OldAction)
{
    PSX_API_MESSAGE Message;
    PULONG Args;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigaction, PSX_BODY_DATALEN(9 * sizeof(ULONG)));
    Args = (PULONG)Message.Data.Raw;
    Args[0] = (ULONG)Signal;                 // +0x30
    Args[1] = (Action != NULL) ? 1 : 0;      // +0x34 HasAct
    if (Action != NULL)
    {
        const ULONG *Act = (const ULONG *)Action;
        Args[2] = Act[0];                    // +0x38 sa_handler
        Args[3] = Act[1];                    // +0x3C sa_mask
        Args[4] = Act[2];                    // +0x40 sa_flags
    }

    Result = PsxCallServer(&Message);
    if (Result >= 0 && OldAction != NULL)
    {
        ULONG *Old = (ULONG *)OldAction;
        Old[0] = Args[6];                    // +0x48 old handler
        Old[1] = Args[7];                    // +0x4C old mask
        Old[2] = Args[8];                    // +0x50 old flags
    }
    return (int)Result;
}

//
// sigprocmask(how, set, oldset) -- ApiNumber 0x06. Old mask returns at Args[1].
//
int __cdecl
sigprocmask(int How, const unsigned long *Set, unsigned long *OldSet)
{
    PSX_API_MESSAGE Message;
    PULONG Args;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigprocmask, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    Args = (PULONG)Message.Data.Raw;
    if (Set != NULL)
    {
        Args[0] = (ULONG)How;               // +0x30
        Args[2] = *Set;                     // +0x38
    }
    else
    {
        Args[0] = 1;                        // SIG_BLOCK an empty set == query only
        Args[2] = 0;
    }

    Result = PsxCallServer(&Message);
    if (Result >= 0 && OldSet != NULL)
        *OldSet = Args[1];                  // +0x34 old mask
    return (int)Result;
}

//
// sigpending(set) -- ApiNumber 0x07. Pending mask returns at Args[1].
//
int __cdecl
sigpending(unsigned long *Set)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigpending, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    Result = PsxCallServer(&Message);
    if (Result >= 0 && Set != NULL)
        *Set = ((PULONG)Message.Data.Raw)[1];   // +0x34
    return (int)Result;
}

//
// sigsuspend(mask) / pause() -- ApiNumber 0x08. Both block until a signal and
// return -1 with errno == EINTR.
//
int __cdecl
sigsuspend(const unsigned long *Mask)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSigsuspend, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (Mask != NULL) ? *Mask : 0;
    return (int)PsxCallServer(&Message);
}

int __cdecl
pause(void)
{
    unsigned long Empty = 0;
    return sigsuspend(&Empty);
}

//
// raise(sig) -- self-directed kill.
//
int __cdecl raise(int Signal) { return kill(getpid(), Signal); }

//
// signal(sig, handler) -- the historical wrapper over sigaction. Returns the
// previous handler, or SIG_ERR ((void*)-1 cast) on failure.
//
void (*__cdecl signal(int Signal, void (*Handler)(int)))(int)
{
    ULONG NewAct[3];
    ULONG OldAct[3];

    NewAct[0] = (ULONG)(ULONG_PTR)Handler;      // sa_handler
    NewAct[1] = 0;                              // sa_mask
    NewAct[2] = 0;                              // sa_flags
    OldAct[0] = 0;

    if (sigaction(Signal, NewAct, OldAct) < 0)
        return (void (*)(int))(ULONG_PTR)-1;    // SIG_ERR
    return (void (*)(int))(ULONG_PTR)OldAct[0];
}

//
// sleep(seconds) -- served locally via the NT delay timer (no signal races
// with an SIGALRM-based implementation). Returns 0 (never interrupted here).
//
unsigned int __cdecl
sleep(unsigned int Seconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -((LONGLONG)Seconds * 10000000);    // relative, 100ns
    NtDelayExecution(FALSE, &Interval);
    return 0;
}
