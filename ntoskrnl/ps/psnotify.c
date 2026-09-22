/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/psnotify.c
 * PURPOSE:         Process Manager: Callbacks to Registered Clients (Drivers)
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Thomas Weidenmueller (w3seek@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

BOOLEAN PsImageNotifyEnabled = FALSE;
ULONG PspThreadNotifyRoutineCount, PspProcessNotifyRoutineCount;
ULONG PspLoadImageNotifyRoutineCount;
EX_CALLBACK PspThreadNotifyRoutine[PSP_MAX_CREATE_THREAD_NOTIFY];
EX_CALLBACK PspProcessNotifyRoutine[PSP_MAX_CREATE_PROCESS_NOTIFY];
EX_CALLBACK PspLoadImageNotifyRoutine[PSP_MAX_LOAD_IMAGE_NOTIFY];
PLEGO_NOTIFY_ROUTINE PspLegoNotifyRoutine;

/* Callback block context of a routine registered with the Ex variant */
#define PSP_NOTIFY_EXTENDED ((PVOID)1)

/* PRIVATE FUNCTIONS *********************************************************/

static
NTSTATUS
PspSetCreateProcessNotifyRoutine(
    _In_ PVOID NotifyRoutine,
    _In_opt_ PVOID Context,
    _In_ BOOLEAN Remove)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Look for the routine; it is either being removed or must not be there yet */
    for (i = 0; i < PSP_MAX_CREATE_PROCESS_NOTIFY; i++)
    {
        CallBack = ExReferenceCallBackBlock(&PspProcessNotifyRoutine[i]);
        if (!CallBack)
            continue;

        if ((ExGetCallBackBlockRoutine(CallBack) != NotifyRoutine) ||
            (ExGetCallBackBlockContext(CallBack) != Context))
        {
            ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i], CallBack);
            continue;
        }

        if (!Remove)
        {
            ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i], CallBack);
            return STATUS_INVALID_PARAMETER;
        }

        if (ExCompareExchangeCallBack(&PspProcessNotifyRoutine[i], NULL, CallBack))
        {
            InterlockedDecrement((PLONG)&PspProcessNotifyRoutineCount);
            ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i], CallBack);

            /* Wait for callbacks still running, then free the block */
            ExWaitForCallBacks(CallBack);
            ExFreeCallBack(CallBack);
            return STATUS_SUCCESS;
        }

        ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i], CallBack);
    }

    if (Remove)
        return STATUS_PROCEDURE_NOT_FOUND;

    CallBack = ExAllocateCallBack(NotifyRoutine, Context);
    if (!CallBack)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < PSP_MAX_CREATE_PROCESS_NOTIFY; i++)
    {
        if (ExCompareExchangeCallBack(&PspProcessNotifyRoutine[i], CallBack, NULL))
        {
            InterlockedIncrement((PLONG)&PspProcessNotifyRoutineCount);
            return STATUS_SUCCESS;
        }
    }

    /* Every slot is taken */
    ExFreeCallBack(CallBack);
    return STATUS_INVALID_PARAMETER;
}

/**
 * @brief
 * Calls the registered process notify routines.
 *
 * @param[in] Process
 * The process being created or deleted.
 *
 * @param[in] Create
 * TRUE on creation, FALSE on deletion.
 *
 * @return
 * On creation, the status an extended routine used to refuse the process, or
 * STATUS_SUCCESS. Always STATUS_SUCCESS on deletion.
 */
NTSTATUS
NTAPI
PspRunCreateProcessNotifyRoutines(
    _In_ PEPROCESS Process,
    _In_ BOOLEAN Create)
{
    PS_CREATE_NOTIFY_INFO CreateInfo;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PFILE_OBJECT FileObject = NULL;
    PVOID Routine;
    ULONG i;

    if (!PspProcessNotifyRoutineCount)
        return STATUS_SUCCESS;

    RtlZeroMemory(&CreateInfo, sizeof(CreateInfo));
    if (Create)
    {
        CreateInfo.Size = sizeof(CreateInfo);
        CreateInfo.ParentProcessId = Process->InheritedFromUniqueProcessId;
        CreateInfo.CreatingThreadId = PsGetCurrentThread()->Cid;
        CreateInfo.CreationStatus = STATUS_SUCCESS;

        /* System processes have no image file */
        if (NT_SUCCESS(PsReferenceProcessFilePointer(Process, &FileObject)))
        {
            CreateInfo.FileObject = FileObject;
            CreateInfo.ImageFileName = &FileObject->FileName;
        }
    }

    for (i = 0; i < PSP_MAX_CREATE_PROCESS_NOTIFY; i++)
    {
        CallBack = ExReferenceCallBackBlock(&PspProcessNotifyRoutine[i]);
        if (!CallBack)
            continue;

        Routine = ExGetCallBackBlockRoutine(CallBack);
        if (ExGetCallBackBlockContext(CallBack) == PSP_NOTIFY_EXTENDED)
        {
            ((PCREATE_PROCESS_NOTIFY_ROUTINE_EX)Routine)(Process,
                                                         Process->UniqueProcessId,
                                                         Create ? &CreateInfo : NULL);
        }
        else
        {
            ((PCREATE_PROCESS_NOTIFY_ROUTINE)Routine)(Process->InheritedFromUniqueProcessId,
                                                      Process->UniqueProcessId,
                                                      Create);
        }

        ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i], CallBack);

        /* An extended routine refused the new process */
        if (!NT_SUCCESS(CreateInfo.CreationStatus))
            break;
    }

    if (FileObject)
        ObDereferenceObject(FileObject);

    return CreateInfo.CreationStatus;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetCreateProcessNotifyRoutine(IN PCREATE_PROCESS_NOTIFY_ROUTINE NotifyRoutine,
                                IN BOOLEAN Remove)
{
    return PspSetCreateProcessNotifyRoutine((PVOID)NotifyRoutine, NULL, Remove);
}

