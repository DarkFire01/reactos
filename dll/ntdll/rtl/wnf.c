/*
 * PROJECT:     ReactOS NT User-Mode DLL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Notification Facility user mode subscriptions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The kernel knows one subscription per state name and process. Every user
 * subscription hangs off the name it watches, and one thread per process
 * picks up what the kernel signals and runs the callbacks one at a time.
 */

/* INCLUDES *******************************************************************/

#include <ntdll.h>
#include <csr/csr.h>

#define NDEBUG
#include <debug.h>

/* DATA ***********************************************************************/

typedef struct _RTLP_WNF_NAME
{
    LIST_ENTRY Link;
    WNF_STATE_NAME StateName;
    LIST_ENTRY Subscriptions;
    ULONG References;
} RTLP_WNF_NAME, *PRTLP_WNF_NAME;

typedef struct _RTLP_WNF_SUBSCRIPTION
{
    LIST_ENTRY Link;
    PRTLP_WNF_NAME Name;
    PWNF_USER_CALLBACK Callback;
    PVOID CallbackContext;
    WNF_CHANGE_STAMP ChangeStamp;
    ULONG References;
    BOOLEAN Unsubscribed;
} RTLP_WNF_SUBSCRIPTION, *PRTLP_WNF_SUBSCRIPTION;

#define RTLP_WNF_DESCRIPTOR_SIZE \
    ((ULONG)ALIGN_UP_BY(sizeof(WNF_DELIVERY_DESCRIPTOR), sizeof(ULONGLONG)) + WNF_MAXIMUM_DATA_SIZE)

static RTL_SRWLOCK RtlpWnfLock = RTL_SRWLOCK_INIT;
static LIST_ENTRY RtlpWnfNames = { &RtlpWnfNames, &RtlpWnfNames };
static HANDLE RtlpWnfEvent;

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
RtlpWnfIsSameName(
    _In_ const WNF_STATE_NAME *First,
    _In_ const WNF_STATE_NAME *Second)
{
    return (First->Data[0] == Second->Data[0] && First->Data[1] == Second->Data[1]);
}

/* Caller holds the lock */
static
PRTLP_WNF_NAME
RtlpWnfFindName(
    _In_ const WNF_STATE_NAME *StateName)
{
    PRTLP_WNF_NAME Name;
    PLIST_ENTRY Link;

    for (Link = RtlpWnfNames.Flink; Link != &RtlpWnfNames; Link = Link->Flink)
    {
        Name = CONTAINING_RECORD(Link, RTLP_WNF_NAME, Link);
        if (RtlpWnfIsSameName(&Name->StateName, StateName))
            return Name;
    }

    return NULL;
}

/* Caller holds the lock. The kernel forgets the name with its last user subscription */
static
VOID
RtlpWnfDereferenceName(
    _In_ PRTLP_WNF_NAME Name)
{
    if (--Name->References)
        return;

    NtUnsubscribeWnfStateChange(&Name->StateName);
    RemoveEntryList(&Name->Link);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Name);
}

/* Caller holds the lock */
static
VOID
RtlpWnfDereferenceSubscription(
    _In_ PRTLP_WNF_SUBSCRIPTION Subscription)
{
    if (--Subscription->References)
        return;

    RemoveEntryList(&Subscription->Link);
    RtlpWnfDereferenceName(Subscription->Name);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Subscription);
}

/**
 * @brief
 * Runs the callbacks that have not seen the delivered change stamp yet. The
 * lock is dropped around each callback, so a callback may unsubscribe.
 */
static
VOID
RtlpWnfDeliver(
    _In_ PWNF_DELIVERY_DESCRIPTOR Descriptor)
{
    const VOID *Data = (PUCHAR)Descriptor + Descriptor->StateDataOffset;
    PRTLP_WNF_SUBSCRIPTION Subscription;
    PLIST_ENTRY Link, Next;
    PRTLP_WNF_NAME Name;

    if (!(Descriptor->EventMask & WNF_EVENT_DATA_CHANGED))
        return;

    RtlAcquireSRWLockExclusive(&RtlpWnfLock);

    Name = RtlpWnfFindName(&Descriptor->StateName);
    if (!Name)
    {
        RtlReleaseSRWLockExclusive(&RtlpWnfLock);
        return;
    }

    /* Keep the name around while its list is walked without the lock */
    Name->References++;

    for (Link = Name->Subscriptions.Flink; Link != &Name->Subscriptions; Link = Next)
    {
        Subscription = CONTAINING_RECORD(Link, RTLP_WNF_SUBSCRIPTION, Link);
        if (Subscription->Unsubscribed ||
            (Subscription->ChangeStamp && (LONG)(Descriptor->ChangeStamp - Subscription->ChangeStamp) <= 0))
        {
            Next = Link->Flink;
            continue;
        }

        Subscription->ChangeStamp = Descriptor->ChangeStamp;
        Subscription->References++;
        RtlReleaseSRWLockExclusive(&RtlpWnfLock);

        Subscription->Callback(Name->StateName,
                               Descriptor->ChangeStamp,
                               NULL,
                               Subscription->CallbackContext,
                               Data,
                               Descriptor->StateDataSize);

        RtlAcquireSRWLockExclusive(&RtlpWnfLock);
        Next = Link->Flink;
        RtlpWnfDereferenceSubscription(Subscription);
    }

    RtlpWnfDereferenceName(Name);
    RtlReleaseSRWLockExclusive(&RtlpWnfLock);
}

