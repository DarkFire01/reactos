/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX session registration port (\PSXSS\SESPORT)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <stdio.h>      // _snwprintf
#include <string.h>     // strlen/strcpy

/* \PSXSS\SESPORT */
HANDLE g_SesApiPort = NULL;

static LIST_ENTRY g_SessionList;
static RTL_CRITICAL_SECTION g_SessionLock;

/*
 * SESPORT connect message. The leader passes its PID as connection information
 * and receives the assigned session id in the same field.
 */
typedef struct _PSX_SES_CONNECT
{
    PORT_MESSAGE Header;
    ULONG LeaderPid;
} PSX_SES_CONNECT, *PPSX_SES_CONNECT;

/* Receive buffer large enough for every message posix.exe sends on SESPORT */
typedef struct _PSX_SES_MESSAGE
{
    PORT_MESSAGE Header;
    UCHAR Data[0x70 - sizeof(PORT_MESSAGE)];
} PSX_SES_MESSAGE, *PPSX_SES_MESSAGE;

/*
 * Teardown notification sent to the leader's own port. Selector 1 with sub-op 0
 * makes posix.exe exit with ExitCode.
 */
typedef struct _PSX_SES_EXIT
{
    PORT_MESSAGE Header;        // 0x00
    ULONG Selector;             // 0x18: 1 = exit
    ULONG Reserved1C;           // 0x1C
    ULONG SubOp;                // 0x20: 0 = teardown
    LONG ExitCode;              // 0x24
    UCHAR Pad[0x70 - 0x28];
} PSX_SES_EXIT, *PPSX_SES_EXIT;

/**
 * @brief Looks up a session by id (the leader's PID). Takes the session lock.
 */
PPSX_SESSION
PsxFindSession(
    _In_ ULONG SessionId)
{
    PLIST_ENTRY Entry;
    PPSX_SESSION Session = NULL;
    PPSX_SESSION Candidate;

    RtlEnterCriticalSection(&g_SessionLock);
    for (Entry = g_SessionList.Flink; Entry != &g_SessionList; Entry = Entry->Flink)
    {
        Candidate = CONTAINING_RECORD(Entry, PSX_SESSION, Entry);
        if (Candidate->SessionId == SessionId)
        {
            Session = Candidate;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_SessionLock);

    return Session;
}

/**
 * @brief Tells a session's posix.exe leader to exit once the session's top-level
 * process has exited.
 */
VOID
PsxNotifySessionExit(
    _In_ ULONG SessionId,
    _In_ LONG ExitCode)
{
    PPSX_SESSION Session = PsxFindSession(SessionId);
    PSX_SES_EXIT Message;

    if ((Session == NULL) || (Session->LeaderPort == NULL))
        return;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x70;
    Message.Header.u1.s1.DataLength = 0x58;
    Message.Selector = 1;
    Message.SubOp = 0;
    Message.ExitCode = ExitCode;

    /* posix.exe exits without replying, so a disconnect status is expected */
    NtRequestWaitReplyPort(Session->LeaderPort, &Message.Header, &Message.Header);
    PSXTRACE("session %lu: sent teardown(code %ld) to posix.exe\n", SessionId, ExitCode);
}

/**
 * @brief Registers a connecting posix.exe leader and echoes back its session id.
 */
static
VOID
PsxSesAcceptConnection(
    _Inout_ PPSX_SES_CONNECT ConnectMessage)
{
    PPSX_SESSION Session;
    HANDLE PortHandle;
    NTSTATUS Status;
    ULONG LeaderPid = ConnectMessage->LeaderPid;
    WCHAR NameBuffer[64];
    UNICODE_STRING LeaderPortName;
    SECURITY_QUALITY_OF_SERVICE Qos;
    ULONG ConnInfo = 0;
    ULONG ConnInfoLen = sizeof(ConnInfo);
    HANDLE LeaderPort = NULL;

    Session = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Session));
    if (Session == NULL)
    {
        NtAcceptConnectPort(&PortHandle, NULL, &ConnectMessage->Header, FALSE, NULL, NULL);
        return;
    }

    /* The session id is the leader's PID */
    Session->LeaderPid = LeaderPid;
    Session->SessionId = LeaderPid;
    Session->LeaderClientId = ConnectMessage->Header.ClientId;

    ConnectMessage->LeaderPid = Session->SessionId;

    /* The session record becomes the port context for this connection */
    Status = NtAcceptConnectPort(&PortHandle, Session, &ConnectMessage->Header, TRUE, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session);
        return;
    }

    Session->SesCommPort = PortHandle;
    NtCompleteConnectPort(PortHandle);

    /* Connect back to posix.exe's own port so it can be told to exit later */
    _snwprintf(NameBuffer,
               sizeof(NameBuffer) / sizeof(WCHAR),
               L"\\PSXSS\\PSXSES\\P%u",
               LeaderPid);
    RtlInitUnicodeString(&LeaderPortName, NameBuffer);
    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;
    if (NT_SUCCESS(NtConnectPort(&LeaderPort,
                                 &LeaderPortName,
                                 &Qos,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &ConnInfo,
                                 &ConnInfoLen)))
    {
        Session->LeaderPort = LeaderPort;
    }
    PSXTRACE("SESPORT: leader back-connect P%lu port %p\n", LeaderPid, Session->LeaderPort);

    RtlEnterCriticalSection(&g_SessionLock);
    InsertTailList(&g_SessionList, &Session->Entry);
    RtlLeaveCriticalSection(&g_SessionLock);

    PSXTRACE("SESPORT: registered session %lu (leader posix.exe pid %lu)\n",
             Session->SessionId,
             LeaderPid);
}

