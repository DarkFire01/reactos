/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX session leader and controlling terminal host
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdarg.h>
#include <stdio.h>

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <ndk/rtlfuncs.h>
#include <ndk/lpcfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>

#include <subsys/sm/smmsg.h>

#include "vt100.h"

/*
 * Every session message is a PORT_MESSAGE followed by a ULONG body, so Data[i] sits at
 * message offset 0x18 + 4 * i.
 */
typedef struct _SES_MSG
{
    PORT_MESSAGE Header;
    ULONG Data[25];
} SES_MSG, *PSES_MSG;

/* Body indexes on our \PSXSS\PSXSES\P<pid> server port */
#define IDX_SELECTOR    0   /* 0x18 */
#define IDX_ERRNO       1   /* 0x1C */
#define IDX_REQ0        2   /* 0x20 */
#define IDX_REQ1        3   /* 0x24 */
#define IDX_REQ2        4   /* 0x28 */
#define IDX_REQ3        5   /* 0x2C */

/* Body indexes on \PSXSS\SESPORT, where we are the client */
#define IDX_DISCRIM     1   /* 0x1C */
#define IDX_STATUS      2   /* 0x20, reply */
#define IDX_SIGNAL_PID  3   /* 0x24 */
#define IDX_SIGNAL_CODE 4   /* 0x28 */
#define IDX_LEADERPID   4   /* 0x28 */
#define IDX_SESSIONID   5   /* 0x2C */
#define IDX_FDCOUNT     7   /* 0x34 */
#define IDX_IMGOFF      8   /* 0x38 */
#define IDX_CWDOFF      9   /* 0x3C */
#define IDX_ARGVOFF     10  /* 0x40 */
#define IDX_ENVOFF      11  /* 0x44 */
#define IDX_SHAREDBASE  12  /* 0x48 */

#define SES_TOTAL_LENGTH 0x50
#define SES_DATA_LENGTH  0x38

#define SECTION_SIZE     0x10000
#define INBUF_SIZE       4096
#define TERMIOS_SIZE     0x44
#define TTY_EINVAL       22

/* Tty signal codes sent to SESPORT */
#define TTY_SIGNAL_INTR  0
#define TTY_SIGNAL_SUSP  1
#define TTY_SIGNAL_CLOSE 2

#define TERMIOS_IFLAG(t) (*(PULONG)((t) + 0x00))
#define TERMIOS_OFLAG(t) (*(PULONG)((t) + 0x04))
#define TERMIOS_CFLAG(t) (*(PULONG)((t) + 0x08))
#define TERMIOS_LFLAG(t) (*(PULONG)((t) + 0x0C))
#define TERMIOS_CC(t, i) ((t)[0x18 + (i)])

#define LFLAG_ECHO       0x01
#define LFLAG_ICANON     0x10
#define LFLAG_ISIG       0x40
#define IFLAG_ICRNL      0x02

static PCSTR ProgramName = "posixterm";

static HANDLE SectionHandle;
static PVOID SharedBase;
static SIZE_T ViewSize = SECTION_SIZE;
static HANDLE SesPort;
static HANDLE LeaderPort;
static ULONG SessionId;
static ULONG MyPid;
static ULONG ReplyContext;
static CRITICAL_SECTION SesLock;

static UCHAR Termios[TERMIOS_SIZE];

/* Cooked input ring buffer, filled by the input thread and drained by read requests */
static CRITICAL_SECTION InLock;
static HANDLE InReady;
static CHAR InBuf[INBUF_SIZE];
static INT InHead;
static INT InTail;
static BOOL InEof;

static HANDLE ConInHandle;

static
VOID
SendTtySignal(
    _In_ ULONG Code);

static
VOID
Trace(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Buffer[512];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(Buffer, sizeof(Buffer) - 1, Format, Args);
    Buffer[sizeof(Buffer) - 1] = 0;
    va_end(Args);
    OutputDebugStringA(Buffer);
}

static
VOID
InBufPush(
    _In_ CHAR Char)
{
    EnterCriticalSection(&InLock);
    if (((InTail + 1) % INBUF_SIZE) != InHead)
    {
        InBuf[InTail] = Char;
        InTail = (InTail + 1) % INBUF_SIZE;
    }
    LeaveCriticalSection(&InLock);
    SetEvent(InReady);
}

