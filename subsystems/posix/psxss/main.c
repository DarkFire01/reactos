/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Server startup, \PSXSS namespace and API port creation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

/* Client ANSI_STRING header as laid out in the client's 32-bit address space */
typedef struct _PSX_CLIENT_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    ULONG Buffer;
} PSX_CLIENT_STRING;

/**
 * @brief Handle a client connecting to \PSXSS\ApiPort.
 *
 * The process record was created when posix.exe asked us to spawn the image, so
 * it is looked up here rather than created. Unknown clients are rejected.
 */
NTSTATUS
PsxAcceptConnection(
    _Inout_ PPORT_MESSAGE ConnectMessage)
{
    PPSX_PROCESS Process;
    HANDLE PortHandle;
    NTSTATUS Status;
    REMOTE_PORT_VIEW ClientView;
    BOOLEAN Accept = TRUE;
    PULONG ConnInfo;
    PSX_CLIENT_STRING Strings[3];
    ULONG_PTR StringsAddress;

    /* The 32-byte connection info follows the PORT_MESSAGE header */
    ConnInfo = (PULONG)((PUCHAR)ConnectMessage + sizeof(PORT_MESSAGE));

    Process = PsxFindProcessByClientId(&ConnectMessage->ClientId);
    PSXTRACE("AcceptConnection: cid %p, found process %p, viewsize 0x%Ix\n",
             ConnectMessage->ClientId.UniqueProcess, Process,
             (SIZE_T)ConnectMessage->ClientViewSize);

    if ((Process != NULL) && (ConnectMessage->ClientViewSize <= 0x8000))
    {
        /* Slot 3 echoes the 16-byte subsystem block size, slot 5 carries the session id */
        ConnInfo[3] = 16;
        ConnInfo[4] = 0;
        ConnInfo[5] = Process->SessionId;
        ConnInfo[6] = 0;
        Process->Connected = TRUE;

        /* Slot 1 is the client's signal trampoline used for async delivery */
        Process->SignalTrampoline = ConnInfo[1];

        /*
         * On first connect, fill the client's CWD and root path buffers so relative
         * paths resolve. Slot 2 is the client address of three consecutive string
         * headers {CWD, scratch, root}.
         */
        if (Process->StartupBlockValid && !Process->StartupBlockDone)
        {
            StringsAddress = ConnInfo[2];

            if (NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle,
                                               (PVOID)StringsAddress,
                                               Strings,
                                               sizeof(Strings),
                                               NULL)) &&
                (Strings[0].MaximumLength >= Process->StartupCwdLen) &&
                (Strings[2].MaximumLength >= Process->StartupRootLen))
            {
                NtWriteVirtualMemory(Process->ProcessHandle,
                                     (PVOID)(ULONG_PTR)Strings[0].Buffer,
                                     Process->StartupCwd,
                                     Process->StartupCwdLen,
                                     NULL);
                NtWriteVirtualMemory(Process->ProcessHandle,
                                     (PVOID)(ULONG_PTR)Strings[2].Buffer,
                                     Process->StartupRoot,
                                     Process->StartupRootLen,
                                     NULL);
                Strings[0].Length = Process->StartupCwdLen;
                Strings[1].Length = 0;
                Strings[2].Length = Process->StartupRootLen;
                NtWriteVirtualMemory(Process->ProcessHandle,
                                     (PVOID)StringsAddress,
                                     Strings,
                                     sizeof(Strings),
                                     NULL);
                Process->StartupBlockDone = TRUE;
                PSXTRACE("startup exchange: wrote cwd(%u)+root(%u) to client\n",
                         Process->StartupCwdLen, Process->StartupRootLen);
            }
            else
            {
                PSXTRACE("startup exchange: skipped (read/capacity failed)\n");
            }
        }
    }
    else
    {
        /* Only processes spawned through SESPORT may connect */
        Accept = FALSE;
    }

    /* Map the client's shared section; Process becomes the LPC PortContext */
    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(ClientView);

    Status = NtAcceptConnectPort(&PortHandle, Process, ConnectMessage, Accept, NULL, &ClientView);
    PSXTRACE("AcceptConnection: NtAcceptConnectPort(accept=%u) status 0x%08lx\n", Accept, Status);
    if (!NT_SUCCESS(Status) || !Accept)
        return Status;

    Process->ClientPort = PortHandle;
    Process->ViewBase = (ULONG_PTR)ClientView.ViewBase;
    Process->ViewEnd = (ULONG_PTR)ClientView.ViewBase + ClientView.ViewSize;

    Status = NtCompleteConnectPort(PortHandle);
    PSXTRACE("AcceptConnection: completed, view [%p..%p) session %lu status 0x%08lx\n",
             (PVOID)Process->ViewBase, (PVOID)Process->ViewEnd, Process->SessionId, Status);
    return Status;
}

/**
 * @brief Check that a client pointer lies within the client's mapped shared section.
 */
