/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header for PSXSS.EXE (the POSIX subsystem server). Models
 *              the NT 4.0 psxss.exe: an LPC server holding all POSIX state.
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
#include <ndk/setypes.h>   // SE_CREATE_PERMANENT_PRIVILEGE (for OBJ_PERMANENT \PSXSS)

#include <subsys/posix/psxmsg.h>
#include <subsys/posix/psxext.h>    // extension opcodes (poll, ...) -- not in NT4

#include <debug.h>

//
// Debug tracing -> kernel debugger (kd). PSXTRACE always prints (it maps to
// ReactOS's DPRINT1, which prefixes file/line); loud on purpose during bring-up.
//
#define PSXTRACE  DPRINT1

//
// Initialize a security descriptor with a *NULL DACL* (which grants everyone
// full access). The real NT 4.0 psxss stamps this on every \PSXSS object --
// the directory, PSXSES, and all the ports -- because posix.exe and the client
// psxdll run under the *user's* token, not SYSTEM: without an allow-all DACL
// they cannot create session objects under \PSXSS\PSXSES or connect to the
// ports. Faithful to psxss main (0x1F45E77) and sub_1F41BE6.
//
FORCEINLINE VOID
PsxInitAllowAllSd(OUT PSECURITY_DESCRIPTOR Sd)
{
    RtlCreateSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(Sd, TRUE, NULL, FALSE);
}

//
// The SM (smss) callback port this subsystem creates and registers; smss
// connects here to deliver SbCreateSession / SbTerminateSession. (NT 4.0 name:
// \PSXSS\SbApiPort.) Distinct from psxmsg.h's PSX_SB_PORT_NAME (\PSXSS\SESPORT),
// which is the posix.exe <-> psxss session-leader port.
//
#define PSX_SM_CALLBACK_PORT_NAME   L"\\PSXSS\\SbApiPort"

//
// Number of API-port worker threads (so a blocking waitpid() on one thread does
// not stall other requests).
//
#define PSX_API_WORKER_COUNT    4

//
// The largest POSIX descriptor table we model per process.
//
#define PSX_OPEN_MAX    64

//
// File-object "kinds". Disk files are serviced directly by psxss; pipe and tty
// objects route through their own paths (the tty's bytes move via posix.exe).
//
#define PSX_FILE_DISK   0
#define PSX_FILE_PIPE   1
#define PSX_FILE_TTY    2
// Synthetic /dev nodes (Phase 2 extension, not in NT 4.0). Served entirely in
// psxss -- no NT handle, no posix.exe phase. read/write in fd.c dispatch on these.
#define PSX_FILE_DEVNULL    3   // /dev/null:  read EOF, write discards
#define PSX_FILE_DEVZERO    4   // /dev/zero:  read zeros, write discards
#define PSX_FILE_DEVRANDOM  5   // /dev/random + /dev/urandom: read random bytes
#define PSX_FILE_DEVFULL    6   // /dev/full:  read zeros, write ENOSPC
// X11 display connection (Phase 2 extension). open("/dev/x11") dials the companion
// X server psxx11.exe over a Win32 named pipe; read/write relay the X wire protocol
// between the POSIX client and the Win32-side server (which owns the GDI drawing).
#define PSX_FILE_XCONN      7   // /dev/x11:   byte stream to psxx11.exe
#define PSX_FILE_XPOLL      8   // /dev/xpoll: read = wait for the X connection to
                                //             become readable (select primitive; the
                                //             MS psxdll can't carry new opcodes/fcntls)
// Pseudo-terminal (Phase 2 extension). open("/dev/ptmx") allocates a pty pair and
// returns the MASTER; the terminal emulator (dtterm) drives it. open("/dev/pts/N")
// returns the SLAVE the child shell runs on. Two in-server ring buffers connect
// them (master<->slave), plus termios (line discipline) and winsize. See pty.c.
#define PSX_FILE_PTMX       9   // /dev/ptmx:  pty master (terminal side)
#define PSX_FILE_PTS        10  // /dev/pts/N: pty slave (shell side)

//
// An open file description. Shared between descriptors by dup2 (RefCount). The
// NT 4.0 server hangs an ops vtable off this (read/write/ioctl/seek/close); we
// start with disk files and dispatch on FileType.
//
typedef struct _PSX_FILE_OBJECT
{
    LONG          RefCount;
    HANDLE        NtHandle;      // underlying NT file handle (disk files)
    LARGE_INTEGER Offset;        // current file position
    ULONG         OpenFlags;     // POSIX O_* it was opened with
    ULONG         FileType;      // PSX_FILE_*
    PVOID         Pipe;          // PSX_PIPE for pipe ends (read end O_RDONLY/write O_WRONLY)
    HANDLE        XPipe;         // Win32 named-pipe handle to psxx11.exe (PSX_FILE_XCONN)
    PVOID         Pty;           // PSX_PTY for pty master/slave ends (PSX_FILE_PTMX/PTS)
} PSX_FILE_OBJECT, *PPSX_FILE_OBJECT;