static
VOID
EmitToConsole(
    _In_reads_(Length) PCSTR Buffer,
    _In_ ULONG Length)
{
    vtProcessedTextOut((PCHAR)Buffer, (INT)Length);
}

static
VOID
EmitCharToConsole(
    _In_ CHAR Char)
{
    CHAR Echo[1];

    /* The emulator may translate the buffer in place, so echo a copy */
    Echo[0] = Char;
    EmitToConsole(Echo, 1);
}

/**
 * @brief Reads console keys and runs them through the line discipline into the input buffer.
 */
static
DWORD
WINAPI
InputThread(
    _In_ LPVOID Parameter)
{
    INPUT_RECORD Records[32];
    DWORD Count;
    DWORD i;
    CHAR Line[INBUF_SIZE];
    INT LineLength = 0;

    UNREFERENCED_PARAMETER(Parameter);

    for (;;)
    {
        if (!ReadConsoleInputA(ConInHandle, Records, 32, &Count))
            break;

        for (i = 0; i < Count; i++)
        {
            CHAR Char;
            BOOL Canonical;
            BOOL Echo;
            WORD Repeat;

            if (Records[i].EventType != KEY_EVENT || !Records[i].Event.KeyEvent.bKeyDown)
                continue;

            Char = Records[i].Event.KeyEvent.uChar.AsciiChar;
            Repeat = Records[i].Event.KeyEvent.wRepeatCount;
            if (Repeat == 0)
                Repeat = 1;

            Canonical = (TERMIOS_LFLAG(Termios) & LFLAG_ICANON) != 0;
            Echo = (TERMIOS_LFLAG(Termios) & LFLAG_ECHO) != 0;

            if (Char == 0)
            {
                /* Non character keys become VT100 sequences in raw mode only */
                PCSTR Sequence = NULL;

                switch (Records[i].Event.KeyEvent.wVirtualKeyCode)
                {
                    case VK_UP:
                        Sequence = "\x1B[A";
                        break;
                    case VK_DOWN:
                        Sequence = "\x1B[B";
                        break;
                    case VK_RIGHT:
                        Sequence = "\x1B[C";
                        break;
                    case VK_LEFT:
                        Sequence = "\x1B[D";
                        break;
                    case VK_HOME:
                        Sequence = "\x1B[H";
                        break;
                    case VK_END:
                        Sequence = "\x1B[F";
                        break;
                    case VK_DELETE:
                        Sequence = "\x1B[3~";
                        break;
                }

                if (Sequence != NULL && !Canonical)
                {
                    while (Repeat--)
                    {
                        PCSTR Current;

                        for (Current = Sequence; *Current; Current++)
                            InBufPush(*Current);
                    }
                }
                continue;
            }

            if (!Canonical)
            {
                /* ICRNL is an input flag and applies regardless of ICANON */
                if (Char == '\r' && (TERMIOS_IFLAG(Termios) & IFLAG_ICRNL))
                    Char = '\n';

                while (Repeat--)
                {
                    if (Echo)
                        EmitCharToConsole(Char);
                    InBufPush(Char);
                }
                continue;
            }

            /* Canonical mode: key repeat only applies to printable characters */
            if (Char == '\r' || Char == '\n')
            {
                INT k;

                if (Echo)
                    EmitToConsole("\r\n", 2);
                for (k = 0; k < LineLength; k++)
                    InBufPush(Line[k]);
                InBufPush('\n');
                LineLength = 0;
            }
            else if (Char == '\b' || Char == 0x7F)
            {
                /* ERASE */
                if (LineLength > 0)
                {
                    LineLength--;
                    if (Echo)
                        EmitToConsole("\b \b", 3);
                }
            }
            else if (Char == 0x04)
            {
                /* ^D ends the input */
                INT k;

                for (k = 0; k < LineLength; k++)
                    InBufPush(Line[k]);
                LineLength = 0;
                EnterCriticalSection(&InLock);
                InEof = TRUE;
                LeaveCriticalSection(&InLock);
                SetEvent(InReady);
            }
            else if (Char == 0x03)
            {
                /* ^C sends SIGINT */
                if (Echo)
                    EmitToConsole("^C\r\n", 4);
                LineLength = 0;
                SendTtySignal(TTY_SIGNAL_INTR);
            }
            else
            {
                while (Repeat--)
                {
                    if (LineLength < (INT)sizeof(Line) - 1)
                    {
                        Line[LineLength++] = Char;
                        if (Echo)
                            EmitCharToConsole(Char);
                    }
                }
            }
        }
    }

    return 0;
}