/**
 * @brief Unlinks and frees a session whose leader port went away.
 */
static
VOID
PsxReapSession(
    _In_opt_ PPSX_SESSION Session)
{
    if (Session == NULL)
        return;

    RtlEnterCriticalSection(&g_SessionLock);
    RemoveEntryList(&Session->Entry);
    RtlLeaveCriticalSection(&g_SessionLock);

    if (Session->SesCommPort != NULL)
        NtClose(Session->SesCommPort);
    if (Session->LeaderPort != NULL)
        NtClose(Session->LeaderPort);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Session);
}

/**
 * @brief Returns the NUL-terminated ANSI string at Offset in the session section,
 * or NULL if Offset is out of range.
 */
static
PCSTR
PsxSectionString(
    _In_ PVOID Base,
    _In_ SIZE_T Size,
    _In_ ULONG Offset)
{
    if (Offset >= Size)
        return NULL;

    return (PCSTR)((PUCHAR)Base + Offset);
}

/**
 * @brief Maps the session data section \PSXSS\PSXSES\D<id> read-only.
 */
static
NTSTATUS
PsxMapSessionData(
    _In_ ULONG SessionId,
    _Out_ PHANDLE SectionHandle,
    _Out_ PVOID *ViewBase,
    _Out_ PSIZE_T ViewSize)
{
    WCHAR NameBuffer[64];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE Section;
    PVOID Base = NULL;
    SIZE_T Size = 0;

    _snwprintf(NameBuffer,
               sizeof(NameBuffer) / sizeof(WCHAR),
               PSX_SESSION_DATA_TEMPLATE,
               SessionId);
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenSection(&Section, SECTION_MAP_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtMapViewOfSection(Section,
                                NtCurrentProcess(),
                                &Base,
                                0,
                                0,
                                NULL,
                                &Size,
                                ViewUnmap,
                                0,
                                PAGE_READONLY);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Section);
        return Status;
    }

    *SectionHandle = Section;
    *ViewBase = Base;
    *ViewSize = Size;
    return STATUS_SUCCESS;
}

/**
 * @brief Builds a double-NUL-terminated UNICODE environment block from the env
 * offset table and extracts the _PSXLIBPATH= value into DllPath.
 *
 * @return The environment block, or NULL on failure. DllPath is empty unless
 * _PSXLIBPATH= is present.
 */
