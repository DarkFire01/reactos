/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pseudo-terminals -- /dev/ptmx (master) + /dev/pts/N (slave). A pty
 *              is two in-server ring buffers connecting a master (the terminal
 *              emulator, dtterm) and a slave (the shell): master writes -> slave
 *              reads (keyboard), slave writes -> master reads (program output).
 *              Plus the slave's termios (line discipline) and window size. This
 *              first cut is a RAW conduit -- no canonical/echo/signal line
 *              discipline; interactive readline supplies its own echo/editing.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/exfuncs.h>    // NtCreateEvent / NtSetEvent / NtClearEvent / NtWaitForSingleObject

#define PSX_ENOMEM   12
#define PSX_EMFILE   24
#define PSX_EINVAL   22
#define PSX_ENXIO    6
#define PSX_EIO      5
#define PSX_ENOTTY   25

#define PSX_PTY_SIZE   4096
#define PSX_PTY_MAX    16       // concurrent pseudo-terminals
#define PSX_TERMIOS_SIZE  68    // matches the client termios (tty.c second phase)

// pty ioctls (values mirror sdk/include/psx/termios.h; psxss has its own header world)
#define IO_TCGETS      0x5401   // tcgetattr on a pts (arg -> 68-byte termios)
#define IO_TCSETS      0x5402   // tcsetattr (TCSANOW)
#define IO_TCSETSW     0x5403   // tcsetattr (TCSADRAIN)
#define IO_TCSETSF     0x5404   // tcsetattr (TCSAFLUSH)
#define IO_TIOCSCTTY   0x540E
#define IO_TIOCGWINSZ  0x5413
#define IO_TIOCSWINSZ  0x5414
#define IO_TIOCGPTN    0x80045430

typedef struct _PSX_PTY
{
    RTL_CRITICAL_SECTION Lock;
    BOOLEAN InUse;
    ULONG   Index;
    LONG    MasterRefs;
    LONG    SlaveRefs;
    // master -> slave (the shell reads this: keyboard input)
    ULONG   M2SCount, M2SRead, M2SWrite;
    HANDLE  M2SData;                    // signalled when M2S has data / master closed
    UCHAR   M2S[PSX_PTY_SIZE];
    // slave -> master (the terminal reads this: program output)
    ULONG   S2MCount, S2MRead, S2MWrite;
    HANDLE  S2MData;                    // signalled when S2M has data / slave closed
    UCHAR   S2M[PSX_PTY_SIZE];
    // slave line discipline + geometry
    UCHAR   Termios[PSX_TERMIOS_SIZE];  // opaque client termios blob
    USHORT  WsRow, WsCol, WsXpixel, WsYpixel;
} PSX_PTY, *PPSX_PTY;

static PSX_PTY g_PtyTable[PSX_PTY_MAX];
static RTL_CRITICAL_SECTION g_PtyTableLock;
static LONG g_PtyTableInit = 0;

static VOID
PsxPtyInitTable(VOID)
{
    if (InterlockedCompareExchange(&g_PtyTableInit, 1, 0) == 0)
        RtlInitializeCriticalSection(&g_PtyTableLock);
}

static HANDLE
PsxPtyGate(VOID)
{
    OBJECT_ATTRIBUTES Oa;
    HANDLE H = NULL;
    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);
    NtCreateEvent(&H, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    return H;
}

