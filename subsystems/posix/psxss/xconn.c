/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     /dev/x11 descriptors relaying the X protocol to psxx11.exe
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * POSIX clients can only talk to psxss over LPC, so they cannot open a socket to
 * an X server. Opening /dev/x11 makes psxss connect to the psxx11.exe Win32 X
 * server over a named pipe, and read()/write() relay bytes through that pipe.
 */

#include "psxss.h"

#define PSX_EBADF    9
#define PSX_EIO      5
#define PSX_ENOMEM  12
#define PSX_EMFILE  24
#define PSX_EPIPE   32

/* Pipe for display 0. TODO: Select the pipe from the requested display number. */
#define PSX_X11_PIPE_NAME   L"\\\\.\\pipe\\ReactOS-X11-0"
#define PSX_X11_SERVER_EXE  L"psxx11.exe"

/* Largest chunk relayed per read() or write() call */
#define PSX_X11_CHUNK   0x4000

/*
 * Launch guard (0 = not launched). It is cleared again if the launched server has
 * exited, so the next open relaunches it.
 */
static LONG PsxX11ServerLaunched = 0;
static HANDLE PsxX11ServerProcess = NULL;

/**
 * @brief Launches psxx11.exe if it is not already running. Callers retry the
 * pipe connect regardless, so a lost launch race is harmless.
 */
static
VOID
PsxLaunchXServer(VOID)
{
    WCHAR Path[MAX_PATH];
    UINT Length;
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    /* Clear the guard if the previously launched server has exited */
    if ((PsxX11ServerLaunched != 0) &&
        (PsxX11ServerProcess != NULL) &&
        (WaitForSingleObject(PsxX11ServerProcess, 0) == WAIT_OBJECT_0))
    {
        NtClose(PsxX11ServerProcess);
        PsxX11ServerProcess = NULL;
        InterlockedExchange(&PsxX11ServerLaunched, 0);
        PSXTRACE("PSXSS: psxx11 exited, clearing launch guard for respawn\n");
    }

    if (InterlockedCompareExchange(&PsxX11ServerLaunched, 1, 0) != 0)
        return;

    /* The current directory is not guaranteed to be system32 */
    Length = GetSystemDirectoryW(Path, MAX_PATH);
    if ((Length == 0) || (Length + 1 + wcslen(PSX_X11_SERVER_EXE) + 1 > MAX_PATH))
        return;
    Path[Length] = L'\\';
    wcscpy(&Path[Length + 1], PSX_X11_SERVER_EXE);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    /* psxss does not run on the interactive window station, so name it explicitly */
    StartupInfo.lpDesktop = L"WinSta0\\Default";
    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    if (CreateProcessW(Path,
                       NULL,
                       NULL,
                       NULL,
                       FALSE,
                       0,
                       NULL,
                       NULL,
                       &StartupInfo,
                       &ProcessInfo))
    {
        /* The process handle is kept to detect a server that has exited */
        NtClose(ProcessInfo.hThread);
        PsxX11ServerProcess = ProcessInfo.hProcess;
    }
    else
    {
        PSXTRACE("PSXSS: failed to launch %S (err %lu)\n", Path, GetLastError());
        InterlockedExchange(&PsxX11ServerLaunched, 0);
    }
}

/**
 * @brief Connects to the X server pipe, launching the server on first use.
 *
 * @return A connected pipe handle, or INVALID_HANDLE_VALUE.
 */
static
HANDLE
PsxConnectXServer(VOID)
{
    HANDLE Pipe;
    ULONG Attempt;

    /* Up to 80 attempts of about 250ms each, allowing for a cold start */
    for (Attempt = 0; Attempt < 80; Attempt++)
    {
        Pipe = CreateFileW(PSX_X11_PIPE_NAME,
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           NULL,
                           OPEN_EXISTING,
                           0,
                           NULL);
        if (Pipe != INVALID_HANDLE_VALUE)
            return Pipe;

        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            /* WaitNamedPipe returns at once when no instance exists, so sleep instead */
            PsxLaunchXServer();
            Sleep(250);
        }
        else
        {
            /* The pipe is busy; wait for a free instance */
            if (!WaitNamedPipeW(PSX_X11_PIPE_NAME, 250))
                Sleep(50);
        }
    }

    return INVALID_HANDLE_VALUE;
}

/**
 * @brief open("/dev/x11"). Creates a descriptor bound to a new X server connection.
 *
 * @return The descriptor, or a negative errno.
 */
