/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     X11 display connection -- the POSIX<->Win32 bridge for the X Window
 *              System. A faithful NT 4.0 POSIX process links only psxdll->ntdll and
 *              can neither call GDI nor open a socket; its ONLY channel out is LPC to
 *              psxss. So an X client cannot reach an X server the usual (TCP) way.
 *
 *              This file adds a new file-object kind, PSX_FILE_XCONN: open("/dev/x11")
 *              dials the companion Win32 GUI server psxx11.exe over a named pipe, and
 *              read()/write() relay the X wire protocol between the client and that
 *              server (which owns all the GDI drawing). psxss is a Win32 process, so
 *              *it* can hold the pipe even though its POSIX client cannot.
 *
 *              This mirrors how tty fds route their bytes out to Win32-side posixterm:
 *              posixterm is to the terminal what psxx11.exe is to graphics.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

#define PSX_EBADF    9
#define PSX_EIO      5
#define PSX_ENOMEM  12
#define PSX_EMFILE  24
#define PSX_EPIPE   32

//
// Display 0's pipe. The companion server creates this with PIPE_UNLIMITED_INSTANCES
// and accepts one connection per X client. (Per-display numbering is a TODO: the
// name would become ...-X11-<n> keyed off the DISPLAY the client asked for.)
//
#define PSX_X11_PIPE_NAME   L"\\\\.\\pipe\\ReactOS-X11-0"
#define PSX_X11_SERVER_EXE  L"psxx11.exe"

// The largest chunk we relay per read()/write() syscall. Xlib does its own buffered
// reads/writes in chunks well under this; a byte-mode pipe returns as soon as any
// data is available, which is exactly the recv()/send() semantics Xlib expects.
#define PSX_X11_CHUNK   0x4000      // 16 KiB

// We launch the server at most once per psxss lifetime (0 = not yet tried).
static LONG PsxX11ServerLaunched = 0;

//
// Best-effort launch of the companion X server. Idempotent: guarded so concurrent
// /dev/x11 opens spawn it only once; callers still WaitNamedPipe + retry regardless,
// so a lost race just means the loser waits for the winner's server.
//
static VOID
PsxLaunchXServer(VOID)
{
    WCHAR Path[MAX_PATH];
    UINT Len;
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    if (InterlockedCompareExchange(&PsxX11ServerLaunched, 1, 0) != 0)
        return;     // someone else already kicked it off

    // psxss's cwd is not guaranteed to be system32, so build an absolute path.
    Len = GetSystemDirectoryW(Path, MAX_PATH);
    if ((Len == 0) || (Len + 1 + wcslen(PSX_X11_SERVER_EXE) + 1 > MAX_PATH))
        return;
    Path[Len] = L'\\';
    wcscpy(&Path[Len + 1], PSX_X11_SERVER_EXE);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    // psxss is a session-manager-spawned subsystem: its inherited window station is
    // NOT the interactive WinSta0\Default, so a psxx11 launched with lpDesktop=NULL
    // creates its GUI window on an invisible (non-interactive) desktop -- the X server
    // runs but nothing shows. Pin it to the interactive desktop so the window is visible.
    StartupInfo.lpDesktop = L"WinSta0\\Default";
    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    if (CreateProcessW(Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                       &StartupInfo, &ProcessInfo))
    {
        NtClose(ProcessInfo.hThread);
        NtClose(ProcessInfo.hProcess);
    }
    else
    {
        PSXTRACE("PSXSS: failed to launch %S (err %lu)\n", Path, GetLastError());
        // Allow a later open() to retry the launch.
        InterlockedExchange(&PsxX11ServerLaunched, 0);
    }
}

//
// Connect to the companion server's named pipe, launching it on first use. Returns
// a connected pipe handle or INVALID_HANDLE_VALUE.
//
static HANDLE
PsxConnectXServer(VOID)
{
    HANDLE Pipe;
    ULONG Attempt;

    for (Attempt = 0; Attempt < 80; Attempt++)   // ~ up to 80 * 250ms = 20s cold start
    {
        Pipe = CreateFileW(PSX_X11_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
        if (Pipe != INVALID_HANDLE_VALUE)
            return Pipe;

        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            // Pipe doesn't exist yet -> the server is still starting (a cold launch).
            // CRITICAL: WaitNamedPipe returns *immediately* when NO instance of the
            // pipe exists (MSDN), so we must Sleep ourselves to give psxx11.exe time
            // to come up -- otherwise the loop spins to the cap in milliseconds.
            PsxLaunchXServer();     // idempotent: only spawns once
            Sleep(250);
        }
        else
        {
            // ERROR_PIPE_BUSY (an instance exists but is busy): WaitNamedPipe honours
            // the timeout here. Fall back to Sleep if it returns without a free pipe.
            if (!WaitNamedPipeW(PSX_X11_PIPE_NAME, 250))
                Sleep(50);
        }
    }
    return INVALID_HANDLE_VALUE;
}

//
// open("/dev/x11") -- create a fd bound to a fresh connection to psxx11.exe. Returns
// the fd, or a negative errno.
//
INT
PsxOpenXConnFd(IN PPSX_PROCESS Process, IN ULONG OpenFlags)
{
    PPSX_FILE_OBJECT File;
    HANDLE Pipe;
    INT Fd;

    Pipe = PsxConnectXServer();
    if (Pipe == INVALID_HANDLE_VALUE)
        return -PSX_EIO;        // no display available (server didn't come up)

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
    if (File == NULL)
    {
        NtClose(Pipe);
        return -PSX_ENOMEM;
    }
    File->RefCount = 1;
    File->NtHandle = NULL;
    File->OpenFlags = OpenFlags;
    File->FileType = PSX_FILE_XCONN;
    File->XPipe = Pipe;

    Fd = PsxAllocateFd(Process, File);
    if (Fd < 0)
    {
        NtClose(Pipe);
        RtlFreeHeap(RtlGetProcessHeap(), 0, File);
        return -PSX_EMFILE;
    }
    return Fd;
}

