/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controlling terminal data exchange with the session leader
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * For controlling terminal I/O the server only validates the request and sets
 * HasData. The client then talks to the session leader (posixterm) directly over
 * \PSXSS\PSXSES\P<sid>, with read/write data in the D<sid> section at offset 0
 * and termios blocks inline in the message.
 */

#include "psxdllp.h"

/* Session message body ULONG indices (message offset 0x18 + 4 * index) */
#define SES_SELECTOR        0
#define SES_ERRNO           1
#define SES_SUBOP           2
#define SES_FD              3
#define SES_LENGTH          4

#define SES_SEL_IO          0
#define SES_SEL_TERMIOS     2
#define SES_SUBOP_READ      3
#define SES_SUBOP_WRITE     4
#define SES_SUBOP_TCGET     0
#define SES_SUBOP_TCSET     1

#define SES_SECTION_SIZE    0x10000
#define PSX_TERMIOS_SIZE    0x44
#define PSX_EIO             5

typedef struct _PSX_SESSION_MESSAGE
{
    PORT_MESSAGE Header;
    ULONG Body[25];
} PSX_SESSION_MESSAGE, *PPSX_SESSION_MESSAGE;

static HANDLE PsxSessionPort = NULL;
static PVOID PsxSessionData = NULL;

/**
 * @brief Forgets the parent's session port and data view in a fork child, so
 * the first terminal operation reconnects. The inherited handles are not touched.
 */
VOID
PsxTtyForkReset(VOID)
{
    PsxSessionPort = NULL;
    PsxSessionData = NULL;
}

/**
 * @brief Builds "\PSXSS\PSXSES\<Kind><Id>" into Out.
 */
static
VOID
PsxSessionObjectName(
    _Out_writes_z_(64) PWSTR Out,
    _In_ WCHAR Kind,
    _In_ ULONG Id)
{
    static const WCHAR Prefix[] = L"\\PSXSS\\PSXSES\\";
    WCHAR Digits[16];
    ULONG Length = 0;
    ULONG DigitCount = 0;
    ULONG Value = Id;

    while (Prefix[Length] != L'\0')
    {
        Out[Length] = Prefix[Length];
        Length++;
    }
    Out[Length++] = Kind;

    if (Value == 0)
        Digits[DigitCount++] = L'0';

    while (Value != 0)
    {
        Digits[DigitCount++] = (WCHAR)(L'0' + (Value % 10));
        Value /= 10;
    }

    while (DigitCount != 0)
        Out[Length++] = Digits[--DigitCount];

    Out[Length] = L'\0';
}

/**
 * @brief Connects to the session leader port and maps its data section on first use.
 */
static
NTSTATUS
PsxConnectSession(VOID)
{
    WCHAR NameBuffer[64];
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

    PsxSessionObjectName(NameBuffer, L'P', PsxSessionId);
    RtlInitUnicodeString(&Name, NameBuffer);
    Status = NtConnectPort(&PsxSessionPort,
                           &Name,
                           &Qos,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           NULL);
    if (!NT_SUCCESS(Status))
    {
        PsxSessionPort = NULL;
        return Status;
    }

    PsxSessionObjectName(NameBuffer, L'D', PsxSessionId);
    RtlInitUnicodeString(&Name, NameBuffer);
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
    Status = NtMapViewOfSection(Section,
                                NtCurrentProcess(),
                                &PsxSessionData,
                                0,
                                0,
                                NULL,
                                &ViewSize,
                                ViewUnmap,
                                0,
                                PAGE_READWRITE);
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

/**
 * @brief Moves controlling terminal read/write data through the session leader.
 *
 * @return The number of bytes transferred, or -1 with errno set.
 */
int
PsxTtyReadWrite(
    _In_ int IsWrite,
    _In_ int FileDescriptor,
    _Inout_updates_bytes_(Count) void *Buffer,
    _In_ unsigned int Count)
{
    PSX_SESSION_MESSAGE Message;
    ULONG Length = Count;
    NTSTATUS Status;

    if (!NT_SUCCESS(PsxConnectSession()))
    {
        PsxSetErrno(PSX_EIO);
        return -1;
    }

    if (Length > SES_SECTION_SIZE)
        Length = SES_SECTION_SIZE;

    if (IsWrite && Length != 0)
        RtlCopyMemory(PsxSessionData, Buffer, Length);

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x50;
    Message.Header.u1.s1.DataLength = 0x38;
    Message.Body[SES_SELECTOR] = SES_SEL_IO;
    Message.Body[SES_SUBOP] = IsWrite ? SES_SUBOP_WRITE : SES_SUBOP_READ;
    Message.Body[SES_FD] = (ULONG)FileDescriptor;
    Message.Body[SES_LENGTH] = Length;

    Status = NtRequestWaitReplyPort(PsxSessionPort, &Message.Header, &Message.Header);
    if (!NT_SUCCESS(Status))
    {
        PsxSetErrno(PSX_EIO);
        return -1;
    }
    if (Message.Body[SES_ERRNO] != 0)
    {
        PsxSetErrno((LONG)Message.Body[SES_ERRNO]);
        return -1;
    }

    Length = Message.Body[SES_LENGTH];
    if (!IsWrite && Length != 0)
        RtlCopyMemory(Buffer, PsxSessionData, Length);

    return (int)Length;
}

/**
 * @brief Gets or sets the controlling terminal termios through the session
 * leader. The termios block travels inline starting at Body[SES_LENGTH].
 *
 * @return 0, or -1 with errno set.
 */
int
PsxTtyTermios(
    _In_ int IsSet,
    _Inout_ void *Termios)
{
    PSX_SESSION_MESSAGE Message;
    PUCHAR Block = (PUCHAR)&Message.Body[SES_LENGTH];
    NTSTATUS Status;

    if (!NT_SUCCESS(PsxConnectSession()))
    {
        PsxSetErrno(PSX_EIO);
        return -1;
    }

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x6C;
    Message.Header.u1.s1.DataLength = 0x54;
    Message.Body[SES_SELECTOR] = SES_SEL_TERMIOS;
    Message.Body[SES_SUBOP] = IsSet ? SES_SUBOP_TCSET : SES_SUBOP_TCGET;
    if (IsSet)
        RtlCopyMemory(Block, Termios, PSX_TERMIOS_SIZE);

    Status = NtRequestWaitReplyPort(PsxSessionPort, &Message.Header, &Message.Header);
    if (!NT_SUCCESS(Status))
    {
        PsxSetErrno(PSX_EIO);
        return -1;
    }
    if (Message.Body[SES_ERRNO] != 0)
    {
        PsxSetErrno((LONG)Message.Body[SES_ERRNO]);
        return -1;
    }

    if (!IsSet)
        RtlCopyMemory(Termios, Block, PSX_TERMIOS_SIZE);

    return 0;
}
