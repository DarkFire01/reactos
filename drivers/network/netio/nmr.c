/*
 * PROJECT:     NetIO driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network Module Registrar, which pairs NPI clients with providers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <netioddk.h>

#define NDEBUG
#include <reactos/debug.h>

#define TAG_NMR 'rmNN'

typedef struct _NMR_MODULE
{
    LIST_ENTRY Link;
    BOOLEAN IsProvider;
    BOOLEAN Deregistering;
    PNPI_CLIENT_CHARACTERISTICS Client;
    PNPI_PROVIDER_CHARACTERISTICS Provider;
    PVOID Context;
    LIST_ENTRY Bindings;
    LONG BindingCount;
    KEVENT Idle;
} NMR_MODULE, *PNMR_MODULE;

typedef struct _NMR_BINDING
{
    LIST_ENTRY ClientLink;
    LIST_ENTRY ProviderLink;
    PNMR_MODULE ClientModule;
    PNMR_MODULE ProviderModule;
    PVOID ClientBindingContext;
    PVOID ProviderBindingContext;
    BOOLEAN Attached;
    BOOLEAN Detaching;
    LONG PendingSides;
} NMR_BINDING, *PNMR_BINDING;

/* NETIO is usually loaded as an import, so DriverEntry cannot be relied on to set these up */
static LONG NmrInitState;
static ERESOURCE NmrLock;
static LIST_ENTRY NmrClients;
static LIST_ENTRY NmrProviders;

static
VOID
NmrpInitialize(VOID)
{
    if (NmrInitState == 2)
        return;

    if (InterlockedCompareExchange(&NmrInitState, 1, 0) == 0)
    {
        ExInitializeResourceLite(&NmrLock);
        InitializeListHead(&NmrClients);
        InitializeListHead(&NmrProviders);
        InterlockedExchange(&NmrInitState, 2);
        return;
    }

    while (NmrInitState != 2)
        YieldProcessor();
}

static
VOID
NmrpLock(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&NmrLock, TRUE);
}

static
VOID
NmrpUnlock(VOID)
{
    ExReleaseResourceLite(&NmrLock);
    KeLeaveCriticalRegion();
}

static
BOOLEAN
NmrpSameNpi(
    _In_ PNMR_MODULE Client,
    _In_ PNMR_MODULE Provider)
{
    PNPIID ClientNpi = Client->Client->ClientRegistrationInstance.NpiId;
    PNPIID ProviderNpi = Provider->Provider->ProviderRegistrationInstance.NpiId;

    if (ClientNpi == NULL || ProviderNpi == NULL)
        return FALSE;

    return RtlCompareMemory(ClientNpi, ProviderNpi, sizeof(*ClientNpi)) == sizeof(*ClientNpi);
}

/* Drops a module's binding count, waking a deregistration that waits for it */
static
VOID
NmrpReleaseModule(
    _In_ PNMR_MODULE Module)
{
    if (InterlockedDecrement(&Module->BindingCount) == 0 && Module->Deregistering)
        KeSetEvent(&Module->Idle, IO_NO_INCREMENT, FALSE);
}

/* Both sides have let go, so the binding contexts can be cleaned up and the binding freed */
static
VOID
NmrpFinishDetach(
    _In_ PNMR_BINDING Binding)
{
    PNMR_MODULE Client = Binding->ClientModule;
    PNMR_MODULE Provider = Binding->ProviderModule;

    NmrpLock();
    RemoveEntryList(&Binding->ClientLink);
    RemoveEntryList(&Binding->ProviderLink);
    NmrpUnlock();

    if (Client->Client->ClientCleanupBindingContext != NULL)
        Client->Client->ClientCleanupBindingContext(Binding->ClientBindingContext);
    if (Provider->Provider->ProviderCleanupBindingContext != NULL)
        Provider->Provider->ProviderCleanupBindingContext(Binding->ProviderBindingContext);

    ExFreePoolWithTag(Binding, TAG_NMR);

    NmrpReleaseModule(Client);
    NmrpReleaseModule(Provider);
}

static
VOID
NmrpDetachSideDone(
    _In_ PNMR_BINDING Binding)
{
    if (InterlockedDecrement(&Binding->PendingSides) == 0)
        NmrpFinishDetach(Binding);
}

/*
 * Asks both sides of every binding of a module to let go. A side that answers
 * STATUS_PENDING finishes later through its DetachComplete call. The extra
 * count held here keeps each binding alive until both callbacks have returned.
 */