//
// Allocate a free pty slot and return it locked-out (InUse set). NULL if full.
//
static PPSX_PTY
PsxPtyAllocate(VOID)
{
    ULONG i;
    PPSX_PTY Pty = NULL;

    PsxPtyInitTable();
    RtlEnterCriticalSection(&g_PtyTableLock);
    for (i = 0; i < PSX_PTY_MAX; i++)
    {
        if (!g_PtyTable[i].InUse)
        {
            Pty = &g_PtyTable[i];
            RtlZeroMemory(Pty, sizeof(*Pty));
            Pty->InUse = TRUE;
            Pty->Index = i;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_PtyTableLock);
    if (Pty == NULL)
        return NULL;

    RtlInitializeCriticalSection(&Pty->Lock);
    Pty->M2SData = PsxPtyGate();
    Pty->S2MData = PsxPtyGate();
    Pty->MasterRefs = 1;
    Pty->SlaveRefs = 0;
    Pty->WsRow = 24; Pty->WsCol = 80;
    // Seed a sane cooked-mode termios (matches psxdll's tcgetattr default) so the
    // shell's first tcgetattr(slave) sees ISIG|ICANON|ECHO, not a zeroed blob.
    {
        PULONG T = (PULONG)Pty->Termios;
        T[0] = 0x0500;   // c_iflag: ICRNL | IXON
        T[1] = 0x0005;   // c_oflag: OPOST | ONLCR
        T[2] = 0x00B0;   // c_cflag: CS8 | CREAD
        T[3] = 0x001B;   // c_lflag: ISIG | ICANON | ECHO | ECHOE
        T[4] = 13; T[5] = 13;                       // ispeed/ospeed = B9600
        Pty->Termios[24] = 3;   Pty->Termios[25] = 28;   // VINTR=^C, VQUIT=FS
        Pty->Termios[26] = 127; Pty->Termios[27] = 21;   // VERASE=DEL, VKILL=^U
        Pty->Termios[28] = 4;                            // VEOF ^D
    }
    if ((Pty->M2SData == NULL) || (Pty->S2MData == NULL))
    {
        if (Pty->M2SData) NtClose(Pty->M2SData);
        if (Pty->S2MData) NtClose(Pty->S2MData);
        RtlDeleteCriticalSection(&Pty->Lock);
        Pty->InUse = FALSE;
        return NULL;
    }
    return Pty;
}

//
// open("/dev/ptmx"): allocate a pty pair, return the master fd.
//
INT
PsxPtyOpenMaster(IN PPSX_PROCESS Process, IN ULONG OpenFlags)
{
    PPSX_PTY Pty = PsxPtyAllocate();
    PPSX_FILE_OBJECT File;
    INT Fd;

    if (Pty == NULL)
        return -PSX_EMFILE;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
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

//
// open("/dev/pts/N"): return the slave fd of pty N (must have a live master).
//
INT
PsxPtyOpenSlave(IN PPSX_PROCESS Process, IN ULONG Index, IN ULONG OpenFlags)
{
    PPSX_PTY Pty;
    PPSX_FILE_OBJECT File;
    INT Fd;

    if (Index >= PSX_PTY_MAX)
        return -PSX_ENXIO;
    PsxPtyInitTable();
    Pty = &g_PtyTable[Index];
    if (!Pty->InUse || (Pty->MasterRefs <= 0))
        return -PSX_ENXIO;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
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

// Copy up to Count bytes out of a ring buffer into Dst; returns bytes taken.
static ULONG
PsxRingTake(PUCHAR Buf, PULONG Count, PULONG Read, PUCHAR Dst, ULONG Want)
{
    ULONG Take = (Want < *Count) ? Want : *Count;
    ULONG i;
    for (i = 0; i < Take; i++)
    {
        Dst[i] = Buf[*Read];
        *Read = (*Read + 1) % PSX_PTY_SIZE;
    }
    *Count -= Take;
    return Take;
}

// Copy up to Want bytes from Src into a ring buffer; returns bytes stored.
static ULONG
PsxRingPut(PUCHAR Buf, PULONG Count, PULONG Write, PUCHAR Src, ULONG Want)
{
    ULONG Done = 0;
    while ((Done < Want) && (*Count < PSX_PTY_SIZE))
    {
        Buf[*Write] = Src[Done++];
        *Write = (*Write + 1) % PSX_PTY_SIZE;
        (*Count)++;
    }
    return Done;
}

//
// read(pty). Master reads S2M (program output); slave reads M2S (keyboard).
// Blocks until data arrives, or the opposite end closes (EOF -> 0).
//
VOID
PsxPtyRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Master = (File->FileType == PSX_FILE_PTMX);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Got = 0;

    if ((Pty == NULL) || (Count == 0)) { Message->Errno = 0; Message->ReturnValue = 0; return; }
    if (Count > PSX_PTY_SIZE) Count = PSX_PTY_SIZE;
    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL) { Message->Errno = PSX_ENOMEM; Message->ReturnValue = -1; return; }

    for (;;)
    {
        RtlEnterCriticalSection(&Pty->Lock);
        if (Master)
        {
            if (Pty->S2MCount > 0)
            {
                Got = PsxRingTake(Pty->S2M, &Pty->S2MCount, &Pty->S2MRead, Bounce, Count);
                if (Pty->S2MCount == 0) NtClearEvent(Pty->S2MData);
                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }
            if (Pty->SlaveRefs == 0) { RtlLeaveCriticalSection(&Pty->Lock); break; }  // EOF
            RtlLeaveCriticalSection(&Pty->Lock);
            NtWaitForSingleObject(Pty->S2MData, FALSE, NULL);
        }
        else
        {
            if (Pty->M2SCount > 0)
            {
                Got = PsxRingTake(Pty->M2S, &Pty->M2SCount, &Pty->M2SRead, Bounce, Count);
                if (Pty->M2SCount == 0) NtClearEvent(Pty->M2SData);
                RtlLeaveCriticalSection(&Pty->Lock);
                break;
            }
            if (Pty->MasterRefs == 0) { RtlLeaveCriticalSection(&Pty->Lock); break; }  // EOF
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

//
// write(pty). Master writes M2S (keyboard to shell); slave writes S2M (output to
// terminal). Non-blocking-ish: stores what fits, drops the rest (a real terminal
// keeps up). Wakes the reader.
//
VOID
PsxPtyWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Master = (File->FileType == PSX_FILE_PTMX);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Done = 0;

    if ((Pty == NULL) || (Count == 0)) { Message->Errno = 0; Message->ReturnValue = 0; return; }
    if (Count > PSX_PTY_SIZE) Count = PSX_PTY_SIZE;
    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL) { Message->Errno = PSX_ENOMEM; Message->ReturnValue = -1; return; }
    if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Count, NULL)))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL; Message->ReturnValue = -1; return;
    }

    RtlEnterCriticalSection(&Pty->Lock);
    if (Master)
    {
        Done = PsxRingPut(Pty->M2S, &Pty->M2SCount, &Pty->M2SWrite, Bounce, Count);
        if (Done > 0) NtSetEvent(Pty->M2SData, NULL);
    }
    else
    {
        // Output post-processing on the shell->terminal path. With OPOST|ONLCR
        // set (the cooked default), map each '\n' to "\r\n" so program output
        // (ls, etc.) returns to column 0 on every line. Without it a bare '\n'
        // only moves down -> the "staircase" text. readline emits its own \r\n
        // for the prompt, and typically clears OPOST while in raw mode, so this
        // fires only for cooked-mode program output.
        ULONG Oflag = ((PULONG)Pty->Termios)[1];    // c_oflag
        if ((Oflag & 0x0001) && (Oflag & 0x0004))   // OPOST && ONLCR
        {
            static const UCHAR Cr = '\r';
            ULONG i;
            for (i = 0; i < Count; i++)
            {
                if (Bounce[i] == '\n')
                    PsxRingPut(Pty->S2M, &Pty->S2MCount, &Pty->S2MWrite, (PUCHAR)&Cr, 1);
                PsxRingPut(Pty->S2M, &Pty->S2MCount, &Pty->S2MWrite, &Bounce[i], 1);
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
    Message->Errno = 0;
    // A pty buffers; report the full write (any overflow past 4 KB is dropped --
    // fine for interactive use where the peer drains promptly).
    Message->ReturnValue = (LONG)Count;
}

//
// Readability for poll()/select(). Master ready if program output is buffered or
// the slave closed; slave ready if keyboard input is buffered or the master closed.
//
BOOLEAN
PsxPtyReady(IN PPSX_FILE_OBJECT File)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Ready;

    if (Pty == NULL) return TRUE;
    RtlEnterCriticalSection(&Pty->Lock);
    if (File->FileType == PSX_FILE_PTMX)
        Ready = (Pty->S2MCount > 0) || (Pty->SlaveRefs == 0);
    else
        Ready = (Pty->M2SCount > 0) || (Pty->MasterRefs == 0);
    RtlLeaveCriticalSection(&Pty->Lock);
    return Ready;
}

//
// A pty end (master or slave file object) is closing. Wake the peer; free the pty
// once both sides are gone.
//
VOID
PsxPtyClose(IN PPSX_FILE_OBJECT File)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    BOOLEAN Free;

    if (Pty == NULL) return;
    RtlEnterCriticalSection(&Pty->Lock);
    if (File->FileType == PSX_FILE_PTMX)
    {
        Pty->MasterRefs--;
        if (Pty->MasterRefs <= 0) NtSetEvent(Pty->M2SData, NULL);   // slave reads see EOF
    }
    else
    {
        Pty->SlaveRefs--;
        if (Pty->SlaveRefs <= 0) NtSetEvent(Pty->S2MData, NULL);    // master reads see EOF
    }
    Free = (Pty->MasterRefs <= 0) && (Pty->SlaveRefs <= 0);
    RtlLeaveCriticalSection(&Pty->Lock);
    File->Pty = NULL;

    if (Free)
    {
        if (Pty->M2SData) NtClose(Pty->M2SData);
        if (Pty->S2MData) NtClose(Pty->S2MData);
        RtlDeleteCriticalSection(&Pty->Lock);
        RtlEnterCriticalSection(&g_PtyTableLock);
        Pty->InUse = FALSE;
        RtlLeaveCriticalSection(&g_PtyTableLock);
    }
}