/**
 * @brief Blocks until input or end of file is available, then drains up to Length bytes.
 *
 * @return The number of bytes read, 0 at end of file.
 */
static
ULONG
TtyRead(
    _Out_writes_bytes_to_(Length, return) PCHAR Destination,
    _In_ ULONG Length)
{
    ULONG Read = 0;

    for (;;)
    {
        EnterCriticalSection(&InLock);
        while (Read < Length && InHead != InTail)
        {
            Destination[Read++] = InBuf[InHead];
            InHead = (InHead + 1) % INBUF_SIZE;
        }

        if (Read > 0)
        {
            LeaveCriticalSection(&InLock);
            return Read;
        }

        if (InEof)
        {
            /* End of file is reported once */
            InEof = FALSE;
            LeaveCriticalSection(&InLock);
            return 0;
        }

        LeaveCriticalSection(&InLock);
        WaitForSingleObject(InReady, INFINITE);
    }
}

/**
 * @brief Asks psxss to deliver a controlling tty signal to the session.
 *
 * @param Code
 * 0 for INTR, 1 for SUSP, 2 for close, 3 for QUIT.
 */
static
VOID
SendTtySignal(
    _In_ ULONG Code)
{
    SES_MSG Message;

    if (SesPort == NULL)
        return;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = SES_TOTAL_LENGTH;
    Message.Header.u1.s1.DataLength = SES_DATA_LENGTH;
    Message.Data[IDX_SELECTOR] = ReplyContext;
    Message.Data[IDX_DISCRIM] = 1;
    Message.Data[IDX_SIGNAL_PID] = MyPid;
    Message.Data[IDX_SIGNAL_CODE] = Code;

    EnterCriticalSection(&SesLock);
    NtRequestWaitReplyPort(SesPort, &Message.Header, &Message.Header);
    LeaveCriticalSection(&SesLock);
}

/**
 * @brief Forwards console control events to the POSIX session and keeps this process alive.
 */
static
BOOL
WINAPI
CtrlHandler(
    _In_ DWORD Type)
{
    if (Type == CTRL_C_EVENT)
    {
        SendTtySignal(TTY_SIGNAL_INTR);
        return TRUE;
    }

    if (Type == CTRL_BREAK_EVENT)
    {
        SendTtySignal(TTY_SIGNAL_SUSP);
        return TRUE;
    }

    if (Type == CTRL_CLOSE_EVENT)
    {
        SendTtySignal(TTY_SIGNAL_CLOSE);
        return TRUE;
    }

    return FALSE;
}

static
BOOL
DispatchIoRequest(
    _Inout_ PSES_MSG Message)
{
    PCHAR Shared = (PCHAR)SharedBase;
    ULONG Length;

    Message->Data[IDX_ERRNO] = 0;
    switch (Message->Data[IDX_REQ0])
    {
        case 1:
            /* Open the controlling tty as fd 0 */
            Message->Data[IDX_REQ1] = 0;
            break;

        case 3:
        {
            /* Read into the shared section */
            Length = Message->Data[IDX_REQ2];
            if (Length > SECTION_SIZE)
                Length = SECTION_SIZE;
            Message->Data[IDX_REQ2] = TtyRead(Shared, Length);
            break;
        }

        case 4:
        {
            /* Write from the shared section */
            Length = Message->Data[IDX_REQ2];
            if (Length > SECTION_SIZE)
                Length = SECTION_SIZE;
            EmitToConsole(Shared, Length);
            Message->Data[IDX_REQ2] = Length;
            break;
        }

        case 6:
        case 7:
            /* isatty */
            Message->Data[IDX_REQ2] = 1;
            break;

        default:
            Message->Data[IDX_ERRNO] = TTY_EINVAL;
            break;
    }

    return TRUE;
}

static
BOOL
DispatchExit(
    _Inout_ PSES_MSG Message)
{
    Message->Data[IDX_ERRNO] = 0;
    if (Message->Data[IDX_REQ0] == 0)
        return FALSE;

    return TRUE;
}

