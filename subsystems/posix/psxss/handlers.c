/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXSS API handlers + the opcode dispatch table (the NT 4.0
 *              63-entry table at VA 0x1F55030). Worked examples; rest are TODO.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

HANDLE g_ApiPort = NULL;
HANDLE g_SbApiPort = NULL;
PPSX_API_HANDLER g_OpHandlers[PsxApiMaxApiNumber] = { NULL };

//
// getpid/getuid/... bundle -- ApiNumber 0x0A. One reply carries every id; the
// client reads the slot it wants. Cannot fail.
//
static VOID
PsxSrvGetIds(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    Message->Data.Ids.Pid           = Process->Pid;
    Message->Data.Ids.ParentPid     = Process->ParentPid;
    Message->Data.Ids.ProcessGroup  = Process->ProcessGroup;
    Message->Data.Ids.Uid           = Process->Uid;
    Message->Data.Ids.EffectiveUid  = Process->EffectiveUid;
    Message->Data.Ids.Gid           = Process->Gid;
    Message->Data.Ids.EffectiveGid  = Process->EffectiveGid;
    Message->Errno = 0;

    PSXTRACE("getids: pid %lu ppid %lu pgrp %lu uid %lu gid %lu\n",
             Process->Pid, Process->ParentPid, Process->ProcessGroup,
             Process->Uid, Process->Gid);
}

//
// _exit(status) -- ApiNumber 0x03.
//
static VOID
PsxSrvExit(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG ExitCode = ((PULONG)Message->Data.Raw)[0];        // body +0x30

    Message->Errno = 0;
    Message->ReturnValue = 0;

    if (Process == NULL)
        return;

    // Become a zombie so the parent's waitpid() can collect the status; the
    // record is freed when waited. (TODO: deliver SIGCHLD to the parent.)
    PsxCloseAllFds(Process);
    Process->State = PSX_STATE_ZOMBIE;
    Process->ExitStatus = (LONG)((ExitCode & 0xFF) << 8);   // WIFEXITED status

    // If this is the session's top-level process (its parent is the posix.exe
    // leader), tell posix.exe to tear down so it exits too.
    if (Process->ParentPid == Process->SessionId)
        PsxNotifySessionExit(Process->SessionId, (LONG)(ExitCode & 0xFF));

    if (Process->ProcessHandle != NULL)
        NtTerminateProcess(Process->ProcessHandle, (NTSTATUS)ExitCode);
}

//
// setsid() -- ApiNumber 0x10. Create a new session: the caller becomes session
// and process-group leader with no controlling terminal, returning the new
// session id (== its pid). The client (op 16) reads the sid from ReturnValue.
// We keep Process->SessionId (our posix.exe I/O-session link) untouched and only
// move the process group; a distinct POSIX-session id is not separately tracked.
//
static VOID
PsxSrvSetsid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    if (Process == NULL)
    {
        Message->Errno = 3;             // ESRCH
        Message->ReturnValue = -1;
        return;
    }
    Process->ProcessGroup = Process->Pid;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Process->Pid;
}

//
// setpgid(pid, pgid) -- ApiNumber 0x11. pid at Data[0] (msg+0x30), pgid at
// Data[1] (msg+0x34). pid 0 means "the caller"; pgid 0 means "same as pid".
//
static VOID
PsxSrvSetpgid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG Pid  = ((PULONG)Message->Data.Raw)[0];        // msg +0x30
    ULONG Pgid = ((PULONG)Message->Data.Raw)[1];        // msg +0x34
    PPSX_PROCESS Target;

    if (Process == NULL)
    {
        Message->Errno = 3;             // ESRCH
        Message->ReturnValue = -1;
        return;
    }

    if (Pid == 0)
        Pid = Process->Pid;
    Target = (Pid == Process->Pid) ? Process : PsxFindProcessByPid(Pid);
    if (Target == NULL)
    {
        Message->Errno = 3;             // ESRCH
        Message->ReturnValue = -1;
        return;
    }

    Target->ProcessGroup = (Pgid == 0) ? Pid : Pgid;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