INT
PsxOpenXConnFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG OpenFlags)
{
    PPSX_FILE_OBJECT File;
    HANDLE Pipe;
    INT Fd;

    Pipe = PsxConnectXServer();
    if (Pipe == INVALID_HANDLE_VALUE)
        return -PSX_EIO;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*File));
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

/**
 * @brief read() on an X connection. Blocks until data arrives; a closed server
 * pipe reads as end of file.
 */
VOID
PsxXConnRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    DWORD Got = 0;
    DWORD Error;

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
        Error = GetLastError();
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        if ((Error == ERROR_BROKEN_PIPE) || (Error == ERROR_PIPE_NOT_CONNECTED))
        {
            Message->Errno = 0;
            Message->ReturnValue = 0;
            return;
        }
        Message->Errno = PSX_EIO;
        Message->ReturnValue = -1;
        return;
    }

    if (Got > 0)
    {
        if (!NT_SUCCESS(NtWriteVirtualMemory(Process->ProcessHandle,
                                             (PVOID)ClientBuffer,
                                             Bounce,
                                             Got,
                                             NULL)))
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

/**
 * @brief write() on an X connection. A dead server reports EPIPE.
 * TODO: Also raise SIGPIPE.
 */
VOID
PsxXConnWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
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

    if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle,
                                        (PVOID)ClientBuffer,
                                        Bounce,
                                        Count,
                                        NULL)))
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

/**
 * @brief Returns the number of bytes readable on an X connection without blocking.
 */
ULONG
PsxXConnBytesReadable(
    _In_ PPSX_FILE_OBJECT File)
{
    DWORD Available = 0;

    if (File->XPipe == NULL)
        return 0;
    if (!PeekNamedPipe(File->XPipe, NULL, 0, NULL, &Available, NULL))
        return 0;

    return Available;
}

/**
 * @brief Checks whether a read() on File would return without blocking, either
 * with data or with end of file or an error.
 */
BOOLEAN
PsxPollReady(
    _In_ PPSX_FILE_OBJECT File)
{
    DWORD Available = 0;

    switch (File->FileType)
    {
        case PSX_FILE_XCONN:
            /* A missing or broken pipe makes read() return at once */
            if (File->XPipe == NULL)
                return TRUE;
            if (!PeekNamedPipe(File->XPipe, NULL, 0, NULL, &Available, NULL))
                return TRUE;
            return (Available > 0);

        case PSX_FILE_PIPE:
            return PsxPipeReady(File);

        case PSX_FILE_PTMX:
        case PSX_FILE_PTS:
            return PsxPtyReady(File);

        /* Synthetic devices never block a read */
        case PSX_FILE_DEVNULL:
        case PSX_FILE_DEVZERO:
        case PSX_FILE_DEVRANDOM:
        case PSX_FILE_DEVFULL:
            return TRUE;

        /* TODO: Ask posix.exe whether tty input is available */
        case PSX_FILE_TTY:
            return TRUE;

        /* Disk files are always ready */
        default:
            return TRUE;
    }
}

/**
 * @brief Waits up to TimeoutMs for File to become readable. 0 probes without
 * waiting and 0xFFFFFFFF waits forever.
 *
 * @return 1 if readable, 0 on timeout.
 *
 * TODO: Wait on an event instead of polling every 10ms.
 */
LONG
PsxPollWait(
    _In_ PPSX_FILE_OBJECT File,
    _In_ ULONG TimeoutMs)
{
    ULONG Waited = 0;

    for (;;)
    {
        if (PsxPollReady(File))
            return 1;
        if (TimeoutMs == 0)
            return 0;
        if ((TimeoutMs != 0xFFFFFFFF) && (Waited >= TimeoutMs))
            return 0;

        Sleep(10);
        Waited += 10;
    }
}

/**
 * @brief PSX_API_POLL (extension 0x40). Waits for one descriptor to become readable.
 *
 * The descriptor is in FileDescriptor and the timeout in milliseconds is in Count.
 * ReturnValue is 1 if readable, 0 on timeout and -1 on error.
 */
VOID
PsxSrvPoll(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
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

/**
 * @brief Closes the pipe when the last descriptor on an X connection goes away.
 * The server then frees the client's resources.
 */
VOID
PsxXConnClose(
    _Inout_ PPSX_FILE_OBJECT File)
{
    if (File->XPipe != NULL)
    {
        NtClose(File->XPipe);
        File->XPipe = NULL;
    }
}