static
VOID
NmrpDetachAll(
    _In_ PNMR_MODULE Module)
{
    PNMR_BINDING Binding;
    PLIST_ENTRY Entry;
    NTSTATUS Status;

    for (;;)
    {
        Binding = NULL;

        NmrpLock();
        for (Entry = Module->Bindings.Flink; Entry != &Module->Bindings; Entry = Entry->Flink)
        {
            if (Module->IsProvider)
                Binding = CONTAINING_RECORD(Entry, NMR_BINDING, ProviderLink);
            else
                Binding = CONTAINING_RECORD(Entry, NMR_BINDING, ClientLink);

            if (!Binding->Detaching)
                break;
            Binding = NULL;
        }

        if (Binding != NULL)
        {
            Binding->Detaching = TRUE;
            Binding->PendingSides = 3;
        }
        NmrpUnlock();

        if (Binding == NULL)
            break;

        Status = Binding->ClientModule->Client->ClientDetachProvider(Binding->ClientBindingContext);
        if (Status != STATUS_PENDING)
            NmrpDetachSideDone(Binding);

        Status = Binding->ProviderModule->Provider->ProviderDetachClient(Binding->ProviderBindingContext);
        if (Status != STATUS_PENDING)
            NmrpDetachSideDone(Binding);

        NmrpDetachSideDone(Binding);
    }
}

/* Offers one provider to one client; the client binds by calling NmrClientAttachProvider */
static
VOID
NmrpOfferBinding(
    _In_ PNMR_MODULE Client,
    _In_ PNMR_MODULE Provider)
{
    PNMR_BINDING Binding;
    NTSTATUS Status;

    if (Client->Deregistering || Provider->Deregistering || !NmrpSameNpi(Client, Provider))
        return;

    Binding = ExAllocatePoolZero(NonPagedPool, sizeof(*Binding), TAG_NMR);
    if (Binding == NULL)
        return;

    Binding->ClientModule = Client;
    Binding->ProviderModule = Provider;

    Status = Client->Client->ClientAttachProvider((HANDLE)Binding,
                                                  Client->Context,
                                                  &Provider->Provider->ProviderRegistrationInstance);
    if (!Binding->Attached)
    {
        DPRINT("Client %p passed on provider %p: 0x%lx\n", Client, Provider, Status);
        ExFreePoolWithTag(Binding, TAG_NMR);
        return;
    }

    InterlockedIncrement(&Client->BindingCount);
    InterlockedIncrement(&Provider->BindingCount);
    InsertTailList(&Client->Bindings, &Binding->ClientLink);
    InsertTailList(&Provider->Bindings, &Binding->ProviderLink);
}