static
PWSTR
PsxBuildEnvironment(
    _In_ PVOID Base,
    _In_ SIZE_T Size,
    _In_ ULONG EnvOffset,
    _Out_ PUNICODE_STRING DllPath)
{
    PULONG Table;
    ULONG Index;
    SIZE_T AnsiBytes = 1;
    PSTR AnsiBlock;
    PSTR Cursor;
    PWSTR UniBlock = NULL;
    ULONG UniBytes;
    PCSTR LibPath = NULL;
    PCSTR Entry;
    SIZE_T Length;
    ANSI_STRING AnsiLib;

    RtlInitUnicodeString(DllPath, NULL);

    if ((EnvOffset >= Size) || (EnvOffset & 3))
        return NULL;
    Table = (PULONG)((PUCHAR)Base + EnvOffset);

    /* Size the block (AnsiBytes starts at 1 for the final NUL) and find _PSXLIBPATH= */
    for (Index = 0;
         ((ULONG_PTR)&Table[Index] < (ULONG_PTR)Base + Size) && (Table[Index] != 0);
         Index++)
    {
        Entry = PsxSectionString(Base, Size, Table[Index]);
        if (Entry == NULL)
            return NULL;

        AnsiBytes += strlen(Entry) + 1;
        if ((LibPath == NULL) && (_strnicmp(Entry, "_PSXLIBPATH=", 12) == 0))
            LibPath = Entry + 12;
    }

    AnsiBlock = RtlAllocateHeap(RtlGetProcessHeap(), 0, AnsiBytes);
    if (AnsiBlock == NULL)
        return NULL;

    Cursor = AnsiBlock;
    for (Index = 0; Table[Index] != 0; Index++)
    {
        Entry = (PCSTR)((PUCHAR)Base + Table[Index]);
        Length = strlen(Entry) + 1;
        RtlCopyMemory(Cursor, Entry, Length);
        Cursor += Length;
    }
    *Cursor = '\0';

    /* Convert the whole block at once; embedded NULs are kept */
    UniBytes = (ULONG)(AnsiBytes * sizeof(WCHAR));
    UniBlock = RtlAllocateHeap(RtlGetProcessHeap(), 0, UniBytes);
    if (UniBlock != NULL)
        RtlMultiByteToUnicodeN(UniBlock, UniBytes, NULL, AnsiBlock, (ULONG)AnsiBytes);

    if ((LibPath != NULL) && (UniBlock != NULL))
    {
        RtlInitAnsiString(&AnsiLib, LibPath);
        RtlAnsiStringToUnicodeString(DllPath, &AnsiLib, TRUE);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, AnsiBlock);
    return UniBlock;
}

/**
 * @brief Builds the initial CWD and root NT path prefixes for the startup exchange.
 *
 * The CWD is "\DosDevices\" plus the DOS CWD with a trailing backslash. The root
 * is the prefix up to and including the drive colon.
 */
static
VOID
PsxBuildStartupPaths(
    _Inout_ PPSX_PROCESS Process,
    _In_opt_z_ PCSTR DosCwd)
{
    PSTR Cwd = Process->StartupCwd;
    SIZE_T Length;
    SIZE_T Index;

    if ((DosCwd == NULL) || (DosCwd[0] == '\0'))
        DosCwd = "C:\\";

    /* A path starting with a backslash is already an NT path */
    if (DosCwd[0] == '\\')
    {
        strncpy(Cwd, DosCwd, sizeof(Process->StartupCwd) - 2);
        Cwd[sizeof(Process->StartupCwd) - 2] = '\0';
    }
    else
    {
        strcpy(Cwd, "\\DosDevices\\");
        strncat(Cwd, DosCwd, sizeof(Process->StartupCwd) - 2 - strlen(Cwd));
    }

    /* The trailing backslash lets "." resolve to the directory itself */
    Length = strlen(Cwd);
    if ((Length == 0) || (Cwd[Length - 1] != '\\'))
    {
        Cwd[Length++] = '\\';
        Cwd[Length] = '\0';
    }
    Process->StartupCwdLen = (USHORT)Length;

    for (Index = 0;
         (Index < Length) && (Cwd[Index] != ':') && (Index < sizeof(Process->StartupRoot) - 2);
         Index++)
    {
        Process->StartupRoot[Index] = Cwd[Index];
    }
    if ((Index < Length) && (Cwd[Index] == ':'))
        Process->StartupRoot[Index++] = ':';
    Process->StartupRoot[Index] = '\0';
    Process->StartupRootLen = (USHORT)Index;

    Process->StartupBlockValid = TRUE;
    PSXTRACE("startup paths: cwd '%s' root '%s'\n",
             Process->StartupCwd,
             Process->StartupRoot);
}

