/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header for the POSIX subsystem server
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#undef WIN32_NO_STATUS

#include <ndk/lpcfuncs.h>
#include <ndk/lpctypes.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>

#include <subsys/posix/psxmsg.h>
#include <subsys/posix/psxext.h>

#include <debug.h>

/* Server tracing goes to the kernel debugger */
#define PSXTRACE  DPRINT1

/**
 * @brief Initialize a security descriptor with a NULL DACL (full access for everyone).
 *
 * Clients run under the user's token, so every object under \PSXSS uses this descriptor.
 */
FORCEINLINE
VOID
PsxInitAllowAllSd(
    _Out_ PSECURITY_DESCRIPTOR Sd)
{
    RtlCreateSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(Sd, TRUE, NULL, FALSE);
}

/*
 * Callback port registered with smss for SbCreateSession / SbTerminateSession.
 * Not the same as PSX_SB_PORT_NAME (\PSXSS\SESPORT), which posix.exe uses.
 */
#define PSX_SM_CALLBACK_PORT_NAME   L"\\PSXSS\\SbApiPort"

/* API port worker threads, so a blocking waitpid() does not stall other requests */
#define PSX_API_WORKER_COUNT    4

/* Size of the per-process descriptor table */
#define PSX_OPEN_MAX    64

/* File object kinds */
#define PSX_FILE_DISK       0
#define PSX_FILE_PIPE       1
#define PSX_FILE_TTY        2   /* Bytes move through posix.exe */
#define PSX_FILE_DEVNULL    3   /* /dev/null: read EOF, write discards */
#define PSX_FILE_DEVZERO    4   /* /dev/zero: read zeros, write discards */
#define PSX_FILE_DEVRANDOM  5   /* /dev/random and /dev/urandom */
#define PSX_FILE_DEVFULL    6   /* /dev/full: read zeros, write ENOSPC */
#define PSX_FILE_XCONN      7   /* /dev/x11: byte stream to psxx11.exe over a named pipe */
#define PSX_FILE_XPOLL      8   /* /dev/xpoll: read waits until the X connection is readable */
#define PSX_FILE_PTMX       9   /* /dev/ptmx: pty master (terminal side) */
#define PSX_FILE_PTS        10  /* /dev/pts/N: pty slave (shell side) */

/* An open file description, shared between descriptors by dup/dup2 */
typedef struct _PSX_FILE_OBJECT
{
    LONG          RefCount;
    HANDLE        NtHandle;      /* NT file handle for disk files */
    LARGE_INTEGER Offset;        /* Current file position */
    ULONG         OpenFlags;     /* POSIX O_* flags */
    ULONG         FileType;      /* PSX_FILE_* */
    PVOID         Pipe;          /* PSX_PIPE for pipe ends */
    HANDLE        XPipe;         /* Named pipe to psxx11.exe (PSX_FILE_XCONN) */
    PVOID         Pty;           /* PSX_PTY for pty ends (PSX_FILE_PTMX/PTS) */
} PSX_FILE_OBJECT, *PPSX_FILE_OBJECT;

/*
 * Signal numbering used by the client library (SIGKILL=7, SIGSTOP=16, SIGCHLD=14).
 * Masks use bit (1 << (sig - 1)).
 */
#define PSX_NSIG        20
#define PSX_SIGALRM     2
#define PSX_SIGKILL     7
#define PSX_SIGCHLD     14
#define PSX_SIGCONT     15
#define PSX_SIGSTOP     16
#define PSX_SIGBIT(s)   (1u << ((s) - 1))

#define PSX_SIG_DFL     0xFFFFFFFF
#define PSX_SIG_IGN     1

typedef struct _PSX_SIGACTION
{
    ULONG Handler;      /* PSX_SIG_DFL, PSX_SIG_IGN or client handler address */
    ULONG Mask;         /* Signals blocked while the handler runs */
    ULONG Flags;        /* SA_* */
} PSX_SIGACTION;

/* Per-client process object. It is the PortContext of the client's LPC connection. */
typedef struct _PSX_PROCESS
{
    LIST_ENTRY Entry;           /* Link in the global process table */
    HANDLE    ProcessHandle;
    HANDLE    ClientPort;       /* LPC reply port for this client */
    CLIENT_ID ClientId;
    ULONG     Pid;
    ULONG     ParentPid;
    ULONG     ProcessGroup;
    ULONG     SessionId;        /* Owning posix.exe session (the leader PID) */
    ULONG     Uid;
    ULONG     EffectiveUid;
    ULONG     Gid;
    ULONG     EffectiveGid;
    BOOLEAN   Connected;        /* TRUE once the client has connected to ApiPort */
    /* Client shared section as mapped in the server. Client pointers are checked against it. */
    ULONG_PTR ViewBase;
    ULONG_PTR ViewEnd;
    ULONG     Umask;            /* File creation mode mask */
    PPSX_FILE_OBJECT FdTable[PSX_OPEN_MAX];
    ULONG     State;            /* PSX_STATE_* */
    LONG      ExitStatus;       /* POSIX wait status once a zombie */
    BOOLEAN   ExecInProgress;   /* TRUE while the old image of an execve() exits */
    ULONG     SignalTrampoline;  /* Client callback used for async signal delivery */
    ULONG     PendingSignals;
    ULONG     BlockedSignals;
    PSX_SIGACTION SigActions[PSX_NSIG];
    HANDLE        AlarmTimer;    /* alarm() timer, NULL when unarmed */
    LARGE_INTEGER AlarmDeadline; /* Absolute NT time the alarm fires, 0 when unarmed */
    /* Initial CWD and root NT path prefixes, written to the client on first connect */
    CHAR   StartupCwd[528];     /* e.g. "\DosDevices\X:\bin\" */
    CHAR   StartupRoot[64];     /* e.g. "\DosDevices\X:" */
    USHORT StartupCwdLen;
    USHORT StartupRootLen;
    BOOLEAN StartupBlockValid;  /* Strings computed at spawn */
    BOOLEAN StartupBlockDone;   /* Strings delivered at connect */
} PSX_PROCESS, *PPSX_PROCESS;