static
BOOL
DispatchTcSetAttr(
    _Inout_ PSES_MSG Message)
{
    /* The termios block starts at message offset 0x28 */
    PUCHAR Block = (PUCHAR)&Message->Data[IDX_REQ2];
    DWORD Mode = 0;

    Message->Data[IDX_ERRNO] = 0;
    if (Message->Data[IDX_REQ0] == 0)
    {
        /* tcgetattr */
        memcpy(Block, Termios, TERMIOS_SIZE);
    }
    else if (Message->Data[IDX_REQ0] == 1)
    {
        /* tcsetattr */
        memcpy(Termios, Block, TERMIOS_SIZE);
        if (TERMIOS_LFLAG(Termios) & LFLAG_ICANON)
            Mode |= ENABLE_LINE_INPUT;
        if (TERMIOS_LFLAG(Termios) & LFLAG_ECHO)
            Mode |= ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT;
        if (TERMIOS_LFLAG(Termios) & LFLAG_ISIG)
            Mode |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(ConInHandle, Mode);
    }
    else
    {
        Message->Data[IDX_ERRNO] = TTY_EINVAL;
    }

    return TRUE;
}

/**
 * @brief Services tty requests on the P<pid> port and exits the process on session teardown.
 */
static
DWORD
WINAPI
ServerThread(
    _In_ LPVOID Parameter)
{
    SES_MSG Request;
    PSES_MSG Reply = NULL;
    NTSTATUS Status;
    BOOL Running = TRUE;

    UNREFERENCED_PARAMETER(Parameter);
    Trace("%s: P<pid> server started\n", ProgramName);

    while (Running)
    {
        Status = NtReplyWaitReceivePort(LeaderPort,
                                        NULL,
                                        Reply ? &Reply->Header : NULL,
                                        &Request.Header);
        if (!NT_SUCCESS(Status))
        {
            Reply = NULL;
            continue;
        }

        switch (Request.Header.u2.s2.Type & 0xFF)
        {
            case LPC_CONNECTION_REQUEST:
            {
                /* Accept psxss and child connections. Requests arrive on the listening port. */
                HANDLE CommPort = NULL;

                NtAcceptConnectPort(&CommPort, NULL, &Request.Header, TRUE, NULL, NULL);
                if (CommPort)
                    NtCompleteConnectPort(CommPort);
                Reply = NULL;
                continue;
            }

            case LPC_PORT_CLOSED:
            case LPC_CLIENT_DIED:
                Reply = NULL;
                continue;

            case LPC_REQUEST:
            default:
                break;
        }

        switch (Request.Data[IDX_SELECTOR])
        {
            case 0:
                Running = DispatchIoRequest(&Request);
                break;
            case 1:
                Running = DispatchExit(&Request);
                break;
            case 2:
                Running = DispatchTcSetAttr(&Request);
                break;
            default:
                Request.Data[IDX_ERRNO] = TTY_EINVAL;
                break;
        }
        Reply = &Request;
    }

    Trace("%s: P<pid> server exiting (teardown)\n", ProgramName);
    ExitProcess(0);
    return 0;
}

/**
 * @brief Asks SM to load the deferred POSIX subsystem.
 *
 * Starting a Win32 image does not load it the way starting a POSIX image does.
 */
static
VOID
EnsurePsxSs(VOID)
{
    HANDLE SmPort = NULL;
    UNICODE_STRING Posix;
    NTSTATUS Status;

    Status = SmConnectToSm(NULL, NULL, 0, &SmPort);
    if (!NT_SUCCESS(Status) || SmPort == NULL)
    {
        Trace("%s: SmConnectToSm %08x\n", ProgramName, Status);
        return;
    }

    RtlInitUnicodeString(&Posix, L"Posix");
    Status = SmLoadDeferedSubsystem(SmPort, &Posix);
    Trace("%s: SmLoadDeferedSubsystem(Posix) %08x\n", ProgramName, Status);
    NtClose(SmPort);
}

/**
 * @brief Creates the D<pid> section and the P<pid> port, then starts the server thread.
 */
