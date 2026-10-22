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
LARGE_INTEGER CmpCallbackCookie;

/* Callbacks registered without an altitude share this one */
static const UNICODE_STRING CmpLegacyAltitude = RTL_CONSTANT_STRING(L"425000");

typedef struct _REGISTRY_CALLBACK
{
    LIST_ENTRY ListEntry;
    EX_RUNDOWN_REF RundownRef;
    PEX_CALLBACK_FUNCTION Function;
    PVOID Context;
    LARGE_INTEGER Cookie;
    BOOLEAN PendingDelete;
    UNICODE_STRING Altitude;
} REGISTRY_CALLBACK, *PREGISTRY_CALLBACK;

typedef struct _CMP_ALTITUDE_PARTS
{
    PCWCH Whole;
    USHORT WholeLength;
    PCWCH Fraction;
    USHORT FractionLength;
} CMP_ALTITUDE_PARTS, *PCMP_ALTITUDE_PARTS;

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

/* Leading zeros of the whole part and trailing zeros of the fraction carry no weight */
static
VOID
CmpSplitAltitude(
    _In_ PCUNICODE_STRING Altitude,
    _Out_ PCMP_ALTITUDE_PARTS Parts)
{
    USHORT Length = Altitude->Length / sizeof(WCHAR);
    USHORT Dot = 0;

    while (Dot < Length && Altitude->Buffer[Dot] != L'.')
        Dot++;

    Parts->Whole = Altitude->Buffer;
    Parts->WholeLength = Dot;
    while (Parts->WholeLength > 0 && *Parts->Whole == L'0')
    {
        Parts->Whole++;
        Parts->WholeLength--;
    }

    Parts->Fraction = &Altitude->Buffer[Dot];
    Parts->FractionLength = 0;
    if (Dot < Length)
    {
        Parts->Fraction++;
        Parts->FractionLength = Length - Dot - 1;
    }
    while (Parts->FractionLength > 0 && Parts->Fraction[Parts->FractionLength - 1] == L'0')
        Parts->FractionLength--;
}

/* Altitudes are decimal numbers written out as strings; returns -1, 0 or 1 */
static
LONG
CmpCompareAltitudes(
    _In_ PCUNICODE_STRING Altitude1,
    _In_ PCUNICODE_STRING Altitude2)
{
    CMP_ALTITUDE_PARTS First, Second;
    USHORT Index, Shorter;

    CmpSplitAltitude(Altitude1, &First);
    CmpSplitAltitude(Altitude2, &Second);

    if (First.WholeLength != Second.WholeLength)
        return (First.WholeLength > Second.WholeLength) ? 1 : -1;

    for (Index = 0; Index < First.WholeLength; Index++)
    {
        if (First.Whole[Index] != Second.Whole[Index])
            return (First.Whole[Index] > Second.Whole[Index]) ? 1 : -1;
    }

    Shorter = min(First.FractionLength, Second.FractionLength);
    for (Index = 0; Index < Shorter; Index++)
    {
        if (First.Fraction[Index] != Second.Fraction[Index])
            return (First.Fraction[Index] > Second.Fraction[Index]) ? 1 : -1;
    }

    if (First.FractionLength != Second.FractionLength)
        return (First.FractionLength > Second.FractionLength) ? 1 : -1;

    return 0;
}

/*
 * The list runs from the highest altitude down. Legacy callbacks queue up
 * behind others at the same altitude, anyone else must pick a free one.
 */
static
NTSTATUS
CmpRegisterCallbackInternal(
    _In_ PEX_CALLBACK_FUNCTION Function,
    _In_opt_ PVOID Context,
    _In_ PCUNICODE_STRING Altitude,
    _In_ BOOLEAN LegacyCallback,
    _Out_ PLARGE_INTEGER Cookie)
{
    PREGISTRY_CALLBACK Callback, Existing;
    PLIST_ENTRY InsertBefore;
    NTSTATUS Status = STATUS_SUCCESS;
    LONG Order;
    PAGED_CODE();

    Callback = ExAllocatePoolWithTag(PagedPool,
                                     sizeof(*Callback),
                                     'bcMC');
    if (Callback == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Callback->Altitude.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                      max(Altitude->Length, sizeof(WCHAR)),
                                                      'acMC');
    if (Callback->Altitude.Buffer == NULL)
    {
        ExFreePoolWithTag(Callback, 'bcMC');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Callback->Altitude.Buffer, Altitude->Buffer, Altitude->Length);
    Callback->Altitude.Length = Altitude->Length;
    Callback->Altitude.MaximumLength = Altitude->Length;

    ExInitializeRundownProtection(&Callback->RundownRef);
    Callback->Function = Function;
    Callback->Context = Context;
    Callback->PendingDelete = FALSE;

    ExAcquireFastMutex(&CmiCallbackLock);

    CmpCallbackCookie.QuadPart++;
    Callback->Cookie = CmpCallbackCookie;

    for (InsertBefore = CmiCallbackHead.Flink;
         InsertBefore != &CmiCallbackHead;
         InsertBefore = InsertBefore->Flink)
    {
        Existing = CONTAINING_RECORD(InsertBefore, REGISTRY_CALLBACK, ListEntry);
        Order = CmpCompareAltitudes(&Existing->Altitude, Altitude);
        if (Order < 0)
            break;
        if (Order == 0 && !LegacyCallback)
        {
            Status = STATUS_FLT_INSTANCE_ALTITUDE_COLLISION;
            break;
        }
    }

    if (NT_SUCCESS(Status))
        InsertTailList(InsertBefore, &Callback->ListEntry);

    ExReleaseFastMutex(&CmiCallbackLock);

    /* The cookie is handed out even when the altitude was taken */
    *Cookie = Callback->Cookie;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Callback->Altitude.Buffer, 'acMC');
        ExFreePoolWithTag(Callback, 'bcMC');
    }

    return Status;
}

/* PUBLIC FUNCTIONS **********************************************************/

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

    return CmpRegisterCallbackInternal(Function,
                                       Context,
                                       &CmpLegacyAltitude,
                                       TRUE,
                                       Cookie);
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
    PAGED_CODE();

    if (Driver == NULL)
        return STATUS_INVALID_PARAMETER_3;
    if (Reserved != NULL)
        return STATUS_INVALID_PARAMETER_6;

    return CmpRegisterCallbackInternal(Function,
                                       Context,
                                       Altitude,
                                       FALSE,
                                       Cookie);
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
                ExFreePoolWithTag(CurrentCallback->Altitude.Buffer, 'acMC');
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
