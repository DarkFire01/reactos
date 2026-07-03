/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX session registration. Serves \PSXSS\SESPORT, the port that
 *              each posix.exe session leader connects to (passing its PID); psxss
 *              tracks the session and echoes the session id back.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <stdio.h>      // _snwprintf
#include <string.h>     // strlen/strcpy

HANDLE g_SesApiPort = NULL;                 // \PSXSS\SESPORT (session-register port)

static LIST_ENTRY           g_SessionList;
static RTL_CRITICAL_SECTION g_SessionLock;

//
// The SESPORT connect message: a posix.exe leader passes its PID (4 bytes) as
// connection information; psxss echoes the assigned session id back in place.
// (posix.exe defaults this to its own PID and uses whatever comes back, so the
// session id is the leader PID either way -- the echo is belt-and-suspenders.)
//
typedef struct _PSX_SES_CONNECT
{
    PORT_MESSAGE Header;
    ULONG        LeaderPid;     // IN: leader PID / OUT: assigned session id
} PSX_SES_CONNECT, *PPSX_SES_CONNECT;

//
// A receive buffer big enough for both the connect message and the larger
// request messages posix.exe sends over this port (spawn/signal; max ~0x70).
//
typedef struct _PSX_SES_MESSAGE
{
    PORT_MESSAGE Header;
    UCHAR        Data[0x70 - sizeof(PORT_MESSAGE)];
} PSX_SES_MESSAGE, *PPSX_SES_MESSAGE;

//
// The notification psxss pushes to posix.exe's own port to make it tear down and
// exit. posix.exe's ServerThread reads the API selector at body +0x18 (1 = exit)
// and the sub-op/code at +0x20/+0x24; selector 1 + sub-op 0 => Teardown(code) =>
// _exit. TotalLength 0x70 / DataLength 0x58 like the other session-port messages.
//
typedef struct _PSX_SES_EXIT
{
    PORT_MESSAGE Header;        // 0x00
    ULONG        Selector;      // 0x18: 1 = exit
    ULONG        Reserved1C;    // 0x1C
    ULONG        SubOp;         // 0x20: 0 = teardown
    LONG         ExitCode;      // 0x24
    UCHAR        Pad[0x70 - 0x28];
} PSX_SES_EXIT, *PPSX_SES_EXIT;

