/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Notification Facility kernel interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * State names are kept system wide, scopes are not told apart. Each name
 * holds the data last published to it and a change stamp. Subscribers are
 * called back from a worker thread whenever the stamp moves past what they
 * have seen.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DATA ***********************************************************************/

#define TAG_WNF                     'fnWE'
#define EXP_WNF_MAX_DATA_SIZE       4096
#define EXP_WNF_VALID_DELIVERY      0x1F
#define EXP_WNF_EVENT_DATA_CHANGED  1

typedef NTSTATUS
(NTAPI *PEXP_WNF_CALLBACK)(
    _In_ PVOID Subscription,
    _In_ const VOID *StateName,
    _In_ ULONG SubscribedEventSet,
    _In_ ULONG ChangeStamp,
    _In_opt_ LPCGUID TypeId,
    _In_opt_ PVOID CallbackContext);

typedef struct _EXP_WNF_NAME
{
    LIST_ENTRY Link;
    ULONGLONG Name;
    ULONG ChangeStamp;
    ULONG DataSize;
    PVOID Data;
    LIST_ENTRY Subscribers;
} EXP_WNF_NAME, *PEXP_WNF_NAME;

typedef struct _EXP_WNF_SUBSCRIBER
{
    LIST_ENTRY Link;
    PEXP_WNF_NAME Name;
    PEXP_WNF_CALLBACK Callback;
    PVOID CallbackContext;
    ULONG DeliveryOption;
    ULONG SeenStamp;
    volatile LONG References;
    BOOLEAN Removed;
    PETHREAD CallingThread;
    EX_RUNDOWN_REF Rundown;
} EXP_WNF_SUBSCRIBER, *PEXP_WNF_SUBSCRIBER;

typedef struct _EXP_WNF_DELIVERY
{
    WORK_QUEUE_ITEM WorkItem;
    PEXP_WNF_SUBSCRIBER Subscriber;
    ULONG ChangeStamp;
} EXP_WNF_DELIVERY, *PEXP_WNF_DELIVERY;

static LIST_ENTRY ExpWnfNames = { &ExpWnfNames, &ExpWnfNames };
static EX_PUSH_LOCK ExpWnfLock;

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
ExpWnfLockExclusive(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&ExpWnfLock);
}

static
VOID
ExpWnfUnlock(VOID)
{
    ExReleasePushLockExclusive(&ExpWnfLock);
    KeLeaveCriticalRegion();
}

/* Caller holds the lock */
static
PEXP_WNF_NAME
ExpWnfFindName(
    _In_ ULONGLONG Name,
    _In_ BOOLEAN Create)
{
    PEXP_WNF_NAME Entry;
    PLIST_ENTRY Link;

    for (Link = ExpWnfNames.Flink; Link != &ExpWnfNames; Link = Link->Flink)
    {
        Entry = CONTAINING_RECORD(Link, EXP_WNF_NAME, Link);
        if (Entry->Name == Name)
            return Entry;
    }

    if (!Create)
        return NULL;

    Entry = ExAllocatePoolZero(NonPagedPool, sizeof(*Entry), TAG_WNF);
    if (!Entry)
        return NULL;

    Entry->Name = Name;
    InitializeListHead(&Entry->Subscribers);
    InsertTailList(&ExpWnfNames, &Entry->Link);
    return Entry;
}

static
VOID
ExpWnfDereferenceSubscriber(
    _In_ PEXP_WNF_SUBSCRIBER Subscriber)
{
    if (InterlockedDecrement(&Subscriber->References) == 0)
        ExFreePoolWithTag(Subscriber, TAG_WNF);
}

static
VOID
NTAPI
ExpWnfDeliveryWorker(
    _In_ PVOID Context)
{
    PEXP_WNF_DELIVERY Delivery = Context;
    PEXP_WNF_SUBSCRIBER Subscriber = Delivery->Subscriber;
    ULONGLONG Name = Subscriber->Name->Name;

    /* A delivery queued before the subscriber left is dropped */
    if (!Subscriber->Removed && ExAcquireRundownProtection(&Subscriber->Rundown))
    {
        Subscriber->CallingThread = PsGetCurrentThread();
        Subscriber->Callback(Subscriber, &Name, EXP_WNF_EVENT_DATA_CHANGED,
                             Delivery->ChangeStamp, NULL, Subscriber->CallbackContext);
        Subscriber->CallingThread = NULL;
        ExReleaseRundownProtection(&Subscriber->Rundown);
    }

    ExpWnfDereferenceSubscriber(Subscriber);
    ExFreePoolWithTag(Delivery, TAG_WNF);
}