static
NTSTATUS
CreateSessionObjects(VOID)
{
    NTSTATUS Status;
    WCHAR NameBuffer[64];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER Size;
    HANDLE Thread;

    _snwprintf(NameBuffer, ARRAYSIZE(NameBuffer) - 1, L"\\PSXSS\\PSXSES\\D%u", MyPid);
    NameBuffer[ARRAYSIZE(NameBuffer) - 1] = 0;
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, NULL);
    Size.QuadPart = SECTION_SIZE;

    /* Commit the whole section up front so writes to the marshalling area never fault */
    Status = NtCreateSection(&SectionHandle,
                             SECTION_MAP_READ | SECTION_MAP_WRITE,
                             &ObjectAttributes,
                             &Size,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL);
    if (!NT_SUCCESS(Status))
    {
        Trace("%s: NtCreateSection(D) %08x\n", ProgramName, Status);
        return Status;
    }

    SharedBase = NULL;
    Status = NtMapViewOfSection(SectionHandle,
                                NtCurrentProcess(),
                                &SharedBase,
                                0,
                                0,
                                NULL,
                                &ViewSize,
                                ViewUnmap,
                                0,
                                PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        Trace("%s: NtMapViewOfSection %08x\n", ProgramName, Status);
        return Status;
    }

    _snwprintf(NameBuffer, ARRAYSIZE(NameBuffer) - 1, L"\\PSXSS\\PSXSES\\P%u", MyPid);
    NameBuffer[ARRAYSIZE(NameBuffer) - 1] = 0;
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, NULL);
    Status = NtCreatePort(&LeaderPort, &ObjectAttributes, 4, 0x70, 0x10000);
    if (!NT_SUCCESS(Status))
    {
        Trace("%s: NtCreatePort(P) %08x\n", ProgramName, Status);
        return Status;
    }

    Thread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    if (Thread == NULL)
        return STATUS_UNSUCCESSFUL;

    CloseHandle(Thread);
    return STATUS_SUCCESS;
}

/**
 * @brief Connects to \PSXSS\SESPORT as the session leader.
 */
static
NTSTATUS
ConnectSesPort(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING Name;
    SECURITY_QUALITY_OF_SERVICE Qos;
    ULONG ConnectInfo = MyPid;
    ULONG ConnectInfoLength = sizeof(ConnectInfo);

    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;

    RtlInitUnicodeString(&Name, L"\\PSXSS\\SESPORT");
    Status = NtConnectPort(&SesPort,
                           &Name,
                           &Qos,
                           NULL,
                           NULL,
                           NULL,
                           &ConnectInfo,
                           &ConnectInfoLength);
    if (!NT_SUCCESS(Status))
    {
        Trace("%s: NtConnectPort(SESPORT) %08x\n", ProgramName, Status);
        return Status;
    }

    /* psxss returns the assigned session id in the connect data */
    SessionId = ConnectInfo;
    Trace("%s: registered, session id %u\n", ProgramName, SessionId);
    return STATUS_SUCCESS;
}

static
ULONG
AlignUp4(
    _In_ ULONG Value)
{
    return (Value + 3) & ~3u;
}

/**
 * @brief Returns TRUE if <Drive>:\bin exists, which marks the volume holding the POSIX tree.
 */