//
// Look up a session by id (== the leader's PID). Caller-agnostic locking: takes
// the session lock internally.
//
PPSX_SESSION
PsxFindSession(IN ULONG SessionId)
{
    PLIST_ENTRY Entry;
    PPSX_SESSION Session = NULL;

    RtlEnterCriticalSection(&g_SessionLock);
    for (Entry = g_SessionList.Flink; Entry != &g_SessionList; Entry = Entry->Flink)
    {
        PPSX_SESSION Candidate = CONTAINING_RECORD(Entry, PSX_SESSION, Entry);
        if (Candidate->SessionId == SessionId)
        {
            Session = Candidate;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_SessionLock);
    return Session;
}

//
// Tell the posix.exe leader of a session to tear down and exit. Sent when the
// session's top-level process exits: without it posix.exe's ServerThread would
// loop forever. Faithful to the psxss->posix.exe teardown send (sub_1F42196 path).
//
VOID
PsxNotifySessionExit(IN ULONG SessionId, IN LONG ExitCode)
{
    PPSX_SESSION Session = PsxFindSession(SessionId);
    PSX_SES_EXIT Message;

    if ((Session == NULL) || (Session->LeaderPort == NULL))
        return;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.TotalLength = 0x70;
    Message.Header.u1.s1.DataLength = 0x58;
    Message.Selector = 1;               // exit
    Message.SubOp = 0;                   // teardown
    Message.ExitCode = ExitCode;

    // posix.exe _exit()s inside its handler and never replies, so this returns
    // with a disconnect status rather than a reply -- which is fine.
    NtRequestWaitReplyPort(Session->LeaderPort, &Message.Header, &Message.Header);
    PSXTRACE("session %lu: sent teardown(code %ld) to posix.exe\n", SessionId, ExitCode);
}

//
// A posix.exe session leader is registering. Record the session and echo back
// the session id (== leader PID).
//
static VOID
PsxSesAcceptConnection(IN PPSX_SES_CONNECT ConnectMessage)
{
    PPSX_SESSION Session;
    HANDLE PortHandle;
    NTSTATUS Status;
    ULONG LeaderPid = ConnectMessage->LeaderPid;

    Session = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_SESSION));
    if (Session == NULL)
    {
        NtAcceptConnectPort(&PortHandle, NULL, &ConnectMessage->Header, FALSE, NULL, NULL);
        return;
    }

    Session->LeaderPid = LeaderPid;
    Session->SessionId = LeaderPid;                 // session id == leader PID
    Session->LeaderClientId = ConnectMessage->Header.ClientId;

    // Echo the session id back to the connecting posix.exe.
    ConnectMessage->LeaderPid = Session->SessionId;

    // Session becomes the PortContext, so requests/teardown on this port arrive
    // tagged with their PSX_SESSION.
    Status = NtAcceptConnectPort(&PortHandle, Session, &ConnectMessage->Header, TRUE, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session);
        return;
    }

    Session->SesCommPort = PortHandle;
    NtCompleteConnectPort(PortHandle);

    // Connect back to posix.exe's own port \PSXSS\PSXSES\P<pid> so we can push it
    // notifications -- notably the session-teardown message when the session's
    // process exits, which is what makes posix.exe terminate. posix.exe creates
    // this port before it connects to SESPORT. Faithful to sub_1F42000.
    {
        WCHAR NameBuffer[64];
        UNICODE_STRING LeaderPortName;
        SECURITY_QUALITY_OF_SERVICE Qos;
        ULONG ConnInfo = 0;
        ULONG ConnInfoLen = sizeof(ConnInfo);
        HANDLE LeaderPort = NULL;

        _snwprintf(NameBuffer, sizeof(NameBuffer) / sizeof(WCHAR),
                   L"\\PSXSS\\PSXSES\\P%u", LeaderPid);
        RtlInitUnicodeString(&LeaderPortName, NameBuffer);
        Qos.Length = sizeof(Qos);
        Qos.ImpersonationLevel = SecurityImpersonation;
        Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
        Qos.EffectiveOnly = TRUE;
        if (NT_SUCCESS(NtConnectPort(&LeaderPort, &LeaderPortName, &Qos, NULL, NULL, NULL,
                                     &ConnInfo, &ConnInfoLen)))
            Session->LeaderPort = LeaderPort;
        PSXTRACE("SESPORT: leader back-connect P%lu -> %p\n", LeaderPid, Session->LeaderPort);
    }

    RtlEnterCriticalSection(&g_SessionLock);
    InsertTailList(&g_SessionList, &Session->Entry);
    RtlLeaveCriticalSection(&g_SessionLock);

    PSXTRACE("SESPORT: registered session %lu (leader posix.exe pid %lu)\n",
             Session->SessionId, LeaderPid);
}

//
// A session leader's port died: unlink and free its session record.
//
static VOID
PsxReapSession(IN PPSX_SESSION Session)
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

//
// Resolve a NUL-terminated ANSI string living at a byte offset inside the mapped
// session section (posix.exe packs them NUL-terminated). Bounds-checked.
//
static PCSTR
PsxSectionString(IN PVOID Base, IN SIZE_T Size, IN ULONG Offset)
{
    if (Offset >= Size)
        return NULL;
    return (PCSTR)((PUCHAR)Base + Offset);
}

