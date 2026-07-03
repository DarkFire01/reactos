/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX signal system calls and signal delivery
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

#define PSX_ESRCH    3
#define PSX_EINTR    4
#define PSX_EINVAL   22

#define PSX_SIGCONT_NUM  PSX_SIGCONT
#define PSX_SIGMAX       19

static
BOOLEAN
PsxSigIgnoredByDefault(
    _In_ ULONG Sig)
{
    return (Sig == PSX_SIGCHLD) || (Sig == PSX_SIGCONT);
}

static
BOOLEAN
PsxSigStopByDefault(
    _In_ ULONG Sig)
{
    /* SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU */
    return (Sig == PSX_SIGSTOP) || (Sig == 17) || (Sig == 18) || (Sig == 19);
}

/**
 * @brief Resets signal state to the defaults: all SIG_DFL, nothing pending or blocked.
 */
VOID
PsxInitSignalState(
    _Inout_ PPSX_PROCESS Process)
{
    ULONG Index;

    Process->PendingSignals = 0;
    Process->BlockedSignals = 0;
    for (Index = 0; Index < PSX_NSIG; Index++)
    {
        Process->SigActions[Index].Handler = PSX_SIG_DFL;
        Process->SigActions[Index].Mask = 0;
        Process->SigActions[Index].Flags = 0;
    }
}

/**
 * @brief Posts a signal to Target and acts on it. The caller holds g_PsxProcessLock.
 */
VOID
PsxDeliverSignal(
    _Inout_ PPSX_PROCESS Target,
    _In_ ULONG Sig)
{
    ULONG Bit;
    ULONG Handler;
    HANDLE Thread = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;

    if ((Sig < 1) || (Sig > PSX_SIGMAX) || (Target->State == PSX_STATE_ZOMBIE))
        return;

    Bit = PSX_SIGBIT(Sig);
    Handler = Target->SigActions[Sig].Handler;

    /* SIGKILL and SIGSTOP can be neither ignored nor blocked */
    if ((Handler == PSX_SIG_IGN) && (Sig != PSX_SIGKILL) && (Sig != PSX_SIGSTOP))
        return;

    Target->PendingSignals |= Bit;

    /* A blocked signal stays pending */
    if ((Target->BlockedSignals & Bit) && (Sig != PSX_SIGKILL) && (Sig != PSX_SIGSTOP))
        return;

    if ((Handler == PSX_SIG_DFL) || (Handler == 0) || (Sig == PSX_SIGKILL))
    {
        Target->PendingSignals &= ~Bit;

        if ((Sig != PSX_SIGKILL) && PsxSigIgnoredByDefault(Sig))
        {
            /* TODO: Resume the target's threads */
            if ((Sig == PSX_SIGCONT) && (Target->State == PSX_STATE_STOPPED))
                Target->State = PSX_STATE_RUNNING;
            return;
        }

        if ((Sig != PSX_SIGKILL) && PsxSigStopByDefault(Sig))
        {
            /* TODO: Suspend the target's threads */
            Target->State = PSX_STATE_STOPPED;
            return;
        }

        /* Default action is to terminate, reported as WIFSIGNALED */
        Target->State = PSX_STATE_ZOMBIE;
        Target->ExitStatus = (LONG)(Sig & 0x7F);
        PsxCloseAllFds(Target);
        if (Target->ProcessHandle != NULL)
            NtTerminateProcess(Target->ProcessHandle, (NTSTATUS)Sig);
        return;
    }

    /*
     * Run the handler in a running target through psxdll's trampoline.
     * TODO: Interrupt a target blocked in a system call with EINTR.
     */
    if ((Target->State == PSX_STATE_RUNNING) &&
        (Target->SignalTrampoline != 0) &&
        (Target->ProcessHandle != NULL))
    {
        InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
        if (NT_SUCCESS(NtOpenThread(&Thread, THREAD_ALL_ACCESS, &ObjectAttributes, &Target->ClientId)))
        {
            RtlRemoteCall(Target->ProcessHandle,
                          Thread,
                          (PVOID)(ULONG_PTR)Target->SignalTrampoline,
                          0,
                          NULL,
                          TRUE,
                          FALSE);
            NtClose(Thread);
        }
    }
}

/**
 * @brief kill() (ApiNumber 0x04). Body layout: +0x30 pid, +0x34 signal.
 */