#define PSX_STATE_RUNNING   0
#define PSX_STATE_STOPPED   2
#define PSX_STATE_ZOMBIE    3

/* Global process table (process.c) */
extern LIST_ENTRY           g_PsxProcessList;
extern RTL_CRITICAL_SECTION g_PsxProcessLock;

/* API handler: reads the request and fills Errno/ReturnValue in the same message */
typedef VOID
(*PPSX_API_HANDLER)(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

/*
 * A POSIX session led by one posix.exe. The session id is the leader PID; the leader
 * owns \PSXSS\PSXSES\P<id> (session port) and \PSXSS\PSXSES\D<id> (data section).
 */
typedef struct _PSX_SESSION
{
    LIST_ENTRY Entry;
    ULONG      SessionId;       /* Same as LeaderPid */
    ULONG      LeaderPid;
    HANDLE     SesCommPort;     /* SESPORT communication port for the leader */
    HANDLE     LeaderPort;      /* Our connection to \PSXSS\PSXSES\P<id> */
    CLIENT_ID  LeaderClientId;
} PSX_SESSION, *PPSX_SESSION;

VOID
PsxNotifySessionExit(
    _In_ ULONG SessionId,
    _In_ LONG ExitCode);

extern HANDLE g_ApiPort;
extern HANDLE g_SbApiPort;
extern HANDLE g_SmApiPort;
extern HANDLE g_SesApiPort;
extern PPSX_API_HANDLER g_OpHandlers[PsxApiMaxApiNumber];

/* main.c, server.c, handlers.c */
NTSTATUS
PsxServerInitialization(VOID);

VOID
NTAPI
PsxApiServerLoop(
    _In_opt_ PVOID Parameter);

VOID
PsxInitDispatchTable(VOID);

NTSTATUS
PsxAcceptConnection(
    _Inout_ PPORT_MESSAGE ConnectMessage);

VOID
PsxReapProcess(
    _Inout_opt_ PPSX_PROCESS Process);

BOOLEAN
PsxValidateClientPointer(
    _In_opt_ PPSX_PROCESS Process,
    _In_ ULONG_PTR Pointer,
    _In_ ULONG Length);

/* identity.c */
ULONG
PsxSidToPosixId(
    _In_opt_ PSID Sid);

VOID
PsxAssignIdentity(
    _In_ PCLIENT_ID ClientId,
    _Inout_ PPSX_PROCESS Process);

/* sbapi.c */
NTSTATUS
PsxConnectToSm(VOID);

VOID
NTAPI
PsxSbApiRequestThread(
    _In_opt_ PVOID Parameter);

/* session.c */
NTSTATUS
PsxCreateSessionPort(VOID);

VOID
NTAPI
PsxSesApiRequestThread(
    _In_opt_ PVOID Parameter);

PPSX_SESSION
PsxFindSession(
    _In_ ULONG SessionId);

/* process.c */
VOID
PsxInitProcessTable(VOID);

PPSX_PROCESS
PsxAllocateProcess(VOID);

VOID
PsxInsertProcess(
    _Inout_ PPSX_PROCESS Process);

VOID
PsxRemoveProcess(
    _Inout_ PPSX_PROCESS Process);

PPSX_PROCESS
PsxFindProcessByClientId(
    _In_ PCLIENT_ID ClientId);

PPSX_PROCESS
PsxFindProcessByPid(
    _In_ ULONG Pid);

/* fd.c */
ULONG
PsxErrnoFromStatus(
    _In_ NTSTATUS Status);

INT
PsxAllocateFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File);

PPSX_FILE_OBJECT
PsxGetFile(
    _In_ PPSX_PROCESS Process,
    _In_ INT FileDescriptor);

INT
PsxCloseFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ INT FileDescriptor);

VOID
PsxCloseAllFds(
    _Inout_ PPSX_PROCESS Process);