/**
 * @brief SESPORT create process request. Spawns the POSIX image described in the
 * session data section and pre-creates its process record so the API port
 * connect can find it.
 */
static
NTSTATUS
PsxSesCreateProcess(
    _In_ PPSX_SPAWN_REQUEST Request)
{
    NTSTATUS Status;
    HANDLE SectionHandle = NULL;
    PVOID SectionBase = NULL;
    SIZE_T SectionSize = 0;
    PCSTR ImagePath;
    PCSTR Cwd;
    ANSI_STRING AnsiImage;
    ANSI_STRING AnsiCwd;
    UNICODE_STRING UniImage;
    UNICODE_STRING UniCwd;
    UNICODE_STRING RawArgvEnv;
    UNICODE_STRING DllPath;
    PWSTR Environment = NULL;
    ULONG BlockLen;
    PRTL_USER_PROCESS_PARAMETERS Parameters = NULL;
    RTL_USER_PROCESS_INFORMATION ProcessInfo;
    PPSX_PROCESS Process;
    HANDLE LeaderProcess = NULL;
    HANDLE ExceptionPort;
    OBJECT_ATTRIBUTES ObjectAttributes;
    CLIENT_ID LeaderCid;
    ULONG Zero = 0;

    RtlZeroMemory(&UniImage, sizeof(UniImage));
    RtlZeroMemory(&UniCwd, sizeof(UniCwd));
    RtlZeroMemory(&RawArgvEnv, sizeof(RawArgvEnv));
    RtlZeroMemory(&DllPath, sizeof(DllPath));
    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    Status = PsxMapSessionData(Request->SessionId, &SectionHandle, &SectionBase, &SectionSize);
    if (!NT_SUCCESS(Status))
        return Status;

    ImagePath = PsxSectionString(SectionBase, SectionSize, Request->ImagePathOffset);
    Cwd = PsxSectionString(SectionBase, SectionSize, Request->CwdOffset);
    if ((ImagePath == NULL) || (Cwd == NULL) || (Request->ArgvOffset >= SectionSize))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    RtlInitAnsiString(&AnsiImage, ImagePath);
    RtlInitAnsiString(&AnsiCwd, Cwd);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&UniImage, &AnsiImage, TRUE)) ||
        !NT_SUCCESS(RtlAnsiStringToUnicodeString(&UniCwd, &AnsiCwd, TRUE)))
    {
        Status = STATUS_NO_MEMORY;
        goto Cleanup;
    }

    /*
     * The child's crt0 rebuilds argv and environ from a self-relative offset table
     * read through CommandLine.Buffer, so the marshalled block is passed unchanged
     * with no NT environment. The env table follows argv in the same block.
     */
    BlockLen = 0x4000;
    if ((ULONG_PTR)Request->ArgvOffset + BlockLen > SectionSize)
        BlockLen = (ULONG)(SectionSize - Request->ArgvOffset);
    RawArgvEnv.Length = (USHORT)BlockLen;
    RawArgvEnv.MaximumLength = (USHORT)BlockLen;
    RawArgvEnv.Buffer = (PWSTR)((PUCHAR)SectionBase + Request->ArgvOffset);

    /* Only the _PSXLIBPATH DLL search path is used from the environment */
    Environment = PsxBuildEnvironment(SectionBase, SectionSize, Request->EnvOffset, &DllPath);

    Status = RtlCreateProcessParameters(&Parameters,
                                        &UniImage,
                                        (DllPath.Buffer != NULL) ? &DllPath : NULL,
                                        &UniCwd,
                                        &RawArgvEnv,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Parent the image to the session leader when it can be opened */
    LeaderCid.UniqueProcess = (HANDLE)(ULONG_PTR)Request->LeaderPid;
    LeaderCid.UniqueThread = NULL;
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    NtOpenProcess(&LeaderProcess,
                  PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE,
                  &ObjectAttributes,
                  &LeaderCid);

    /* Parameters is de-normalized, so pass UniImage rather than its ImagePathName */
    Status = RtlCreateUserProcess(&UniImage,
                                  OBJ_CASE_INSENSITIVE,
                                  Parameters,
                                  NULL,
                                  NULL,
                                  (LeaderProcess != NULL) ? LeaderProcess : NtCurrentProcess(),
                                  TRUE,
                                  NULL,
                                  NULL,
                                  &ProcessInfo);
    RtlDestroyProcessParameters(Parameters);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (ProcessInfo.ImageInformation.SubSystemType != IMAGE_SUBSYSTEM_POSIX_CUI)
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        NtTerminateProcess(ProcessInfo.ProcessHandle, Status);
        NtClose(ProcessInfo.ProcessHandle);
        NtClose(ProcessInfo.ThreadHandle);
        goto Cleanup;
    }

    /* Route exceptions and the connect to our API port, and suppress hard errors */
    ExceptionPort = g_ApiPort;
    NtSetInformationProcess(ProcessInfo.ProcessHandle,
                            ProcessExceptionPort,
                            &ExceptionPort,
                            sizeof(ExceptionPort));
    NtSetInformationProcess(ProcessInfo.ProcessHandle,
                            ProcessDefaultHardErrorMode,
                            &Zero,
                            sizeof(Zero));

    Process = PsxAllocateProcess();
    if (Process == NULL)
    {
        Status = STATUS_NO_MEMORY;
        NtTerminateProcess(ProcessInfo.ProcessHandle, Status);
        NtClose(ProcessInfo.ProcessHandle);
        NtClose(ProcessInfo.ThreadHandle);
        goto Cleanup;
    }
    Process->ClientId = ProcessInfo.ClientId;
    Process->Pid = (ULONG)(ULONG_PTR)ProcessInfo.ClientId.UniqueProcess;
    Process->ParentPid = Request->LeaderPid;
    Process->ProcessGroup = Request->SessionId;
    Process->SessionId = Request->SessionId;
    Process->ProcessHandle = ProcessInfo.ProcessHandle;
    PsxAssignIdentity(&ProcessInfo.ClientId, Process);
    PsxInitSignalState(Process);

    /* The first InheritedFdCount descriptors all bind to the leader's console */
    PsxWireControllingTty(Process, Request->InheritedFdCount);

    PsxBuildStartupPaths(Process, Cwd);

    PsxInsertProcess(Process);

    PSXTRACE("SESPORT: spawned POSIX pid %lu (cid %p) in session %lu, %lu tty fds, image '%wZ'\n",
             Process->Pid,
             ProcessInfo.ClientId.UniqueProcess,
             Request->SessionId,
             Request->InheritedFdCount,
             &UniImage);

    NtResumeThread(ProcessInfo.ThreadHandle, NULL);
    NtClose(ProcessInfo.ThreadHandle);
    Status = STATUS_SUCCESS;

