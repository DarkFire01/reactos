/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX signals: kill / sigaction / sigprocmask / sigpending /
 *              sigsuspend, plus signal delivery. A SIG_DFL terminate signal kills
 *              the target; a custom handler on a running target is injected via
 *              RtlRemoteCall into psxdll's signal trampoline
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

#define PSX_ESRCH    3
#define PSX_EINTR    4
#define PSX_EINVAL   22

#define PSX_SIGCONT_NUM  PSX_SIGCONT
#define PSX_SIGMAX       19

static BOOLEAN
PsxSigIgnoredByDefault(IN ULONG Sig)
{
    return (Sig == PSX_SIGCHLD) || (Sig == PSX_SIGCONT);
}

static BOOLEAN
PsxSigStopByDefault(IN ULONG Sig)
{
    return (Sig == PSX_SIGSTOP) || (Sig == 17) || (Sig == 18) || (Sig == 19);   // STOP/TSTP/TTIN/TTOU
}

//
// Reset signal state to POSIX defaults (all SIG_DFL, nothing pending or blocked).
//
VOID
PsxInitSignalState(IN PPSX_PROCESS Process)
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

//
// Post a signal to a target process and act on it. Caller holds g_PsxProcessLock.
//
VOID
PsxDeliverSignal(IN PPSX_PROCESS Target, IN ULONG Sig)
{
    ULONG Bit;
    ULONG Handler;

    if ((Sig < 1) || (Sig > PSX_SIGMAX) || (Target->State == PSX_STATE_ZOMBIE))
        return;

    Bit = PSX_SIGBIT(Sig);
    Handler = Target->SigActions[Sig].Handler;

    // SIG_IGN -> drop (SIGKILL/SIGSTOP can be neither ignored nor blocked).
    if ((Handler == PSX_SIG_IGN) && (Sig != PSX_SIGKILL) && (Sig != PSX_SIGSTOP))
        return;

    Target->PendingSignals |= Bit;

    if ((Target->BlockedSignals & Bit) && (Sig != PSX_SIGKILL) && (Sig != PSX_SIGSTOP))
        return;     // blocked -> stays pending

    // SIG_DFL (or unset) disposition.
    if ((Handler == PSX_SIG_DFL) || (Handler == 0) || (Sig == PSX_SIGKILL))
    {
        Target->PendingSignals &= ~Bit;

        if ((Sig != PSX_SIGKILL) && PsxSigIgnoredByDefault(Sig))
        {
            if ((Sig == PSX_SIGCONT) && (Target->State == PSX_STATE_STOPPED))
                Target->State = PSX_STATE_RUNNING;      // TODO: resume threads
            return;
        }
        if ((Sig != PSX_SIGKILL) && PsxSigStopByDefault(Sig))
        {
            Target->State = PSX_STATE_STOPPED;          // TODO: suspend threads
            return;
        }

        // Terminate by default.
        Target->State = PSX_STATE_ZOMBIE;
        Target->ExitStatus = (LONG)(Sig & 0x7F);        // WIFSIGNALED status
        PsxCloseAllFds(Target);
        if (Target->ProcessHandle != NULL)
            NtTerminateProcess(Target->ProcessHandle, (NTSTATUS)Sig);
        return;
    }

    // Custom handler on a running target: inject psxdll's trampoline so it runs
    // the handler in the target. (Targets blocked in a syscall leave it pending;
    // EINTR delivery on the in-flight call is a TODO.)
    if ((Target->State == PSX_STATE_RUNNING) &&
        (Target->SignalTrampoline != 0) &&
        (Target->ProcessHandle != NULL))
    {
        HANDLE Thread = NULL;
        OBJECT_ATTRIBUTES ObjectAttributes;

        InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
        if (NT_SUCCESS(NtOpenThread(&Thread, THREAD_ALL_ACCESS, &ObjectAttributes, &Target->ClientId)))
        {
            RtlRemoteCall(Target->ProcessHandle, Thread,
                          (PVOID)(ULONG_PTR)Target->SignalTrampoline,
                          0, NULL, TRUE, FALSE);
            NtClose(Thread);
        }
    }
}

