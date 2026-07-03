/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL startup: connect to the POSIX server, run the NT 4.0
 *              connect handshake (session id + CWD/root path-translation blob),
 *              map the shared section, and lay a marshalling heap over it.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

HANDLE PsxApiPort = NULL;
LONG   PsxClientToServer = 0;
PVOID  PsxSharedHeap = NULL;
PLONG  PsxErrnoLocation = NULL;
char ***PsxEnvironLocation = NULL;
ULONG  PsxSessionId = 0;

//
// CWD + root supplied by the server at connect time (ANSI NT device paths, e.g.
// "\DosDevices\X:" for the root). Path translation (path.c) builds NT paths on
// top of these; the CWD is updated locally by chdir().
//
CHAR  PsxStartupCwd[PSX_PATH_MAX]  = "\\DosDevices\\C:\\";
CHAR  PsxStartupRoot[PSX_ROOT_MAX] = "\\DosDevices\\C:";
ULONG PsxStartupCwdLen  = 0;
ULONG PsxStartupRootLen = 0;

static LONG PsxFallbackErrno = 0;

VOID
PsxSetErrno(IN LONG ErrnoValue)
{
    *(PsxErrnoLocation ? PsxErrnoLocation : &PsxFallbackErrno) = ErrnoValue;
}

//
// The NT 4.0 connect information blob (32 bytes) the server reads in
// PsxAcceptConnection. [0]/[1] are the client's exception/signal callback
// thunks; [2] points at three consecutive ANSI STRING headers {cwd, scratch,
// root} whose buffers the server fills; [3] echoes the 16-byte subsystem block
// size; [5] receives the assigned session id.
//
typedef struct _PSX_ANSI_HDR
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} PSX_ANSI_HDR;

typedef struct _PSX_CONNECT_BLOB
{
    ULONG        ExceptionThunk;    // [0]
    ULONG        SignalTrampoline;  // [1]
    PSX_ANSI_HDR *StartupHdr;       // [2]
    ULONG        SubsysBlockSize;   // [3]
    ULONG        Reserved4;         // [4]
    ULONG        SessionId;         // [5] OUT
    ULONG        Reserved6;         // [6]
    ULONG        Reserved7;         // [7]
} PSX_CONNECT_BLOB;