//
// POSIX signal numbering is NT-specific (SIGKILL=7, SIGSTOP=16, SIGCHLD=14);
// 19 signals total. Masks use bit (1 << (sig - 1)).
//
#define PSX_NSIG        20
#define PSX_SIGALRM     2
#define PSX_SIGKILL     7
#define PSX_SIGCHLD     14
#define PSX_SIGCONT     15
#define PSX_SIGSTOP     16
#define PSX_SIGBIT(s)   (1u << ((s) - 1))

#define PSX_SIG_DFL     0xFFFFFFFF       // (void*)-1
#define PSX_SIG_IGN     1

typedef struct _PSX_SIGACTION
{
    ULONG Handler;      // PSX_SIG_DFL / PSX_SIG_IGN / client handler address
    ULONG Mask;         // signals blocked during the handler
    ULONG Flags;        // SA_*
} PSX_SIGACTION;

//
// Per-client process object. The server keeps the real POSIX state here; the
// LPC connection's PortContext points to it (set at AcceptConnection time).
//
typedef struct _PSX_PROCESS
{
    LIST_ENTRY Entry;           // link in the global process table
    HANDLE    ProcessHandle;
    HANDLE    ClientPort;       // LPC reply port for this client
    CLIENT_ID ClientId;
    ULONG     Pid;
    ULONG     ParentPid;
    ULONG     ProcessGroup;
    ULONG     SessionId;        // owning posix.exe session (== leader PID)
    ULONG     Uid;
    ULONG     EffectiveUid;
    ULONG     Gid;
    ULONG     EffectiveGid;
    BOOLEAN   Connected;        // TRUE once its psxdll has connected to ApiPort
    //
    // The client's 32 KiB shared section, as mapped into THIS (server) address
    // space. Client-sent pointers into the section arrive already translated to
    // our address space; handlers bounds-check them against [ViewBase, ViewEnd).
    //
    ULONG_PTR ViewBase;
    ULONG_PTR ViewEnd;
    ULONG     Umask;            // file-creation mode mask
    PPSX_FILE_OBJECT FdTable[PSX_OPEN_MAX];
    ULONG     State;            // PSX_STATE_*
    LONG      ExitStatus;       // POSIX wait status once a zombie
    BOOLEAN   ExecInProgress;   // TRUE while the old image of an execve() dies
    ULONG     SignalTrampoline;  // psxdll callback (connect cb2) for async delivery
    ULONG     PendingSignals;    // pending signal mask
    ULONG     BlockedSignals;    // blocked (masked) signal mask
    PSX_SIGACTION SigActions[PSX_NSIG];
    HANDLE        AlarmTimer;    // alarm() one-shot timer (NULL when unarmed)
    LARGE_INTEGER AlarmDeadline; // absolute NT time the alarm fires (0 == unarmed)
    //
    // Initial CWD + root NT-path prefixes, written into the client's path-
    // translation buffers during the connect startup-block exchange so relative
    // paths ("." etc.) resolve. Built at spawn from the marshalled cwd.
    //
    CHAR   StartupCwd[528];     // e.g. "\DosDevices\X:\bin\" (trailing backslash)
    CHAR   StartupRoot[64];     // e.g. "\DosDevices\X:"       (drive device, no slash)
    USHORT StartupCwdLen;       // strlen(StartupCwd)
    USHORT StartupRootLen;      // strlen(StartupRoot)
    BOOLEAN StartupBlockValid;  // strings computed at spawn
    BOOLEAN StartupBlockDone;   // exchange completed at connect
} PSX_PROCESS, *PPSX_PROCESS;

#define PSX_STATE_RUNNING   0
#define PSX_STATE_STOPPED   2
#define PSX_STATE_ZOMBIE    3

// The global process table lives in process.c; waitpid scans it under this lock.
extern LIST_ENTRY           g_PsxProcessList;
extern RTL_CRITICAL_SECTION g_PsxProcessLock;

//
// An API handler: read the request, do the work, and fill the reply
// (Errno / ReturnValue) in the SAME message buffer.
//
typedef VOID (*PPSX_API_HANDLER)(IN PPSX_PROCESS Process,
                                 IN OUT PPSX_API_MESSAGE Message);

