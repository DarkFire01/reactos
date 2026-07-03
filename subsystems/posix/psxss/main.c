/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXSS startup: create the \PSXSS namespace + API port, then run
 *              the server. Models the NT 4.0 psxss.exe init.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

//
// A POSIX client (its psxdll) is connecting to \PSXSS\ApiPort. The process
// record was already created when posix.exe asked us to spawn this image
// (PsxSesCreateProcess); we FIND it here -- we do not create one -- and hand the
// client its session id. Faithful to AcceptPosixConnection (sub_1F413D1).
//
NTSTATUS
PsxAcceptConnection(IN PPORT_MESSAGE ConnectMessage)
{
    PPSX_PROCESS Process;
    HANDLE PortHandle;
    NTSTATUS Status;
    REMOTE_PORT_VIEW ClientView;
    BOOLEAN Accept = TRUE;

    // The 32-byte connection blob begins right after the PORT_MESSAGE header.
    // psxdll sends {cb1, cb2, &pathbuf, size, ...} and reads the session id back
    // from dword[5] (message + 0x2C). (Offsets re-derived from sub_1F413D1.)
    PULONG ConnInfo = (PULONG)((PUCHAR)ConnectMessage + sizeof(PORT_MESSAGE));

    Process = PsxFindProcessByClientId(&ConnectMessage->ClientId);
    PSXTRACE("AcceptConnection: cid %p, found process %p, viewsize 0x%Ix\n",
             ConnectMessage->ClientId.UniqueProcess, Process,
             (SIZE_T)ConnectMessage->ClientViewSize);

    if ((Process != NULL) && (ConnectMessage->ClientViewSize <= 0x8000))
    {
        // Hand back the assigned session id so psxdll can connect to its
        // \PSXSS\PSXSES\P<id> session port. dword[3] echoes the 16-byte subsys
        // block size; dword[5] is the session id psxdll actually consumes.
        ConnInfo[3] = 16;
        ConnInfo[4] = 0;
        ConnInfo[5] = Process->SessionId;
        ConnInfo[6] = 0;
        Process->Connected = TRUE;

        // ConnInfo[0]/[1] are psxdll's exception/signal callback thunks; cb2 is
        // the signal trampoline RtlRemoteCall invokes for async delivery.
        Process->SignalTrampoline = ConnInfo[1];

        // Startup-block exchange (first connect): initialize the client's CWD +
        // root path-translation buffers so relative paths ("." etc.) resolve.
        // ConnInfo[2] is the client address of its three consecutive ANSI STRING
        // headers {CWD, scratch, root}; each is {USHORT Length, MaximumLength;
        // PCHAR Buffer}. We write our CWD/root strings into the client's buffers
        // and update the Length fields. Faithful to sub_1F413D1.
        if (Process->StartupBlockValid && !Process->StartupBlockDone)
        {
            struct { USHORT Length; USHORT MaximumLength; ULONG Buffer; } Hdr[3];
            ULONG_PTR HdrAddr = ConnInfo[2];

            if (NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)HdrAddr,
                                               Hdr, sizeof(Hdr), NULL)) &&
                (Hdr[0].MaximumLength >= Process->StartupCwdLen) &&
                (Hdr[2].MaximumLength >= Process->StartupRootLen))
            {
                NtWriteVirtualMemory(Process->ProcessHandle,
                                     (PVOID)(ULONG_PTR)Hdr[0].Buffer,
                                     Process->StartupCwd, Process->StartupCwdLen, NULL);
                NtWriteVirtualMemory(Process->ProcessHandle,
                                     (PVOID)(ULONG_PTR)Hdr[2].Buffer,
                                     Process->StartupRoot, Process->StartupRootLen, NULL);
                Hdr[0].Length = Process->StartupCwdLen;
                Hdr[1].Length = 0;
                Hdr[2].Length = Process->StartupRootLen;
                NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)HdrAddr,
                                     Hdr, sizeof(Hdr), NULL);
                Process->StartupBlockDone = TRUE;
                PSXTRACE("startup exchange: wrote cwd(%u)+root(%u) to client\n",
                         Process->StartupCwdLen, Process->StartupRootLen);
            }
            else
            {
                PSXTRACE("startup exchange: SKIPPED (read/capacity failed)\n");
            }
        }
    }
    else
    {
        // The NT 4.0 server only accepts processes it spawned (always pre-created
        // via the posix.exe -> SESPORT path). Reject anything unknown.
        Accept = FALSE;
    }

    // Accept and map the client's 32 KiB shared section into our address space.
    // The REMOTE_PORT_VIEW reports where it landed here (ViewBase/ViewSize) and
    // sets the client's ViewRemoteBase, from which it computes its client->server
    // pointer delta. Process becomes the LPC PortContext.
    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(REMOTE_PORT_VIEW);

    Status = NtAcceptConnectPort(&PortHandle, Process, ConnectMessage, Accept, NULL, &ClientView);
    PSXTRACE("AcceptConnection: NtAcceptConnectPort(accept=%u) -> 0x%08lx\n", Accept, Status);
    if (!NT_SUCCESS(Status) || !Accept)
        return Status;

    Process->ClientPort = PortHandle;
    Process->ViewBase = (ULONG_PTR)ClientView.ViewBase;
    Process->ViewEnd  = (ULONG_PTR)ClientView.ViewBase + ClientView.ViewSize;

    Status = NtCompleteConnectPort(PortHandle);
    PSXTRACE("AcceptConnection: completed, view [%p..%p) session %lu -> 0x%08lx\n",
             (PVOID)Process->ViewBase, (PVOID)Process->ViewEnd, Process->SessionId, Status);
    return Status;
}