/* Caller holds the lock. A stamp the subscriber has already seen is not delivered again */
static
VOID
ExpWnfQueueDelivery(
    _In_ PEXP_WNF_SUBSCRIBER Subscriber,
    _In_ ULONG ChangeStamp)
{
    PEXP_WNF_DELIVERY Delivery;

    if (Subscriber->Removed || ChangeStamp <= Subscriber->SeenStamp)
        return;

    Delivery = ExAllocatePoolZero(NonPagedPool, sizeof(*Delivery), TAG_WNF);
    if (!Delivery)
        return;

    Subscriber->SeenStamp = ChangeStamp;
    InterlockedIncrement(&Subscriber->References);
    Delivery->Subscriber = Subscriber;
    Delivery->ChangeStamp = ChangeStamp;
    ExInitializeWorkItem(&Delivery->WorkItem, ExpWnfDeliveryWorker, Delivery);
    ExQueueWorkItem(&Delivery->WorkItem, DelayedWorkQueue);
}

/* Caller holds the lock */
static
NTSTATUS
ExpWnfCopyData(
    _In_ PEXP_WNF_NAME Entry,
    _Out_ PULONG ChangeStamp,
    _Out_writes_bytes_to_opt_(*BufferSize, *BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    ULONG Available = *BufferSize;

    *ChangeStamp = Entry ? Entry->ChangeStamp : 0;
    *BufferSize = Entry ? Entry->DataSize : 0;
    if (!Entry || !Entry->DataSize)
        return STATUS_SUCCESS;

    if (!Buffer || Available < Entry->DataSize)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Buffer, Entry->Data, Entry->DataSize);
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Publishes new data for a notification state and tells its subscribers.
 *
 * @param[in] MatchingChangeStamp
 * The change stamp the state must have when @p CheckStamp is set.
 *
 * @return
 * STATUS_UNSUCCESSFUL when @p CheckStamp is set and the state moved on.
 */
NTSTATUS
NTAPI
ZwUpdateWnfStateData(
    _In_ const VOID *StateName,
    _In_reads_bytes_opt_(Length) const VOID *Buffer,
    _In_opt_ ULONG Length,
    _In_opt_ LPCGUID TypeId,
    _In_opt_ const VOID *ExplicitScope,
    _In_ ULONG MatchingChangeStamp,
    _In_ LOGICAL CheckStamp)
{
    PEXP_WNF_SUBSCRIBER Subscriber;
    PEXP_WNF_NAME Entry;
    PVOID Data = NULL;
    PLIST_ENTRY Link;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(TypeId);
    UNREFERENCED_PARAMETER(ExplicitScope);

    if (!StateName || Length > EXP_WNF_MAX_DATA_SIZE || (Length && !Buffer))
        return STATUS_INVALID_PARAMETER;

    if (Length)
    {
        Data = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_WNF);
        if (!Data)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(Data, Buffer, Length);
    }

    ExpWnfLockExclusive();

    Entry = ExpWnfFindName(*(const ULONGLONG *)StateName, TRUE);
    if (!Entry)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else if (CheckStamp && Entry->ChangeStamp != MatchingChangeStamp)
    {
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        if (Entry->Data)
            ExFreePoolWithTag(Entry->Data, TAG_WNF);
        Entry->Data = Data;
        Entry->DataSize = Length;
        Entry->ChangeStamp++;
        Data = NULL;

        for (Link = Entry->Subscribers.Flink; Link != &Entry->Subscribers; Link = Link->Flink)
        {
            Subscriber = CONTAINING_RECORD(Link, EXP_WNF_SUBSCRIBER, Link);
            ExpWnfQueueDelivery(Subscriber, Entry->ChangeStamp);
        }
    }

    ExpWnfUnlock();

    if (Data)
        ExFreePoolWithTag(Data, TAG_WNF);
    return Status;
}

/**
 * @brief Reads the data last published under a state name.
 *
 * @remarks A name nothing has published to reads as empty, with change stamp 0.
 */