//
// A POSIX session, led by one posix.exe (the controlling-terminal server). The
// session id is the leader's PID; the leader owns \PSXSS\PSXSES\P<id> (its LPC
// session port) and \PSXSS\PSXSES\D<id> (the session data section). A POSIX
// process's psxdll connects to those after the API port hands it the session id.
//
typedef struct _PSX_SESSION
{
    LIST_ENTRY Entry;
    ULONG      SessionId;       // == LeaderPid
    ULONG      LeaderPid;
    HANDLE     SesCommPort;     // SESPORT server-side comm port (posix.exe -> us)
    HANDLE     LeaderPort;      // our client conn to posix.exe's \PSXSS\PSXSES\P<id>
    CLIENT_ID  LeaderClientId;
} PSX_SESSION, *PPSX_SESSION;

VOID PsxNotifySessionExit(IN ULONG SessionId, IN LONG ExitCode);

extern HANDLE g_ApiPort;
extern HANDLE g_SbApiPort;
extern HANDLE g_SmApiPort;
extern HANDLE g_SesApiPort;
extern PPSX_API_HANDLER g_OpHandlers[PsxApiMaxApiNumber];

NTSTATUS PsxServerInitialization(VOID);
VOID NTAPI PsxApiServerLoop(PVOID Parameter);
VOID PsxInitDispatchTable(VOID);
NTSTATUS PsxAcceptConnection(IN PPORT_MESSAGE ConnectMessage);
VOID PsxReapProcess(IN PPSX_PROCESS Process);
BOOLEAN PsxValidateClientPointer(IN PPSX_PROCESS Process,
                                 IN ULONG_PTR Pointer,
                                 IN ULONG Length);

/* identity.c -- NT SID <-> POSIX uid/gid */
ULONG PsxSidToPosixId(IN PSID Sid);
VOID PsxAssignIdentity(IN PCLIENT_ID ClientId, IN OUT PPSX_PROCESS Process);

/* sbapi.c -- SM registration + the Sb callback port */
NTSTATUS PsxConnectToSm(VOID);
VOID NTAPI PsxSbApiRequestThread(IN PVOID Parameter);

/* session.c -- the \PSXSS\SESPORT session-register port + session table */
NTSTATUS PsxCreateSessionPort(VOID);
VOID NTAPI PsxSesApiRequestThread(IN PVOID Parameter);
PPSX_SESSION PsxFindSession(IN ULONG SessionId);

/* process.c -- the global process table (keyed by ClientId) */
VOID PsxInitProcessTable(VOID);
PPSX_PROCESS PsxAllocateProcess(VOID);
VOID PsxInsertProcess(IN PPSX_PROCESS Process);
VOID PsxRemoveProcess(IN PPSX_PROCESS Process);
PPSX_PROCESS PsxFindProcessByClientId(IN PCLIENT_ID ClientId);
PPSX_PROCESS PsxFindProcessByPid(IN ULONG Pid);