BOOLEAN
PsxValidateClientPointer(
    _In_opt_ PPSX_PROCESS Process,
    _In_ ULONG_PTR Pointer,
    _In_ ULONG Length)
{
    if ((Process == NULL) || (Process->ViewBase == 0))
        return FALSE;
    if (Pointer < Process->ViewBase)
        return FALSE;
    if ((Pointer + Length) > Process->ViewEnd)
        return FALSE;
    return TRUE;
}

/**
 * @brief Handle the death of a client process.
 */
VOID
PsxReapProcess(
    _Inout_opt_ PPSX_PROCESS Process)
{
    if (Process == NULL)
        return;

    /* The old image of an execve() is expected to exit; its record now belongs to the new image */
    if (Process->ExecInProgress)
    {
        Process->ExecInProgress = FALSE;
        return;
    }

    PsxCancelAlarm(Process);

    /* A cleanly exited process stays a zombie until its parent waits on it */
    if (Process->State == PSX_STATE_ZOMBIE)
    {
        if (Process->ClientPort != NULL)
        {
            NtClose(Process->ClientPort);
            Process->ClientPort = NULL;
        }
        return;
    }

    /* Unexpected death: report SIGKILL so a waiting parent still collects it. TODO: reparent orphans. */
    PsxCloseAllFds(Process);
    Process->State = PSX_STATE_ZOMBIE;
    Process->ExitStatus = 9;
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
    HANDLE ThreadHandle;
    BOOLEAN WasEnabled;
    NTSTATUS Status;
    ULONG Worker;

    PsxInitProcessTable();

    /* Creating the permanent \PSXSS directory requires SeCreatePermanentPrivilege */
    RtlAdjustPrivilege(SE_CREATE_PERMANENT_PRIVILEGE, TRUE, FALSE, &WasEnabled);

    /*
     * The permanent \PSXSS directory doubles as the single-instance check: a second
     * server fails here with STATUS_OBJECT_NAME_COLLISION.
     */
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_SS_OBJECT_DIRECTORY);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_PERMANENT, NULL, &Sd);
    Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
    PSXTRACE("NtCreateDirectoryObject(\\PSXSS) status 0x%08lx\n", Status);
    if (Status == STATUS_PRIVILEGE_NOT_HELD)
    {
        /* Fall back to a temporary directory; the port creation below then catches duplicates */
        PSXTRACE("NtCreateDirectoryObject(\\PSXSS): permanent denied, retrying non-permanent\n");
        PsxInitAllowAllSd(&Sd);
        InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_OPENIF, NULL, &Sd);
        Status = NtCreateDirectoryObject(&DirectoryHandle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
        PSXTRACE("NtCreateDirectoryObject(\\PSXSS) retry status 0x%08lx\n", Status);
    }
    if (!NT_SUCCESS(Status))
        return Status;

    /* Register with smss. Failure is tolerated so the server can run standalone. */
    Status = PsxConnectToSm();
    PSXTRACE("PsxConnectToSm status 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        /* TODO: this should be fatal on a normal boot */
    }

    /* \PSXSS\ApiPort, used by every POSIX client process */
    PsxInitAllowAllSd(&Sd);
    RtlInitUnicodeString(&Name, PSX_API_PORT_NAME);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, NULL, &Sd);
    Status = NtCreatePort(&g_ApiPort,
                          &ObjectAttributes,
                          sizeof(PSX_CONNECT_INFO),
                          sizeof(PSX_API_MESSAGE),
                          0);
    PSXTRACE("NtCreatePort(\\PSXSS\\ApiPort) status 0x%08lx (handle %p)\n", Status, g_ApiPort);
    if (!NT_SUCCESS(Status))
        return Status;

    /* \PSXSS\SESPORT, used by posix.exe session leaders */
    Status = PsxCreateSessionPort();
    PSXTRACE("PsxCreateSessionPort status 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    PsxInitDispatchTable();
    PSXTRACE("Dispatch table ready; spawning workers\n");

    /* The calling thread runs one worker, start the rest */
    for (Worker = 1; Worker < PSX_API_WORKER_COUNT; Worker++)
    {
        ThreadHandle = CreateThread(NULL,
                                    0,
                                    (LPTHREAD_START_ROUTINE)PsxApiServerLoop,
                                    NULL,
                                    0,
                                    NULL);
        if (ThreadHandle != NULL)
            NtClose(ThreadHandle);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Image entry point. The C runtime startup is not used.
 */
VOID
NTAPI
PsxServerStartup(
    _In_ PVOID Peb)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Peb);

    PSXTRACE("PsxServerStartup: entry\n");
    Status = PsxServerInitialization();
    PSXTRACE("PsxServerStartup: PsxServerInitialization status 0x%08lx\n", Status);

    if (NT_SUCCESS(Status))
        PsxApiServerLoop(NULL);

    /*
     * A name collision means another instance already owns \PSXSS. ReactOS smss can
     * start redundant instances, so exit with success in that case.
     */
    if (Status == STATUS_OBJECT_NAME_COLLISION)
    {
        PSXTRACE("PsxServerStartup: \\PSXSS already owned by another instance, exiting\n");
        NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
    }

    PSXTRACE("PsxServerStartup: terminating (init failed or loop returned)\n");
    NtTerminateProcess(NtCurrentProcess(), STATUS_UNSUCCESSFUL);
}
