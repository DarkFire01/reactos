/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Win32k subsystem
 * PURPOSE:          Interface between Win32k and USERSRV
 * FILE:             win32ss/user/ntuser/csr.c
 * PROGRAMMER:       Hermes Belusca-Maito (hermes.belusca@sfr.fr), based on
 *                   the original code by Ge van Geldorp (ge@gse.nl) and by
 *                   the CSR code in NTDLL.
 */

#include <win32k.h>

DBG_DEFAULT_CHANNEL(UserCsr);

PEPROCESS gpepCSRSS = NULL;
PVOID CsrApiPort = NULL;
DWORD gdwPendingSystemThreads = 0;

VOID
InitCsrProcess(VOID /*IN PEPROCESS CsrProcess*/)
{
    /* Save the EPROCESS of CSRSS */
    gpepCSRSS = PsGetCurrentProcess();
    // gpepCSRSS = CsrProcess;
    ObReferenceObject(gpepCSRSS);
}

VOID
ResetCsrProcess(VOID)
{
    if (gpepCSRSS)
        ObDereferenceObject(gpepCSRSS);

    gpepCSRSS = NULL;
}

NTSTATUS
InitCsrApiPort(IN HANDLE CsrPortHandle)
{
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(CsrPortHandle,
                                       0,
                                       /* * */LpcPortObjectType, // or NULL,
                                       UserMode,
                                       &CsrApiPort,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        CsrApiPort = NULL;
        ERR("Failed to set CSR API Port.\n");
    }

    return Status;
}

VOID
ResetCsrApiPort(VOID)
{
    if (CsrApiPort)
        ObDereferenceObject(CsrApiPort);

    CsrApiPort = NULL;
}

/*
 * Function copied from ntdll/csr/connect.c::CsrClientCallServer
 * and adapted for kernel-mode.
 *
 * NOTE: This is really a co_* function!
 */