static
BOOL
DriveHasPosixTree(
    _In_ CHAR Drive)
{
    CHAR Probe[8];
    DWORD Attributes;

    Probe[0] = Drive;
    Probe[1] = ':';
    Probe[2] = '\\';
    Probe[3] = 'b';
    Probe[4] = 'i';
    Probe[5] = 'n';
    Probe[6] = 0;

    Attributes = GetFileAttributesA(Probe);
    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/**
 * @brief Picks the drive holding the POSIX tree.
 *
 * psxss derives the POSIX root from the drive of the spawn cwd, so this choice is the root.
 * Tries the current directory's drive, then SystemDrive, then every drive.
 */
static
CHAR
PickPosixDrive(VOID)
{
    CHAR Buffer[MAX_PATH];
    DWORD Mask;
    INT i;

    if (GetCurrentDirectoryA(sizeof(Buffer), Buffer) >= 2 &&
        Buffer[1] == ':' &&
        DriveHasPosixTree(Buffer[0]))
    {
        return Buffer[0];
    }

    if (GetEnvironmentVariableA("SystemDrive", Buffer, sizeof(Buffer)) >= 2 &&
        Buffer[1] == ':' &&
        DriveHasPosixTree(Buffer[0]))
    {
        return Buffer[0];
    }

    Mask = GetLogicalDrives();
    for (i = 0; i < 26; i++)
    {
        if ((Mask & (1u << i)) && DriveHasPosixTree((CHAR)('A' + i)))
            return (CHAR)('A' + i);
    }

    /* Nothing found, fall back to C: and let the spawn fail visibly */
    return 'C';
}

/**
 * @brief Resolves a command to a DOS image path on the POSIX volume.
 *
 * A bare name maps to <root>:\bin\name, a POSIX absolute path is placed on the POSIX drive,
 * and a DOS style path is used as given. ".exe" is appended only if the path does not exist.
 */
static
VOID
ResolveImage(
    _In_z_ PCSTR Command,
    _In_ CHAR PosixDrive,
    _Out_writes_z_(DosPathSize) PCHAR DosPath,
    _In_ SIZE_T DosPathSize)
{
    SIZE_T Length;
    SIZE_T i;

    if (Command[0] == '/')
    {
        _snprintf(DosPath, DosPathSize - 1, "%c:%s", PosixDrive, Command);
        DosPath[DosPathSize - 1] = 0;
        for (i = 0; DosPath[i]; i++)
        {
            if (DosPath[i] == '/')
                DosPath[i] = '\\';
        }
    }
    else if (strchr(Command, '\\') ||
             strchr(Command, '/') ||
             (Command[0] != 0 && Command[1] == ':'))
    {
        _snprintf(DosPath, DosPathSize - 1, "%s", Command);
        DosPath[DosPathSize - 1] = 0;
    }
    else
    {
        _snprintf(DosPath, DosPathSize - 1, "%c:\\bin\\%s", PosixDrive, Command);
        DosPath[DosPathSize - 1] = 0;
    }

    Length = strlen(DosPath);
    if (GetFileAttributesA(DosPath) == INVALID_FILE_ATTRIBUTES &&
        Length + 4 < DosPathSize &&
        (Length < 4 || _stricmp(DosPath + Length - 4, ".exe") != 0))
    {
        strcpy(DosPath + Length, ".exe");
    }
}

/**
 * @brief Marshals the child image, cwd, argv and env into D<pid> and asks psxss to spawn it.
 *
 * Offsets in the argv/env table are relative to the argv offset.
 *
 * @param ArgCount
 * Number of entries in ArgVector. Entry 0 is the command, the rest are passed to the child.
 *
 * @param DosImage
 * Receives the resolved image path, for error reporting.
 *
 * @param LoginShell
 * If TRUE, argv[0] gets a leading '-' so the shell reads /etc/profile.
 */
static
NTSTATUS
SpawnChild(
    _In_ INT ArgCount,
    _In_reads_(ArgCount) PCHAR *ArgVector,
    _Out_writes_z_(DosImageSize) PCHAR DosImage,
    _In_ SIZE_T DosImageSize,
    _In_ BOOLEAN LoginShell)
{
    /* The native toolchain comes first in PATH so a non login spawn still finds gcc */
    static PCSTR EnvVector[] =
    {
        "PATH=/psxtc/bin:/bin", "HOME=/", "TERM=vt100", "_POSIX_TERM=on", NULL
    };
    static CHAR LoginArg0[64];
    PCHAR Section = (PCHAR)SharedBase;
    CHAR Cwd[300];
    WCHAR WideImage[300];
    UNICODE_STRING NtImage;
    ANSI_STRING AnsiImage;
    ULONG Offset;
    ULONG ImageOffset;
    ULONG CwdOffset;
    ULONG ArgvOffset;
    ULONG TableEntries;
    ULONG StringArea;
    ULONG Current;
    ULONG StringLength;
    PULONG Table;
    SES_MSG Message;
    NTSTATUS Status;
    CHAR PosixDrive;
    PCSTR Command;
    PCSTR ChildArgv[64];
    INT ChildArgc = 0;
    INT EnvCount = 0;
    INT EnvIndex = 0;
    INT i;
    SIZE_T Length;

    while (EnvVector[EnvCount])
        EnvCount++;

    if (ArgCount > 0 && ArgVector[0] != NULL && ArgVector[0][0] != 0)
        Command = ArgVector[0];
    else
        Command = "sh";

    /* The image is still resolved from the undecorated command */
    if (LoginShell)
    {
        Length = strlen(Command);
        if (Length > sizeof(LoginArg0) - 2)
            Length = sizeof(LoginArg0) - 2;
        LoginArg0[0] = '-';
        memcpy(LoginArg0 + 1, Command, Length);
        LoginArg0[Length + 1] = '\0';
        ChildArgv[ChildArgc++] = LoginArg0;
    }
    else
    {
        ChildArgv[ChildArgc++] = Command;
    }

    for (i = 1; i < ArgCount && ChildArgc < (INT)ARRAYSIZE(ChildArgv) - 1; i++)
        ChildArgv[ChildArgc++] = ArgVector[i];

    PosixDrive = PickPosixDrive();
    ResolveImage(Command, PosixDrive, DosImage, DosImageSize);

    /* Keep the current directory if it is on the POSIX volume, else use that volume's root */
    if (GetCurrentDirectoryA(sizeof(Cwd) - 2, Cwd) == 0 ||
        !(Cwd[1] == ':' && (Cwd[0] & ~0x20) == (PosixDrive & ~0x20)))
    {
        Cwd[0] = PosixDrive;
        Cwd[1] = ':';
        Cwd[2] = '\\';
        Cwd[3] = 0;
    }

    Length = strlen(Cwd);
    if (Length && Cwd[Length - 1] != '\\')
    {
        Cwd[Length] = '\\';
        Cwd[Length + 1] = 0;
    }

    /* Image as an ANSI NT path at offset 0 */
    MultiByteToWideChar(CP_ACP, 0, DosImage, -1, WideImage, ARRAYSIZE(WideImage));
    RtlInitUnicodeString(&NtImage, NULL);
    if (!RtlDosPathNameToNtPathName_U(WideImage, &NtImage, NULL, NULL))
        return STATUS_OBJECT_NAME_INVALID;
    RtlUnicodeStringToAnsiString(&AnsiImage, &NtImage, TRUE);

    Offset = 0;
    ImageOffset = 0;
    memcpy(Section + Offset, AnsiImage.Buffer, AnsiImage.Length + 1);
    Offset = AlignUp4(Offset + AnsiImage.Length + 1);
    RtlFreeAnsiString(&AnsiImage);
    RtlFreeUnicodeString(&NtImage);

    CwdOffset = Offset;
    memcpy(Section + Offset, Cwd, strlen(Cwd) + 1);
    Offset = AlignUp4(Offset + (ULONG)strlen(Cwd) + 1);

    /* argv table, NULL, env table, NULL, then the strings */
    ArgvOffset = Offset;
    TableEntries = (ULONG)ChildArgc + 1 + (ULONG)EnvCount + 1;
    Table = (PULONG)(Section + ArgvOffset);
    StringArea = ArgvOffset + TableEntries * sizeof(ULONG);
    StringArea = AlignUp4(StringArea);

    Current = StringArea;
    for (i = 0; i < ChildArgc; i++)
    {
        StringLength = (ULONG)strlen(ChildArgv[i]) + 1;
        memcpy(Section + Current, ChildArgv[i], StringLength);
        Table[i] = Current - ArgvOffset;
        Current += StringLength;
    }
    Table[ChildArgc] = 0;

    for (i = 0; EnvVector[i]; i++)
    {
        StringLength = (ULONG)strlen(EnvVector[i]) + 1;
        memcpy(Section + Current, EnvVector[i], StringLength);
        Table[ChildArgc + 1 + i] = Current - ArgvOffset;
        Current += StringLength;
        EnvIndex++;
    }
    Table[ChildArgc + 1 + EnvIndex] = 0;
    Offset = AlignUp4(Current);

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = SES_TOTAL_LENGTH;
    Message.Header.u1.s1.DataLength = SES_DATA_LENGTH;
    Message.Data[IDX_SELECTOR] = 0;
    Message.Data[IDX_DISCRIM] = 0;
    Message.Data[IDX_LEADERPID] = MyPid;
    Message.Data[IDX_SESSIONID] = SessionId;
    Message.Data[IDX_FDCOUNT] = 3;
    Message.Data[IDX_IMGOFF] = ImageOffset;
    Message.Data[IDX_CWDOFF] = CwdOffset;
    Message.Data[IDX_ARGVOFF] = ArgvOffset;
    /* The env table directly follows the argv table */
    Message.Data[IDX_ENVOFF] = ArgvOffset + ((ULONG)ChildArgc + 1) * sizeof(ULONG);
    Message.Data[IDX_SHAREDBASE] = (ULONG)(ULONG_PTR)SharedBase;

    Status = NtRequestWaitReplyPort(SesPort, &Message.Header, &Message.Header);
    if (!NT_SUCCESS(Status))
    {
        Trace("%s: spawn LPC %08x\n", ProgramName, Status);
        return Status;
    }

    if ((LONG)Message.Data[IDX_STATUS] < 0)
    {
        Trace("%s: spawn reply %08x\n", ProgramName, Message.Data[IDX_STATUS]);
        return (NTSTATUS)Message.Data[IDX_STATUS];
    }

    /* The reply carries the context used for later tty signals */
    ReplyContext = Message.Data[IDX_SESSIONID];
    Trace("%s: spawned '%s' in session %u\n", ProgramName, DosImage, SessionId);
    return STATUS_SUCCESS;
}

/**
 * @brief Sets the default termios: canonical input with echo and signals.
 */
static
VOID
InitTermios(VOID)
{
    RtlZeroMemory(Termios, sizeof(Termios));
    TERMIOS_IFLAG(Termios) = 3;     /* BRKINT | ICRNL */
    TERMIOS_OFLAG(Termios) = 3;     /* OPOST | ONLCR */
    TERMIOS_CFLAG(Termios) = 0x82;  /* CREAD | CS8 */
    TERMIOS_LFLAG(Termios) = 0x57;  /* ECHO | ECHOE | ECHOK | ICANON | ISIG */
    TERMIOS_CC(Termios, 0) = 26;    /* VEOF = ^Z */
    TERMIOS_CC(Termios, 2) = 8;     /* VERASE = ^H */
    TERMIOS_CC(Termios, 3) = 3;     /* VINTR = ^C */
    TERMIOS_CC(Termios, 4) = 24;    /* VKILL = ^X */
    TERMIOS_CC(Termios, 9) = 17;    /* VSTART = ^Q */
    TERMIOS_CC(Termios, 10) = 19;   /* VSTOP = ^S */
}

int
main(
    _In_ int argc,
    _In_reads_(argc) char **argv)
{
    static PCHAR DefaultArgv[] = {"sh", NULL};
    NTSTATUS Status;
    HANDLE Thread;
    CHAR ResolvedImage[300];
    INT ChildArgc = (argc > 1) ? argc - 1 : 1;
    PCHAR *ChildArgv = (argc > 1) ? argv + 1 : DefaultArgv;
    INT i;

    MyPid = GetCurrentProcessId();

    /* The emulator and console back end must be ready before any output */
    vtInitVT100();
    ConInHandle = GetStdHandle(STD_INPUT_HANDLE);
    InitTermios();
    RtlInitializeCriticalSection(&InLock);
    RtlInitializeCriticalSection(&SesLock);
    InReady = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    /* If psxss is not running the section create fails; load it through SM and retry */
    Status = CreateSessionObjects();
    if (Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        vtprintf("%s: POSIX subsystem not running, asking SM to load it...\r\n", ProgramName);
        EnsurePsxSs();
        for (i = 0; i < 40 && !NT_SUCCESS(Status); i++)
        {
            Sleep(250);
            Status = CreateSessionObjects();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        vtprintf("%s: session objects failed (%08x)\r\n", ProgramName, Status);
        return 1;
    }

    Status = ConnectSesPort();
    if (!NT_SUCCESS(Status))
    {
        vtprintf("%s: connect to psxss failed (%08x)\r\n", ProgramName, Status);
        return 1;
    }

    /* With no command given, start the session shell as a login shell */
    Status = SpawnChild(ChildArgc,
                       ChildArgv,
                       ResolvedImage,
                       sizeof(ResolvedImage),
                       (BOOLEAN)(argc <= 1));
    if (!NT_SUCCESS(Status))
    {
        vtprintf("%s: could not start '%s' as '%s' (%08x)\r\n",
                 ProgramName,
                 ChildArgv[0],
                 ResolvedImage,
                 Status);
        return 1;
    }

    Thread = CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
    if (Thread)
        CloseHandle(Thread);

    /* The server thread exits the process when the child's session ends */
    for (;;)
        Sleep(1000);

    return 0;
}
