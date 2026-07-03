/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SM registration: create the Sb callback port, register with the
 *              Session Manager (smss), and serve its Sb session callbacks.
 *              Models the NT 4.0 psxss CreateSbApiPortAndThread + SmConnectToSm.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <subsys/sm/smmsg.h>

HANDLE g_SmApiPort = NULL;      // client side of \SmApiPort (to talk back to SM)

//
// SbCreateSession (SM -> us): a new POSIX session is being created. The SM has
// already created the subsystem session process and passes it in ProcessInfo.
//
static BOOLEAN NTAPI
PsxSbCreateSession(IN PSB_API_MSG Message)
{
    PSXTRACE("SbCreateSession: smss creating a POSIX session\n");
    // TODO: record the session and start its leader (posix.exe, the controlling-
    //       terminal server). For now just acknowledge so boot can proceed.
    Message->ReturnValue = STATUS_SUCCESS;
    return TRUE;
}

static BOOLEAN NTAPI
PsxSbTerminateSession(IN PSB_API_MSG Message)
{
    Message->ReturnValue = STATUS_NOT_IMPLEMENTED;
    return TRUE;
}

static BOOLEAN NTAPI
PsxSbForeignSessionComplete(IN PSB_API_MSG Message)
{
    Message->ReturnValue = STATUS_NOT_IMPLEMENTED;
    return TRUE;
}

static BOOLEAN NTAPI
PsxSbCreateProcess(IN PSB_API_MSG Message)
{
    Message->ReturnValue = STATUS_NOT_IMPLEMENTED;
    return TRUE;
}

static PSB_API_ROUTINE PsxSbApiDispatch[SbpMaxApiNumber] =
{
    PsxSbCreateSession,
    PsxSbTerminateSession,
    PsxSbForeignSessionComplete,
    PsxSbCreateProcess,
};

//
// Accept the SM's connection to our Sb callback port (the SM is the only valid
// peer; there is no shared section on this port).
//
static VOID
PsxSbHandleConnectionRequest(IN PSB_API_MSG Message)
{
    HANDLE PortHandle = NULL;
    REMOTE_PORT_VIEW ClientView;
    NTSTATUS Status;

    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(REMOTE_PORT_VIEW);

    Status = NtAcceptConnectPort(&PortHandle, NULL, &Message->h, TRUE, NULL, &ClientView);
    PSXTRACE("SbAccept: NtAcceptConnectPort -> 0x%08lx (port %p)\n", Status, PortHandle);
    if (NT_SUCCESS(Status))
    {
        Status = NtCompleteConnectPort(PortHandle);
        PSXTRACE("SbAccept: NtCompleteConnectPort -> 0x%08lx\n", Status);
    }
}

//
// The Sb callback-port loop (mirrors CsrSbApiRequestThread): receive SM session
// events, dispatch on the SB ApiNumber, reply fused into the next receive.
//
VOID NTAPI
PsxSbApiRequestThread(IN PVOID Parameter)
{
    NTSTATUS Status;
    SB_API_MSG ReceiveMsg;
    PSB_API_MSG ReplyMsg = NULL;
    PVOID PortContext;
    ULONG MessageType;

    UNREFERENCED_PARAMETER(Parameter);

    PSXTRACE("SbApiLoop: worker started, waiting on \\PSXSS\\SbApiPort\n");

    for (;;)
    {
        Status = NtReplyWaitReceivePort(g_SbApiPort,
                                        &PortContext,
                                        (ReplyMsg != NULL) ? &ReplyMsg->h : NULL,
                                        &ReceiveMsg.h);
        if (Status != STATUS_SUCCESS)
        {
            if (NT_SUCCESS(Status))
                continue;
            PSXTRACE("SbApiLoop: NtReplyWaitReceivePort -> 0x%08lx\n", Status);
            ReplyMsg = NULL;
            continue;
        }

        MessageType = ReceiveMsg.h.u2.s2.Type;
        PSXTRACE("SbApiLoop: message type %lu\n", MessageType);

        if (MessageType == LPC_CONNECTION_REQUEST)
        {
            PSXTRACE("SbApiLoop: SM connecting to our Sb port\n");
            PsxSbHandleConnectionRequest(&ReceiveMsg);
            PSXTRACE("SbApiLoop: handled SM connect, looping\n");
            ReplyMsg = NULL;
            continue;
        }

        if (MessageType == LPC_PORT_CLOSED)
        {
            if (PortContext != NULL)
                NtClose((HANDLE)PortContext);
            ReplyMsg = NULL;
            continue;
        }
        else if (MessageType == LPC_CLIENT_DIED)
        {
            ReplyMsg = NULL;
            continue;
        }

        ReplyMsg = &ReceiveMsg;

        if (ReceiveMsg.ApiNumber < SbpMaxApiNumber)
        {
            if (!PsxSbApiDispatch[ReceiveMsg.ApiNumber](&ReceiveMsg))
                ReplyMsg = NULL;        // handler owns the reply / failed
        }
        else
        {
            ReplyMsg->ReturnValue = STATUS_NOT_IMPLEMENTED;
        }
    }
}

//
// Create \PSXSS\SbApiPort, start its loop, and register this subsystem with the
// SM (handing it the Sb port name + the POSIX image type). After this, smss
// routes POSIX-image launches to us and calls SbCreateSession on new sessions.
//
NTSTATUS
PsxConnectToSm(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING PortName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ThreadHandle;

    RtlInitUnicodeString(&PortName, PSX_SM_CALLBACK_PORT_NAME);
    InitializeObjectAttributes(&ObjectAttributes, &PortName, 0, NULL, NULL);

    Status = NtCreatePort(&g_SbApiPort,
                          &ObjectAttributes,
                          sizeof(SB_CONNECTION_INFO),
                          sizeof(SB_API_MSG),
                          32 * sizeof(SB_API_MSG));
    PSXTRACE("NtCreatePort(\\PSXSS\\SbApiPort) -> 0x%08lx (handle %p)\n", Status, g_SbApiPort);
    if (!NT_SUCCESS(Status))
        return Status;

    // Use kernel32 CreateThread (like the real psxss) so the worker is a proper
    // csrss-registered Win32 thread, not a raw native thread.
    ThreadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PsxSbApiRequestThread,
                                NULL, 0, NULL);
    PSXTRACE("CreateThread(SbApiLoop) -> %p\n", ThreadHandle);
    if (ThreadHandle == NULL)
        return STATUS_UNSUCCESSFUL;
    NtClose(ThreadHandle);

    /* Register with the Session Manager (NTDLL!RtlConnectToSm under the hood). */
    PSXTRACE("Calling SmConnectToSm(name='%wZ', image=POSIX_CUI)...\n", &PortName);
    Status = SmConnectToSm(&PortName,
                           g_SbApiPort,
                           IMAGE_SUBSYSTEM_POSIX_CUI,
                           &g_SmApiPort);
    PSXTRACE("SmConnectToSm -> 0x%08lx (SmApiPort %p)\n", Status, g_SmApiPort);
    return Status;
}
