/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pseudo-terminals (/dev/ptmx master and /dev/pts/N slave)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A pty is two ring buffers: master writes are read by the slave (keyboard input) and
 * slave writes are read by the master (program output). There is no canonical mode,
 * echo or signal processing; only ONLCR output mapping is applied.
 */

#include "psxss.h"
#include <ndk/exfuncs.h>

#define PSX_EIO      5
#define PSX_ENXIO    6
#define PSX_EBADF    9
#define PSX_ENOMEM   12
#define PSX_EINVAL   22
#define PSX_EMFILE   24
#define PSX_ENOTTY   25

#define PSX_PTY_SIZE      4096
#define PSX_PTY_MAX       16

/* Size of the client termios structure */
#define PSX_TERMIOS_SIZE  68

/* Terminal ioctl requests, matching sdk/include/psx/termios.h */
#define PSX_IOCTL_TCGETS      0x5401
#define PSX_IOCTL_TCSETS      0x5402
#define PSX_IOCTL_TCSETSW     0x5403
#define PSX_IOCTL_TCSETSF     0x5404
#define PSX_IOCTL_TIOCSCTTY   0x540E
#define PSX_IOCTL_TIOCGWINSZ  0x5413
#define PSX_IOCTL_TIOCSWINSZ  0x5414
#define PSX_IOCTL_TIOCGPTN    0x80045430

/* STREAMS requests, matching the psx <stropts.h> */
#define PSX_IOCTL_I_STR       0x5301
#define PSX_IOCTL_I_PUSH      0x5302
#define PSX_IOCTL_I_POP       0x5303
#define PSX_IOCTL_I_FIND      0x5305

/* termios c_oflag bits */
#define PSX_OPOST   0x0001
#define PSX_ONLCR   0x0002

typedef struct _PSX_PTY
{
    RTL_CRITICAL_SECTION Lock;
    BOOLEAN InUse;
    ULONG Index;
    LONG MasterRefs;
    LONG SlaveRefs;

    /* Master to slave (keyboard input) */
    ULONG M2SCount;
    ULONG M2SRead;
    ULONG M2SWrite;
    HANDLE M2SData;     // Set when M2S has data or the master closed
    UCHAR M2S[PSX_PTY_SIZE];

    /* Slave to master (program output) */
    ULONG S2MCount;
    ULONG S2MRead;
    ULONG S2MWrite;
    HANDLE S2MData;     // Set when S2M has data or the slave closed
    UCHAR S2M[PSX_PTY_SIZE];

    /* Slave termios and window size */
    UCHAR Termios[PSX_TERMIOS_SIZE];
    USHORT WsRow;
    USHORT WsCol;
    USHORT WsXpixel;
    USHORT WsYpixel;
} PSX_PTY, *PPSX_PTY;

static PSX_PTY PsxPtyTable[PSX_PTY_MAX];
static RTL_CRITICAL_SECTION PsxPtyTableLock;
static LONG PsxPtyTableInitialized = 0;

static
VOID
PsxPtyInitTable(VOID)
{
    if (InterlockedCompareExchange(&PsxPtyTableInitialized, 1, 0) == 0)
        RtlInitializeCriticalSection(&PsxPtyTableLock);
}

