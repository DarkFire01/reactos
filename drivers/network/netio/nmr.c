/*
 * PROJECT:     ReactOS NETIO driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Network Module Registrar
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <netioddk.h>

#include <reactos/debug.h>

/*
 * Clients and providers of the same NPI are bound when the client calls
 * NmrClientAttachProvider from its ClientAttachProvider callback. Nothing here
 * needs run time init, since importers can call in before NETIO's entry point.
 */

#define NMR_TAG 'rmNN'

typedef struct _NMR_MODULE
{
    LIST_ENTRY ListEntry;
    BOOLEAN IsProvider;
    BOOLEAN Deregistering;
    PVOID Context;

    /* The caller's characteristics, which stay valid until deregistration completes */
    union
    {
        PNPI_CLIENT_CHARACTERISTICS Client;
        PNPI_PROVIDER_CHARACTERISTICS Provider;
    } Characteristics;

    /* Every binding the module is part of, including ones still attaching */
    LIST_ENTRY Bindings;
    KEVENT Unbound;
} NMR_MODULE, *PNMR_MODULE;

typedef enum _NMR_BINDING_STEP
{
    NmrBindingAttaching,
    NmrBindingAttached,
    NmrBindingDetachClient,
    NmrBindingDetachProvider,
    NmrBindingCleanup
} NMR_BINDING_STEP;

typedef struct _NMR_BINDING
{
    LIST_ENTRY ClientLink;
    LIST_ENTRY ProviderLink;
    PNMR_MODULE Client;
    PNMR_MODULE Provider;
    NMR_BINDING_STEP Step;

    /* NmrClientAttachProvider succeeded */
    BOOLEAN ProviderAttached;

    PVOID ClientBindingContext;
    PVOID ProviderBindingContext;
    CONST VOID *ClientDispatch;
    CONST VOID *ProviderDispatch;

    WORK_QUEUE_ITEM WorkItem;
} NMR_BINDING, *PNMR_BINDING;

static KSPIN_LOCK NmrpLock;
static LIST_ENTRY NmrpClients = { &NmrpClients, &NmrpClients };
static LIST_ENTRY NmrpProviders = { &NmrpProviders, &NmrpProviders };

static VOID NmrpContinueDetach(_In_ PNMR_BINDING Binding);

static
PNPI_REGISTRATION_INSTANCE
NmrpInstance(
    _In_ PNMR_MODULE Module)
{
    return Module->IsProvider ? &Module->Characteristics.Provider->ProviderRegistrationInstance :
                                &Module->Characteristics.Client->ClientRegistrationInstance;
}

static
BOOLEAN
NmrpSameNpi(
    _In_ PNMR_MODULE Client,
    _In_ PNMR_MODULE Provider)
{
    return IsEqualGUID(NmrpInstance(Client)->NpiId, NmrpInstance(Provider)->NpiId);
}