static
ULONG
NTAPI
RtlpWnfDeliveryThread(
    _In_ PVOID Parameter)
{
    PWNF_DELIVERY_DESCRIPTOR Descriptor;
    HANDLE Event = Parameter;
    NTSTATUS Status;

    /* Callbacks may talk to the window manager, so the thread must be known to CSR */
    CsrNewThread();

    Descriptor = RtlAllocateHeap(RtlGetProcessHeap(), 0, RTLP_WNF_DESCRIPTOR_SIZE);
    if (!Descriptor)
        RtlExitUserThread(STATUS_NO_MEMORY);

    for (;;)
    {
        Status = NtWaitForSingleObject(Event, FALSE, NULL);
        if (!NT_SUCCESS(Status))
            break;

        while (NT_SUCCESS(NtGetCompleteWnfStateSubscription(NULL,
                                                             NULL,
                                                             0,
                                                             0,
                                                             Descriptor,
                                                             RTLP_WNF_DESCRIPTOR_SIZE)))
        {
            RtlpWnfDeliver(Descriptor);
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Descriptor);
    RtlExitUserThread(Status);
    return 0;
}

/* Caller holds the lock */
static
NTSTATUS
RtlpWnfStartDelivery(VOID)
{
    HANDLE Event, Thread;
    NTSTATUS Status;

    if (RtlpWnfEvent)
        return STATUS_SUCCESS;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtSetWnfProcessNotificationEvent(Event);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Event);
        return Status;
    }

    Status = RtlCreateUserThread(NtCurrentProcess(),
                                 NULL,
                                 FALSE,
                                 0,
                                 0,
                                 0,
                                 RtlpWnfDeliveryThread,
                                 Event,
                                 &Thread,
                                 NULL);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Event);
        return Status;
    }

    NtClose(Thread);
    RtlpWnfEvent = Event;
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Calls back whenever data newer than the given change stamp is published
 * under a state name.
 *
 * @param[in] ChangeStamp
 * The change stamp the caller has seen. With 0 the current data, if any, is
 * delivered right after subscribing.
 *
 * @param[in] TypeId
 * Typed state names are not supported, it must be NULL.
 *
 * @remarks
 * Callbacks run one at a time on a thread of their own.
 */
NTSTATUS
NTAPI
RtlSubscribeWnfStateChangeNotification(
    _Outptr_ PVOID *Subscription,
    _In_ WNF_STATE_NAME StateName,
    _In_ WNF_CHANGE_STAMP ChangeStamp,
    _In_ PWNF_USER_CALLBACK Callback,
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PCWNF_TYPE_ID TypeId,
    _In_opt_ ULONG SerializationGroup)
{
    PRTLP_WNF_SUBSCRIPTION NewSubscription;
    PRTLP_WNF_NAME Name, NewName = NULL;
    NTSTATUS Status;

    /* Every callback runs on the one delivery thread, so all of them are serialized */
    UNREFERENCED_PARAMETER(SerializationGroup);

    if (!Subscription || !Callback || TypeId)
        return STATUS_INVALID_PARAMETER;

    *Subscription = NULL;

    NewSubscription = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*NewSubscription));
    if (!NewSubscription)
        return STATUS_NO_MEMORY;

    NewSubscription->Callback = Callback;
    NewSubscription->CallbackContext = CallbackContext;
    NewSubscription->ChangeStamp = ChangeStamp;
    NewSubscription->References = 1;

    RtlAcquireSRWLockExclusive(&RtlpWnfLock);

    Status = RtlpWnfStartDelivery();
    if (!NT_SUCCESS(Status))
        goto Quit;

    Name = RtlpWnfFindName(&StateName);
    if (!Name)
    {
        NewName = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*NewName));
        if (!NewName)
        {
            Status = STATUS_NO_MEMORY;
            goto Quit;
        }

        NewName->StateName = StateName;
        InitializeListHead(&NewName->Subscriptions);
        Name = NewName;
    }

    /* Subscribing again asks the kernel for data the new callback has not seen */
    Status = NtSubscribeWnfStateChange(&StateName,
                                       ChangeStamp,
                                       WNF_EVENT_DATA_CHANGED | WNF_EVENT_NAME_DESTROYED,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        if (NewName)
            RtlFreeHeap(RtlGetProcessHeap(), 0, NewName);
        goto Quit;
    }

    if (NewName)
        InsertTailList(&RtlpWnfNames, &NewName->Link);

    Name->References++;
    NewSubscription->Name = Name;
    InsertTailList(&Name->Subscriptions, &NewSubscription->Link);
    *Subscription = NewSubscription;
    NewSubscription = NULL;

Quit:
    RtlReleaseSRWLockExclusive(&RtlpWnfLock);

    if (NewSubscription)
        RtlFreeHeap(RtlGetProcessHeap(), 0, NewSubscription);

    return Status;
}

/**
 * @brief
 * Ends a subscription. A callback already running for it is not waited for,
 * no callback starts for it afterwards.
 *
 * @return
 * STATUS_UNSUCCESSFUL when the subscription was already ended.
 */
NTSTATUS
NTAPI
RtlUnsubscribeWnfStateChangeNotification(
    _In_ PVOID Subscription)
{
    PRTLP_WNF_SUBSCRIPTION UserSubscription = Subscription;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!UserSubscription)
        return STATUS_INVALID_PARAMETER;

    RtlAcquireSRWLockExclusive(&RtlpWnfLock);
    if (UserSubscription->Unsubscribed)
    {
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        UserSubscription->Unsubscribed = TRUE;
        RtlpWnfDereferenceSubscription(UserSubscription);
    }
    RtlReleaseSRWLockExclusive(&RtlpWnfLock);

    return Status;
}

/* EOF */