static
HANDLE
PsxPtyCreateEvent(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = NULL;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    NtCreateEvent(&Handle, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
    return Handle;
}

/**
 * @brief Seed the termios of a new pty with cooked mode defaults.
 */
static
VOID
PsxPtyInitTermios(
    _Inout_ PPSX_PTY Pty)
{
    PULONG Flags = (PULONG)Pty->Termios;

    /* c_iflag: ICRNL | IXON */
    Flags[0] = 0x0500;

    /* c_oflag: OPOST | ONLCR */
    Flags[1] = PSX_OPOST | PSX_ONLCR;

    /* c_cflag: CS8 | CREAD */
    Flags[2] = 0x00B0;

    /* c_lflag: ISIG | ICANON | ECHO | ECHOE */
    Flags[3] = 0x001B;

    /* Input and output speed: B9600 */
    Flags[4] = 13;
    Flags[5] = 13;

    /* c_cc: VINTR ^C, VQUIT ^\, VERASE DEL, VKILL ^U, VEOF ^D */
    Pty->Termios[24] = 3;
    Pty->Termios[25] = 28;
    Pty->Termios[26] = 127;
    Pty->Termios[27] = 21;
    Pty->Termios[28] = 4;
}

/**
 * @brief Claim a free pty slot.
 * @return The pty with one master reference, or NULL if none is available.
 */
static
PPSX_PTY
PsxPtyAllocate(VOID)
{
    PPSX_PTY Pty = NULL;
    ULONG Slot;

    PsxPtyInitTable();
    RtlEnterCriticalSection(&PsxPtyTableLock);
    for (Slot = 0; Slot < PSX_PTY_MAX; Slot++)
    {
        if (!PsxPtyTable[Slot].InUse)
        {
            Pty = &PsxPtyTable[Slot];
            RtlZeroMemory(Pty, sizeof(*Pty));
            Pty->InUse = TRUE;
            Pty->Index = Slot;
            break;
        }
    }
    RtlLeaveCriticalSection(&PsxPtyTableLock);

    if (Pty == NULL)
        return NULL;

    RtlInitializeCriticalSection(&Pty->Lock);
    Pty->M2SData = PsxPtyCreateEvent();
    Pty->S2MData = PsxPtyCreateEvent();
    Pty->MasterRefs = 1;
    Pty->SlaveRefs = 0;
    Pty->WsRow = 24;
    Pty->WsCol = 80;
    PsxPtyInitTermios(Pty);

    if ((Pty->M2SData == NULL) || (Pty->S2MData == NULL))
    {
        if (Pty->M2SData)
            NtClose(Pty->M2SData);

        if (Pty->S2MData)
            NtClose(Pty->S2MData);

        RtlDeleteCriticalSection(&Pty->Lock);
        Pty->InUse = FALSE;
        return NULL;
    }

    return Pty;
}

/**
 * @brief open("/dev/ptmx"): allocate a pty and return its master descriptor.
 * @return The descriptor, or a negative errno.
 */
INT
PsxPtyOpenMaster(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG OpenFlags)
{
    PPSX_PTY Pty = PsxPtyAllocate();
    PPSX_FILE_OBJECT File;
    INT Fd;

    if (Pty == NULL)
        return -PSX_EMFILE;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*File));
    if (File == NULL)
    {
        Pty->InUse = FALSE;
        return -PSX_ENOMEM;
    }

    File->RefCount = 1;
    File->FileType = PSX_FILE_PTMX;
    File->OpenFlags = OpenFlags;
    File->Pty = Pty;

    Fd = PsxAllocateFd(Process, File);
    if (Fd < 0)
    {
        PsxPtyClose(File);
        RtlFreeHeap(RtlGetProcessHeap(), 0, File);
        return -PSX_EMFILE;
    }

    return Fd;
}

/**
 * @brief open("/dev/pts/N"): return a slave descriptor for pty N, which needs a live master.
 * @return The descriptor, or a negative errno.
 */
INT
PsxPtyOpenSlave(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG Index,
    _In_ ULONG OpenFlags)
{
    PPSX_PTY Pty;
    PPSX_FILE_OBJECT File;
    INT Fd;

    if (Index >= PSX_PTY_MAX)
        return -PSX_ENXIO;

    PsxPtyInitTable();
    Pty = &PsxPtyTable[Index];
    if (!Pty->InUse || (Pty->MasterRefs <= 0))
        return -PSX_ENXIO;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*File));
    if (File == NULL)
        return -PSX_ENOMEM;

    File->RefCount = 1;
    File->FileType = PSX_FILE_PTS;
    File->OpenFlags = OpenFlags;
    File->Pty = Pty;

    RtlEnterCriticalSection(&Pty->Lock);
    Pty->SlaveRefs++;
    RtlLeaveCriticalSection(&Pty->Lock);

    Fd = PsxAllocateFd(Process, File);
    if (Fd < 0)
    {
        PsxPtyClose(File);
        RtlFreeHeap(RtlGetProcessHeap(), 0, File);
        return -PSX_EMFILE;
    }

    return Fd;
}

/**
 * @brief Copy up to Want bytes out of a ring buffer.
 * @return The number of bytes copied.
 */
static
ULONG
PsxRingTake(
    _In_reads_bytes_(PSX_PTY_SIZE) PUCHAR Ring,
    _Inout_ PULONG Count,
    _Inout_ PULONG ReadPos,
    _Out_writes_bytes_(Want) PUCHAR Destination,
    _In_ ULONG Want)
{
    ULONG Take = (Want < *Count) ? Want : *Count;
    ULONG Index;

    for (Index = 0; Index < Take; Index++)
    {
        Destination[Index] = Ring[*ReadPos];
        *ReadPos = (*ReadPos + 1) % PSX_PTY_SIZE;
    }

    *Count -= Take;
    return Take;
}

