/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL controlling-terminal second phase. psxss does not move tty
 *              bytes -- for a PSX_FILE_TTY read/write/termios it flags HasData
 *              and returns, and the client exchanges the data DIRECTLY with the
 *              session leader (posix.exe/posixterm) over the per-session port.
 *              This is that client half: connect to \PSXSS\PSXSES\P<sid>, map the
 *              D<sid> data section, and drive the leader's IO protocol (selector
 *              0 sub-op 3=read / 4=write with the buffer at section offset 0;
 *              selector 2 sub-op 0=tcgetattr / 1=tcsetattr with the termios
 *              block at msg+0x28). Matches posixterm.c's P<pid> ServerThread.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

// SES_MSG body ULONG indices (msg offset = 0x18 + 4*i), matching posixterm.
#define SES_SELECTOR  0     // +0x18: 0=IO request, 1=exit, 2=termios
#define SES_ERRNO     1     // +0x1C: errno (reply)
#define SES_SUBOP     2     // +0x20: IO 1=open/3=read/4=write/7=isatty; termios 0=get/1=set
#define SES_FD        3     // +0x24: fd
#define SES_LEN       4     // +0x28: byte count in / out  (also the termios block)

#define SES_SEL_IO       0
#define SES_SEL_TERMIOS  2
#define SES_SUBOP_READ   3
#define SES_SUBOP_WRITE  4

#define SES_SECTION_SIZE 0x10000    // D<sid> section size (posixterm SECTION_SIZE)
#define PSX_TERMIOS_SIZE 0x44       // 68-byte struct termios

typedef struct _PSX_SES_MSG
{
    PORT_MESSAGE Header;
    ULONG        u[25];             // body: u[i] == msg+0x18+4*i
} PSX_SES_MSG;

static HANDLE PsxSessionPort = NULL;    // client handle to \PSXSS\PSXSES\P<sid>
static PVOID  PsxSessionData = NULL;    // D<sid> section view (data at offset 0)

//
// After fork(), the child must NOT reuse the parent's session-port connection or
// D<sid> mapping -- those are per-process LPC/view state that do not carry across
// the process boundary. Drop them (without touching the possibly-stale handle/
// view) so the child's first tty op reconnects and remaps its own.
//
VOID
PsxTtyForkReset(VOID)
{
    PsxSessionPort = NULL;
    PsxSessionData = NULL;
}

//
// Build "\PSXSS\PSXSES\<Kind><Id>" (Kind 'P' port / 'D' section) into Out.
//
static VOID
PsxSessionObjectName(PWSTR Out, WCHAR Kind, ULONG Id)
{
    static const WCHAR Prefix[] = L"\\PSXSS\\PSXSES\\";
    WCHAR Digits[16];
    ULONG i = 0, n = 0, v = Id;

    while (Prefix[i] != L'\0') { Out[i] = Prefix[i]; i++; }
    Out[i++] = Kind;
    if (v == 0)
        Digits[n++] = L'0';
    while (v != 0) { Digits[n++] = (WCHAR)(L'0' + (v % 10)); v /= 10; }
    while (n != 0)
        Out[i++] = Digits[--n];
    Out[i] = L'\0';
}

//
// Lazily connect to the leader's P<sid> port and map its D<sid> data section.
// Idempotent; returns STATUS_SUCCESS once wired.
//
static NTSTATUS
PsxConnectSession(VOID)
{
    WCHAR NameBuf[64];
    UNICODE_STRING Name;
    SECURITY_QUALITY_OF_SERVICE Qos;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Section;
    SIZE_T ViewSize;
    NTSTATUS Status;

    if (PsxSessionPort != NULL)
        return STATUS_SUCCESS;

    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;

    PsxSessionObjectName(NameBuf, L'P', PsxSessionId);
    RtlInitUnicodeString(&Name, NameBuf);
    Status = NtConnectPort(&PsxSessionPort, &Name, &Qos,
                           NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(Status)) { PsxSessionPort = NULL; return Status; }

    PsxSessionObjectName(NameBuf, L'D', PsxSessionId);
    RtlInitUnicodeString(&Name, NameBuf);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenSection(&Section, SECTION_MAP_READ | SECTION_MAP_WRITE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        NtClose(PsxSessionPort);
        PsxSessionPort = NULL;
        return Status;
    }

    ViewSize = 0;
    PsxSessionData = NULL;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), &PsxSessionData,
                                0, 0, NULL, &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    NtClose(Section);
    if (!NT_SUCCESS(Status))
    {
        NtClose(PsxSessionPort);
        PsxSessionPort = NULL;
        PsxSessionData = NULL;
        return Status;
    }
    return STATUS_SUCCESS;
}

