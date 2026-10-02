/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmhook.c
 * PURPOSE:         Configuration Manager - Registry Notifications/Callbacks
 * PROGRAMMERS:     Thomas Weidenmueller (w3seek@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

ULONG CmpCallBackCount = 0;
EX_CALLBACK CmpCallBackVector[100];

LIST_ENTRY CmiCallbackHead;
FAST_MUTEX CmiCallbackLock;

typedef struct _REGISTRY_CALLBACK
{
    LIST_ENTRY ListEntry;
    EX_RUNDOWN_REF RundownRef;
    PEX_CALLBACK_FUNCTION Function;
    PVOID Context;
    LARGE_INTEGER Cookie;
    ULONG64 Altitude;
    BOOLEAN PendingDelete;
} REGISTRY_CALLBACK, *PREGISTRY_CALLBACK;

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
CmpInitCallback(VOID)
{
    ULONG i;
    PAGED_CODE();

    /* Reset counter */
    CmpCallBackCount = 0;

    /* Loop all the callbacks */
    for (i = 0; i < CMP_MAX_CALLBACKS; i++)
    {
        /* Initialize this one */
        ExInitializeCallBack(&CmpCallBackVector[i]);
    }

    /* ROS: Initialize old-style callbacks for now */
    InitializeListHead(&CmiCallbackHead);
    ExInitializeFastMutex(&CmiCallbackLock);
}

NTSTATUS
CmiCallRegisteredCallbacks(IN REG_NOTIFY_CLASS Argument1,
                           IN PVOID Argument2)
{
    PLIST_ENTRY CurrentEntry;
    NTSTATUS Status = STATUS_SUCCESS;
    PREGISTRY_CALLBACK CurrentCallback;
    PAGED_CODE();

    ExAcquireFastMutex(&CmiCallbackLock);

    for (CurrentEntry = CmiCallbackHead.Flink;
         CurrentEntry != &CmiCallbackHead;
         CurrentEntry = CurrentEntry->Flink)
    {
        CurrentCallback = CONTAINING_RECORD(CurrentEntry, REGISTRY_CALLBACK, ListEntry);
        if (!CurrentCallback->PendingDelete &&
            ExAcquireRundownProtection(&CurrentCallback->RundownRef))
        {
            /* don't hold locks during the callbacks! */
            ExReleaseFastMutex(&CmiCallbackLock);

            Status = CurrentCallback->Function(CurrentCallback->Context,
                                         (PVOID)Argument1,
                                         Argument2);

            ExAcquireFastMutex(&CmiCallbackLock);

            /* don't release the rundown protection before holding the callback lock
            so the pointer to the next callback isn't cleared in case this callback
            get's deleted */
            ExReleaseRundownProtection(&CurrentCallback->RundownRef);
            if(!NT_SUCCESS(Status))
            {
                /* one callback returned failure, don't call any more callbacks */
                break;
            }
        }
    }

    ExReleaseFastMutex(&CmiCallbackLock);

    return Status;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * Reads an altitude, which is a decimal string, into the number it stands for.
 * Only the whole part is kept: it is what puts one callback ahead of another,
 * and a fraction only ever splits a tie between two that share it.
 */
static
BOOLEAN
CmpAltitudeToNumber(
    _In_ PCUNICODE_STRING Altitude,
    _Out_ PULONG64 Number)
{
    ULONG64 Value = 0;
    USHORT Index;
    USHORT Count;

    *Number = 0;

    if ((Altitude == NULL) || (Altitude->Buffer == NULL) || (Altitude->Length == 0))
        return FALSE;

    Count = Altitude->Length / sizeof(WCHAR);
    for (Index = 0; Index < Count; Index++)
    {
        WCHAR Digit = Altitude->Buffer[Index];

        if (Digit == L'.')
            break;

        if ((Digit < L'0') || (Digit > L'9'))
            return FALSE;

        /* Far past anything a real altitude uses, so it is a bad string */
        if (Value > (0xFFFFFFFFFFFFFFFFULL / 10))
            return FALSE;

        Value = (Value * 10) + (Digit - L'0');
    }

    /* A string that starts with the separator names no altitude at all */
    if (Index == 0)
        return FALSE;

    *Number = Value;
    return TRUE;
}

/* Puts a callback on the list, the highest altitude first */
static
NTSTATUS
CmpRegisterCallbackInternal(
    _In_ PEX_CALLBACK_FUNCTION Function,
    _In_opt_ PVOID Context,
    _In_ ULONG64 Altitude,
    _Out_ PLARGE_INTEGER Cookie)
{
    PREGISTRY_CALLBACK Callback;
    PLIST_ENTRY CurrentEntry;
    PAGED_CODE();

    if ((Function == NULL) || (Cookie == NULL))
        return STATUS_INVALID_PARAMETER;

    Callback = ExAllocatePoolWithTag(PagedPool,
                                     sizeof(*Callback),
                                     'bcMC');
    if (Callback == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* initialize the callback */
    ExInitializeRundownProtection(&Callback->RundownRef);
    Callback->Function = Function;
    Callback->Context = Context;
    Callback->Altitude = Altitude;
    Callback->PendingDelete = FALSE;

    /* add it to the callback list and receive a cookie for the callback */
    ExAcquireFastMutex(&CmiCallbackLock);

    /* FIXME - to receive a unique cookie we'll just return the pointer to the
       callback object */
    Callback->Cookie.QuadPart = (ULONG_PTR)Callback;

    /* Go in ahead of the first one that sits lower, so that equal altitudes
       keep the order they were registered in */
    for (CurrentEntry = CmiCallbackHead.Flink;
         CurrentEntry != &CmiCallbackHead;
         CurrentEntry = CurrentEntry->Flink)
    {
        PREGISTRY_CALLBACK Current = CONTAINING_RECORD(CurrentEntry,
                                                       REGISTRY_CALLBACK,
                                                       ListEntry);
        if (Current->Altitude < Altitude)
            break;
    }
    InsertTailList(CurrentEntry, &Callback->ListEntry);

    ExReleaseFastMutex(&CmiCallbackLock);

    *Cookie = Callback->Cookie;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
CmRegisterCallback(IN PEX_CALLBACK_FUNCTION Function,
                   IN PVOID Context,
                   IN OUT PLARGE_INTEGER Cookie)
{
    PAGED_CODE();
    ASSERT(Function && Cookie);

    /* Without an altitude it goes behind everything that named one */
    return CmpRegisterCallbackInternal(Function, Context, 0, Cookie);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
CmRegisterCallbackEx(
    _In_ PEX_CALLBACK_FUNCTION Function,
    _In_ PCUNICODE_STRING Altitude,
    _In_ PVOID Driver,
    _In_opt_ PVOID Context,
    _Out_ PLARGE_INTEGER Cookie,
    _Reserved_ PVOID Reserved)
{
    ULONG64 Number;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Driver);
    UNREFERENCED_PARAMETER(Reserved);

    if ((Function == NULL) || (Cookie == NULL))
        return STATUS_INVALID_PARAMETER;

    if (!CmpAltitudeToNumber(Altitude, &Number))
        return STATUS_INVALID_PARAMETER;

    return CmpRegisterCallbackInternal(Function, Context, Number, Cookie);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
CmCallbackGetKeyObjectID(
    _In_ PLARGE_INTEGER Cookie,
    _In_ PVOID Object,
    _Out_opt_ PULONG_PTR ObjectID,
    _Outptr_opt_ PCUNICODE_STRING *ObjectName)
{
    PCM_KEY_BODY KeyBody;
    PCM_KEY_CONTROL_BLOCK Kcb;
    PUNICODE_STRING Name;
    PUNICODE_STRING Published;
    PAGED_CODE();

    if ((Cookie == NULL) || (Object == NULL))
        return STATUS_INVALID_PARAMETER;

    KeyBody = (PCM_KEY_BODY)Object;
    if (KeyBody->Type != CM_KEY_BODY_TYPE)
        return STATUS_INVALID_PARAMETER;

    Kcb = KeyBody->KeyControlBlock;
    if (Kcb == NULL)
        return STATUS_INVALID_PARAMETER;

    /* The control block is what a key is known by, and it outlasts every
       handle that is open on it, so its address names the key */
    if (ObjectID != NULL)
        *ObjectID = (ULONG_PTR)Kcb;

    if (ObjectName == NULL)
        return STATUS_SUCCESS;

    if (Kcb->CallbackName == NULL)
    {
        Name = CmpConstructName(Kcb);
        if (Name == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Published = InterlockedCompareExchangePointer((PVOID *)&Kcb->CallbackName,
                                                      Name,
                                                      NULL);
        if (Published != NULL)
        {
            /* Another processor built it first, so keep the one it put up */
            ExFreePoolWithTag(Name, TAG_CM);
        }
    }

    *ObjectName = Kcb->CallbackName;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
CmUnRegisterCallback(IN LARGE_INTEGER Cookie)
{
    PLIST_ENTRY CurrentEntry;
    PREGISTRY_CALLBACK CurrentCallback;
    PAGED_CODE();

    ExAcquireFastMutex(&CmiCallbackLock);

    for (CurrentEntry = CmiCallbackHead.Flink;
         CurrentEntry != &CmiCallbackHead;
         CurrentEntry = CurrentEntry->Flink)
    {
        CurrentCallback = CONTAINING_RECORD(CurrentEntry, REGISTRY_CALLBACK, ListEntry);
        if (CurrentCallback->Cookie.QuadPart == Cookie.QuadPart)
        {
            if (!CurrentCallback->PendingDelete)
            {
                /* found the callback, don't unlink it from the list yet so we don't screw
                the calling loop */
                CurrentCallback->PendingDelete = TRUE;
                ExReleaseFastMutex(&CmiCallbackLock);

                /* if the callback is currently executing, wait until it finished */
                ExWaitForRundownProtectionRelease(&CurrentCallback->RundownRef);

                /* time to unlink it. It's now safe because every attempt to acquire a
                runtime protection on this callback will fail */
                ExAcquireFastMutex(&CmiCallbackLock);
                RemoveEntryList(&CurrentCallback->ListEntry);
                ExReleaseFastMutex(&CmiCallbackLock);

                /* free the callback */
                ExFreePoolWithTag(CurrentCallback, 'bcMC');
                return STATUS_SUCCESS;
            }
            else
            {
                /* pending delete, pretend like it already is deleted */
                ExReleaseFastMutex(&CmiCallbackLock);
                return STATUS_UNSUCCESSFUL;
            }
        }
    }

    ExReleaseFastMutex(&CmiCallbackLock);

    return STATUS_UNSUCCESSFUL;
}