VOID
PsxSrvOpen(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvClose(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvLseek(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvUmask(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvDup2(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvRead(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvWrite(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvStat(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvFstat(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvAccess(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvIsatty(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvFcntl(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvReaddir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvDup(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvFtruncate(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvFpathconf(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvTtyQuery(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvTtyStub(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

LONG
PsxQueryPathconf(
    _In_ HANDLE Handle,
    _In_ ULONG Name,
    _Out_ PLONG Errno);

VOID
PsxWireControllingTty(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG FdCount);

/* pipe.c */
VOID
PsxSrvPipe(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxPipeRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxPipeWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxPipeCloseEnd(
    _Inout_ PPSX_FILE_OBJECT File);

/** @brief Readability test used by poll() and select(). */
BOOLEAN
PsxPipeReady(
    _In_ PPSX_FILE_OBJECT File);

/* pty.c: master is the terminal side, slave is the shell side */
INT
PsxPtyOpenMaster(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG OpenFlags);

INT
PsxPtyOpenSlave(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG Index,
    _In_ ULONG OpenFlags);

VOID
PsxPtyRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxPtyWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

/** @brief Readability test used by poll() and select(). */
BOOLEAN
PsxPtyReady(
    _In_ PPSX_FILE_OBJECT File);

VOID
PsxPtyClose(
    _Inout_ PPSX_FILE_OBJECT File);

LONG
PsxPtyIoctl(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _In_ ULONG Request,
    _In_ ULONG_PTR Arg,
    _Out_ PULONG Errno);

VOID
PsxSrvIoctl(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvSelect(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

/** @brief tcgetattr/tcsetattr on a pty; Blob68 is the 68-byte termios image. */
BOOLEAN
PsxPtyTermios(
    _In_opt_ PPSX_FILE_OBJECT File,
    _In_ BOOLEAN Set,
    _Inout_ PUCHAR Blob68);

/* xconn.c: X11 display connection to psxx11.exe */
BOOLEAN
PsxPollReady(
    _In_ PPSX_FILE_OBJECT File);

INT
PsxOpenXConnFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG OpenFlags);

VOID
PsxXConnRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxXConnWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxXConnClose(
    _Inout_ PPSX_FILE_OBJECT File);

ULONG
PsxXConnBytesReadable(
    _In_ PPSX_FILE_OBJECT File);

VOID
PsxSrvPoll(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

LONG
PsxPollWait(
    _In_ PPSX_FILE_OBJECT File,
    _In_ ULONG TimeoutMs);

/* path.c */
VOID
PsxImpersonateClient(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_API_MESSAGE Message);

VOID
PsxRevertToSelf(VOID);

VOID
PsxSrvUnlink(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvRmdir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvRename(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvLink(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvMkdir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvChmod(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvUtime(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvMkfifo(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvPathconf(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

/* identity.c: getgroups, passwd/group lookups and chown */
VOID
PsxSrvGetGroups(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvGetpwuid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvGetpwnam(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvGetgrgid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvGetgrnam(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvChown(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxInitIdentityOps(VOID);

/* timer.c */
VOID
PsxSrvAlarm(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxCancelAlarm(
    _Inout_ PPSX_PROCESS Process);

VOID
PsxInitTimerOps(VOID);

/* procops.c */
VOID
PsxSrvFork(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvExecve(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvWaitpid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

/* signals.c */
VOID
PsxInitSignalState(
    _Inout_ PPSX_PROCESS Process);

VOID
PsxDeliverSignal(
    _Inout_ PPSX_PROCESS Target,
    _In_ ULONG Sig);

VOID
PsxSesDeliverTtySignal(
    _In_ ULONG SessionId,
    _In_ ULONG Code);

VOID
PsxSrvKill(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvSigaction(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvSigprocmask(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvSigpending(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

VOID
PsxSrvSigsuspend(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message);

/*
 * SESPORT create-process request from posix.exe. The *Offset fields are byte offsets
 * into the session data section \PSXSS\PSXSES\D<SessionId>.
 */
typedef struct _PSX_SPAWN_REQUEST
{
    PORT_MESSAGE Header;            /* 0x00 */
    ULONG        Reserved18;        /* 0x18 */
    ULONG        Discriminator;     /* 0x1C: 0 = create process, 1 = tty signal */
    LONG         Status;            /* 0x20: reply status, >= 0 is success */
    ULONG        Reserved24;        /* 0x24 */
    ULONG        LeaderPid;         /* 0x28 */
    ULONG        SessionId;         /* 0x2C: the reply reuses it as context */
    ULONG        Reserved30;        /* 0x30 */
    ULONG        InheritedFdCount;  /* 0x34: controlling tty fds to set up */
    ULONG        ImagePathOffset;   /* 0x38: NT image path (usually 0) */
    ULONG        CwdOffset;         /* 0x3C */
    ULONG        ArgvOffset;        /* 0x40: zero-terminated array of section offsets */
    ULONG        EnvOffset;         /* 0x44: zero-terminated array of section offsets */
    ULONG        SharedBase;        /* 0x48: leader's view base, informational only */
} PSX_SPAWN_REQUEST, *PPSX_SPAWN_REQUEST;
