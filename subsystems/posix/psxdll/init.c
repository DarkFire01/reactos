/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Server connection and per-process client initialization
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_DLL_PROCESS_ATTACH 1

HANDLE PsxApiPort = NULL;
LONG PsxClientToServer = 0;
PVOID PsxSharedHeap = NULL;
PLONG PsxErrnoLocation = NULL;
char ***PsxEnvironLocation = NULL;
ULONG PsxSessionId = 0;

/* Current directory and root as ANSI NT device paths, filled in at connect time */
CHAR PsxStartupCwd[PSX_PATH_MAX] = "\\DosDevices\\C:\\";
CHAR PsxStartupRoot[PSX_ROOT_MAX] = "\\DosDevices\\C:";
ULONG PsxStartupCwdLen = 0;
ULONG PsxStartupRootLen = 0;

static LONG PsxFallbackErrno = 0;

/* Counted ANSI string header the server fills during the connect */
typedef struct _PSXDLL_ANSI_HEADER
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} PSXDLL_ANSI_HEADER, *PPSXDLL_ANSI_HEADER;

/*
 * 32-byte connect information block. StartupHeader points at three headers
 * {cwd, scratch, root}; the server writes the session id back into SessionId.
 */
typedef struct _PSXDLL_CONNECT_BLOCK
{
    ULONG ExceptionThunk;
    ULONG SignalTrampoline;
    PPSXDLL_ANSI_HEADER StartupHeader;
    ULONG SubsystemBlockSize;
    ULONG Reserved4;
    ULONG SessionId;
    ULONG Reserved6;
    ULONG Reserved7;
} PSXDLL_CONNECT_BLOCK;

VOID
PsxSetErrno(
    _In_ LONG ErrnoValue)
{
    if (PsxErrnoLocation != NULL)
        *PsxErrnoLocation = ErrnoValue;
    else
        PsxFallbackErrno = ErrnoValue;
}

/**
 * @brief Connects to the POSIX server, maps the shared section and creates the
 * marshalling heap over it. Does nothing if already connected.
 */
NTSTATUS
PsxInitialize(VOID)
{
    UNICODE_STRING PortName;
    SECURITY_QUALITY_OF_SERVICE Qos;
    PORT_VIEW ClientView;
    REMOTE_PORT_VIEW ServerView;
    PSXDLL_CONNECT_BLOCK ConnectInfo;
    PSXDLL_ANSI_HEADER Headers[3];
    static CHAR CwdBuffer[PSX_PATH_MAX];
    static CHAR RootBuffer[PSX_ROOT_MAX];
    HANDLE SectionHandle = NULL;
    LARGE_INTEGER SectionSize;
    ULONG ConnectInfoLength;
    NTSTATUS Status;

    if (PsxApiPort != NULL)
        return STATUS_SUCCESS;

    RtlInitUnicodeString(&PortName, PSX_API_PORT_NAME);

    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;

    /* Per-process data section, mapped by both sides through the port views */
    SectionSize.QuadPart = PSX_SESSION_SECTION_SIZE;
    Status = NtCreateSection(&SectionHandle,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &SectionSize,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(ClientView);
    ClientView.SectionHandle = SectionHandle;
    ClientView.ViewSize = PSX_SESSION_SECTION_SIZE;

    RtlZeroMemory(&ServerView, sizeof(ServerView));
    ServerView.Length = sizeof(ServerView);

    RtlZeroMemory(Headers, sizeof(Headers));
    Headers[0].MaximumLength = sizeof(CwdBuffer);
    Headers[0].Buffer = CwdBuffer;
    Headers[2].MaximumLength = sizeof(RootBuffer);
    Headers[2].Buffer = RootBuffer;

    RtlZeroMemory(&ConnectInfo, sizeof(ConnectInfo));
    ConnectInfo.StartupHeader = Headers;
    ConnectInfo.SubsystemBlockSize = 16;
    ConnectInfoLength = sizeof(ConnectInfo);

    Status = NtConnectPort(&PsxApiPort,
                           &PortName,
                           &Qos,
                           &ClientView,
                           &ServerView,
                           NULL,
                           &ConnectInfo,
                           &ConnectInfoLength);
    if (!NT_SUCCESS(Status))
        return Status;

    NtRegisterThreadTerminatePort(PsxApiPort);

    /* The server side base of the section is reported in ClientView, not ServerView */
    PsxClientToServer = (LONG)((ULONG_PTR)ClientView.ViewRemoteBase -
                               (ULONG_PTR)ClientView.ViewBase);

    PsxSharedHeap = RtlCreateHeap(HEAP_GROWABLE,
                                  ClientView.ViewBase,
                                  PSX_SESSION_SECTION_SIZE,
                                  PAGE_SIZE,
                                  NULL,
                                  NULL);

    PsxSessionId = ConnectInfo.SessionId;
    if (Headers[0].Length != 0 && Headers[0].Length < sizeof(PsxStartupCwd))
    {
        RtlCopyMemory(PsxStartupCwd, CwdBuffer, Headers[0].Length);
        PsxStartupCwd[Headers[0].Length] = '\0';
        PsxStartupCwdLen = Headers[0].Length;
    }
    if (Headers[2].Length != 0 && Headers[2].Length < sizeof(PsxStartupRoot))
    {
        RtlCopyMemory(PsxStartupRoot, RootBuffer, Headers[2].Length);
        PsxStartupRoot[Headers[2].Length] = '\0';
        PsxStartupRootLen = Headers[2].Length;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Records the addresses of the CRT errno and environ variables.
 */
VOID
__stdcall
__PdxInitializeData(
    _In_ PLONG ErrnoLocation,
    _In_ PVOID EnvironLocation)
{
    PsxErrnoLocation = ErrnoLocation;
    PsxEnvironLocation = (char ***)EnvironLocation;
}

/**
 * @brief Entry point for a POSIX image: runs the program main, then exits.
 */
VOID
__stdcall
__PosixProcessStartup(
    _In_ PVOID Context)
{
    int (*Main)(void);
    PVOID Dispatch;

    PsxInitialize();

    /* Context + 0x14 holds the dispatch table; its second entry is main */
    Dispatch = *(PVOID *)((PUCHAR)Context + 0x14);
    Main = *(int (**)(void))((PUCHAR)Dispatch + 0x04);

    _exit(Main());
}

/**
 * @brief DLL entry point. Connects to the server on process attach, before the
 * image's startup code runs.
 */
BOOLEAN
NTAPI
DllMain(
    _In_ PVOID DllHandle,
    _In_ ULONG Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(DllHandle);
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == PSX_DLL_PROCESS_ATTACH && !NT_SUCCESS(PsxInitialize()))
        return FALSE;

    return TRUE;
}