//
// The controlling-tty read/write second phase (called when psxss flagged
// HasData). The payload rides the D<sid> section (offset 0); the leader reports
// the transferred count. Returns the byte count, or -1 with errno.
//
int
PsxTtyReadWrite(int IsWrite, int FileDescriptor, void *Buffer, unsigned int Count)
{
    PSX_SES_MSG Message;
    ULONG Length = Count;
    NTSTATUS Status;

    if (!NT_SUCCESS(PsxConnectSession())) { PsxSetErrno(5 /* EIO */); return -1; }
    if (Length > SES_SECTION_SIZE)
        Length = SES_SECTION_SIZE;

    if (IsWrite && Length != 0)
        RtlCopyMemory(PsxSessionData, Buffer, Length);

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x50;
    Message.Header.u1.s1.DataLength  = 0x38;
    Message.u[SES_SELECTOR] = SES_SEL_IO;
    Message.u[SES_SUBOP]    = IsWrite ? SES_SUBOP_WRITE : SES_SUBOP_READ;
    Message.u[SES_FD]       = (ULONG)FileDescriptor;
    Message.u[SES_LEN]      = Length;

    Status = NtRequestWaitReplyPort(PsxSessionPort, &Message.Header, &Message.Header);
    if (!NT_SUCCESS(Status)) { PsxSetErrno(5 /* EIO */); return -1; }
    if (Message.u[SES_ERRNO] != 0) { PsxSetErrno((LONG)Message.u[SES_ERRNO]); return -1; }

    Length = Message.u[SES_LEN];        // bytes actually transferred
    if (!IsWrite && Length != 0)
        RtlCopyMemory(Buffer, PsxSessionData, Length);

    return (int)Length;
}

//
// The controlling-tty termios second phase (called when psxss flagged HasData
// for tcgetattr/tcsetattr). The 68-byte termios travels inline in the message
// at msg+0x28 (&u[SES_LEN]). Returns 0, or -1 with errno.
//
int
PsxTtyTermios(int IsSet, void *Termios)
{
    PSX_SES_MSG Message;
    UCHAR *Block = (UCHAR *)&Message.u[SES_LEN];    // termios at msg+0x28
    NTSTATUS Status;

    if (!NT_SUCCESS(PsxConnectSession())) { PsxSetErrno(5 /* EIO */); return -1; }

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x6C;         // 0x18 header + 0x54 body (fits termios)
    Message.Header.u1.s1.DataLength  = 0x54;
    Message.u[SES_SELECTOR] = SES_SEL_TERMIOS;
    Message.u[SES_SUBOP]    = IsSet ? 1 : 0;         // 0=tcgetattr, 1=tcsetattr
    if (IsSet)
        RtlCopyMemory(Block, Termios, PSX_TERMIOS_SIZE);

    Status = NtRequestWaitReplyPort(PsxSessionPort, &Message.Header, &Message.Header);
    if (!NT_SUCCESS(Status)) { PsxSetErrno(5 /* EIO */); return -1; }
    if (Message.u[SES_ERRNO] != 0) { PsxSetErrno((LONG)Message.u[SES_ERRNO]); return -1; }

    if (!IsSet)
        RtlCopyMemory(Termios, Block, PSX_TERMIOS_SIZE);
    return 0;
}