NTSTATUS
NTAPI
CsrClientCallServer(IN OUT PCSR_API_MESSAGE ApiMessage,
                    IN OUT PCSR_CAPTURE_BUFFER CaptureBuffer OPTIONAL,
                    IN CSR_API_NUMBER ApiNumber,
                    IN ULONG DataLength)
{
    NTSTATUS Status;
#if 0
    ULONG PointerCount;
    PULONG_PTR OffsetPointer;
#endif

    /* Do we have a connection to CSR yet? */
    if (!CsrApiPort)
        return STATUS_INVALID_PORT_HANDLE;

    /* Fill out the Port Message Header */
    ApiMessage->Header.u2.ZeroInit = 0;
    ApiMessage->Header.u1.s1.TotalLength = FIELD_OFFSET(CSR_API_MESSAGE, Data) + DataLength;
    ApiMessage->Header.u1.s1.DataLength = ApiMessage->Header.u1.s1.TotalLength -
        sizeof(ApiMessage->Header);

    /* Fill out the CSR Header */
    ApiMessage->ApiNumber = ApiNumber;
    ApiMessage->CsrCaptureData = NULL;

    TRACE("API: %lx, u1.s1.DataLength: %x, u1.s1.TotalLength: %x\n",
          ApiNumber,
          ApiMessage->Header.u1.s1.DataLength,
          ApiMessage->Header.u1.s1.TotalLength);

#if 0
    /* Check if we got a Capture Buffer */
    if (CaptureBuffer)
    {
        /*
         * We have to convert from our local (client) view
         * to the remote (server) view.
         */
        ApiMessage->CsrCaptureData = (PCSR_CAPTURE_BUFFER)
            ((ULONG_PTR)CaptureBuffer + CsrPortMemoryDelta);

        /* Lock the buffer. */
        CaptureBuffer->BufferEnd = NULL;

        /*
         * Each client pointer inside the CSR message is converted into
         * a server pointer, and each pointer to these message pointers
         * is converted into an offset.
         */
        PointerCount  = CaptureBuffer->PointerCount;
        OffsetPointer = CaptureBuffer->PointerOffsetsArray;
        while (PointerCount--)
        {
            if (*OffsetPointer != 0)
            {
                *(PULONG_PTR)*OffsetPointer += CsrPortMemoryDelta;
                *OffsetPointer -= (ULONG_PTR)ApiMessage;
            }
            ++OffsetPointer;
        }
    }
#endif

    UserLeaveCo();

    /* Send the LPC Message */

    // The wait logic below is subject to change in the future. One can
    // imagine adding an external parameter to CsrClientCallServer, or write
    // two versions of CsrClientCallServer, synchronous and asynchronous.
    if (PsGetCurrentProcess() == gpepCSRSS)
    {
        Status = LpcRequestPort(CsrApiPort,
                                &ApiMessage->Header);
    }
    else
    {
        Status = LpcRequestWaitReplyPort(CsrApiPort,
                                         &ApiMessage->Header,
                                         &ApiMessage->Header);
    }

    UserEnterCo();

#if 0
    /* Check if we got a Capture Buffer */
    if (CaptureBuffer)
    {
        /*
         * We have to convert back from the remote (server) view
         * to our local (client) view.
         */
        ApiMessage->CsrCaptureData = (PCSR_CAPTURE_BUFFER)
            ((ULONG_PTR)ApiMessage->CsrCaptureData - CsrPortMemoryDelta);

        /*
         * Convert back the offsets into pointers to CSR message
         * pointers, and convert back these message server pointers
         * into client pointers.
         */
        PointerCount  = CaptureBuffer->PointerCount;
        OffsetPointer = CaptureBuffer->PointerOffsetsArray;
        while (PointerCount--)
        {
            if (*OffsetPointer != 0)
            {
                *OffsetPointer += (ULONG_PTR)ApiMessage;
                *(PULONG_PTR)*OffsetPointer -= CsrPortMemoryDelta;
            }
            ++OffsetPointer;
        }
    }
#endif

    /* Check for success */
    if (!NT_SUCCESS(Status))
    {
        /* We failed. Overwrite the return value with the failure. */
        ERR("LPC Failed: %lx\n", Status);
        ApiMessage->Status = Status;
    }

    /* Return the CSR Result */
    TRACE("Got back: 0x%lx\n", ApiMessage->Status);
    return ApiMessage->Status;
}

/* A system thread winsrv was asked for, and what it is to run once it enters win32k */
typedef struct _USER_PENDING_SYSTEM_THREAD
{
    DWORD Type;
    PVOID Param;
} USER_PENDING_SYSTEM_THREAD, *PUSER_PENDING_SYSTEM_THREAD;

#define USER_MAX_PENDING_SYSTEM_THREADS 30

static USER_PENDING_SYSTEM_THREAD gPendingSystemThreads[USER_MAX_PENDING_SYSTEM_THREADS];

_Requires_lock_held_(UserLock)
static
BOOL
UserpPushPendingSystemThread(
    _In_ DWORD Type,
    _In_opt_ PVOID Param)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(gPendingSystemThreads); i++)
    {
        if (gPendingSystemThreads[i].Type == 0)
        {
            gPendingSystemThreads[i].Type = Type;
            gPendingSystemThreads[i].Param = Param;
            gdwPendingSystemThreads |= Type;
            return TRUE;
        }
    }

    return FALSE;
}

_Requires_lock_held_(UserLock)
static
VOID
UserpUpdatePendingSystemThreadTypes(VOID)
{
    ULONG i;

    gdwPendingSystemThreads = 0;
    for (i = 0; i < RTL_NUMBER_OF(gPendingSystemThreads); i++)
        gdwPendingSystemThreads |= gPendingSystemThreads[i].Type;
}

