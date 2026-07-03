/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Signal system calls and sigset_t helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/* sigset_t is an unsigned long with bit (Signal - 1) per signal */
#define PSX_SIGNAL_COUNT    19
#define PSX_SIGMASK_ALL     0x0007FFFF
#define PSX_SIG_BLOCK       1
#define PSX_EINVAL          22

typedef void (*PSX_SIGNAL_HANDLER)(int);

int
__cdecl
sigemptyset(
    _Out_ unsigned long *Set)
{
    *Set = 0;
    return 0;
}

int
__cdecl
sigfillset(
    _Out_ unsigned long *Set)
{
    *Set = PSX_SIGMASK_ALL;
    return 0;
}

int
__cdecl
sigaddset(
    _Inout_ unsigned long *Set,
    _In_ int Signal)
{
    if (Signal < 1 || Signal > PSX_SIGNAL_COUNT)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    *Set |= (1UL << (Signal - 1));
    return 0;
}

int
__cdecl
sigdelset(
    _Inout_ unsigned long *Set,
    _In_ int Signal)
{
    if (Signal < 1 || Signal > PSX_SIGNAL_COUNT)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    *Set &= ~(1UL << (Signal - 1));
    return 0;
}

int
__cdecl
sigismember(
    _In_ const unsigned long *Set,
    _In_ int Signal)
{
    if (Signal < 1 || Signal > PSX_SIGNAL_COUNT)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    return (int)((*Set >> (Signal - 1)) & 1);
}

int
__cdecl
kill(
    _In_ int Pid,
    _In_ int Signal)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiKill, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Signal;
    return (int)PsxCallServer(&Message);
}

/**
 * @brief Schedules SIGALRM.
 *
 * @return Seconds remaining on the previous alarm.
 */
unsigned int
__cdecl
alarm(
    _In_ unsigned int Seconds)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiAlarm, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = Seconds;
    return (unsigned int)PsxCallServer(&Message);
}

/**
 * @brief struct sigaction is {handler, mask, flags}. The new action goes in
 * argument slots 2 to 4 and the old one comes back in slots 6 to 8.
 */
int
__cdecl
sigaction(
    _In_ int Signal,
    _In_opt_ const void *Action,
    _Out_opt_ void *OldAction)
{
    PSX_API_MESSAGE Message;
    PULONG Args;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigaction, PSX_BODY_DATALEN(9 * sizeof(ULONG)));
    Args = (PULONG)Message.Data.Raw;
    Args[0] = (ULONG)Signal;
    Args[1] = (Action != NULL) ? 1 : 0;
    if (Action != NULL)
    {
        const ULONG *NewAction = (const ULONG *)Action;

        Args[2] = NewAction[0];
        Args[3] = NewAction[1];
        Args[4] = NewAction[2];
    }

    Result = PsxCallServer(&Message);
    if (Result >= 0 && OldAction != NULL)
    {
        PULONG Old = (PULONG)OldAction;

        Old[0] = Args[6];
        Old[1] = Args[7];
        Old[2] = Args[8];
    }

    return (int)Result;
}

int
__cdecl
sigprocmask(
    _In_ int How,
    _In_opt_ const unsigned long *Set,
    _Out_opt_ unsigned long *OldSet)
{
    PSX_API_MESSAGE Message;
    PULONG Args;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigprocmask, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    Args = (PULONG)Message.Data.Raw;
    if (Set != NULL)
    {
        Args[0] = (ULONG)How;
        Args[2] = *Set;
    }
    else
    {
        /* Blocking an empty set only queries the current mask */
        Args[0] = PSX_SIG_BLOCK;
        Args[2] = 0;
    }

    Result = PsxCallServer(&Message);
    if (Result >= 0 && OldSet != NULL)
        *OldSet = Args[1];

    return (int)Result;
}

int
__cdecl
sigpending(
    _Out_opt_ unsigned long *Set)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiSigpending, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    Result = PsxCallServer(&Message);
    if (Result >= 0 && Set != NULL)
        *Set = ((PULONG)Message.Data.Raw)[1];

    return (int)Result;
}

/**
 * @brief Blocks until a signal arrives, then returns -1 with errno set to EINTR.
 */
int
__cdecl
sigsuspend(
    _In_opt_ const unsigned long *Mask)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSigsuspend, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (Mask != NULL) ? *Mask : 0;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
pause(void)
{
    unsigned long Empty = 0;

    return sigsuspend(&Empty);
}

int
__cdecl
raise(
    _In_ int Signal)
{
    return kill(getpid(), Signal);
}

/**
 * @brief Installs a handler through sigaction.
 *
 * @return The previous handler, or SIG_ERR on failure.
 */
PSX_SIGNAL_HANDLER
__cdecl
signal(
    _In_ int Signal,
    _In_opt_ PSX_SIGNAL_HANDLER Handler)
{
    ULONG NewAction[3];
    ULONG OldAction[3];

    NewAction[0] = (ULONG)(ULONG_PTR)Handler;
    NewAction[1] = 0;
    NewAction[2] = 0;
    OldAction[0] = 0;

    if (sigaction(Signal, NewAction, OldAction) < 0)
        return (PSX_SIGNAL_HANDLER)(ULONG_PTR)-1;

    return (PSX_SIGNAL_HANDLER)(ULONG_PTR)OldAction[0];
}

/**
 * @brief Sleeps on the NT delay timer instead of SIGALRM. Never interrupted.
 */
unsigned int
__cdecl
sleep(
    _In_ unsigned int Seconds)
{
    LARGE_INTEGER Interval;

    /* Relative, in 100ns units */
    Interval.QuadPart = -((LONGLONG)Seconds * 10000000);
    NtDelayExecution(FALSE, &Interval);
    return 0;
}