//
// ioctl on a pty fd. Returns 0/-1 and sets *Errno. Handles the pty control set;
// TCGETS/TCSETS (termios) are served by the tcgetattr/tcsetattr interception, not
// here.
//
LONG
PsxPtyIoctl(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN ULONG Request,
            IN ULONG_PTR Arg, OUT PULONG Errno)
{
    PPSX_PTY Pty = (PPSX_PTY)File->Pty;
    USHORT Ws[4];

    if (Pty == NULL) { *Errno = PSX_ENOTTY; return -1; }
    *Errno = 0;

    switch (Request)
    {
        case IO_TIOCGPTN:       // get pts index -> *(int *)Arg
        {
            ULONG N = Pty->Index;
            if (!NT_SUCCESS(NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)Arg, &N, sizeof(N), NULL)))
            { *Errno = PSX_EINVAL; return -1; }
            return 0;
        }
        case IO_TIOCSWINSZ:     // set window size from *(struct winsize *)Arg (4x USHORT)
            if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)Arg, Ws, sizeof(Ws), NULL)))
            { *Errno = PSX_EINVAL; return -1; }
            RtlEnterCriticalSection(&Pty->Lock);
            Pty->WsRow = Ws[0]; Pty->WsCol = Ws[1]; Pty->WsXpixel = Ws[2]; Pty->WsYpixel = Ws[3];
            RtlLeaveCriticalSection(&Pty->Lock);
            return 0;
        case IO_TIOCGWINSZ:     // get window size -> *(struct winsize *)Arg
            Ws[0] = Pty->WsRow; Ws[1] = Pty->WsCol; Ws[2] = Pty->WsXpixel; Ws[3] = Pty->WsYpixel;
            if (!NT_SUCCESS(NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)Arg, Ws, sizeof(Ws), NULL)))
            { *Errno = PSX_EINVAL; return -1; }
            return 0;
        case IO_TIOCSCTTY:      // make controlling tty -- accepted (session work is elsewhere)
            return 0;
        case IO_TCGETS:         // tcgetattr(pts) -> *(termios *)Arg (68 bytes)
        {
            UCHAR Blob[PSX_TERMIOS_SIZE];
            PsxPtyTermios(File, FALSE, Blob);
            if (!NT_SUCCESS(NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)Arg, Blob, sizeof(Blob), NULL)))
            { *Errno = PSX_EINVAL; return -1; }
            return 0;
        }
        case IO_TCSETS:
        case IO_TCSETSW:
        case IO_TCSETSF:        // tcsetattr(pts) <- *(termios *)Arg
        {
            UCHAR Blob[PSX_TERMIOS_SIZE];
            if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)Arg, Blob, sizeof(Blob), NULL)))
            { *Errno = PSX_EINVAL; return -1; }
            PsxPtyTermios(File, TRUE, Blob);
            return 0;
        }
        default:
            *Errno = PSX_ENOTTY;
            return -1;
    }
}