_Requires_lock_held_(UserLock)
static
BOOL
UserpPopPendingSystemThread(
    _Out_ PDWORD Type,
    _Out_ PVOID *Param)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(gPendingSystemThreads); i++)
    {
        if (gPendingSystemThreads[i].Type != 0)
        {
            *Type = gPendingSystemThreads[i].Type;
            *Param = gPendingSystemThreads[i].Param;
            gPendingSystemThreads[i].Type = 0;
            gPendingSystemThreads[i].Param = NULL;
            UserpUpdatePendingSystemThreadTypes();
            return TRUE;
        }
    }

    return FALSE;
}

_Requires_lock_held_(UserLock)
static
VOID
UserpRemovePendingSystemThread(
    _In_ DWORD Type,
    _In_opt_ PVOID Param)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(gPendingSystemThreads); i++)
    {
        if ((gPendingSystemThreads[i].Type == Type) && (gPendingSystemThreads[i].Param == Param))
        {
            gPendingSystemThreads[i].Type = 0;
            gPendingSystemThreads[i].Param = NULL;
            UserpUpdatePendingSystemThreadTypes();
            return;
        }
    }
}

/*
 * UserSystemThreadProc
 *
 * Called form dedicated thread in CSRSS. RIT is started in context of this
 * thread because it needs valid Win32 process with TEB initialized.
 */
DWORD UserSystemThreadProc(BOOL bRemoteProcess)
{
    DWORD Type;
    PVOID Param;

    /* Whichever request was queued first is the one this thread runs */
    if (!UserpPopPendingSystemThread(&Type, &Param))
    {
        ERR("No system thread is pending!\n");
        return 0;
    }

    UserLeave();

    TRACE("UserSystemThreadProc: %d\n", Type);

    switch (Type)
    {
        case ST_RIT: RawInputThreadMain(); break;
        case ST_DESKTOP_THREAD: DesktopThreadMain(); break;
        case ST_GHOST_THREAD: UserGhostThreadEntry(); break;
        case ST_VIDEO_CALLOUT: UserVideoPortCalloutThread(Param); break;
        default: ERR("Wrong type: %x\n", Type);
    }

    UserEnterShared();

    return 0;
}

/**
 * @brief
 * Asks winsrv for a thread in CSRSS that enters win32k and runs the given request.
 *
 * @param[in] Type
 * One of the ST_* values.
 *
 * @param[in] Param
 * Handed to the thread when it runs the request.
 *
 * @return
 * STATUS_SUCCESS once winsrv has the request.
 */
_Requires_exclusive_lock_held_(UserLock)
NTSTATUS
UserQueueSystemThread(
    _In_ DWORD Type,
    _In_opt_ PVOID Param)
{
    USER_API_MESSAGE ApiMessage;
    PUSER_CREATE_SYSTEM_THREAD pCreateThreadRequest = &ApiMessage.Data.CreateSystemThreadRequest;
    NTSTATUS Status;

    ASSERT(UserIsEnteredExclusive());

    if (!UserpPushPendingSystemThread(Type, Param))
    {
        ERR("Too many system threads pending, dropping 0x%x\n", Type);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Ask winsrv to create a new system thread. This new thread will enter win32k again calling UserSystemThreadProc */
    pCreateThreadRequest->bRemote = FALSE;
    ApiMessage.Status = STATUS_SUCCESS;
    Status = CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                                 NULL,
                                 CSR_CREATE_API_NUMBER(USERSRV_SERVERDLL_INDEX, UserpCreateSystemThreads),
                                 sizeof(USER_CREATE_SYSTEM_THREAD));
    if (!NT_SUCCESS(Status))
    {
        ERR("Csr call failed 0x%lx\n", Status);
        UserpRemovePendingSystemThread(Type, Param);
        return Status;
    }

    return STATUS_SUCCESS;
}

BOOL UserCreateSystemThread(DWORD Type)
{
    TRACE("UserCreateSystemThread: %d\n", Type);

    ASSERT(UserIsEnteredExclusive());

    if (gdwPendingSystemThreads & Type)
    {
        ERR("System thread 0x%x already pending for creation\n", Type);
        return TRUE;
    }

    return NT_SUCCESS(UserQueueSystemThread(Type, NULL));
}

/* EOF */