/* Caller holds NmrpLock. Signals a deregistering module once it is part of nothing. */
static
VOID
NmrpUnlinkBinding(
    _In_ PNMR_BINDING Binding)
{
    RemoveEntryList(&Binding->ClientLink);
    RemoveEntryList(&Binding->ProviderLink);

    if (Binding->Client->Deregistering && IsListEmpty(&Binding->Client->Bindings))
        KeSetEvent(&Binding->Client->Unbound, IO_NO_INCREMENT, FALSE);

    if (Binding->Provider->Deregistering && IsListEmpty(&Binding->Provider->Bindings))
        KeSetEvent(&Binding->Provider->Unbound, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
NmrpDetachWorker(
    _In_ PVOID Context)
{
    NmrpContinueDetach(Context);
}

/*
 * Runs a binding's detach from whatever step it is at, until a callback pends
 * or the binding is gone. Runs at PASSIVE_LEVEL with no lock held.
 */
static
VOID
NmrpContinueDetach(
    _In_ PNMR_BINDING Binding)
{
    PNPI_CLIENT_CHARACTERISTICS Client = Binding->Client->Characteristics.Client;
    PNPI_PROVIDER_CHARACTERISTICS Provider = Binding->Provider->Characteristics.Provider;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Binding->Step == NmrBindingDetachClient)
    {
        Status = Client->ClientDetachProvider(Binding->ClientBindingContext);
        if (Status == STATUS_PENDING)
            return;

        Binding->Step = NmrBindingDetachProvider;
    }

    if (Binding->Step == NmrBindingDetachProvider)
    {
        Status = Provider->ProviderDetachClient(Binding->ProviderBindingContext);
        if (Status == STATUS_PENDING)
            return;

        Binding->Step = NmrBindingCleanup;
    }

    if (Client->ClientCleanupBindingContext != NULL)
        Client->ClientCleanupBindingContext(Binding->ClientBindingContext);

    if (Provider->ProviderCleanupBindingContext != NULL)
        Provider->ProviderCleanupBindingContext(Binding->ProviderBindingContext);

    KeAcquireSpinLock(&NmrpLock, &OldIrql);
    NmrpUnlinkBinding(Binding);
    KeReleaseSpinLock(&NmrpLock, OldIrql);

    ExFreePoolWithTag(Binding, NMR_TAG);
}

/* A detach callback that pended has finished. Callers can be at DISPATCH_LEVEL. */
static
VOID
NmrpStepCompleted(
    _In_ PNMR_BINDING Binding,
    _In_ NMR_BINDING_STEP Completed)
{
    if (Binding->Step != Completed)
    {
        DPRINT1("NMR: completion for binding %p at step %d, expected %d\n", Binding, Binding->Step, Completed);
        return;
    }

    Binding->Step = (Completed == NmrBindingDetachClient) ? NmrBindingDetachProvider : NmrBindingCleanup;

    ExInitializeWorkItem(&Binding->WorkItem, NmrpDetachWorker, Binding);
    ExQueueWorkItem(&Binding->WorkItem, DelayedWorkQueue);
}

/*
 * Offers one new binding to its client. The binding is already on both
 * modules' lists, so neither can finish deregistering underneath it.
 */
static
VOID
NmrpOfferBinding(
    _In_ PNMR_BINDING Binding)
{
    PNPI_CLIENT_CHARACTERISTICS Client = Binding->Client->Characteristics.Client;
    BOOLEAN Detach;
    NTSTATUS Status;
    KIRQL OldIrql;

    Status = Client->ClientAttachProvider((HANDLE)Binding,
                                          Binding->Client->Context,
                                          NmrpInstance(Binding->Provider));

    KeAcquireSpinLock(&NmrpLock, &OldIrql);

    if (!Binding->ProviderAttached)
    {
        /* The client passed on this provider */
        NmrpUnlinkBinding(Binding);
        KeReleaseSpinLock(&NmrpLock, OldIrql);
        ExFreePoolWithTag(Binding, NMR_TAG);
        return;
    }

    /* Either side may have started deregistering while the client made up its mind */
    Detach = !NT_SUCCESS(Status) || Binding->Client->Deregistering || Binding->Provider->Deregistering;
    if (Detach)
        Binding->Step = NT_SUCCESS(Status) ? NmrBindingDetachClient : NmrBindingDetachProvider;
    else
        Binding->Step = NmrBindingAttached;

    KeReleaseSpinLock(&NmrpLock, OldIrql);

    if (Detach)
        NmrpContinueDetach(Binding);
}

/* Binds a new module to every live module of the other kind on the same NPI */
static
NTSTATUS
NmrpRegister(
    _In_ PNMR_MODULE Module)
{
    LIST_ENTRY Offers;
    PLIST_ENTRY Others;
    PLIST_ENTRY Entry;
    PNMR_MODULE Other;
    PNMR_BINDING Binding;
    KIRQL OldIrql;

    InitializeListHead(&Offers);

    KeAcquireSpinLock(&NmrpLock, &OldIrql);

    InsertTailList(Module->IsProvider ? &NmrpProviders : &NmrpClients, &Module->ListEntry);

    Others = Module->IsProvider ? &NmrpClients : &NmrpProviders;
    for (Entry = Others->Flink; Entry != Others; Entry = Entry->Flink)
    {
        Other = CONTAINING_RECORD(Entry, NMR_MODULE, ListEntry);
        if (Other->Deregistering ||
            !NmrpSameNpi(Module->IsProvider ? Other : Module, Module->IsProvider ? Module : Other))
        {
            continue;
        }

        Binding = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Binding), NMR_TAG);
        if (Binding == NULL)
        {
            DPRINT1("NMR: no memory to bind module %p to %p\n", Module, Other);
            continue;
        }

        RtlZeroMemory(Binding, sizeof(*Binding));
        Binding->Client = Module->IsProvider ? Other : Module;
        Binding->Provider = Module->IsProvider ? Module : Other;
        Binding->Step = NmrBindingAttaching;
        InsertTailList(&Binding->Client->Bindings, &Binding->ClientLink);
        InsertTailList(&Binding->Provider->Bindings, &Binding->ProviderLink);

        /* The work item's list entry carries the offer until the lock is dropped */
        InsertTailList(&Offers, &Binding->WorkItem.List);
    }

    KeReleaseSpinLock(&NmrpLock, OldIrql);

    while (!IsListEmpty(&Offers))
    {
        Binding = CONTAINING_RECORD(RemoveHeadList(&Offers), NMR_BINDING, WorkItem.List);
        NmrpOfferBinding(Binding);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NmrpDeregister(
    _In_ PNMR_MODULE Module)
{
    LIST_ENTRY Detaches;
    PLIST_ENTRY Entry;
    PNMR_BINDING Binding;
    BOOLEAN Done;
    KIRQL OldIrql;

    InitializeListHead(&Detaches);

    KeAcquireSpinLock(&NmrpLock, &OldIrql);

    if (Module->Deregistering)
    {
        KeReleaseSpinLock(&NmrpLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    Module->Deregistering = TRUE;
    RemoveEntryList(&Module->ListEntry);

    /* Bindings still attaching get detached by their attach path once it sees the flag */
    for (Entry = Module->Bindings.Flink; Entry != &Module->Bindings; Entry = Entry->Flink)
    {
        Binding = Module->IsProvider ? CONTAINING_RECORD(Entry, NMR_BINDING, ProviderLink) :
                                       CONTAINING_RECORD(Entry, NMR_BINDING, ClientLink);
        if (Binding->Step != NmrBindingAttached)
            continue;

        Binding->Step = NmrBindingDetachClient;
        InsertTailList(&Detaches, &Binding->WorkItem.List);
    }

    KeReleaseSpinLock(&NmrpLock, OldIrql);

    while (!IsListEmpty(&Detaches))
    {
        Binding = CONTAINING_RECORD(RemoveHeadList(&Detaches), NMR_BINDING, WorkItem.List);
        NmrpContinueDetach(Binding);
    }

    KeAcquireSpinLock(&NmrpLock, &OldIrql);
    Done = IsListEmpty(&Module->Bindings);
    KeReleaseSpinLock(&NmrpLock, OldIrql);

    if (!Done)
        return STATUS_PENDING;

    ExFreePoolWithTag(Module, NMR_TAG);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NmrpWaitForDeregister(
    _In_ PNMR_MODULE Module)
{
    KeWaitForSingleObject(&Module->Unbound, Executive, KernelMode, FALSE, NULL);
    ExFreePoolWithTag(Module, NMR_TAG);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NmrpCreateModule(
    _In_ BOOLEAN IsProvider,
    _In_ PVOID Characteristics,
    _In_opt_ PVOID Context,
    _Out_ PHANDLE Handle)
{
    PNMR_MODULE Module;

    Module = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Module), NMR_TAG);
    if (Module == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Module, sizeof(*Module));
    Module->IsProvider = IsProvider;
    Module->Context = Context;
    if (IsProvider)
        Module->Characteristics.Provider = Characteristics;
    else
        Module->Characteristics.Client = Characteristics;
    InitializeListHead(&Module->Bindings);
    KeInitializeEvent(&Module->Unbound, NotificationEvent, FALSE);

    /* The handle has to be in place before the first attach callback can use it */
    *Handle = (HANDLE)Module;
    return NmrpRegister(Module);
}

/**
 * @brief
 * Registers a client of an NPI and offers it every provider of that NPI.
 *
 * @param[in] ClientCharacteristics
 * The client's callbacks and registration instance. They have to stay valid
 * until the client is deregistered.
 *
 * @param[in] ClientContext
 * Handed back to ClientAttachProvider.
 *
 * @param[out] NmrClientHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for malformed characteristics or
 * STATUS_INSUFFICIENT_RESOURCES.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrRegisterClient(
    PNPI_CLIENT_CHARACTERISTICS ClientCharacteristics,
    PVOID ClientContext,
    PHANDLE NmrClientHandle)
{
    PAGED_CODE();

    *NmrClientHandle = NULL;

    if (ClientCharacteristics->Length < sizeof(*ClientCharacteristics) ||
        ClientCharacteristics->ClientAttachProvider == NULL ||
        ClientCharacteristics->ClientDetachProvider == NULL ||
        ClientCharacteristics->ClientRegistrationInstance.NpiId == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return NmrpCreateModule(FALSE, (PVOID)ClientCharacteristics, ClientContext, NmrClientHandle);
}

/**
 * @brief
 * Registers a provider of an NPI and offers it to every client of that NPI.
 *
 * @param[in] ProviderCharacteristics
 * The provider's callbacks and registration instance. They have to stay valid
 * until the provider is deregistered.
 *
 * @param[in] ProviderContext
 * Handed back to ProviderAttachClient.
 *
 * @param[out] NmrProviderHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for malformed characteristics or
 * STATUS_INSUFFICIENT_RESOURCES.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrRegisterProvider(
    PNPI_PROVIDER_CHARACTERISTICS ProviderCharacteristics,
    PVOID ProviderContext,
    PHANDLE NmrProviderHandle)
{
    PAGED_CODE();

    *NmrProviderHandle = NULL;

    if (ProviderCharacteristics->Length < sizeof(*ProviderCharacteristics) ||
        ProviderCharacteristics->ProviderAttachClient == NULL ||
        ProviderCharacteristics->ProviderDetachClient == NULL ||
        ProviderCharacteristics->ProviderRegistrationInstance.NpiId == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return NmrpCreateModule(TRUE, (PVOID)ProviderCharacteristics, ProviderContext, NmrProviderHandle);
}

/**
 * @brief
 * Attaches a client to the provider it is being offered, from within its
 * ClientAttachProvider callback.
 *
 * @param[in] NmrBindingHandle
 * The binding ClientAttachProvider was given.
 *
 * @param[in] ClientBindingContext
 * The client's context for the binding.
 *
 * @param[in] ClientDispatch
 * The client's dispatch table for the provider.
 *
 * @param[out] ProviderBindingContext
 * The provider's context for the binding.
 *
 * @param[out] ProviderDispatch
 * The provider's dispatch table for the client.
 *
 * @return
 * What the provider's ProviderAttachClient returned.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrClientAttachProvider(
    HANDLE NmrBindingHandle,
    PVOID ClientBindingContext,
    CONST VOID *ClientDispatch,
    PVOID *ProviderBindingContext,
    CONST VOID **ProviderDispatch)
{
    PNMR_BINDING Binding = (PNMR_BINDING)NmrBindingHandle;
    PNMR_MODULE Provider = Binding->Provider;
    NTSTATUS Status;

    PAGED_CODE();

    *ProviderBindingContext = NULL;
    *ProviderDispatch = NULL;

    if (Binding->Step != NmrBindingAttaching || Binding->ProviderAttached)
        return STATUS_INVALID_DEVICE_STATE;

    Binding->ClientBindingContext = ClientBindingContext;
    Binding->ClientDispatch = ClientDispatch;

    Status = Provider->Characteristics.Provider->ProviderAttachClient(NmrBindingHandle,
                                                                      Provider->Context,
                                                                      NmrpInstance(Binding->Client),
                                                                      ClientBindingContext,
                                                                      ClientDispatch,
                                                                      &Binding->ProviderBindingContext,
                                                                      &Binding->ProviderDispatch);
    if (!NT_SUCCESS(Status))
        return Status;

    Binding->ProviderAttached = TRUE;
    *ProviderBindingContext = Binding->ProviderBindingContext;
    *ProviderDispatch = Binding->ProviderDispatch;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finishes a ClientDetachProvider that returned STATUS_PENDING.
 *
 * @param[in] NmrBindingHandle
 * The binding.
 */
_Use_decl_annotations_
VOID
NTAPI
NmrClientDetachProviderComplete(
    HANDLE NmrBindingHandle)
{
    NmrpStepCompleted((PNMR_BINDING)NmrBindingHandle, NmrBindingDetachClient);
}

/**
 * @brief
 * Finishes a ProviderDetachClient that returned STATUS_PENDING.
 *
 * @param[in] NmrBindingHandle
 * The binding.
 */
_Use_decl_annotations_
VOID
NTAPI
NmrProviderDetachClientComplete(
    HANDLE NmrBindingHandle)
{
    NmrpStepCompleted((PNMR_BINDING)NmrBindingHandle, NmrBindingDetachProvider);
}

/**
 * @brief
 * Starts undoing a client's registration, detaching it from every provider.
 *
 * @param[in] NmrClientHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS once everything is detached and the handle is gone, or
 * STATUS_PENDING, after which NmrWaitForClientDeregisterComplete finishes it.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrDeregisterClient(
    HANDLE NmrClientHandle)
{
    PAGED_CODE();

    return NmrpDeregister((PNMR_MODULE)NmrClientHandle);
}

/**
 * @brief
 * Starts undoing a provider's registration, detaching it from every client.
 *
 * @param[in] NmrProviderHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS once everything is detached and the handle is gone, or
 * STATUS_PENDING, after which NmrWaitForProviderDeregisterComplete finishes it.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrDeregisterProvider(
    HANDLE NmrProviderHandle)
{
    PAGED_CODE();

    return NmrpDeregister((PNMR_MODULE)NmrProviderHandle);
}

/**
 * @brief
 * Waits until a client whose deregistration pended is detached from
 * everything, and frees the registration.
 *
 * @param[in] NmrClientHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrWaitForClientDeregisterComplete(
    HANDLE NmrClientHandle)
{
    PAGED_CODE();

    return NmrpWaitForDeregister((PNMR_MODULE)NmrClientHandle);
}

/**
 * @brief
 * Waits until a provider whose deregistration pended is detached from
 * everything, and frees the registration.
 *
 * @param[in] NmrProviderHandle
 * The registration.
 *
 * @return
 * STATUS_SUCCESS.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NmrWaitForProviderDeregisterComplete(
    HANDLE NmrProviderHandle)
{
    PAGED_CODE();

    return NmrpWaitForDeregister((PNMR_MODULE)NmrProviderHandle);
}