//
// read(): pull bytes from the server pipe (blocking until at least one byte or the
// server closes), then push them into the client's buffer. A server-side close
// reports EOF (0), like a closed socket -- Xlib treats that as the display dying.
//
VOID
PsxXConnRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    DWORD Got = 0;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    if (Count > PSX_X11_CHUNK)
        Count = PSX_X11_CHUNK;

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    if (!ReadFile(File->XPipe, Bounce, Count, &Got, NULL))
    {
        DWORD Error = GetLastError();
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        if ((Error == ERROR_BROKEN_PIPE) || (Error == ERROR_PIPE_NOT_CONNECTED))
        {
            Message->Errno = 0;
            Message->ReturnValue = 0;       // server gone -> EOF
            return;
        }
        Message->Errno = PSX_EIO;
        Message->ReturnValue = -1;
        return;
    }

    if (Got > 0)
    {
        if (!NT_SUCCESS(NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                             Bounce, Got, NULL)))
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
            Message->Errno = PSX_EIO;
            Message->ReturnValue = -1;
            return;
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Got;
}

//
// write(): pull the client's bytes and push them to the server pipe. A dead server
// reports EPIPE, like writing to a closed socket. (TODO: also raise SIGPIPE, matching
// pipe write-to-no-readers.)
//
VOID
PsxXConnWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    DWORD Wrote = 0;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    if (Count > PSX_X11_CHUNK)
        Count = PSX_X11_CHUNK;

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                        Bounce, Count, NULL)))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EIO;
        Message->ReturnValue = -1;
        return;
    }

    if (!WriteFile(File->XPipe, Bounce, Count, &Wrote, NULL))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EPIPE;
        Message->ReturnValue = -1;
        return;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Wrote;
}

//
// Bytes available to read without blocking on an X connection. This is the primitive
// a future select()/poll over /dev/x11 needs -- real libX11's event core polls the
// display fd, which the NT 4.0 subsystem otherwise can't express. Because psxss holds
// the connection as a Win32 named pipe, PeekNamedPipe answers it directly. (Not yet
// wired to a client-visible poll opcode; see subsystems/posix/xlib/README.md.)
//
ULONG
PsxXConnBytesReadable(IN PPSX_FILE_OBJECT File)
{
    DWORD Available = 0;

    if (File->XPipe == NULL)
        return 0;
    if (!PeekNamedPipe(File->XPipe, NULL, 0, NULL, &Available, NULL))
        return 0;
    return Available;
}

//
// Would a read() on this fd return without blocking? "Ready" means data is available
// or a read would return immediately (EOF/error). This is the per-fd predicate poll()
// waits on.
//
BOOLEAN
PsxPollReady(IN PPSX_FILE_OBJECT File)
{
    DWORD Available = 0;

    switch (File->FileType)
    {
        case PSX_FILE_XCONN:
            if (File->XPipe == NULL)
                return TRUE;                    // no pipe -> read returns immediately
            if (!PeekNamedPipe(File->XPipe, NULL, 0, NULL, &Available, NULL))
                return TRUE;                    // broken pipe -> read returns EOF
            return (Available > 0);
        case PSX_FILE_PIPE:
            return PsxPipeReady(File);
        case PSX_FILE_PTMX:
        case PSX_FILE_PTS:
            return PsxPtyReady(File);
        case PSX_FILE_DEVNULL:
        case PSX_FILE_DEVZERO:
        case PSX_FILE_DEVRANDOM:
        case PSX_FILE_DEVFULL:
            return TRUE;                        // synthetic devices never block a read
        case PSX_FILE_TTY:
            return TRUE;                        // TODO: real tty poll via posix.exe
        default:
            return TRUE;                        // disk files are always ready
    }
}

//
// Wait up to TimeoutMs (0 = non-blocking probe, 0xFFFFFFFF = infinite) for the
// file to become readable. Returns 1 = readable, 0 = timed out. Shared by
// PSX_API_POLL and the fcntl(PSX_FCNTL_POLLRD) tunnel (fd.c). This is a ~10ms
// poll loop; an event-backed wait is a TODO (a busy poll ties up an API worker
// while blocked, like a blocking read does).
//
LONG
PsxPollWait(IN PPSX_FILE_OBJECT File, IN ULONG TimeoutMs)
{
    ULONG Waited = 0;

    for (;;)
    {
        if (PsxPollReady(File))
            return 1;
        if (TimeoutMs == 0)                     // non-blocking probe
            return 0;
        if ((TimeoutMs != 0xFFFFFFFF) && (Waited >= TimeoutMs))
            return 0;                           // timed out
        Sleep(10);
        Waited += 10;
    }
}

//
// PSX_API_POLL (extension 0x40): wait for a single fd to become readable. fd is in the
// FileDescriptor slot, timeout (ms; 0 = non-blocking, 0xFFFFFFFF = infinite) in Count.
// ReturnValue: 1 = readable, 0 = timed out, -1 = error. Real libX11's event loop rides
// this (select on the display fd).
//
VOID
PsxSrvPoll(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)Message->Data.ReadWrite.FileDescriptor;
    ULONG Timeout = Message->Data.ReadWrite.Count;
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = PsxPollWait(File, Timeout);
}

//
// The last fd on this connection is closing: drop the pipe (the server sees EOF and
// tears the X client's resources down).
//
VOID
PsxXConnClose(IN PPSX_FILE_OBJECT File)
{
    if (File->XPipe != NULL)
    {
        NtClose(File->XPipe);
        File->XPipe = NULL;
    }
}