static
NTSTATUS
NmrpRegister(
    _In_ BOOLEAN IsProvider,
    _In_ CONST VOID *Characteristics,
    _In_opt_ PVOID Context,
    _Out_ PHANDLE Handle)
{
    PNMR_MODULE Module, Other;
    PLIST_ENTRY Entry;

    if (Characteristics == NULL || Handle == NULL)
        return STATUS_INVALID_PARAMETER;

    NmrpInitialize();

    Module = ExAllocatePoolZero(NonPagedPool, sizeof(*Module), TAG_NMR);
    if (Module == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Module->IsProvider = IsProvider;
    Module->Context = Context;
    InitializeListHead(&Module->Bindings);
    KeInitializeEvent(&Module->Idle, NotificationEvent, FALSE);
    if (IsProvider)
        Module->Provider = Characteristics;
    else
        Module->Client = Characteristics;

    *Handle = (HANDLE)Module;

    /* The lock is recursive, so attach callbacks may register or deregister in turn */
    NmrpLock();
    if (IsProvider)
    {
        InsertTailList(&NmrProviders, &Module->Link);
        for (Entry = NmrClients.Flink; Entry != &NmrClients; Entry = Entry->Flink)
        {
            Other = CONTAINING_RECORD(Entry, NMR_MODULE, Link);
            NmrpOfferBinding(Other, Module);
        }
    }
    else
    {
        InsertTailList(&NmrClients, &Module->Link);
        for (Entry = NmrProviders.Flink; Entry != &NmrProviders; Entry = Entry->Flink)
        {
            Other = CONTAINING_RECORD(Entry, NMR_MODULE, Link);
            NmrpOfferBinding(Module, Other);
        }
    }
    NmrpUnlock();

    return STATUS_SUCCESS;
}

static
NTSTATUS
NmrpDeregister(
    _In_ HANDLE Handle,
    _In_ BOOLEAN IsProvider)
{
    PNMR_MODULE Module = (PNMR_MODULE)Handle;

    if (Module == NULL || Module->IsProvider != IsProvider)
        return STATUS_INVALID_PARAMETER;

    NmrpLock();
    if (Module->Deregistering)
    {
        NmrpUnlock();
        return STATUS_INVALID_PARAMETER;
    }
    Module->Deregistering = TRUE;
    RemoveEntryList(&Module->Link);
    NmrpUnlock();

    NmrpDetachAll(Module);

    if (Module->BindingCount == 0)
        KeSetEvent(&Module->Idle, IO_NO_INCREMENT, FALSE);

    return STATUS_SUCCESS;
}

/* Waits for every binding to be gone, then frees the registration */
static
NTSTATUS
NmrpWaitForDeregister(
    _In_ HANDLE Handle,
    _In_ BOOLEAN IsProvider)
{
    PNMR_MODULE Module = (PNMR_MODULE)Handle;

    if (Module == NULL || Module->IsProvider != IsProvider || !Module->Deregistering)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&Module->Idle, Executive, KernelMode, FALSE, NULL);
    ExFreePoolWithTag(Module, TAG_NMR);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NmrRegisterClient(
    _In_ PNPI_CLIENT_CHARACTERISTICS ClientCharacteristics,
    _In_opt_ PVOID ClientContext,
    _Out_ PHANDLE NmrClientHandle)
{
    return NmrpRegister(FALSE, ClientCharacteristics, ClientContext, NmrClientHandle);
}

NTSTATUS
NTAPI
NmrDeregisterClient(
    _In_ HANDLE NmrClientHandle)
{
    return NmrpDeregister(NmrClientHandle, FALSE);
}

NTSTATUS
NTAPI
NmrWaitForClientDeregisterComplete(
    _In_ HANDLE NmrClientHandle)
{
    return NmrpWaitForDeregister(NmrClientHandle, FALSE);
}

NTSTATUS
NTAPI
NmrRegisterProvider(
    _In_ PNPI_PROVIDER_CHARACTERISTICS ProviderCharacteristics,
    _In_opt_ PVOID ProviderContext,
    _Out_ PHANDLE NmrProviderHandle)
{
    return NmrpRegister(TRUE, ProviderCharacteristics, ProviderContext, NmrProviderHandle);
}

NTSTATUS
NTAPI
NmrDeregisterProvider(
    _In_ HANDLE NmrProviderHandle)
{
    return NmrpDeregister(NmrProviderHandle, TRUE);
}

NTSTATUS
NTAPI
NmrWaitForProviderDeregisterComplete(
    _In_ HANDLE NmrProviderHandle)
{
    return NmrpWaitForDeregister(NmrProviderHandle, TRUE);
}

/* Called by a client from its attach callback to bind to the provider being offered */
NTSTATUS
NTAPI
NmrClientAttachProvider(
    _In_ HANDLE NmrBindingHandle,
    _In_ PVOID ClientBindingContext,
    _In_ CONST VOID *ClientDispatch,
    _Out_ PVOID *ProviderBindingContext,
    _Out_ CONST VOID **ProviderDispatch)
{
    PNMR_BINDING Binding = (PNMR_BINDING)NmrBindingHandle;
    PNMR_MODULE Client, Provider;
    NTSTATUS Status;

    if (Binding == NULL || Binding->Attached)
        return STATUS_INVALID_PARAMETER;

    Client = Binding->ClientModule;
    Provider = Binding->ProviderModule;

    *ProviderBindingContext = NULL;
    *ProviderDispatch = NULL;

    Status = Provider->Provider->ProviderAttachClient(NmrBindingHandle,
                                                      Provider->Context,
                                                      &Client->Client->ClientRegistrationInstance,
                                                      ClientBindingContext,
                                                      ClientDispatch,
                                                      ProviderBindingContext,
                                                      ProviderDispatch);
    if (NT_SUCCESS(Status))
    {
        Binding->ClientBindingContext = ClientBindingContext;
        Binding->ProviderBindingContext = *ProviderBindingContext;
        Binding->Attached = TRUE;
    }

    return Status;
}

VOID
NTAPI
NmrClientDetachProviderComplete(
    _In_ HANDLE NmrBindingHandle)
{
    NmrpDetachSideDone((PNMR_BINDING)NmrBindingHandle);
}

VOID
NTAPI
NmrProviderDetachClientComplete(
    _In_ HANDLE NmrBindingHandle)
{
    NmrpDetachSideDone((PNMR_BINDING)NmrBindingHandle);
}