//
// Open + map the session data section \PSXSS\PSXSES\D<id> read-only, so we can
// read the marshalled image-path / cwd / argv block posix.exe wrote there.
//
static NTSTATUS
PsxMapSessionData(IN ULONG SessionId,
                  OUT PHANDLE SectionHandle,
                  OUT PVOID *ViewBase,
                  OUT PSIZE_T ViewSize)
{
    WCHAR NameBuffer[64];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE Section;
    PVOID Base = NULL;
    SIZE_T Size = 0;

    _snwprintf(NameBuffer, sizeof(NameBuffer) / sizeof(WCHAR),
               PSX_SESSION_DATA_TEMPLATE, SessionId);     // L"\\PSXSS\\PSXSES\\D%u"
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenSection(&Section, SECTION_MAP_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtMapViewOfSection(Section, NtCurrentProcess(), &Base, 0, 0, NULL,
                                &Size, ViewUnmap, 0, PAGE_READONLY);
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

//
// Build a double-NUL-terminated UNICODE environment block from the marshalled
// env offset-table (same packing as argv), and extract the _PSXLIBPATH= value as
// the POSIX dll search path. Faithful to posix.exe's env marshalling + the
// psxss _PSXLIBPATH extractor (sub_1F41867). Returns NULL on failure (the child
// then inherits psxss's environment); *DllPath is empty unless _PSXLIBPATH= set.
//
static PWSTR
PsxBuildEnvironment(IN PVOID Base, IN SIZE_T Size, IN ULONG EnvOffset,
                    OUT PUNICODE_STRING DllPath)
{
    PULONG Table;
    ULONG Index;
    SIZE_T AnsiBytes = 1;       // trailing extra NUL (double-NUL terminator)
    PSTR AnsiBlock;
    PSTR Cursor;
    PWSTR UniBlock = NULL;
    ULONG UniBytes;
    PCSTR LibPath = NULL;

    RtlInitUnicodeString(DllPath, NULL);

    if ((EnvOffset >= Size) || (EnvOffset & 3))
        return NULL;
    Table = (PULONG)((PUCHAR)Base + EnvOffset);

    // First pass: total ANSI size, and locate _PSXLIBPATH=.
    for (Index = 0; (ULONG_PTR)&Table[Index] < (ULONG_PTR)Base + Size && Table[Index] != 0; Index++)
    {
        PCSTR Entry = PsxSectionString(Base, Size, Table[Index]);
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
        PCSTR Entry = (PCSTR)((PUCHAR)Base + Table[Index]);
        SIZE_T Length = strlen(Entry) + 1;
        RtlCopyMemory(Cursor, Entry, Length);
        Cursor += Length;
    }
    *Cursor = '\0';     // close the block with the second NUL

    // Convert the whole block in one shot (internal NULs are preserved).
    UniBytes = (ULONG)(AnsiBytes * sizeof(WCHAR));
    UniBlock = RtlAllocateHeap(RtlGetProcessHeap(), 0, UniBytes);
    if (UniBlock != NULL)
        RtlMultiByteToUnicodeN(UniBlock, UniBytes, NULL, AnsiBlock, (ULONG)AnsiBytes);

    if ((LibPath != NULL) && (UniBlock != NULL))
    {
        ANSI_STRING AnsiLib;
        RtlInitAnsiString(&AnsiLib, LibPath);
        RtlAnsiStringToUnicodeString(DllPath, &AnsiLib, TRUE);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, AnsiBlock);
    return UniBlock;
}

//
//
// Build the child's initial CWD + root NT-path prefixes from the marshalled cwd,
// stored for the connect-time startup-block exchange. Faithful to sub_1F45AD1:
// CWD = "\DosDevices\" + <dos cwd> with a trailing backslash; root = the drive
// device prefix up to and including the drive colon ("\DosDevices\X:").
//
static VOID
PsxBuildStartupPaths(IN PPSX_PROCESS Process, IN PCSTR DosCwd)
{
    PSTR Cwd = Process->StartupCwd;
    SIZE_T Len;
    SIZE_T i;

    if ((DosCwd == NULL) || (DosCwd[0] == '\0'))
        DosCwd = "C:\\";

    // Already an NT path (starts with '\') -> use as-is; else prefix the device.
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

    // Ensure a trailing backslash so "." resolves to the directory itself.
    Len = strlen(Cwd);
    if ((Len == 0) || (Cwd[Len - 1] != '\\'))
    {
        Cwd[Len++] = '\\';
        Cwd[Len] = '\0';
    }
    Process->StartupCwdLen = (USHORT)Len;

    // Root = up to and including the drive colon ("\DosDevices\X:"), no slash.
    for (i = 0; (i < Len) && (Cwd[i] != ':') && (i < sizeof(Process->StartupRoot) - 2); i++)
        Process->StartupRoot[i] = Cwd[i];
    if ((i < Len) && (Cwd[i] == ':'))
        Process->StartupRoot[i++] = ':';
    Process->StartupRoot[i] = '\0';
    Process->StartupRootLen = (USHORT)i;

    Process->StartupBlockValid = TRUE;
    PSXTRACE("startup paths: cwd '%s' root '%s'\n",
             Process->StartupCwd, Process->StartupRoot);
}

// SESPORT "create process" handler: read the marshalled block out of the session
// data section, spawn the POSIX image (parented to the session leader, exception
// port = our API port), pre-create its process record tagged with the session id
// so the ApiPort connect can find it, and resume it. Faithful to psxss sub_1F418AF
// + sub_1F4541C; the env/dll-path translation and the controlling-tty fd wiring
// are still TODO.
//
static NTSTATUS
PsxSesCreateProcess(IN PPSX_SPAWN_REQUEST Request)
{
    NTSTATUS Status;
    HANDLE SectionHandle = NULL;
    PVOID SectionBase = NULL;
    SIZE_T SectionSize = 0;
    PCSTR ImagePath;
    PCSTR Cwd;
    ANSI_STRING AnsiImage, AnsiCwd;
    UNICODE_STRING UniImage, UniCwd, RawArgvEnv, DllPath;
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

    // (1) Map the leader's session data section and resolve the marshalled block.
    Status = PsxMapSessionData(Request->SessionId, &SectionHandle, &SectionBase, &SectionSize);
    if (!NT_SUCCESS(Status))
        return Status;

    ImagePath = PsxSectionString(SectionBase, SectionSize, Request->ImagePathOffset);
    Cwd       = PsxSectionString(SectionBase, SectionSize, Request->CwdOffset);
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

    // (2) The POSIX child's crt0 reconstructs BOTH argv[] and environ[] by walking
    //     a self-relative offset-table blob it reads from CommandLine.Buffer. Pass
    //     the raw marshalled block from the D-section (at ArgvOffset) VERBATIM as
    //     the command line, with Environment = NULL -- exactly like the real psxss
    //     (sub_1F418AF / sub_1F4541C: a {0x4000,0x4000,argv-ptr} STRING is handed
    //     straight to RtlCreateProcessParameters). Joining argv into a string or
    //     building an NT env block breaks it: the child treats our ASCII bytes as
    //     offsets and dereferences wild pointers. The env offset table follows the
    //     argv table contiguously in the same block, so one buffer carries both.
    BlockLen = 0x4000;
    if ((ULONG_PTR)Request->ArgvOffset + BlockLen > SectionSize)
        BlockLen = (ULONG)(SectionSize - Request->ArgvOffset);
    RawArgvEnv.Length = (USHORT)BlockLen;
    RawArgvEnv.MaximumLength = (USHORT)BlockLen;
    RawArgvEnv.Buffer = (PWSTR)((PUCHAR)SectionBase + Request->ArgvOffset);

    // We still need the _PSXLIBPATH -> DLL search path (Environment return unused;
    // the child gets its POSIX env from the blob above, not NT ProcessParameters).
    Environment = PsxBuildEnvironment(SectionBase, SectionSize, Request->EnvOffset, &DllPath);

    Status = RtlCreateProcessParameters(&Parameters, &UniImage,
                                        (DllPath.Buffer != NULL) ? &DllPath : NULL,
                                        &UniCwd, &RawArgvEnv, NULL /* Environment */,
                                        NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    // (3) Parent = the session leader (posix.exe), if we can open it.
    LeaderCid.UniqueProcess = (HANDLE)(ULONG_PTR)Request->LeaderPid;
    LeaderCid.UniqueThread = NULL;
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    NtOpenProcess(&LeaderProcess, PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE,
                  &ObjectAttributes, &LeaderCid);

    // (4) Create the POSIX image, suspended. Pass the absolute UniImage string,
    //     NOT Parameters->ImagePathName: RtlCreateProcessParameters returns a
    //     de-normalized block whose string Buffers are byte offsets, not usable
    //     pointers (that offset landing in NtOpenFile is what crashed us). smss
    //     does the same -- it hands RtlCreateUserProcess the original FileName.
    Status = RtlCreateUserProcess(&UniImage, OBJ_CASE_INSENSITIVE, Parameters, NULL, NULL,
                                  (LeaderProcess != NULL) ? LeaderProcess : NtCurrentProcess(),
                                  TRUE, NULL, NULL, &ProcessInfo);
    RtlDestroyProcessParameters(Parameters);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    // (5) Only manage genuine POSIX images.
    if (ProcessInfo.ImageInformation.SubSystemType != IMAGE_SUBSYSTEM_POSIX_CUI)
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        NtTerminateProcess(ProcessInfo.ProcessHandle, Status);
        NtClose(ProcessInfo.ProcessHandle);
        NtClose(ProcessInfo.ThreadHandle);
        goto Cleanup;
    }

    // (6) Route its exceptions/connect to our API port; quiet hard errors.
    ExceptionPort = g_ApiPort;
    NtSetInformationProcess(ProcessInfo.ProcessHandle, ProcessExceptionPort,
                            &ExceptionPort, sizeof(HANDLE));
    NtSetInformationProcess(ProcessInfo.ProcessHandle, ProcessDefaultHardErrorMode,
                            &Zero, sizeof(ULONG));

    // (7) Pre-create the process record, tagged with the session, so the API-port
    //     connect can find it and learn its session id.
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

    // Wire the controlling-terminal descriptors (InheritedFdCount, usually 3):
    // fds 0..count-1 all bind to one tty object, so the child's stdin/out/err
    // route to posix.exe's console (the sub_1F418AF fd loop).
    PsxWireControllingTty(Process, Request->InheritedFdCount);

    // Build the initial CWD/root prefixes for the connect startup-block exchange.
    PsxBuildStartupPaths(Process, Cwd);

    PsxInsertProcess(Process);

    PSXTRACE("SESPORT: spawned POSIX pid %lu (cid %p) in session %lu, %lu tty fds, image '%wZ'\n",
             Process->Pid, ProcessInfo.ClientId.UniqueProcess, Request->SessionId,
             Request->InheritedFdCount, &UniImage);

    // (8) Run it.
    NtResumeThread(ProcessInfo.ThreadHandle, NULL);
    NtClose(ProcessInfo.ThreadHandle);
    Status = STATUS_SUCCESS;

Cleanup:
    // RawArgvEnv.Buffer points into the mapped section (not heap) -- do not free it.
    if (UniImage.Buffer != NULL)   RtlFreeUnicodeString(&UniImage);
    if (UniCwd.Buffer != NULL)     RtlFreeUnicodeString(&UniCwd);
    if (DllPath.Buffer != NULL)    RtlFreeUnicodeString(&DllPath);
    if (Environment != NULL)       RtlFreeHeap(RtlGetProcessHeap(), 0, Environment);
    if (LeaderProcess != NULL)     NtClose(LeaderProcess);
    if (SectionBase != NULL)       NtUnmapViewOfSection(NtCurrentProcess(), SectionBase);
    if (SectionHandle != NULL)     NtClose(SectionHandle);
    return Status;
}

//
// The SESPORT receive/dispatch loop. Same shape as the API/Sb loops.
//
VOID NTAPI
PsxSesApiRequestThread(IN PVOID Parameter)
{
    PSX_SES_MESSAGE Request;
    PPSX_SES_MESSAGE Reply = NULL;
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
                 Request.Header.u2.s2.Type & 0xFF, Request.Header.ClientId.UniqueProcess);

        switch (Request.Header.u2.s2.Type & 0x000000FF)
        {
            case LPC_CONNECTION_REQUEST:
                PSXTRACE("SESPORT loop: posix.exe connecting\n");
                PsxSesAcceptConnection((PPSX_SES_CONNECT)&Request);
                Reply = NULL;
                continue;

            case LPC_REQUEST:
            {
                PPSX_SPAWN_REQUEST Spawn = (PPSX_SPAWN_REQUEST)&Request;
                PSXTRACE("SESPORT: request discriminator %lu (session %lu)\n",
                         Spawn->Discriminator, Spawn->SessionId);
                if (Spawn->Discriminator == 0)          // create process
                    Spawn->Status = (LONG)PsxSesCreateProcess(Spawn);
                else if (Spawn->Discriminator == 1)     // controlling-tty signal
                {
                    // Signal message reuses the body: pid @0x24 (== session id /
                    // leader pid), code @0x28 (see posixterm SendTtySignal /
                    // posix.exe SendSignal sub_1ED206E).
                    PsxSesDeliverTtySignal(Spawn->Reserved24, Spawn->LeaderPid);
                    Spawn->Status = STATUS_SUCCESS;
                }
                else
                    Spawn->Status = STATUS_NOT_IMPLEMENTED;
                PSXTRACE("SESPORT: request reply status 0x%08lx\n", Spawn->Status);
                Reply = &Request;
                break;
            }

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

//
// Create \PSXSS\SESPORT and start its loop. Called from PsxServerInitialization.
//
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

    // First create the \PSXSS\PSXSES object directory. posix.exe (the session
    // leader) creates \PSXSS\PSXSES\D<pid> (its data section) and P<pid> (its
    // own LPC port) under here -- without this directory those NtCreateSection/
    // NtCreatePort calls fail with OBJECT_PATH_NOT_FOUND, posix.exe bootstrap-
    // retries once and gives up, never reaching SESPORT. Faithful to
    // sub_1F41BE6: allow-all DACL (posix.exe runs under the user's token), not
    // OBJ_PERMANENT. We intentionally keep the handle open (never closed) so the
    // directory lives for the life of the subsystem.
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, L"\\PSXSS\\PSXSES");
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);
    Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
    PSXTRACE("NtCreateDirectoryObject(\\PSXSS\\PSXSES) -> 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    // \PSXSS\SESPORT -- the session-register port. Also allow-all so the user-
    // token posix.exe can connect. Params match the real psxss (4, 0x50, 0xA00).
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_SB_PORT_NAME);  // L"\\PSXSS\\SESPORT"
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);

    Status = NtCreatePort(&g_SesApiPort,
                          &ObjectAttributes,
                          sizeof(PSX_SES_CONNECT) - sizeof(PORT_MESSAGE),  // max connect info
                          sizeof(PSX_SES_MESSAGE),                         // max message
                          0);
    if (!NT_SUCCESS(Status))
        return Status;

    ThreadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PsxSesApiRequestThread,
                                NULL, 0, NULL);
    if (ThreadHandle == NULL)
        return STATUS_UNSUCCESSFUL;
    NtClose(ThreadHandle);

    return STATUS_SUCCESS;
}