/**
 * @brief Copy up to Want bytes into a ring buffer.
 * @return The number of bytes stored.
 */
static
ULONG
PsxRingPut(
    _Inout_updates_bytes_(PSX_PTY_SIZE) PUCHAR Ring,
    _Inout_ PULONG Count,
    _Inout_ PULONG WritePos,
    _In_reads_bytes_(Want) PUCHAR Source,
    _In_ ULONG Want)
{
    ULONG Done = 0;

    while ((Done < Want) && (*Count < PSX_PTY_SIZE))
    {
        Ring[*WritePos] = Source[Done++];
        *WritePos = (*WritePos + 1) % PSX_PTY_SIZE;
        (*Count)++;
    }

    return Done;
}

/**
 * @brief read() on a pty. The master reads program output and the slave reads keyboard input.
 *
 * Blocks until data arrives or the other end closes, which reads as end of file.
 */
VOID
PsxPtyRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Master = (File->FileType == PSX_FILE_PTMX);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Got = 0;

    if ((Pty == NULL) || (Count == 0))
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    if (Count > PSX_PTY_SIZE)
        Count = PSX_PTY_SIZE;

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    for (;;)
    {
        RtlEnterCriticalSection(&Pty->Lock);
        if (Master)
        {
            if (Pty->S2MCount > 0)
            {
                Got = PsxRingTake(Pty->S2M, &Pty->S2MCount, &Pty->S2MRead, Bounce, Count);
                if (Pty->S2MCount == 0)
                    NtClearEvent(Pty->S2MData);

                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }

            if (Pty->SlaveRefs == 0)
            {
                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }

            RtlLeaveCriticalSection(&Pty->Lock);
            NtWaitForSingleObject(Pty->S2MData, FALSE, NULL);
        }
        else
        {
            if (Pty->M2SCount > 0)
            {
                Got = PsxRingTake(Pty->M2S, &Pty->M2SCount, &Pty->M2SRead, Bounce, Count);
                if (Pty->M2SCount == 0)
                    NtClearEvent(Pty->M2SData);

                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }

            if (Pty->MasterRefs == 0)
            {
                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }

            RtlLeaveCriticalSection(&Pty->Lock);
            NtWaitForSingleObject(Pty->M2SData, FALSE, NULL);
        }
    }

    if (Got > 0)
        NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Got, NULL);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Got;
}

/**
 * @brief write() on a pty. The master writes keyboard input and the slave writes program output.
 *
 * Stores what fits in the ring and drops the rest, then wakes the reader.
 */
VOID
PsxPtyWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
{
    static const UCHAR CarriageReturn = '\r';
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Master = (File->FileType == PSX_FILE_PTMX);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Done = 0;
    ULONG OutputFlags;
    ULONG Index;
    NTSTATUS Status;

    if ((Pty == NULL) || (Count == 0))
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    if (Count > PSX_PTY_SIZE)
        Count = PSX_PTY_SIZE;

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Count, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    RtlEnterCriticalSection(&Pty->Lock);
    if (Master)
    {
        Done = PsxRingPut(Pty->M2S, &Pty->M2SCount, &Pty->M2SWrite, Bounce, Count);
        if (Done > 0)
            NtSetEvent(Pty->M2SData, NULL);
    }
    else
    {
        /* With OPOST and ONLCR set, map each '\n' to "\r\n" */
        OutputFlags = ((PULONG)Pty->Termios)[1];
        if ((OutputFlags & PSX_OPOST) && (OutputFlags & PSX_ONLCR))
        {
            for (Index = 0; Index < Count; Index++)
            {
                if (Bounce[Index] == '\n')
                    PsxRingPut(Pty->S2M, &Pty->S2MCount, &Pty->S2MWrite, (PUCHAR)&CarriageReturn, 1);

                PsxRingPut(Pty->S2M, &Pty->S2MCount, &Pty->S2MWrite, &Bounce[Index], 1);
            }
        }
        else
        {
            PsxRingPut(Pty->S2M, &Pty->S2MCount, &Pty->S2MWrite, Bounce, Count);
        }

        Done = Count;
        NtSetEvent(Pty->S2MData, NULL);
    }
    RtlLeaveCriticalSection(&Pty->Lock);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);

    /* Report the full count; anything past the ring capacity was dropped */
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Count;
}