//
// kill(pid, sig) -- ApiNumber 0x04.  pid@0x30, sig@0x34.
//
VOID
PsxSrvKill(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    LONG WantPid = (LONG)Args[0];
    ULONG Sig = Args[1];
    ULONG TargetGroup = (WantPid < -1) ? (ULONG)(-WantPid) : 0;
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    PSXTRACE("kill: caller pid %lu -> WantPid %ld sig %lu\n",
             (Process != NULL) ? Process->Pid : 0, WantPid, Sig);

    if (Sig > PSX_SIGMAX)
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        PPSX_PROCESS Target = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        BOOLEAN Match;

        if (Target->State == PSX_STATE_ZOMBIE)
            continue;

        if (WantPid > 0)        Match = (Target->Pid == (ULONG)WantPid);
        else if (WantPid == 0)  Match = (Target->ProcessGroup == Process->ProcessGroup);
        else if (WantPid == -1) Match = TRUE;
        else                    Match = (Target->ProcessGroup == TargetGroup);

        if (!Match)
            continue;

        Found = TRUE;
        if (Sig != 0)           // signal 0: existence check only
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

//
// sigaction(sig, act, oldact) -- ApiNumber 0x05.
//   sig@0x30, hasAct@0x34, handler@0x38, mask@0x3C, flags@0x40;
//   reply oldact handler@0x48, mask@0x4C, flags@0x50.
//
VOID
PsxSrvSigaction(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG Sig = Args[0];
    ULONG HasAct = Args[1];

    if ((Sig < 1) || (Sig > PSX_SIGMAX))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    if (HasAct != 0)
    {
        ULONG NewHandler = Args[2];

        // SIGKILL / SIGSTOP can only keep the default disposition.
        if (((Sig == PSX_SIGKILL) || (Sig == PSX_SIGSTOP)) && (NewHandler != PSX_SIG_DFL))
        {
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
        }

        // Return the previous action.
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

//
// sigprocmask(how, set, oldset) -- ApiNumber 0x06. how@0x30, set@0x38; old@0x38.
//
VOID
PsxSrvSigprocmask(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG How = Args[0];
    ULONG Set = Args[2];
    ULONG Old = Process->BlockedSignals;

    switch (How)
    {
        case 1: Process->BlockedSignals |= Set; break;      // SIG_BLOCK
        case 2: Process->BlockedSignals &= ~Set; break;     // SIG_UNBLOCK
        case 3: Process->BlockedSignals = Set; break;       // SIG_SETMASK
        default:
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
    }

    // SIGKILL / SIGSTOP can never be blocked; keep only valid signal bits.
    Process->BlockedSignals &= ~(PSX_SIGBIT(PSX_SIGKILL) | PSX_SIGBIT(PSX_SIGSTOP));
    Process->BlockedSignals &= 0x7FFFF;

    Args[2] = Old;      // reply: previous mask
    Message->Errno = 0;
    Message->ReturnValue = 0;
    // TODO: deliver any signals that just became unblocked.
}

//
// sigpending(set) -- ApiNumber 0x07. Reply at 0x34.
//
VOID
PsxSrvSigpending(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;

    Args[1] = Process->PendingSignals & 0x7FFFF;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// sigsuspend(mask) -- ApiNumber 0x08. Should atomically install the mask and
// block until a signal is delivered; for now it always returns -1/EINTR (the
// POSIX return for sigsuspend). TODO: real blocking with signal wakeup.
//
VOID
PsxSrvSigsuspend(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNREFERENCED_PARAMETER(Process);
    Message->Errno = PSX_EINTR;
    Message->ReturnValue = -1;
}

//
// Deliver a controlling-terminal signal (posixterm/posix.exe -> SESPORT,
// discriminator 1) to the session's foreground processes. The tty code maps to
// a POSIX signal; without job control we target every RUNNING process in the
// session (== the leader pid), which is the shell and its current child. Faithful
// to the real psxss's tty-signal path (posix.exe SendSignal sub_1ED206E ->
// server-side group delivery). Codes: 0=INTR ^C, 1=SUSP, 2=close/HUP, 3=QUIT.
//
VOID
PsxSesDeliverTtySignal(IN ULONG SessionId, IN ULONG Code)
{
    PLIST_ENTRY Entry;
    ULONG Sig;

    switch (Code)
    {
        case 0:  Sig = 6;  break;   // SIGINT
        case 1:  Sig = 17; break;   // SIGTSTP
        case 2:  Sig = 4;  break;   // SIGHUP
        case 3:  Sig = 9;  break;   // SIGQUIT
        default: return;
    }

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        PPSX_PROCESS Target = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        if ((Target->SessionId == SessionId) && (Target->State == PSX_STATE_RUNNING))
            PsxDeliverSignal(Target, Sig);
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}