//
// Validate that a client-supplied pointer lies within the client's mapped
// shared section. Client pointers arrive already translated into our address
// space; rejecting out-of-range pointers stops a client aiming the server at
// arbitrary memory. (See the path-op validation in the RE unlink handler.)
//
BOOLEAN
PsxValidateClientPointer(IN PPSX_PROCESS Process,
                         IN ULONG_PTR Pointer,
                         IN ULONG Length)
{
    if ((Process == NULL) || (Process->ViewBase == 0))
        return FALSE;
    if (Pointer < Process->ViewBase)
        return FALSE;
    if ((Pointer + Length) > Process->ViewEnd)
        return FALSE;
    return TRUE;
}

VOID
PsxReapProcess(IN PPSX_PROCESS Process)
{
    if (Process == NULL)
        return;

    // The old image of an execve() is expected to die; its record was re-keyed
    // to the new image, so leave it alone (just clear the one-shot flag).
    if (Process->ExecInProgress)
    {
        Process->ExecInProgress = FALSE;
        return;
    }

    // Stop any pending alarm so its timer callback cannot fire against this record.
    PsxCancelAlarm(Process);

    // A process that already exited cleanly stays as a zombie until its parent
    // waits on it; just drop its client port.
    if (Process->State == PSX_STATE_ZOMBIE)
    {
        if (Process->ClientPort != NULL)
        {
            NtClose(Process->ClientPort);
            Process->ClientPort = NULL;
        }
        return;
    }

    // Unexpected death (crash/kill): record a killed status and keep the record
    // as a zombie so a waiting parent still collects it. (Orphans currently leak
    // -- TODO: reparent to init / reap when no parent remains.)
    PsxCloseAllFds(Process);
    Process->State = PSX_STATE_ZOMBIE;
    Process->ExitStatus = 9;            // terminated by SIGKILL (signal, low 7 bits)
    if (Process->ClientPort != NULL)
    {
        NtClose(Process->ClientPort);
        Process->ClientPort = NULL;
    }
}