/**
 * @brief
 * Registers or removes a process notify routine that receives the process
 * object and, on creation, a PS_CREATE_NOTIFY_INFO it may use to refuse the
 * process.
 *
 * @param[in] NotifyRoutine
 * The routine.
 *
 * @param[in] Remove
 * TRUE to remove the routine.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER if the routine is already
 * registered or no slot is free, or STATUS_PROCEDURE_NOT_FOUND when removing
 * a routine that is not registered.
 */
NTSTATUS
NTAPI
PsSetCreateProcessNotifyRoutineEx(
    _In_ PCREATE_PROCESS_NOTIFY_ROUTINE_EX NotifyRoutine,
    _In_ BOOLEAN Remove)
{
    return PspSetCreateProcessNotifyRoutine((PVOID)NotifyRoutine, PSP_NOTIFY_EXTENDED, Remove);
}

/*
 * @implemented
 */
ULONG
NTAPI
PsSetLegoNotifyRoutine(PVOID LegoNotifyRoutine)
{
    /* Set the System-Wide Lego Routine */
    PspLegoNotifyRoutine = LegoNotifyRoutine;

    /* Return the location to the Lego Data */
    return FIELD_OFFSET(KTHREAD, LegoData);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsRemoveLoadImageNotifyRoutine(IN PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Loop all callbacks */
    for (i = 0; i < PSP_MAX_LOAD_IMAGE_NOTIFY; i++)
    {
        /* Reference this slot */
        CallBack = ExReferenceCallBackBlock(&PspLoadImageNotifyRoutine[i]);
        if (CallBack)
        {
            /* Check for a match */
            if (ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine)
            {
                /* Try removing it if it matches */
                if (ExCompareExchangeCallBack(&PspLoadImageNotifyRoutine[i],
                                              NULL,
                                              CallBack))
                {
                    /* We removed it, now dereference the block */
                    InterlockedDecrement((PLONG)&PspLoadImageNotifyRoutineCount);
                    ExDereferenceCallBackBlock(&PspLoadImageNotifyRoutine[i],
                                               CallBack);

                    /* Wait for active callbacks */
                    ExWaitForCallBacks(CallBack);

                    /* Free the callback and return */
                    ExFreeCallBack(CallBack);
                    return STATUS_SUCCESS;
                }
            }

            /* Dereference the callback */
            ExDereferenceCallBackBlock(&PspLoadImageNotifyRoutine[i], CallBack);
        }
    }

    /* Nothing found to remove */
    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetLoadImageNotifyRoutine(IN PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Allocate a callback */
    CallBack = ExAllocateCallBack((PVOID)NotifyRoutine, NULL);
    if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

    /* Loop callbacks */
    for (i = 0; i < PSP_MAX_LOAD_IMAGE_NOTIFY; i++)
    {
        /* Add this entry if the slot is empty */
        if (ExCompareExchangeCallBack(&PspLoadImageNotifyRoutine[i],
                                      CallBack,
                                      NULL))
        {
            /* Return success */
            InterlockedIncrement((PLONG)&PspLoadImageNotifyRoutineCount);
            PsImageNotifyEnabled = TRUE;
            return STATUS_SUCCESS;
        }
    }

    /* No free space found, fail */
    ExFreeCallBack(CallBack);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsRemoveCreateThreadNotifyRoutine(IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Loop all callbacks */
    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        /* Reference this slot */
        CallBack = ExReferenceCallBackBlock(&PspThreadNotifyRoutine[i]);
        if (CallBack)
        {
            /* Check for a match */
            if (ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine)
            {
                /* Try removing it if it matches */
                if (ExCompareExchangeCallBack(&PspThreadNotifyRoutine[i],
                                              NULL,
                                              CallBack))
                {
                    /* We removed it, now dereference the block */
                    InterlockedDecrement((PLONG)&PspThreadNotifyRoutineCount);
                    ExDereferenceCallBackBlock(&PspThreadNotifyRoutine[i],
                                               CallBack);

                    /* Wait for active callbacks */
                    ExWaitForCallBacks(CallBack);

                    /* Free the callback and return */
                    ExFreeCallBack(CallBack);
                    return STATUS_SUCCESS;
                }
            }

            /* Dereference the callback */
            ExDereferenceCallBackBlock(&PspThreadNotifyRoutine[i], CallBack);
        }
    }

    /* Nothing found to remove */
    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetCreateThreadNotifyRoutine(IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Allocate a callback */
    CallBack = ExAllocateCallBack((PVOID)NotifyRoutine, NULL);
    if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

    /* Loop callbacks */
    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        /* Add this entry if the slot is empty */
        if (ExCompareExchangeCallBack(&PspThreadNotifyRoutine[i],
                                      CallBack,
                                      NULL))
        {
            /* Return success */
            InterlockedIncrement((PLONG)&PspThreadNotifyRoutineCount);
            return STATUS_SUCCESS;
        }
    }

    /* No free space found, fail */
    ExFreeCallBack(CallBack);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* EOF */