Cleanup:
    /* RawArgvEnv.Buffer points into the mapped section and is not freed */
    if (UniImage.Buffer != NULL)
        RtlFreeUnicodeString(&UniImage);
    if (UniCwd.Buffer != NULL)
        RtlFreeUnicodeString(&UniCwd);
    if (DllPath.Buffer != NULL)
        RtlFreeUnicodeString(&DllPath);
    if (Environment != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Environment);
    if (LeaderProcess != NULL)
        NtClose(LeaderProcess);
    if (SectionBase != NULL)
        NtUnmapViewOfSection(NtCurrentProcess(), SectionBase);
    if (SectionHandle != NULL)
        NtClose(SectionHandle);
    return Status;
}

/**
 * @brief SESPORT receive and dispatch loop.
 */
VOID
NTAPI
PsxSesApiRequestThread(
    _In_opt_ PVOID Parameter)
{
    PSX_SES_MESSAGE Request;
    PPSX_SES_MESSAGE Reply = NULL;
    PPSX_SPAWN_REQUEST Spawn;
    PVOID PortContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Parameter);

    PSXTRACE("SESPORT loop: started, waiting on \\PSXSS\\SESPORT\n");

    for (;;)
    {
        Status = NtReplyWaitReceivePort(g_SesApiPort,
                                        &PortContext,
                                        (Reply != NULL) ? &Reply->Header : NULL,
                                        &Request.Header);
        if (!NT_SUCCESS(Status))
        {
            Reply = NULL;
            continue;
        }

        PSXTRACE("SESPORT loop: message type %lu from cid %p\n",
                 Request.Header.u2.s2.Type & 0xFF,
                 Request.Header.ClientId.UniqueProcess);

        switch (Request.Header.u2.s2.Type & 0x000000FF)
        {
            case LPC_CONNECTION_REQUEST:
                PSXTRACE("SESPORT loop: posix.exe connecting\n");
                PsxSesAcceptConnection((PPSX_SES_CONNECT)&Request);
                Reply = NULL;
                continue;

            case LPC_REQUEST:
                Spawn = (PPSX_SPAWN_REQUEST)&Request;
                PSXTRACE("SESPORT: request discriminator %lu (session %lu)\n",
                         Spawn->Discriminator,
                         Spawn->SessionId);

                if (Spawn->Discriminator == 0)
                {
                    /* Create process */
                    Spawn->Status = (LONG)PsxSesCreateProcess(Spawn);
                }
                else if (Spawn->Discriminator == 1)
                {
                    /* Controlling tty signal: session id at +0x24, code at +0x28 */
                    PsxSesDeliverTtySignal(Spawn->Reserved24, Spawn->LeaderPid);
                    Spawn->Status = STATUS_SUCCESS;
                }
                else
                {
                    Spawn->Status = STATUS_NOT_IMPLEMENTED;
                }

                PSXTRACE("SESPORT: request reply status 0x%08lx\n", Spawn->Status);
                Reply = &Request;
                break;

            case LPC_CLIENT_DIED:
            case LPC_PORT_CLOSED:
                PsxReapSession((PPSX_SESSION)PortContext);
                Reply = NULL;
                continue;

            default:
                Reply = NULL;
                continue;
        }
    }
}

