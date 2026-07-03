/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process API handlers and the opcode dispatch table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

HANDLE g_ApiPort = NULL;
HANDLE g_SbApiPort = NULL;
PPSX_API_HANDLER g_OpHandlers[PsxApiMaxApiNumber] = { NULL };

/**
 * @brief getpid/getuid/getgid bundle (ApiNumber 0x0A). One reply carries every id.
 */
static
VOID
PsxSrvGetIds(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    Message->Data.Ids.Pid = Process->Pid;
    Message->Data.Ids.ParentPid = Process->ParentPid;
    Message->Data.Ids.ProcessGroup = Process->ProcessGroup;
    Message->Data.Ids.Uid = Process->Uid;
    Message->Data.Ids.EffectiveUid = Process->EffectiveUid;
    Message->Data.Ids.Gid = Process->Gid;
    Message->Data.Ids.EffectiveGid = Process->EffectiveGid;
    Message->Errno = 0;

    PSXTRACE("getids: pid %lu ppid %lu pgrp %lu uid %lu gid %lu\n",
             Process->Pid, Process->ParentPid, Process->ProcessGroup,
             Process->Uid, Process->Gid);
}

/**
 * @brief _exit(status) (ApiNumber 0x03). Status is at Data[0].
 */
static
VOID
PsxSrvExit(
    _In_opt_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG ExitCode = ((PULONG)Message->Data.Raw)[0];

    Message->Errno = 0;
    Message->ReturnValue = 0;

    if (Process == NULL)
        return;

    /* Stay a zombie until the parent's waitpid() collects it. TODO: send SIGCHLD. */
    PsxCloseAllFds(Process);
    Process->State = PSX_STATE_ZOMBIE;
    Process->ExitStatus = (LONG)((ExitCode & 0xFF) << 8);

    /* The session's top-level process is a child of the posix.exe leader; tell it to exit too */
    if (Process->ParentPid == Process->SessionId)
        PsxNotifySessionExit(Process->SessionId, (LONG)(ExitCode & 0xFF));

    if (Process->ProcessHandle != NULL)
        NtTerminateProcess(Process->ProcessHandle, (NTSTATUS)ExitCode);
}

/**
 * @brief setsid() (ApiNumber 0x10). Returns the new session id (the caller's pid).
 *
 * Process->SessionId still links to the posix.exe I/O session; only the process group moves.
 */
static
VOID
PsxSrvSetsid(
    _In_opt_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    if (Process == NULL)
    {
        Message->Errno = 3; /* ESRCH */
        Message->ReturnValue = -1;
        return;
    }
    Process->ProcessGroup = Process->Pid;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Process->Pid;
}

/**
 * @brief setpgid(pid, pgid) (ApiNumber 0x11). Zero pid means the caller, zero pgid means pid.
 */
