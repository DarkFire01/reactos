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

/* USER MODE SUBSCRIPTIONS ****************************************************/

/*
 * A process subscribes once per state name. New data marks the subscription
 * pending and sets the event the process registered, the process then picks
 * the data up one subscription at a time.
 */

typedef struct _EXP_WNF_PROCESS
{
    LIST_ENTRY Link;
    PEPROCESS Process;
    PKEVENT Event;
    LIST_ENTRY Subscriptions;
    ULONGLONG LastId;
} EXP_WNF_PROCESS, *PEXP_WNF_PROCESS;

typedef struct _EXP_WNF_USER_SUBSCRIPTION
{
    LIST_ENTRY Link;
    PEXP_WNF_PROCESS Owner;
    ULONGLONG Name;
    ULONGLONG Id;
    ULONG EventMask;
    BOOLEAN Pending;
    PVOID Subscription;
} EXP_WNF_USER_SUBSCRIPTION, *PEXP_WNF_USER_SUBSCRIPTION;

static LIST_ENTRY ExpWnfProcesses = { &ExpWnfProcesses, &ExpWnfProcesses };
static EX_PUSH_LOCK ExpWnfProcessLock;
static LONG ExpWnfExitHookSet;

static
VOID
ExpWnfLockProcesses(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&ExpWnfProcessLock);
}

static
VOID
ExpWnfUnlockProcesses(VOID)
{
    ExReleasePushLockExclusive(&ExpWnfProcessLock);
    KeLeaveCriticalRegion();
}

/* Caller holds the process lock */
static
PEXP_WNF_PROCESS
ExpWnfFindProcess(
    _In_ PEPROCESS Process)
{
    PEXP_WNF_PROCESS Context;
    PLIST_ENTRY Link;

    for (Link = ExpWnfProcesses.Flink; Link != &ExpWnfProcesses; Link = Link->Flink)
    {
        Context = CONTAINING_RECORD(Link, EXP_WNF_PROCESS, Link);
        if (Context->Process == Process)
            return Context;
    }

    return NULL;
}

/* Caller holds the process lock */
static
PEXP_WNF_USER_SUBSCRIPTION
ExpWnfFindUserSubscription(
    _In_opt_ PEXP_WNF_PROCESS Context,
    _In_ ULONGLONG Name)
{
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription;
    PLIST_ENTRY Link;

    if (!Context)
        return NULL;

    for (Link = Context->Subscriptions.Flink; Link != &Context->Subscriptions; Link = Link->Flink)
    {
        UserSubscription = CONTAINING_RECORD(Link, EXP_WNF_USER_SUBSCRIPTION, Link);
        if (UserSubscription->Name == Name)
            return UserSubscription;
    }

    return NULL;
}

/* Caller holds the process lock, the new context is preallocated by the caller */
static
PEXP_WNF_PROCESS
ExpWnfAttachProcess(
    _Inout_ PEXP_WNF_PROCESS *Spare)
{
    PEXP_WNF_PROCESS Context;

    Context = ExpWnfFindProcess(PsGetCurrentProcess());
    if (Context || !*Spare)
        return Context;

    Context = *Spare;
    *Spare = NULL;

    Context->Process = PsGetCurrentProcess();
    ObReferenceObject(Context->Process);
    InitializeListHead(&Context->Subscriptions);
    InsertTailList(&ExpWnfProcesses, &Context->Link);
    return Context;
}

static
VOID
ExpWnfFreeUserSubscription(
    _In_ PEXP_WNF_USER_SUBSCRIPTION UserSubscription)
{
    ExUnsubscribeWnfStateChange(UserSubscription->Subscription);
    ExFreePoolWithTag(UserSubscription, TAG_WNF);
}