//
// tcgetattr/tcsetattr interception for pty slaves: copy the 68-byte termios blob
// to/from the pty (instead of the console leader's second phase). Returns TRUE if
// this fd is a pty (handled here), FALSE to fall through to the normal path.
//
BOOLEAN
PsxPtyTermios(IN PPSX_FILE_OBJECT File, IN BOOLEAN Set, IN OUT PUCHAR Blob68)
{
    PPSX_PTY Pty;

    if ((File == NULL) ||
        ((File->FileType != PSX_FILE_PTS) && (File->FileType != PSX_FILE_PTMX)))
        return FALSE;
    Pty = (PPSX_PTY)File->Pty;
    if (Pty == NULL) return TRUE;

    RtlEnterCriticalSection(&Pty->Lock);
    if (Set)
        RtlCopyMemory(Pty->Termios, Blob68, PSX_TERMIOS_SIZE);
    else
        RtlCopyMemory(Blob68, Pty->Termios, PSX_TERMIOS_SIZE);
    RtlLeaveCriticalSection(&Pty->Lock);
    return TRUE;
}

#define PSX_EBADF    9

//
// PSX_API_IOCTL (0x41): dispatch ioctl on a pty fd. Body: Raw[0]=fd, Raw[1]=request,
// Raw[2]=arg (client pointer). Non-pty fds -> ENOTTY.
//
VOID
PsxSrvIoctl(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    INT Fd = (INT)Args[0];
    ULONG Request = Args[1];
    ULONG_PTR Arg = (ULONG_PTR)Args[2];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    ULONG Errno = 0;

    if (File == NULL) { Message->Errno = PSX_EBADF; Message->ReturnValue = -1; return; }
    if ((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS))
    {
        Message->ReturnValue = PsxPtyIoctl(Process, File, Request, Arg, &Errno);
        Message->Errno = Errno;
        return;
    }
    Message->Errno = PSX_ENOTTY;
    Message->ReturnValue = -1;
}