/**
 * @brief Creates the \PSXSS\PSXSES directory and \PSXSS\SESPORT, then starts the
 * SESPORT loop.
 */
NTSTATUS
PsxCreateSessionPort(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_DESCRIPTOR Sd;
    HANDLE DirectoryHandle;
    HANDLE ThreadHandle;

    InitializeListHead(&g_SessionList);
    Status = RtlInitializeCriticalSection(&g_SessionLock);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * posix.exe creates its data section and port under \PSXSS\PSXSES using the
     * user's token, so the directory allows all access. The handle is kept open
     * for the life of the subsystem instead of using OBJ_PERMANENT.
     */
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, L"\\PSXSS\\PSXSES");
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);
    Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
    PSXTRACE("NtCreateDirectoryObject(\\PSXSS\\PSXSES) status 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    /* SESPORT also allows all access so posix.exe can connect */
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_SB_PORT_NAME);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);

    Status = NtCreatePort(&g_SesApiPort,
                          &ObjectAttributes,
                          sizeof(PSX_SES_CONNECT) - sizeof(PORT_MESSAGE),
                          sizeof(PSX_SES_MESSAGE),
                          0);
    if (!NT_SUCCESS(Status))
        return Status;

    ThreadHandle = CreateThread(NULL,
                                0,
                                (LPTHREAD_START_ROUTINE)PsxSesApiRequestThread,
                                NULL,
                                0,
                                NULL);
    if (ThreadHandle == NULL)
        return STATUS_UNSUCCESSFUL;
    NtClose(ThreadHandle);

    return STATUS_SUCCESS;
}