static
VOID
NTAPI
ExpWnfProcessNotify(
    _In_ HANDLE ParentId,
    _In_ HANDLE ProcessId,
    _In_ BOOLEAN Create)
{
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription;
    PEXP_WNF_PROCESS Context = NULL;
    LIST_ENTRY Detached;
    PLIST_ENTRY Link;

    UNREFERENCED_PARAMETER(ParentId);

    if (Create)
        return;

    InitializeListHead(&Detached);

    ExpWnfLockProcesses();
    for (Link = ExpWnfProcesses.Flink; Link != &ExpWnfProcesses; Link = Link->Flink)
    {
        if (PsGetProcessId(CONTAINING_RECORD(Link, EXP_WNF_PROCESS, Link)->Process) == ProcessId)
        {
            Context = CONTAINING_RECORD(Link, EXP_WNF_PROCESS, Link);
            RemoveEntryList(&Context->Link);
            break;
        }
    }

    if (Context)
    {
        while (!IsListEmpty(&Context->Subscriptions))
        {
            Link = RemoveHeadList(&Context->Subscriptions);
            CONTAINING_RECORD(Link, EXP_WNF_USER_SUBSCRIPTION, Link)->Owner = NULL;
            InsertTailList(&Detached, Link);
        }
    }
    ExpWnfUnlockProcesses();

    if (!Context)
        return;

    /* Callbacks take the process lock, so the subscriptions go away without it */
    while (!IsListEmpty(&Detached))
    {
        UserSubscription = CONTAINING_RECORD(RemoveHeadList(&Detached), EXP_WNF_USER_SUBSCRIPTION, Link);
        ExpWnfFreeUserSubscription(UserSubscription);
    }

    if (Context->Event)
        ObDereferenceObject(Context->Event);
    ObDereferenceObject(Context->Process);
    ExFreePoolWithTag(Context, TAG_WNF);
}

/* Process contexts are torn down when their process exits */
static
VOID
ExpWnfHookProcessExit(VOID)
{
    if (InterlockedCompareExchange(&ExpWnfExitHookSet, 1, 0) != 0)
        return;

    if (!NT_SUCCESS(PsSetCreateProcessNotifyRoutine(ExpWnfProcessNotify, FALSE)))
        InterlockedExchange(&ExpWnfExitHookSet, 0);
}

static
NTSTATUS
NTAPI
ExpWnfUserCallback(
    _In_ PVOID Subscription,
    _In_ const VOID *StateName,
    _In_ ULONG SubscribedEventSet,
    _In_ ULONG ChangeStamp,
    _In_opt_ LPCGUID TypeId,
    _In_opt_ PVOID CallbackContext)
{
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription = CallbackContext;

    UNREFERENCED_PARAMETER(Subscription);
    UNREFERENCED_PARAMETER(StateName);
    UNREFERENCED_PARAMETER(ChangeStamp);
    UNREFERENCED_PARAMETER(TypeId);

    ExpWnfLockProcesses();
    if (UserSubscription->Owner &&
        (SubscribedEventSet & UserSubscription->EventMask & WNF_EVENT_DATA_CHANGED))
    {
        UserSubscription->Pending = TRUE;
        if (UserSubscription->Owner->Event)
            KeSetEvent(UserSubscription->Owner->Event, IO_NO_INCREMENT, FALSE);
    }
    ExpWnfUnlockProcesses();

    return STATUS_SUCCESS;
}