NTSTATUS
PsxServerInitialization(VOID)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_DESCRIPTOR Sd;
    HANDLE DirectoryHandle;
    BOOLEAN WasEnabled;
    NTSTATUS Status;

    PsxInitProcessTable();

    // Creating a permanent object (the \PSXSS directory, below) requires
    // SeCreatePermanentPrivilege to be enabled. The real psxss relies on it
    // being on by default in the SYSTEM token; we enable it explicitly so the
    // permanent-directory create does not fail with STATUS_PRIVILEGE_NOT_HELD.
    RtlAdjustPrivilege(SE_CREATE_PERMANENT_PRIVILEGE, TRUE, FALSE, &WasEnabled);

    // The \PSXSS object directory that holds our ports and per-session objects.
    // Faithful to the real psxss (main, 0x1F45E77): created OBJ_PERMANENT with a
    // NULL-DACL security descriptor. The permanence is the single-instance
    // mechanism -- a redundant psxss (see the SMP_POSIX_FLAG gap in ReactOS smss)
    // collides here on STATUS_OBJECT_NAME_COLLISION and bails cleanly, leaving
    // the first instance owning the namespace. The allow-all DACL lets the
    // user-token posix.exe/psxdll reach the objects underneath.
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_SS_OBJECT_DIRECTORY);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_PERMANENT, NULL, &Sd);
    Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
    PSXTRACE("NtCreateDirectoryObject(\\PSXSS) -> 0x%08lx\n", Status);
    if (Status == STATUS_PRIVILEGE_NOT_HELD)
    {
        // SYSTEM should hold SeCreatePermanentPrivilege, but if this ROS token
        // does not, fall back to a non-permanent directory so bring-up still
        // works (single-instance is then enforced by the port collision below,
        // which PsxServerStartup turns into a clean exit). A name collision
        // still propagates so a redundant instance bails.
        PSXTRACE("NtCreateDirectoryObject(\\PSXSS): permanent denied; retrying non-permanent\n");
        PsxInitAllowAllSd(&Sd);
        InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_OPENIF, NULL, &Sd);
        Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
        PSXTRACE("NtCreateDirectoryObject(\\PSXSS) retry -> 0x%08lx\n", Status);
    }
    if (!NT_SUCCESS(Status))
        return Status;

    // Create \PSXSS\SbApiPort + its loop and register with smss, so it routes
    // POSIX-image launches to us and delivers session events. Non-fatal here:
    // on a real boot smss must be present; for standalone testing we continue
    // and still bring up the client ApiPort.
    Status = PsxConnectToSm();
    PSXTRACE("PsxConnectToSm -> 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        // TODO: on the real boot path this should be fatal.
    }

    // \PSXSS\ApiPort -- every POSIX client process connects here. The client
    // psxdll runs under the user's token, so this port also needs the allow-all
    // DACL (faithful to sub_1F412A0: NtCreatePort with a NULL-DACL SD).
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_API_PORT_NAME);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);
    Status = NtCreatePort(&g_ApiPort,
                          &ObjectAttributes,
                          sizeof(PSX_CONNECT_INFO),     // max connect-info length
                          sizeof(PSX_API_MESSAGE),      // max message length (~0x70)
                          0);                           // default pool usage
    PSXTRACE("NtCreatePort(\\PSXSS\\ApiPort) -> 0x%08lx (handle %p)\n", Status, g_ApiPort);
    if (!NT_SUCCESS(Status))
        return Status;

    // \PSXSS\SESPORT -- the session-register port each posix.exe session leader
    // connects to. Without it posix.exe's ConnectToSubsystem() fails outright.
    Status = PsxCreateSessionPort();
    PSXTRACE("PsxCreateSessionPort -> 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    PsxInitDispatchTable();
    PSXTRACE("Dispatch table ready; spawning workers\n");

    // Spawn additional API worker threads (the main thread runs one too). A
    // blocking waitpid() occupies its worker, so other requests -- notably the
    // child's _exit() that unblocks it -- need sibling workers to make progress.
    {
        ULONG Worker;
        HANDLE ThreadHandle;
        for (Worker = 1; Worker < PSX_API_WORKER_COUNT; Worker++)
        {
            ThreadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PsxApiServerLoop,
                                        NULL, 0, NULL);
            if (ThreadHandle != NULL)
                NtClose(ThreadHandle);
        }
    }

    return STATUS_SUCCESS;
}

//
// Process entry point (PE AddressOfEntryPoint). Like the real NT 4.0 psxss we
// avoid the C runtime startup -- the loader has already initialized ntdll and
// kernel32 (Win32 env) before we run, so OpenProcess/LSA work here. This thread
// becomes one of the API workers (PsxServerInitialization spawns the rest).
//
VOID NTAPI
PsxServerStartup(IN PVOID Peb)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Peb);

    PSXTRACE("PsxServerStartup: entry\n");
    Status = PsxServerInitialization();
    PSXTRACE("PsxServerStartup: PsxServerInitialization -> 0x%08lx\n", Status);

    if (NT_SUCCESS(Status))
        PsxApiServerLoop(NULL);     // never returns on success

    // A name collision on our ports means another POSIX subsystem instance is
    // already serving \PSXSS. ReactOS smss never tags POSIX loads with
    // SMP_POSIX_FLAG, so SmpLoadSubSystem's "already loaded" dedup never fires
    // and it spawns a redundant psxss for every deferred load. That extra
    // instance is expected -- the first one owns the namespace and keeps
    // serving; we simply step aside cleanly (the real NT 4.0 psxss does the
    // same). Exit with success so this is not logged as a subsystem failure.
    if (Status == STATUS_OBJECT_NAME_COLLISION)
    {
        PSXTRACE("PsxServerStartup: \\PSXSS already owned by another instance; exiting cleanly\n");
        NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
    }

    PSXTRACE("PsxServerStartup: terminating (init failed or loop returned)\n");
    NtTerminateProcess(NtCurrentProcess(), STATUS_UNSUCCESSFUL);
}