VOID
PsxSrvKill(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    LONG WantPid = (LONG)Args[0];
    ULONG Sig = Args[1];
    ULONG TargetGroup = (WantPid < -1) ? (ULONG)(-WantPid) : 0;
    PLIST_ENTRY Entry;
    PPSX_PROCESS Target;
    BOOLEAN Found = FALSE;
    BOOLEAN Match;

    PSXTRACE("kill: caller pid %lu, WantPid %ld sig %lu\n",
             (Process != NULL) ? Process->Pid : 0,
             WantPid,
             Sig);

    if (Sig > PSX_SIGMAX)
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        Target = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);

        if (Target->State == PSX_STATE_ZOMBIE)
            continue;

        if (WantPid > 0)
            Match = (Target->Pid == (ULONG)WantPid);
        else if (WantPid == 0)
            Match = (Target->ProcessGroup == Process->ProcessGroup);
        else if (WantPid == -1)
            Match = TRUE;
        else
            Match = (Target->ProcessGroup == TargetGroup);

        if (!Match)
            continue;

        /* Signal 0 only checks for existence */
        Found = TRUE;
        if (Sig != 0)
            PsxDeliverSignal(Target, Sig);
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);

    if (!Found)
    {
        Message->Errno = PSX_ESRCH;
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief sigaction() (ApiNumber 0x05).
 *
 * Body layout: +0x30 signal, +0x34 act present, +0x38 handler, +0x3C mask,
 * +0x40 flags. The old action is returned at +0x48, +0x4C and +0x50.
 */
VOID
PsxSrvSigaction(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG Sig = Args[0];
    ULONG HasAct = Args[1];
    ULONG NewHandler;

    if ((Sig < 1) || (Sig > PSX_SIGMAX))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    if (HasAct != 0)
    {
        NewHandler = Args[2];

        /* SIGKILL and SIGSTOP must keep the default disposition */
        if (((Sig == PSX_SIGKILL) || (Sig == PSX_SIGSTOP)) && (NewHandler != PSX_SIG_DFL))
        {
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
        }

        Args[6] = Process->SigActions[Sig].Handler;
        Args[7] = Process->SigActions[Sig].Mask;
        Args[8] = Process->SigActions[Sig].Flags;

        Process->SigActions[Sig].Handler = NewHandler;
        Process->SigActions[Sig].Mask = Args[3] & 0x7FFFF;
        Process->SigActions[Sig].Flags = Args[4];

        if ((NewHandler == PSX_SIG_IGN) ||
            ((NewHandler == PSX_SIG_DFL) && PsxSigIgnoredByDefault(Sig)))
        {
            Process->PendingSignals &= ~PSX_SIGBIT(Sig);
        }
    }
    else
    {
        Args[6] = Process->SigActions[Sig].Handler;
        Args[7] = Process->SigActions[Sig].Mask;
        Args[8] = Process->SigActions[Sig].Flags;
    }

    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief sigprocmask() (ApiNumber 0x06).
 *
 * Body layout: +0x30 how, +0x38 set. The old mask is returned at +0x38.
 */
VOID
PsxSrvSigprocmask(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG How = Args[0];
    ULONG Set = Args[2];
    ULONG Old = Process->BlockedSignals;

    switch (How)
    {
        /* SIG_BLOCK */
        case 1:
            Process->BlockedSignals |= Set;
            break;

        /* SIG_UNBLOCK */
        case 2:
            Process->BlockedSignals &= ~Set;
            break;

        /* SIG_SETMASK */
        case 3:
            Process->BlockedSignals = Set;
            break;

        default:
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
    }

    /* SIGKILL and SIGSTOP can never be blocked; keep only valid signal bits */
    Process->BlockedSignals &= ~(PSX_SIGBIT(PSX_SIGKILL) | PSX_SIGBIT(PSX_SIGSTOP));
    Process->BlockedSignals &= 0x7FFFF;

    /* TODO: Deliver signals that just became unblocked */
    Args[2] = Old;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief sigpending() (ApiNumber 0x07). The pending set is returned at +0x34.
 */
VOID
PsxSrvSigpending(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;

    Args[1] = Process->PendingSignals & 0x7FFFF;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief sigsuspend() (ApiNumber 0x08). Currently returns EINTR immediately.
 * TODO: Install the mask and block until a signal arrives.
 */
VOID
PsxSrvSigsuspend(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNREFERENCED_PARAMETER(Process);

    Message->Errno = PSX_EINTR;
    Message->ReturnValue = -1;
}

/**
 * @brief Delivers a controlling terminal signal to every running process in the
 * session. Codes: 0 = INTR, 1 = SUSP, 2 = HUP, 3 = QUIT.
 */
VOID
PsxSesDeliverTtySignal(
    _In_ ULONG SessionId,
    _In_ ULONG Code)
{
    PLIST_ENTRY Entry;
    PPSX_PROCESS Target;
    ULONG Sig;

    switch (Code)
    {
        /* SIGINT */
        case 0:
            Sig = 6;
            break;

        /* SIGTSTP */
        case 1:
            Sig = 17;
            break;

        /* SIGHUP */
        case 2:
            Sig = 4;
            break;

        /* SIGQUIT */
        case 3:
            Sig = 9;
            break;

        default:
            return;
    }

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        Target = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        if ((Target->SessionId == SessionId) && (Target->State == PSX_STATE_RUNNING))
            PsxDeliverSignal(Target, Sig);
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}
