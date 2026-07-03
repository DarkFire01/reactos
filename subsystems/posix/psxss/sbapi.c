/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Session Manager registration and Sb callback port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <subsys/sm/smmsg.h>

/* Client side of \SmApiPort */
HANDLE g_SmApiPort = NULL;

/**
 * @brief SbCreateSession: the SM is creating a new POSIX session.
 */
static
BOOLEAN
NTAPI
PsxSbCreateSession(
    _Inout_ PSB_API_MSG Message)
{
    PSXTRACE("SbCreateSession: smss creating a POSIX session\n");

    /* TODO: Record the session and start its leader. For now just acknowledge it. */
    Message->ReturnValue = STATUS_SUCCESS;
    return TRUE;
}

static
BOOLEAN
NTAPI
PsxSbTerminateSession(
    _Inout_ PSB_API_MSG Message)
{
    Message->ReturnValue = STATUS_NOT_IMPLEMENTED;
    return TRUE;
}

static
BOOLEAN
NTAPI
PsxSbForeignSessionComplete(
    _Inout_ PSB_API_MSG Message)
{
    Message->ReturnValue = STATUS_NOT_IMPLEMENTED;
    return TRUE;
}

static
BOOLEAN
NTAPI
PsxSbCreateProcess(
    _Inout_ PSB_API_MSG Message)
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

/**
 * @brief Accepts the SM's connection to the Sb callback port. No section is shared.
 */
static
VOID
PsxSbHandleConnectionRequest(
    _Inout_ PSB_API_MSG Message)
{
    HANDLE PortHandle = NULL;
    REMOTE_PORT_VIEW ClientView;
    NTSTATUS Status;

    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(ClientView);

    Status = NtAcceptConnectPort(&PortHandle, NULL, &Message->h, TRUE, NULL, &ClientView);
    PSXTRACE("SbAccept: NtAcceptConnectPort status 0x%08lx (port %p)\n", Status, PortHandle);
    if (NT_SUCCESS(Status))
    {
        Status = NtCompleteConnectPort(PortHandle);
        PSXTRACE("SbAccept: NtCompleteConnectPort status 0x%08lx\n", Status);
    }
}

/**
 * @brief Sb callback port loop. Receives SM session events and dispatches them
 * by SB ApiNumber, with each reply sent on the next receive.
 */
VOID
NTAPI
PsxSbApiRequestThread(
    _In_opt_ PVOID Parameter)
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

            PSXTRACE("SbApiLoop: NtReplyWaitReceivePort status 0x%08lx\n", Status);
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
            /* A FALSE return means the handler owns the reply or failed */
            if (!PsxSbApiDispatch[ReceiveMsg.ApiNumber](&ReceiveMsg))
                ReplyMsg = NULL;
        }
        else
        {
            ReplyMsg->ReturnValue = STATUS_NOT_IMPLEMENTED;
        }
    }
}

/**
 * @brief Creates \PSXSS\SbApiPort, starts its loop and registers this subsystem
 * with the SM for POSIX images.
 */
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
    PSXTRACE("NtCreatePort(\\PSXSS\\SbApiPort) status 0x%08lx (handle %p)\n", Status, g_SbApiPort);
    if (!NT_SUCCESS(Status))
        return Status;

    /* CreateThread makes the worker a registered Win32 thread */
    ThreadHandle = CreateThread(NULL,
                                0,
                                (LPTHREAD_START_ROUTINE)PsxSbApiRequestThread,
                                NULL,
                                0,
                                NULL);
    PSXTRACE("CreateThread(SbApiLoop) returned %p\n", ThreadHandle);
    if (ThreadHandle == NULL)
        return STATUS_UNSUCCESSFUL;
    NtClose(ThreadHandle);

    PSXTRACE("Calling SmConnectToSm(name='%wZ', image=POSIX_CUI)\n", &PortName);
    Status = SmConnectToSm(&PortName,
                           g_SbApiPort,
                           IMAGE_SUBSYSTEM_POSIX_CUI,
                           &g_SmApiPort);
    PSXTRACE("SmConnectToSm status 0x%08lx (SmApiPort %p)\n", Status, g_SmApiPort);
    return Status;
}