/**
 * @brief Check whether a read on the pty end would complete without blocking.
 */
BOOLEAN
PsxPtyReady(
    _In_ PPSX_FILE_OBJECT File)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Ready;

    if (Pty == NULL)
        return TRUE;

    RtlEnterCriticalSection(&Pty->Lock);
    if (File->FileType == PSX_FILE_PTMX)
        Ready = (Pty->S2MCount > 0) || (Pty->SlaveRefs == 0);
    else
        Ready = (Pty->M2SCount > 0) || (Pty->MasterRefs == 0);
    RtlLeaveCriticalSection(&Pty->Lock);

    return Ready;
}

/**
 * @brief Release a master or slave end, waking the peer and freeing the pty once both are closed.
 */
VOID
PsxPtyClose(
    _Inout_ PPSX_FILE_OBJECT File)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN FreePty;

    if (Pty == NULL)
        return;

    RtlEnterCriticalSection(&Pty->Lock);
    if (File->FileType == PSX_FILE_PTMX)
    {
        /* Slave reads now see end of file */
        Pty->MasterRefs--;
        if (Pty->MasterRefs <= 0)
            NtSetEvent(Pty->M2SData, NULL);
    }
    else
    {
        /* Master reads now see end of file */
        Pty->SlaveRefs--;
        if (Pty->SlaveRefs <= 0)
            NtSetEvent(Pty->S2MData, NULL);
    }

    FreePty = (Pty->MasterRefs <= 0) && (Pty->SlaveRefs <= 0);
    RtlLeaveCriticalSection(&Pty->Lock);
    File->Pty = NULL;

    if (FreePty)
    {
        if (Pty->M2SData)
            NtClose(Pty->M2SData);

        if (Pty->S2MData)
            NtClose(Pty->S2MData);

        RtlDeleteCriticalSection(&Pty->Lock);
        RtlEnterCriticalSection(&PsxPtyTableLock);
        Pty->InUse = FALSE;
        RtlLeaveCriticalSection(&PsxPtyTableLock);
    }
}

/**
 * @brief Handle an ioctl on a pty descriptor.
 * @return 0 or 1 on success, -1 with Errno set on failure.
 */
LONG
PsxPtyIoctl(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _In_ ULONG Request,
    _In_ ULONG_PTR Arg,
    _Out_ PULONG Errno)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    UCHAR Termios[PSX_TERMIOS_SIZE];
    USHORT WinSize[4];
    ULONG PtsIndex;
    NTSTATUS Status;

    if (Pty == NULL)
    {
        *Errno = PSX_ENOTTY;
        return -1;
    }

    *Errno = 0;

    switch (Request)
    {
        case PSX_IOCTL_TIOCGPTN:
            /* Return the pts index as an int */
            PtsIndex = Pty->Index;
            Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                          (PVOID)Arg,
                                          &PtsIndex,
                                          sizeof(PtsIndex),
                                          NULL);
            if (!NT_SUCCESS(Status))
            {
                *Errno = PSX_EINVAL;
                return -1;
            }
            return 0;

        case PSX_IOCTL_TIOCSWINSZ:
            /* struct winsize is four USHORTs */
            Status = NtReadVirtualMemory(Process->ProcessHandle,
                                         (PVOID)Arg,
                                         WinSize,
                                         sizeof(WinSize),
                                         NULL);
            if (!NT_SUCCESS(Status))
            {
                *Errno = PSX_EINVAL;
                return -1;
            }

            RtlEnterCriticalSection(&Pty->Lock);
            Pty->WsRow = WinSize[0];
            Pty->WsCol = WinSize[1];
            Pty->WsXpixel = WinSize[2];
            Pty->WsYpixel = WinSize[3];
            RtlLeaveCriticalSection(&Pty->Lock);
            return 0;

        case PSX_IOCTL_TIOCGWINSZ:
            WinSize[0] = Pty->WsRow;
            WinSize[1] = Pty->WsCol;
            WinSize[2] = Pty->WsXpixel;
            WinSize[3] = Pty->WsYpixel;
            Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                          (PVOID)Arg,
                                          WinSize,
                                          sizeof(WinSize),
                                          NULL);
            if (!NT_SUCCESS(Status))
            {
                *Errno = PSX_EINVAL;
                return -1;
            }
            return 0;

        case PSX_IOCTL_TIOCSCTTY:
            /* Accepted; session handling is done elsewhere */
            return 0;

        case PSX_IOCTL_I_PUSH:
        case PSX_IOCTL_I_POP:
        case PSX_IOCTL_I_STR:
            /* The pty already acts as the full STREAMS stack; terminal emulators exit if these fail */
            return 0;

        case PSX_IOCTL_I_FIND:
            /* Report every module as present */
            return 1;

        case PSX_IOCTL_TCGETS:
            PsxPtyTermios(File, FALSE, Termios);
            Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                          (PVOID)Arg,
                                          Termios,
                                          sizeof(Termios),
                                          NULL);
            if (!NT_SUCCESS(Status))
            {
                *Errno = PSX_EINVAL;
                return -1;
            }
            return 0;

        case PSX_IOCTL_TCSETS:
        case PSX_IOCTL_TCSETSW:
        case PSX_IOCTL_TCSETSF:
            Status = NtReadVirtualMemory(Process->ProcessHandle,
                                         (PVOID)Arg,
                                         Termios,
                                         sizeof(Termios),
                                         NULL);
            if (!NT_SUCCESS(Status))
            {
                *Errno = PSX_EINVAL;
                return -1;
            }

            PsxPtyTermios(File, TRUE, Termios);
            return 0;

        default:
            *Errno = PSX_ENOTTY;
            return -1;
    }
}