static
VOID
PsxSrvSetpgid(
    _In_opt_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG Pid = ((PULONG)Message->Data.Raw)[0];
    ULONG Pgid = ((PULONG)Message->Data.Raw)[1];
    PPSX_PROCESS Target;

    if (Process == NULL)
    {
        Message->Errno = 3; /* ESRCH */
        Message->ReturnValue = -1;
        return;
    }

    if (Pid == 0)
        Pid = Process->Pid;
    Target = (Pid == Process->Pid) ? Process : PsxFindProcessByPid(Pid);
    if (Target == NULL)
    {
        Message->Errno = 3; /* ESRCH */
        Message->ReturnValue = -1;
        return;
    }

    Target->ProcessGroup = (Pgid == 0) ? Pid : Pgid;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief sysconf(name) (ApiNumber 0x17). _SC_CHILD_MAX reports the live process count.
 */
static
VOID
PsxSrvSysconf(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG Name = ((PULONG)Message->Data.Raw)[0];
    PLIST_ENTRY Entry;
    LONG Value;

    UNREFERENCED_PARAMETER(Process);

    switch (Name)
    {
        case 1: /* _SC_ARG_MAX */
            Value = 14500;
            break;

        case 2: /* _SC_CHILD_MAX */
            Value = 1;
            RtlEnterCriticalSection(&g_PsxProcessLock);
            for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
                Value++;
            RtlLeaveCriticalSection(&g_PsxProcessLock);
            break;

        case 3: /* _SC_CLK_TCK */
            Value = 1000;
            break;

        case 4: /* _SC_NGROUPS_MAX */
            Value = 16;
            break;

        case 5: /* _SC_OPEN_MAX */
            Value = 32;
            break;

        case 6: /* _SC_JOB_CONTROL */
        case 7: /* _SC_SAVED_IDS */
            Value = 1;
            break;

        case 8:
            Value = 20;
            break;

        case 9:
            Value = 10;
            break;

        case 10: /* _SC_VERSION */
            Value = 199009;
            break;

        default:
            Message->Errno = 22; /* EINVAL */
            Message->ReturnValue = -1;
            return;
    }
    Message->Errno = 0;
    Message->ReturnValue = Value;
}

/**
 * @brief Post-execve acknowledgement (ApiNumber 0x3E). Clears the exec-in-progress flag.
 */
static
VOID
PsxSrvClearExecFlag(
    _In_opt_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    if (Process != NULL)
        Process->ExecInProgress = FALSE;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

VOID
PsxInitDispatchTable(VOID)
{
    RtlZeroMemory(g_OpHandlers, sizeof(g_OpHandlers));

    g_OpHandlers[PsxApiExit] = PsxSrvExit;
    g_OpHandlers[PsxApiGetIds] = PsxSrvGetIds;

    /* Process lifecycle (procops.c) */
    g_OpHandlers[PsxApiFork] = PsxSrvFork;
    g_OpHandlers[PsxApiExecve] = PsxSrvExecve;
    g_OpHandlers[PsxApiWaitpid] = PsxSrvWaitpid;

    /* Process group, session and sysconf */
    g_OpHandlers[PsxApiSetsid] = PsxSrvSetsid;
    g_OpHandlers[PsxApiSetpgid] = PsxSrvSetpgid;
    g_OpHandlers[PsxApiSysconf] = PsxSrvSysconf;
    g_OpHandlers[PsxApiClearExecFlag] = PsxSrvClearExecFlag;

    /* Signals (signals.c) */
    g_OpHandlers[PsxApiKill] = PsxSrvKill;
    g_OpHandlers[PsxApiSigaction] = PsxSrvSigaction;
    g_OpHandlers[PsxApiSigprocmask] = PsxSrvSigprocmask;
    g_OpHandlers[PsxApiSigpending] = PsxSrvSigpending;
    g_OpHandlers[PsxApiSigsuspend] = PsxSrvSigsuspend;

    /* Path based filesystem calls (path.c) */
    g_OpHandlers[PsxApiLink] = PsxSrvLink;
    g_OpHandlers[PsxApiMkdir] = PsxSrvMkdir;
    g_OpHandlers[PsxApiMkfifo] = PsxSrvMkfifo;
    g_OpHandlers[PsxApiUnlink] = PsxSrvUnlink;
    g_OpHandlers[PsxApiRename] = PsxSrvRename;
    g_OpHandlers[PsxApiChmod] = PsxSrvChmod;
    g_OpHandlers[PsxApiUtime] = PsxSrvUtime;
    g_OpHandlers[PsxApiPathconf] = PsxSrvPathconf;
    g_OpHandlers[PsxApiRmdir] = PsxSrvRmdir;

    /* Identity (identity.c) */
    g_OpHandlers[PsxApiGetGroups] = PsxSrvGetGroups;

    /* File descriptors and metadata (fd.c, pipe.c) */
    g_OpHandlers[PsxApiStat] = PsxSrvStat;
    g_OpHandlers[PsxApiFstat] = PsxSrvFstat;
    g_OpHandlers[PsxApiAccess] = PsxSrvAccess;
    g_OpHandlers[PsxApiOpen] = PsxSrvOpen;
    g_OpHandlers[PsxApiUmask] = PsxSrvUmask;
    g_OpHandlers[PsxApiPipe] = PsxSrvPipe;
    g_OpHandlers[PsxApiDup2] = PsxSrvDup2;
    g_OpHandlers[PsxApiClose] = PsxSrvClose;
    g_OpHandlers[PsxApiRead] = PsxSrvRead;
    g_OpHandlers[PsxApiWrite] = PsxSrvWrite;
    g_OpHandlers[PsxApiFcntl] = PsxSrvFcntl;
    g_OpHandlers[PsxApiLseek] = PsxSrvLseek;
    g_OpHandlers[PsxApiIsatty] = PsxSrvIsatty;
    g_OpHandlers[PsxApiReaddir] = PsxSrvReaddir;
    g_OpHandlers[PsxApiDup] = PsxSrvDup;
    g_OpHandlers[PsxApiFtruncate] = PsxSrvFtruncate;
    g_OpHandlers[PsxApiFpathconf] = PsxSrvFpathconf;

    /* Terminal control; termios is handled by posix.exe (fd.c) */
    g_OpHandlers[PsxApiTcgetattr] = PsxSrvTtyQuery;
    g_OpHandlers[PsxApiTcsetattr] = PsxSrvTtyQuery;
    g_OpHandlers[0x15] = PsxSrvTtyStub;     /* tty query, returns ENOTTY */
    g_OpHandlers[0x31] = PsxSrvTtyStub;     /* tcsendbreak */
    g_OpHandlers[0x32] = PsxSrvTtyStub;     /* tcdrain */
    g_OpHandlers[0x33] = PsxSrvTtyStub;     /* tcflush */
    g_OpHandlers[0x34] = PsxSrvTtyStub;     /* tcflow */
    g_OpHandlers[0x35] = PsxSrvTtyStub;     /* tcgetpgrp */
    g_OpHandlers[0x36] = PsxSrvTtyStub;     /* tcsetpgrp */

    /* alarm, passwd/group lookups and chown. Unregistered opcodes return ENOSYS. */
    PsxInitIdentityOps();
    PsxInitTimerOps();
}