/* fd.c -- the per-process descriptor table + file-syscall handlers */
ULONG PsxErrnoFromStatus(IN NTSTATUS Status);
INT PsxAllocateFd(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File);
PPSX_FILE_OBJECT PsxGetFile(IN PPSX_PROCESS Process, IN INT FileDescriptor);
INT PsxCloseFd(IN PPSX_PROCESS Process, IN INT FileDescriptor);
VOID PsxCloseAllFds(IN PPSX_PROCESS Process);
VOID PsxSrvOpen(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvClose(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvLseek(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvUmask(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvDup2(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvRead(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvWrite(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvStat(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvFstat(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvAccess(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvIsatty(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvFcntl(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvReaddir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvDup(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvFtruncate(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvFpathconf(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvTtyQuery(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvTtyStub(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
LONG PsxQueryPathconf(IN HANDLE Handle, IN ULONG Name, OUT PLONG Errno);
VOID PsxWireControllingTty(IN PPSX_PROCESS Process, IN ULONG FdCount);

/* pipe.c -- anonymous pipes */
VOID PsxSrvPipe(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxPipeRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
VOID PsxPipeWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
VOID PsxPipeCloseEnd(IN PPSX_FILE_OBJECT File);
BOOLEAN PsxPipeReady(IN PPSX_FILE_OBJECT File);   // poll()/select() readability

// Pseudo-terminal (pty.c). Master = terminal side, slave = shell side.
INT  PsxPtyOpenMaster(IN PPSX_PROCESS Process, IN ULONG OpenFlags);         // /dev/ptmx
INT  PsxPtyOpenSlave(IN PPSX_PROCESS Process, IN ULONG Index, IN ULONG OpenFlags); // /dev/pts/N
VOID PsxPtyRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
VOID PsxPtyWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
BOOLEAN PsxPtyReady(IN PPSX_FILE_OBJECT File);    // poll()/select() readability
VOID PsxPtyClose(IN PPSX_FILE_OBJECT File);
LONG PsxPtyIoctl(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN ULONG Request, IN ULONG_PTR Arg, OUT PULONG Errno);
VOID PsxSrvIoctl(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);  // PSX_API_IOCTL
VOID PsxSrvSelect(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message); // PSX_API_SELECT
BOOLEAN PsxPtyTermios(IN PPSX_FILE_OBJECT File, IN BOOLEAN Set, IN OUT PUCHAR Blob68); // tcget/setattr on a pts
BOOLEAN PsxPollReady(IN PPSX_FILE_OBJECT File);   // shared readiness (xconn.c)

/* xconn.c -- X11 display connection to the companion server psxx11.exe */
INT  PsxOpenXConnFd(IN PPSX_PROCESS Process, IN ULONG OpenFlags);
VOID PsxXConnRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
VOID PsxXConnWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message);
VOID PsxXConnClose(IN PPSX_FILE_OBJECT File);
ULONG PsxXConnBytesReadable(IN PPSX_FILE_OBJECT File);   // for a future select()/poll over /dev/x11
VOID PsxSrvPoll(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);  // PSX_API_POLL (extension)
LONG PsxPollWait(IN PPSX_FILE_OBJECT File, IN ULONG TimeoutMs);             // shared poll core (also the fcntl tunnel)

/* path.c -- path-based filesystem syscalls + client impersonation helpers */
VOID PsxImpersonateClient(IN PPSX_PROCESS Process, IN PPSX_API_MESSAGE Message);
VOID PsxRevertToSelf(VOID);
VOID PsxSrvUnlink(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvRmdir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvRename(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvLink(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvMkdir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvChmod(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvUtime(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvMkfifo(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvPathconf(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);

/* identity.c -- SID<->uid/gid + getgroups + passwd/group + chown */
VOID PsxSrvGetGroups(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvGetpwuid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvGetpwnam(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvGetgrgid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvGetgrnam(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvChown(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxInitIdentityOps(VOID);

/* timer.c -- alarm() */
VOID PsxSrvAlarm(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxCancelAlarm(IN PPSX_PROCESS Process);
VOID PsxInitTimerOps(VOID);

/* procops.c -- process lifecycle: fork / execve / waitpid */
VOID PsxSrvFork(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvExecve(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvWaitpid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);

/* signals.c -- signals + delivery */
VOID PsxInitSignalState(IN PPSX_PROCESS Process);
VOID PsxDeliverSignal(IN PPSX_PROCESS Target, IN ULONG Sig);
VOID PsxSesDeliverTtySignal(IN ULONG SessionId, IN ULONG Code);
VOID PsxSrvKill(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvSigaction(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvSigprocmask(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvSigpending(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);
VOID PsxSrvSigsuspend(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message);

//
// SESPORT "create process" request (posix.exe -> psxss). Offsets re-derived from
// posix.exe sub_1ED15C0 (the marshaller) and psxss sub_1F418AF (the handler).
// The *Offset fields are byte offsets into the session data section
// \PSXSS\PSXSES\D<SessionId>; the image NT path sits at section offset 0.
//
typedef struct _PSX_SPAWN_REQUEST
{
    PORT_MESSAGE Header;            // 0x00
    ULONG        Reserved18;        // 0x18
    ULONG        Discriminator;     // 0x1C: 0 = create process, 1 = tty signal
    LONG         Status;            // 0x20: reply status (>= 0 == success)
    ULONG        Reserved24;        // 0x24
    ULONG        LeaderPid;         // 0x28
    ULONG        SessionId;         // 0x2C  (reply reuses as context)
    ULONG        Reserved30;        // 0x30
    ULONG        InheritedFdCount;  // 0x34: controlling-tty fds to wire up
    ULONG        ImagePathOffset;   // 0x38: NT image path (usually 0)
    ULONG        CwdOffset;         // 0x3C
    ULONG        ArgvOffset;        // 0x40: array of section offsets, 0-terminated
    ULONG        EnvOffset;         // 0x44: array of section offsets, 0-terminated
    ULONG        SharedBase;        // 0x48: leader's view base (informational)
} PSX_SPAWN_REQUEST, *PPSX_SPAWN_REQUEST;