//
// sysconf(name) -- ApiNumber 0x17. Returns a system-wide configurable limit.
// Faithful to sub_1F4BC03 (switch on name; _SC_CHILD_MAX counts live processes).
//
static VOID
PsxSrvSysconf(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG Name = ((PULONG)Message->Data.Raw)[0];        // body +0x30
    LONG Value;

    UNREFERENCED_PARAMETER(Process);

    switch (Name)
    {
        case 1:  Value = 14500;  break;     // _SC_ARG_MAX
        case 2:                             // _SC_CHILD_MAX -- count live processes
        {
            PLIST_ENTRY Entry;
            Value = 1;
            RtlEnterCriticalSection(&g_PsxProcessLock);
            for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
                Value++;
            RtlLeaveCriticalSection(&g_PsxProcessLock);
            break;
        }
        case 3:  Value = 1000;   break;     // _SC_CLK_TCK
        case 4:  Value = 16;     break;     // _SC_NGROUPS_MAX
        case 5:  Value = 32;     break;     // _SC_OPEN_MAX
        case 6:  Value = 1;      break;     // _SC_JOB_CONTROL
        case 7:  Value = 1;      break;     // _SC_SAVED_IDS
        case 8:  Value = 20;     break;
        case 9:  Value = 10;     break;
        case 10: Value = 199009; break;     // _SC_VERSION (_POSIX_VERSION)
        default:
            Message->Errno = 22;            // EINVAL
            Message->ReturnValue = -1;
            return;
    }
    Message->Errno = 0;
    Message->ReturnValue = Value;
}