NTSTATUS
NTAPI
ZwQueryWnfStateData(
    _In_ const VOID *StateName,
    _In_opt_ LPCGUID TypeId,
    _In_opt_ const VOID *ExplicitScope,
    _Out_ PULONG ChangeStamp,
    _Out_writes_bytes_to_opt_(*BufferSize, *BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(TypeId);
    UNREFERENCED_PARAMETER(ExplicitScope);

    if ((StateName == NULL) || (ChangeStamp == NULL) || (BufferSize == NULL))
        return STATUS_INVALID_PARAMETER;

    ExpWnfLockExclusive();
    Status = ExpWnfCopyData(ExpWnfFindName(*(const ULONGLONG *)StateName, FALSE), ChangeStamp, Buffer, BufferSize);
    ExpWnfUnlock();
    return Status;
}

/**
 * @brief
 * Registers a callback for changes of a notification state.
 *
 * @param[out] Subscription
 * Receives the subscription, NULL on failure.
 *
 * @param[in] OriginalChangeStamp
 * The change stamp the caller has already seen. Data published after it is delivered right away.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a delivery option Windows does not define.
 */
NTSTATUS
NTAPI
ExSubscribeWnfStateChange(
    _Out_ PVOID *Subscription,
    _In_ const VOID *StateName,
    _In_ ULONG DeliveryOption,
    _In_ ULONG OriginalChangeStamp,
    _In_ PVOID Callback,
    _In_opt_ PVOID CallbackContext)
{
    PEXP_WNF_SUBSCRIBER Subscriber;
    PEXP_WNF_NAME Entry;

    PAGED_CODE();

    if (!Subscription)
        return STATUS_INVALID_PARAMETER;
    *Subscription = NULL;

    if (!StateName || !Callback || (DeliveryOption & ~EXP_WNF_VALID_DELIVERY))
        return STATUS_INVALID_PARAMETER;

    Subscriber = ExAllocatePoolZero(NonPagedPool, sizeof(*Subscriber), TAG_WNF);
    if (!Subscriber)
        return STATUS_INSUFFICIENT_RESOURCES;

    Subscriber->Callback = (PEXP_WNF_CALLBACK)Callback;
    Subscriber->CallbackContext = CallbackContext;
    Subscriber->DeliveryOption = DeliveryOption;
    Subscriber->SeenStamp = OriginalChangeStamp;
    Subscriber->References = 1;
    ExInitializeRundownProtection(&Subscriber->Rundown);

    ExpWnfLockExclusive();

    Entry = ExpWnfFindName(*(const ULONGLONG *)StateName, TRUE);
    if (!Entry)
    {
        ExpWnfUnlock();
        ExFreePoolWithTag(Subscriber, TAG_WNF);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Subscriber->Name = Entry;
    InsertTailList(&Entry->Subscribers, &Subscriber->Link);
    ExpWnfQueueDelivery(Subscriber, Entry->ChangeStamp);

    ExpWnfUnlock();

    *Subscription = Subscriber;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the current data of the state a subscription watches.
 *
 * @return
 * STATUS_OBJECT_NAME_NOT_FOUND without a subscription,
 * STATUS_BUFFER_TOO_SMALL with the needed size in @p BufferSize.
 */
NTSTATUS
NTAPI
ExQueryWnfStateData(
    _In_ PVOID Subscription,
    _Out_ PULONG ChangeStamp,
    _Out_writes_bytes_to_opt_(*BufferSize, *BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    PEXP_WNF_SUBSCRIBER Subscriber = Subscription;
    NTSTATUS Status;

    if (!Subscriber || !ChangeStamp || !BufferSize)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    ExpWnfLockExclusive();
    Status = ExpWnfCopyData(Subscriber->Name, ChangeStamp, Buffer, BufferSize);
    ExpWnfUnlock();
    return Status;
}

/**
 * @brief
 * Removes a subscription made with ExSubscribeWnfStateChange.
 *
 * @remarks Waits for a callback in progress, unless called from that callback.
 */
VOID
NTAPI
ExUnsubscribeWnfStateChange(
    _In_opt_ PVOID Subscription)
{
    PEXP_WNF_SUBSCRIBER Subscriber = Subscription;

    if (!Subscriber)
        return;

    ExpWnfLockExclusive();
    Subscriber->Removed = TRUE;
    RemoveEntryList(&Subscriber->Link);
    ExpWnfUnlock();

    if (Subscriber->CallingThread != PsGetCurrentThread())
        ExWaitForRundownProtectionRelease(&Subscriber->Rundown);

    ExpWnfDereferenceSubscriber(Subscriber);
}

/* EOF */