//
// Connect the client to \PSXSS\ApiPort. NtConnectPort maps the server's shared
// section into our address space; PsxClientToServer is the base delta used to
// turn a client pointer into the server-relative pointer carried in messages.
// The server fills the CWD/root headers during the connect (startup exchange).
//
NTSTATUS
PsxInitialize(VOID)
{
    UNICODE_STRING PortName;
    SECURITY_QUALITY_OF_SERVICE Qos;
    PORT_VIEW ClientView;
    REMOTE_PORT_VIEW ServerView;
    PSX_CONNECT_BLOB Blob;
    PSX_ANSI_HDR Hdr[3];
    static CHAR CwdBuffer[PSX_PATH_MAX];
    static CHAR RootBuffer[PSX_ROOT_MAX];
    HANDLE SectionHandle = NULL;
    LARGE_INTEGER SectionSize;
    ULONG BlobLength;
    NTSTATUS Status;

    // Idempotent: the connect happens once, from DllMain. A POSIX image's crt0
    // does NOT redo it (it assumes psxdll already connected before main runs).
    if (PsxApiPort != NULL)
        return STATUS_SUCCESS;

    RtlInitUnicodeString(&PortName, PSX_API_PORT_NAME);

    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;

    // Create the per-process 32 KiB data section the server maps in (the
    // REMOTE_PORT_VIEW). NtConnectPort maps it into our view via ClientView.
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

    // Startup-block headers: give the server buffers to write our CWD/root into.
    RtlZeroMemory(Hdr, sizeof(Hdr));
    Hdr[0].MaximumLength = sizeof(CwdBuffer);
    Hdr[0].Buffer = CwdBuffer;
    Hdr[2].MaximumLength = sizeof(RootBuffer);
    Hdr[2].Buffer = RootBuffer;

    RtlZeroMemory(&Blob, sizeof(Blob));
    Blob.StartupHdr = Hdr;
    Blob.SubsysBlockSize = 16;
    BlobLength = sizeof(Blob);

    Status = NtConnectPort(&PsxApiPort,
                           &PortName,
                           &Qos,
                           &ClientView,
                           &ServerView,
                           NULL,
                           &Blob,
                           &BlobLength);
    if (!NT_SUCCESS(Status))
        return Status;

    NtRegisterThreadTerminatePort(PsxApiPort);

    // Pointer fixup: a client pointer P maps to the server view as
    // (P + PsxClientToServer). The kernel reports where the server mapped our
    // section in ClientView.ViewRemoteBase (NOT the separate ServerView
    // REMOTE_PORT_VIEW, which NT/ReactOS does not fill on the client side) -- this
    // is the field the real NT 4.0 psxdll uses.
    PsxClientToServer = (LONG)((ULONG_PTR)ClientView.ViewRemoteBase -
                               (ULONG_PTR)ClientView.ViewBase);

    // Marshalling heap over the shared section (paths, struct stat, argv).
    PsxSharedHeap = RtlCreateHeap(HEAP_GROWABLE,
                                  ClientView.ViewBase,
                                  PSX_SESSION_SECTION_SIZE,
                                  PAGE_SIZE,
                                  NULL,
                                  NULL);

    // Consume the handshake results: session id + the filled CWD/root buffers.
    PsxSessionId = Blob.SessionId;
    if (Hdr[0].Length != 0 && Hdr[0].Length < sizeof(PsxStartupCwd))
    {
        RtlCopyMemory(PsxStartupCwd, CwdBuffer, Hdr[0].Length);
        PsxStartupCwd[Hdr[0].Length] = '\0';
        PsxStartupCwdLen = Hdr[0].Length;
    }
    if (Hdr[2].Length != 0 && Hdr[2].Length < sizeof(PsxStartupRoot))
    {
        RtlCopyMemory(PsxStartupRoot, RootBuffer, Hdr[2].Length);
        PsxStartupRoot[Hdr[2].Length] = '\0';
        PsxStartupRootLen = Hdr[2].Length;
    }

    return STATUS_SUCCESS;
}

//
// PSXDLL ordinal 17 (stdcall). The POSIX CRT startup publishes the addresses of
// its 'errno' and 'environ' cells so psxdll's stubs can report into them.
//
VOID __stdcall
__PdxInitializeData(PLONG ErrnoLocation, PVOID EnvironLocation)
{
    PsxErrnoLocation = ErrnoLocation;
    PsxEnvironLocation = (char ***)EnvironLocation;
}

//
// PSXDLL entry point invoked by a POSIX image: run the program then _exit().
//
VOID __stdcall
__PosixProcessStartup(PVOID Context)
{
    int (*Main)(void);
    PVOID Dispatch;

    PsxInitialize();

    // Context->[0x14] -> dispatch table; entry at +0x04 is the program main.
    Dispatch = *(PVOID *)((PUCHAR)Context + 0x14);
    Main = *(int (**)(void))((PUCHAR)Dispatch + 0x04);

    _exit(Main());
}

//
// psxdll's DLL init routine. The loader calls this at DLL_PROCESS_ATTACH when a
// POSIX image maps psxdll -- and this is where the per-process server connect +
// shared-section map happen, BEFORE the image's crt0/main runs (the reskit crt0
// explicitly relies on this and does not redo it).
//
#define PSX_DLL_PROCESS_ATTACH 1

BOOLEAN NTAPI
DllMain(IN PVOID DllHandle, IN ULONG Reason, IN PVOID Reserved)
{
    UNREFERENCED_PARAMETER(DllHandle);
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == PSX_DLL_PROCESS_ATTACH)
    {
        if (!NT_SUCCESS(PsxInitialize()))
            return FALSE;
    }
    return TRUE;
}