//
// The post-execve acknowledgement -- ApiNumber 0x3E. After psxdll finishes
// swapping in the new image it sends this so the server clears the
// exec-in-progress flag (re-enabling fork/exec on the process). No args.
// Faithful to sub_1F46DE1 (`Flags &= ~0x20`).
//
static VOID
PsxSrvClearExecFlag(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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

    g_OpHandlers[PsxApiExit]   = PsxSrvExit;
    g_OpHandlers[PsxApiGetIds] = PsxSrvGetIds;

    // Process lifecycle (procops.c).
    g_OpHandlers[PsxApiFork]    = PsxSrvFork;     // 0x00
    g_OpHandlers[PsxApiExecve]  = PsxSrvExecve;   // 0x01
    g_OpHandlers[PsxApiWaitpid] = PsxSrvWaitpid;  // 0x02

    // Process group / session / accounting / sysconf (this file).
    g_OpHandlers[PsxApiSetsid]       = PsxSrvSetsid;        // 0x10
    g_OpHandlers[PsxApiSetpgid]      = PsxSrvSetpgid;       // 0x11
    g_OpHandlers[PsxApiSysconf]      = PsxSrvSysconf;       // 0x17
    g_OpHandlers[PsxApiClearExecFlag] = PsxSrvClearExecFlag; // 0x3E

    // Signals (signals.c).
    g_OpHandlers[PsxApiKill]        = PsxSrvKill;        // 0x04
    g_OpHandlers[PsxApiSigaction]   = PsxSrvSigaction;   // 0x05
    g_OpHandlers[PsxApiSigprocmask] = PsxSrvSigprocmask; // 0x06
    g_OpHandlers[PsxApiSigpending]  = PsxSrvSigpending;  // 0x07
    g_OpHandlers[PsxApiSigsuspend]  = PsxSrvSigsuspend;  // 0x08

    // Path-based filesystem ops (path.c).
    g_OpHandlers[PsxApiLink]     = PsxSrvLink;      // 0x1A
    g_OpHandlers[PsxApiMkdir]    = PsxSrvMkdir;     // 0x1B
    g_OpHandlers[PsxApiMkfifo]   = PsxSrvMkfifo;    // 0x1C
    g_OpHandlers[PsxApiUnlink]   = PsxSrvUnlink;    // 0x1D
    g_OpHandlers[PsxApiRename]   = PsxSrvRename;    // 0x1E
    g_OpHandlers[PsxApiChmod]    = PsxSrvChmod;     // 0x22
    g_OpHandlers[PsxApiUtime]    = PsxSrvUtime;     // 0x24
    g_OpHandlers[PsxApiPathconf] = PsxSrvPathconf;  // 0x25
    g_OpHandlers[PsxApiRmdir]    = PsxSrvRmdir;     // 0x3B

    // Identity (identity.c).
    g_OpHandlers[PsxApiGetGroups] = PsxSrvGetGroups; // 0x0D

    // File descriptors + metadata (fd.c).
    g_OpHandlers[PsxApiStat]   = PsxSrvStat;     // 0x1F
    g_OpHandlers[PsxApiFstat]  = PsxSrvFstat;    // 0x20
    g_OpHandlers[PsxApiAccess] = PsxSrvAccess;   // 0x21
    g_OpHandlers[PsxApiOpen]   = PsxSrvOpen;     // 0x18
    g_OpHandlers[PsxApiUmask]  = PsxSrvUmask;    // 0x19
    g_OpHandlers[PsxApiPipe]   = PsxSrvPipe;     // 0x27
    g_OpHandlers[PsxApiDup2]   = PsxSrvDup2;     // 0x29
    g_OpHandlers[PsxApiClose]  = PsxSrvClose;    // 0x2A
    g_OpHandlers[PsxApiRead]   = PsxSrvRead;     // 0x2B (disk files)
    g_OpHandlers[PsxApiWrite]  = PsxSrvWrite;    // 0x2C (disk files)
    g_OpHandlers[PsxApiFcntl]     = PsxSrvFcntl;     // 0x2D
    g_OpHandlers[PsxApiLseek]     = PsxSrvLseek;     // 0x2E
    g_OpHandlers[PsxApiIsatty]    = PsxSrvIsatty;    // 0x16
    g_OpHandlers[PsxApiReaddir]   = PsxSrvReaddir;   // 0x3C
    g_OpHandlers[PsxApiDup]       = PsxSrvDup;       // 0x28
    g_OpHandlers[PsxApiFtruncate] = PsxSrvFtruncate; // 0x3D
    g_OpHandlers[PsxApiFpathconf] = PsxSrvFpathconf; // 0x26

    // Terminal-control ops -- psxss defers termios to posix.exe (fd.c).
    g_OpHandlers[PsxApiTcgetattr] = PsxSrvTtyQuery;  // 0x2F
    g_OpHandlers[PsxApiTcsetattr] = PsxSrvTtyQuery;  // 0x30
    g_OpHandlers[0x15]            = PsxSrvTtyStub;   // tty query -> ENOTTY
    g_OpHandlers[0x31]            = PsxSrvTtyStub;   // tcsendbreak
    g_OpHandlers[0x32]            = PsxSrvTtyStub;   // tcdrain
    g_OpHandlers[0x33]            = PsxSrvTtyStub;   // tcflush
    g_OpHandlers[0x34]            = PsxSrvTtyStub;   // tcflow
    g_OpHandlers[0x35]            = PsxSrvTtyStub;   // tcgetpgrp
    g_OpHandlers[0x36]            = PsxSrvTtyStub;   // tcsetpgrp

    // The remaining slots -- alarm 0x09, identity getpwuid/getpwnam/getgrgid/
    // getgrnam 0x37-0x3A, and chown 0x23 -- are registered by PsxInitIdentityOps /
    // PsxInitTimerOps below. Unregistered opcodes (setuid/setgid/setgroups stubs
    // 0x0B/0x0C/0x0E/0x0F/0x12/0x13) correctly return ENOSYS via the dispatch loop.
    PsxInitIdentityOps();
    PsxInitTimerOps();
}