//
// PSX_API_SELECT (0x42): wait for any of N read fds to become readable. Body:
// Raw[0]=N, Raw[1]=timeout ms, Raw[2]=client pointer to ULONG fds[N]. ReturnValue
// = bitmask of ready fds (bit i for fds[i]); 0 = timeout; -1 = error.
//
VOID
PsxSrvSelect(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    ULONG N = Args[0];
    ULONG Timeout = Args[1];
    ULONG_PTR FdArray = (ULONG_PTR)Args[2];
    ULONG Fds[PSX_SELECT_MAXFDS];
    ULONG ReadyMask = 0, Waited = 0, i;

    if (N > PSX_SELECT_MAXFDS) N = PSX_SELECT_MAXFDS;
    if (N > 0)
    {
        if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)FdArray,
                                            Fds, N * sizeof(ULONG), NULL)))
        {
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
        }
    }

    // Single non-blocking poll -- NEVER park this thread. psxss is a single-threaded
    // LPC server (server.c ApiServerLoop): a blocking wait here freezes EVERY other
    // POSIX client, so a busy dtterm (whose Xt loop select()s constantly) would stall
    // the whole subsystem. The wait/retry now lives in the client (psxdll select),
    // which re-issues this poll until its own timeout; we just report current readiness.
    (void)Timeout; (void)Waited;
    for (i = 0; i < N; i++)
    {
        PPSX_FILE_OBJECT F = PsxGetFile(Process, (INT)Fds[i]);
        if ((F != NULL) && PsxPollReady(F))
            ReadyMask |= (1u << i);
    }

    Message->Errno = 0;
    Message->ReturnValue = (LONG)ReadyMask;
}