/**
 * @brief Get or set the termios of a pty end.
 * @return TRUE if the descriptor is a pty and was handled here, FALSE otherwise.
 */
BOOLEAN
PsxPtyTermios(
    _In_opt_ PPSX_FILE_OBJECT File,
    _In_ BOOLEAN Set,
    _Inout_updates_bytes_(PSX_TERMIOS_SIZE) PUCHAR Blob68)
{
    PPSX_PTY Pty;

    if ((File == NULL) ||
        ((File->FileType != PSX_FILE_PTS) && (File->FileType != PSX_FILE_PTMX)))
    {
        return FALSE;
    }

    Pty = (PPSX_PTY)File->Pty;
    if (Pty == NULL)
        return TRUE;

    RtlEnterCriticalSection(&Pty->Lock);
    if (Set)
        RtlCopyMemory(Pty->Termios, Blob68, PSX_TERMIOS_SIZE);
    else
        RtlCopyMemory(Blob68, Pty->Termios, PSX_TERMIOS_SIZE);
    RtlLeaveCriticalSection(&Pty->Lock);

    return TRUE;
}

/**
 * @brief ioctl(fd, request, arg), PSX_API_IOCTL.
 *
 * Only pty descriptors are supported; anything else fails with ENOTTY.
 */
VOID
PsxSrvIoctl(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    INT Fd = (INT)Args[0];
    ULONG Request = Args[1];
    ULONG_PTR Arg = (ULONG_PTR)Args[2];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    ULONG Errno = 0;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    if ((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS))
    {
        Message->ReturnValue = PsxPtyIoctl(Process, File, Request, Arg, &Errno);
        Message->Errno = Errno;
        return;
    }

    Message->Errno = PSX_ENOTTY;
    Message->ReturnValue = -1;
}

/**
 * @brief select() readability poll, PSX_API_SELECT.
 *
 * Body: descriptor count, timeout in ms, and a client pointer to a ULONG descriptor array.
 * ReturnValue is a bitmask with bit i set when descriptor i is readable.
 * psxss serves all clients from one thread, so this never waits; the client repeats the
 * poll until its timeout expires.
 */
VOID
PsxSrvSelect(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG FdCount = Args[0];
    ULONG_PTR FdArray = (ULONG_PTR)Args[2];
    ULONG Fds[PSX_SELECT_MAXFDS];
    ULONG ReadyMask = 0;
    PPSX_FILE_OBJECT File;
    NTSTATUS Status;
    ULONG Index;

    if (FdCount > PSX_SELECT_MAXFDS)
        FdCount = PSX_SELECT_MAXFDS;

    if (FdCount > 0)
    {
        Status = NtReadVirtualMemory(Process->ProcessHandle,
                                     (PVOID)FdArray,
                                     Fds,
                                     FdCount * sizeof(ULONG),
                                     NULL);
        if (!NT_SUCCESS(Status))
        {
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
        }
    }

    for (Index = 0; Index < FdCount; Index++)
    {
        File = PsxGetFile(Process, (INT)Fds[Index]);
        if ((File != NULL) && PsxPollReady(File))
            ReadyMask |= (1u << Index);
    }

    Message->Errno = 0;
    Message->ReturnValue = (LONG)ReadyMask;
}