static
NTSTATUS
ExpWnfCaptureName(
    _In_ PCWNF_STATE_NAME StateName,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PULONGLONG Name)
{
    if (!StateName)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead((PVOID)StateName, sizeof(*StateName), sizeof(ULONG));
        RtlCopyMemory(Name, StateName, sizeof(*Name));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Subscribes the calling process to changes of a state name.
 *
 * @param[in] ChangeStamp
 * The change stamp the process has seen. Newer data is delivered right away.
 *
 * @remarks
 * Subscribing to a name again updates the event mask and keeps the identifier.
 */
NTSTATUS
NTAPI
NtSubscribeWnfStateChange(
    _In_ PCWNF_STATE_NAME StateName,
    _In_opt_ WNF_CHANGE_STAMP ChangeStamp,
    _In_ ULONG EventMask,
    _Out_opt_ PULONG64 SubscriptionId)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription, Existing;
    PEXP_WNF_PROCESS Context, Spare;
    ULONG CurrentStamp, Size = 0;
    ULONGLONG Name, Id;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ExpWnfCaptureName(StateName, PreviousMode, &Name);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!EventMask)
        return STATUS_INVALID_PARAMETER;

    if (SubscriptionId && PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWrite(SubscriptionId, sizeof(*SubscriptionId), sizeof(ULONG));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Spare = ExAllocatePoolZero(PagedPool, sizeof(*Spare), TAG_WNF);
    UserSubscription = ExAllocatePoolZero(PagedPool, sizeof(*UserSubscription), TAG_WNF);
    if (!Spare || !UserSubscription)
    {
        if (Spare)
            ExFreePoolWithTag(Spare, TAG_WNF);
        if (UserSubscription)
            ExFreePoolWithTag(UserSubscription, TAG_WNF);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExpWnfHookProcessExit();

    ExpWnfLockProcesses();
    Context = ExpWnfAttachProcess(&Spare);
    Existing = ExpWnfFindUserSubscription(Context, Name);
    if (Existing)
    {
        Existing->EventMask = EventMask;
        Id = Existing->Id;

        /* A caller that has seen less than what is published gets it again */
        if (ExQueryWnfStateData(Existing->Subscription, &CurrentStamp, NULL, &Size) != STATUS_OBJECT_NAME_NOT_FOUND &&
            CurrentStamp > ChangeStamp && (EventMask & WNF_EVENT_DATA_CHANGED))
        {
            Existing->Pending = TRUE;
            if (Context->Event)
                KeSetEvent(Context->Event, IO_NO_INCREMENT, FALSE);
        }
    }
    else
    {
        UserSubscription->Owner = Context;
        UserSubscription->Name = Name;
        UserSubscription->EventMask = EventMask;
        UserSubscription->Id = Id = ++Context->LastId;
    }
    ExpWnfUnlockProcesses();

    if (Spare)
        ExFreePoolWithTag(Spare, TAG_WNF);

    if (Existing)
    {
        ExFreePoolWithTag(UserSubscription, TAG_WNF);
    }
    else
    {
        /* The first delivery may run before the subscription is listed, it only marks it pending */
        Status = ExSubscribeWnfStateChange(&UserSubscription->Subscription,
                                           &Name,
                                           0,
                                           ChangeStamp,
                                           (PVOID)ExpWnfUserCallback,
                                           UserSubscription);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(UserSubscription, TAG_WNF);
            return Status;
        }

        /* A delivery that came in early could not be picked up yet, set the event for it again */
        ExpWnfLockProcesses();
        InsertTailList(&Context->Subscriptions, &UserSubscription->Link);
        if (UserSubscription->Pending && Context->Event)
            KeSetEvent(Context->Event, IO_NO_INCREMENT, FALSE);
        ExpWnfUnlockProcesses();
    }

    if (SubscriptionId)
    {
        _SEH2_TRY
        {
            *SubscriptionId = Id;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    return Status;
}

/**
 * @brief Removes the calling process's subscription to a state name.
 *
 * @return STATUS_NOT_FOUND when the process is not subscribed to the name.
 */
NTSTATUS
NTAPI
NtUnsubscribeWnfStateChange(
    _In_ PCWNF_STATE_NAME StateName)
{
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription;
    ULONGLONG Name;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ExpWnfCaptureName(StateName, ExGetPreviousMode(), &Name);
    if (!NT_SUCCESS(Status))
        return Status;

    ExpWnfLockProcesses();
    UserSubscription = ExpWnfFindUserSubscription(ExpWnfFindProcess(PsGetCurrentProcess()), Name);
    if (UserSubscription)
    {
        RemoveEntryList(&UserSubscription->Link);
        UserSubscription->Owner = NULL;
    }
    ExpWnfUnlockProcesses();

    if (!UserSubscription)
        return STATUS_NOT_FOUND;

    ExpWnfFreeUserSubscription(UserSubscription);
    return STATUS_SUCCESS;
}

/**
 * @brief Sets the event the calling process waits on for its notifications.
 */
NTSTATUS
NTAPI
NtSetWnfProcessNotificationEvent(
    _In_ HANDLE NotificationEvent)
{
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription;
    PEXP_WNF_PROCESS Context, Spare;
    PKEVENT Event, Previous = NULL;
    PLIST_ENTRY Link;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByHandle(NotificationEvent,
                                       EVENT_MODIFY_STATE,
                                       ExEventObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID *)&Event,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Spare = ExAllocatePoolZero(PagedPool, sizeof(*Spare), TAG_WNF);
    if (!Spare)
    {
        ObDereferenceObject(Event);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExpWnfHookProcessExit();

    ExpWnfLockProcesses();
    Context = ExpWnfAttachProcess(&Spare);
    Previous = Context->Event;
    Context->Event = Event;

    /* Anything that came in before the event was set is not lost */
    for (Link = Context->Subscriptions.Flink; Link != &Context->Subscriptions; Link = Link->Flink)
    {
        UserSubscription = CONTAINING_RECORD(Link, EXP_WNF_USER_SUBSCRIPTION, Link);
        if (UserSubscription->Pending)
        {
            KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
            break;
        }
    }
    ExpWnfUnlockProcesses();

    if (Spare)
        ExFreePoolWithTag(Spare, TAG_WNF);
    if (Previous)
        ObDereferenceObject(Previous);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Hands out the next pending notification of the calling process, with the
 * current data of its state name after the descriptor.
 *
 * @return
 * STATUS_NO_MORE_ENTRIES when nothing is pending, STATUS_BUFFER_TOO_SMALL
 * when the data does not fit. The notification stays pending in that case.
 */
NTSTATUS
NTAPI
NtGetCompleteWnfStateSubscription(
    _In_opt_ PWNF_STATE_NAME OldDescriptorStateName,
    _In_opt_ ULONG64 *OldSubscriptionId,
    _In_opt_ ULONG OldDescriptorEventMask,
    _In_opt_ ULONG OldDescriptorStatus,
    _Out_writes_bytes_(DescriptorSize) PWNF_DELIVERY_DESCRIPTOR NewDeliveryDescriptor,
    _In_ ULONG DescriptorSize)
{
    const ULONG Header = (ULONG)ALIGN_UP_BY(sizeof(*NewDeliveryDescriptor), sizeof(ULONGLONG));
    PEXP_WNF_USER_SUBSCRIPTION UserSubscription = NULL;
    WNF_DELIVERY_DESCRIPTOR Descriptor;
    ULONG Size = WNF_MAXIMUM_DATA_SIZE;
    PEXP_WNF_PROCESS Context;
    PLIST_ENTRY Link;
    PVOID Data;
    NTSTATUS Status;

    PAGED_CODE();

    /* Every delivery is final, there is nothing to retry */
    UNREFERENCED_PARAMETER(OldDescriptorStateName);
    UNREFERENCED_PARAMETER(OldSubscriptionId);
    UNREFERENCED_PARAMETER(OldDescriptorEventMask);
    UNREFERENCED_PARAMETER(OldDescriptorStatus);

    if (!NewDeliveryDescriptor || DescriptorSize < Header)
        return STATUS_BUFFER_TOO_SMALL;

    if (ExGetPreviousMode() != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWrite(NewDeliveryDescriptor, DescriptorSize, sizeof(ULONG));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Data = ExAllocatePoolWithTag(PagedPool, WNF_MAXIMUM_DATA_SIZE, TAG_WNF);
    if (!Data)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Status = STATUS_NO_MORE_ENTRIES;

    ExpWnfLockProcesses();
    Context = ExpWnfFindProcess(PsGetCurrentProcess());
    for (Link = Context ? Context->Subscriptions.Flink : NULL;
         Link && Link != &Context->Subscriptions;
         Link = Link->Flink)
    {
        UserSubscription = CONTAINING_RECORD(Link, EXP_WNF_USER_SUBSCRIPTION, Link);
        if (UserSubscription->Pending)
            break;
        UserSubscription = NULL;
    }

    if (UserSubscription)
    {
        /* Data published from here on marks it pending again */
        UserSubscription->Pending = FALSE;
        Status = ExQueryWnfStateData(UserSubscription->Subscription, &Descriptor.ChangeStamp, Data, &Size);
        if (NT_SUCCESS(Status) && DescriptorSize - Header < Size)
        {
            UserSubscription->Pending = TRUE;
            Status = STATUS_BUFFER_TOO_SMALL;
        }

        Descriptor.SubscriptionId = UserSubscription->Id;
        RtlCopyMemory(&Descriptor.StateName, &UserSubscription->Name, sizeof(Descriptor.StateName));
        Descriptor.StateDataSize = Size;
        Descriptor.EventMask = WNF_EVENT_DATA_CHANGED;
        Descriptor.StateDataOffset = Header;
    }
    ExpWnfUnlockProcesses();

    if (UserSubscription && (NT_SUCCESS(Status) || Status == STATUS_BUFFER_TOO_SMALL))
    {
        _SEH2_TRY
        {
            RtlCopyMemory(NewDeliveryDescriptor, &Descriptor, sizeof(Descriptor));
            if (NT_SUCCESS(Status))
                RtlCopyMemory((PUCHAR)NewDeliveryDescriptor + Header, Data, Size);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    ExFreePoolWithTag(Data, TAG_WNF);
    return Status;
}

/* EOF */
