/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Miniport hooks: WLAN miniports under a WDI upper edge
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include <initguid.h>
#include <drivers/ndis/ndishook.h>

/*
 * Each hook type has a slot that gets every OID completion and status
 * indication of its adapters. A WDI slot hands them to the upper edge once it
 * has attached through NMR, the unhooked slot passes them straight on.
 */
typedef struct _CORE_HOOK
{
    /* The provider attached, and Dispatch is complete */
    BOOLEAN Ready;
    BOOLEAN Attached;

    /* One reference per driver registered through the hook */
    EX_RUNDOWN_REF Rundown;

    HANDLE NmrBinding;
    PVOID ProviderBindingContext;
    NDIS_HOOK_PROVIDER_DISPATCH Dispatch;

    WORK_QUEUE_ITEM DetachWorkItem;
    LONG DetachQueued;
} CORE_HOOK, *PCORE_HOOK;

typedef enum _CORE_WDI_STATE
{
    CoreWdiUnloaded,
    CoreWdiLoading,
    CoreWdiLoaded,
    CoreWdiUnloading
} CORE_WDI_STATE;

static NDIS_HOOK_OID_REQUEST_COMPLETE CoreHookPlainOidRequestComplete;
static NDIS_HOOK_OID_REQUEST_COMPLETE CoreHookPlainDirectOidRequestComplete;
static NDIS_HOOK_INDICATE_STATUS CoreHookPlainIndicateStatus;

static CORE_HOOK CoreHooks[NdisHookTypeCount] =
{
    {
        FALSE, FALSE, { 0 }, NULL, NULL,
        {
            NULL,
            NULL,
            CoreHookPlainOidRequestComplete,
            CoreHookPlainDirectOidRequestComplete,
            CoreHookPlainIndicateStatus
        }
    }
};

static KSPIN_LOCK CoreHookLock;
static HANDLE CoreHookClient;

static FAST_MUTEX CoreWdiLock;
static KEVENT CoreWdiSettled;
static CORE_WDI_STATE CoreWdiState;
static ULONG CoreWdiReferences;
static BOOLEAN CoreWdiLoadedHere;

static UNICODE_STRING CoreWdiService =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\WdiWifi");

/* {EB004A11-9B1A-11D4-9123-0050047759BC} */
static const NPI_MODULEID CoreNdisModuleId =
{
    sizeof(NPI_MODULEID),
    MIT_GUID,
    { { 0xEB004A11, 0x9B1A, 0x11D4, { 0x91, 0x23, 0x00, 0x50, 0x04, 0x77, 0x59, 0xBC } } }
};

static NPI_CLIENT_ATTACH_PROVIDER_FN CoreHookAttachProvider;
static NPI_CLIENT_DETACH_PROVIDER_FN CoreHookDetachProvider;

static const NPI_CLIENT_CHARACTERISTICS CoreHookClientCharacteristics =
{
    0,
    sizeof(NPI_CLIENT_CHARACTERISTICS),
    CoreHookAttachProvider,
    CoreHookDetachProvider,
    NULL,
    {
        1,
        sizeof(NPI_REGISTRATION_INSTANCE),
        &NDIS_HOOK_NPI_ID,
        &CoreNdisModuleId,
        0,
        NULL
    }
};

/* The unhooked paths */

static
VOID
NTAPI
CoreHookRawOidRequestComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    CoreOidRequestComplete((PLOGICAL_ADAPTER)MiniportAdapterHandle, OidRequest, Status);
}

static
VOID
NTAPI
CoreHookRawDirectOidRequestComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);

    NDIS_DbgPrint(MIN_TRACE, ("Direct OID request %p completed with 0x%x, none was sent.\n", OidRequest, Status));
}

static
VOID
NTAPI
CoreHookRawIndicateStatus(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    CoreIndicateStatus((PLOGICAL_ADAPTER)MiniportAdapterHandle, StatusIndication);
}

static
VOID
NTAPI
CoreHookPlainOidRequestComplete(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookAdapterHandle);

    CoreHookRawOidRequestComplete(MiniportAdapterHandle, OidRequest, Status);
}

static
VOID
NTAPI
CoreHookPlainDirectOidRequestComplete(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookAdapterHandle);

    CoreHookRawDirectOidRequestComplete(MiniportAdapterHandle, OidRequest, Status);
}

static
VOID
NTAPI
CoreHookPlainIndicateStatus(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookAdapterHandle);

    CoreHookRawIndicateStatus(MiniportAdapterHandle, StatusIndication);
}

/* Where every miniport's completions and status go, by its driver's hook type */

static
PCORE_HOOK
CoreHookOfAdapter(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    return &CoreHooks[Adapter->NdisMiniportBlock.DriverHandle->HookType];
}

/**
 * @brief
 * A miniport completes an OID request that pended.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] OidRequest
 * The request.
 *
 * @param[in] Status
 * How it ended.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMOidRequestComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PCORE_HOOK Hook = CoreHookOfAdapter(Adapter);

    Hook->Dispatch.OidRequestComplete(Hook->ProviderBindingContext,
                                      Adapter->HookAdapterHandle,
                                      Adapter,
                                      OidRequest,
                                      Status);
}

/**
 * @brief
 * A miniport completes a direct OID request that pended.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] OidRequest
 * The request.
 *
 * @param[in] Status
 * How it ended.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMDirectOidRequestComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PCORE_HOOK Hook = CoreHookOfAdapter(Adapter);

    Hook->Dispatch.DirectOidRequestComplete(Hook->ProviderBindingContext,
                                            Adapter->HookAdapterHandle,
                                            Adapter,
                                            OidRequest,
                                            Status);
}

/**
 * @brief
 * A miniport indicates a status.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] StatusIndication
 * The status code and its buffer.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMIndicateStatusEx(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_STATUS_INDICATION StatusIndication)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PCORE_HOOK Hook = CoreHookOfAdapter(Adapter);

    Hook->Dispatch.IndicateStatus(Hook->ProviderBindingContext,
                                  Adapter->HookAdapterHandle,
                                  Adapter,
                                  StatusIndication);
}

/* What the provider reaches NDIS and the hooked miniport with */

static
VOID
NTAPI
CoreHookSetDriverContext(
    _In_ NDIS_HANDLE NdisMiniportDriverHandle,
    _In_ NDIS_HANDLE HookDriverHandle)
{
    ((PNDIS_M_DRIVER_BLOCK)NdisMiniportDriverHandle)->HookDriverHandle = HookDriverHandle;
}

static
VOID
NTAPI
CoreHookSetAdapterContext(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE HookAdapterHandle)
{
    ((PLOGICAL_ADAPTER)MiniportAdapterHandle)->HookAdapterHandle = HookAdapterHandle;
}

static
NDIS_HANDLE
NTAPI
CoreHookGetDriverHandle(
    _In_ NDIS_HANDLE MiniportDriverContext)
{
    PNDIS_M_DRIVER_BLOCK Driver;
    NDIS_HANDLE Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (MiniportDriverContext == NULL)
        return NULL;

    KeAcquireSpinLock(&MiniportListLock, &OldIrql);
    for (Entry = MiniportListHead.Flink; Entry != &MiniportListHead; Entry = Entry->Flink)
    {
        Driver = CONTAINING_RECORD(Entry, NDIS_M_DRIVER_BLOCK, ListEntry);
        if (Driver->MiniportDriverContext == MiniportDriverContext)
        {
            Found = Driver->HookDriverHandle;
            break;
        }
    }
    KeReleaseSpinLock(&MiniportListLock, OldIrql);

    return Found;
}

static
NDIS_HANDLE
NTAPI
CoreHookGetAdapterHandle(
    _In_ NDIS_HANDLE MiniportAdapterContext)
{
    PLOGICAL_ADAPTER Adapter;
    NDIS_HANDLE Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (MiniportAdapterContext == NULL)
        return NULL;

    KeAcquireSpinLock(&AdapterListLock, &OldIrql);
    for (Entry = AdapterListHead.Flink; Entry != &AdapterListHead; Entry = Entry->Flink)
    {
        Adapter = CONTAINING_RECORD(Entry, LOGICAL_ADAPTER, ListEntry);
        if (Adapter->NdisMiniportBlock.MiniportAdapterContext == MiniportAdapterContext)
        {
            Found = Adapter->HookAdapterHandle;
            break;
        }
    }
    KeReleaseSpinLock(&AdapterListLock, OldIrql);

    return Found;
}

static
NDIS_STATUS
NTAPI
CoreHookInvokeOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    return Adapter->NdisMiniportBlock.DriverHandle->UnhookedCharacteristics->OidRequestHandler(
        Adapter->NdisMiniportBlock.MiniportAdapterContext,
        OidRequest);
}

static
VOID
NTAPI
CoreHookInvokeCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PVOID RequestId)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Unhooked = Adapter->NdisMiniportBlock.DriverHandle->UnhookedCharacteristics;

    if (Unhooked->CancelOidRequestHandler != NULL)
        Unhooked->CancelOidRequestHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext, RequestId);
}

static
NDIS_STATUS
NTAPI
CoreHookInvokeDirectOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Unhooked = Adapter->NdisMiniportBlock.DriverHandle->UnhookedCharacteristics;

    if (Unhooked->DirectOidRequestHandler == NULL)
        return NDIS_STATUS_NOT_SUPPORTED;

    return Unhooked->DirectOidRequestHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext, OidRequest);
}

static
VOID
NTAPI
CoreHookInvokeCancelDirectOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PVOID RequestId)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Unhooked = Adapter->NdisMiniportBlock.DriverHandle->UnhookedCharacteristics;

    if (Unhooked->CancelDirectOidRequestHandler != NULL)
        Unhooked->CancelDirectOidRequestHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext, RequestId);
}

static const NDIS_HOOK_CLIENT_DISPATCH CoreHookClientDispatch =
{
    CoreHookSetDriverContext,
    CoreHookSetAdapterContext,
    CoreHookGetDriverHandle,
    CoreHookGetAdapterHandle,
    CoreHookRawOidRequestComplete,
    CoreHookRawDirectOidRequestComplete,
    CoreHookRawIndicateStatus,
    CoreHookInvokeOidRequest,
    CoreHookInvokeCancelOidRequest,
    CoreHookInvokeDirectOidRequest,
    CoreHookInvokeCancelDirectOidRequest
};

/* The provider comes and goes through NMR */

static
NTSTATUS
NTAPI
CoreHookAttachProvider(
    _In_ HANDLE NmrBindingHandle,
    _In_ PVOID ClientContext,
    _In_ PNPI_REGISTRATION_INSTANCE ProviderRegistrationInstance)
{
    const NDIS_HOOK_PROVIDER_DISPATCH *Dispatch;
    PNDIS_HOOK_PROVIDER_CHARACTERISTICS Characteristics;
    PCORE_HOOK Hook;
    NTSTATUS Status;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(ClientContext);

    Characteristics = (PNDIS_HOOK_PROVIDER_CHARACTERISTICS)ProviderRegistrationInstance->NpiSpecificCharacteristics;
    if (ProviderRegistrationInstance->Version != 1 ||
        ProviderRegistrationInstance->Size != sizeof(NPI_REGISTRATION_INSTANCE) ||
        Characteristics == NULL ||
        Characteristics->Type != NdisHookTypeWdi)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Hook = &CoreHooks[Characteristics->Type];

    KeAcquireSpinLock(&CoreHookLock, &OldIrql);
    if (Hook->Attached)
    {
        KeReleaseSpinLock(&CoreHookLock, OldIrql);
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }
    Hook->Attached = TRUE;
    Hook->NmrBinding = NmrBindingHandle;
    KeReleaseSpinLock(&CoreHookLock, OldIrql);

    /* The hook type doubles as the binding context, which is all a detach needs */
    Status = NmrClientAttachProvider(NmrBindingHandle,
                                     (PVOID)(ULONG_PTR)Characteristics->Type,
                                     &CoreHookClientDispatch,
                                     &Hook->ProviderBindingContext,
                                     (CONST VOID **)&Dispatch);

    KeAcquireSpinLock(&CoreHookLock, &OldIrql);
    if (NT_SUCCESS(Status))
    {
        Hook->Dispatch = *Dispatch;

        /* A provider can leave the completion and status handlers to NDIS */
        if (Hook->Dispatch.OidRequestComplete == NULL)
            Hook->Dispatch.OidRequestComplete = CoreHookPlainOidRequestComplete;
        if (Hook->Dispatch.DirectOidRequestComplete == NULL)
            Hook->Dispatch.DirectOidRequestComplete = CoreHookPlainDirectOidRequestComplete;
        if (Hook->Dispatch.IndicateStatus == NULL)
            Hook->Dispatch.IndicateStatus = CoreHookPlainIndicateStatus;

        Hook->Ready = TRUE;
    }
    else
    {
        Hook->Attached = FALSE;
        Hook->NmrBinding = NULL;
    }
    KeReleaseSpinLock(&CoreHookLock, OldIrql);

    return Status;
}

/* Waits out every driver still registered through the hook, then forgets the provider */
static
HANDLE
CoreHookDetach(
    _In_ PCORE_HOOK Hook)
{
    HANDLE Binding;
    KIRQL OldIrql;

    ExWaitForRundownProtectionRelease(&Hook->Rundown);

    KeAcquireSpinLock(&CoreHookLock, &OldIrql);
    Binding = Hook->NmrBinding;
    Hook->Ready = FALSE;
    Hook->Attached = FALSE;
    Hook->NmrBinding = NULL;
    Hook->ProviderBindingContext = NULL;
    RtlZeroMemory(&Hook->Dispatch, sizeof(Hook->Dispatch));
    ExReInitializeRundownProtection(&Hook->Rundown);
    InterlockedExchange(&Hook->DetachQueued, 0);
    KeReleaseSpinLock(&CoreHookLock, OldIrql);

    return Binding;
}

static
VOID
NTAPI
CoreHookDetachWorker(
    _In_ PVOID Context)
{
    NmrClientDetachProviderComplete(CoreHookDetach(Context));
}

static
NTSTATUS
NTAPI
CoreHookDetachProvider(
    _In_ PVOID ClientBindingContext)
{
    ULONG_PTR Type = (ULONG_PTR)ClientBindingContext;
    PCORE_HOOK Hook;

    if (Type != NdisHookTypeWdi)
        return STATUS_INVALID_PARAMETER;

    Hook = &CoreHooks[Type];

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        CoreHookDetach(Hook);
        return STATUS_SUCCESS;
    }

    if (InterlockedCompareExchange(&Hook->DetachQueued, 1, 0) == 0)
    {
        ExInitializeWorkItem(&Hook->DetachWorkItem, CoreHookDetachWorker, Hook);
        ExQueueWorkItem(&Hook->DetachWorkItem, DelayedWorkQueue);
    }

    return STATUS_PENDING;
}

/* A referenced hook, or NULL when its provider is not there */
static
PCORE_HOOK
CoreHookAcquire(
    _In_ NDIS_HOOK_TYPE Type)
{
    PCORE_HOOK Hook = &CoreHooks[Type];
    BOOLEAN Ready;
    KIRQL OldIrql;

    KeAcquireSpinLock(&CoreHookLock, &OldIrql);
    Ready = Hook->Ready && ExAcquireRundownProtection(&Hook->Rundown);
    KeReleaseSpinLock(&CoreHookLock, OldIrql);

    return Ready ? Hook : NULL;
}

/* Loading the upper edge */

/* The first reference loads the upper edge's service, unless something else already did */
static
BOOLEAN
CoreReferenceWdi(VOID)
{
    NTSTATUS Status;
    BOOLEAN Loaded;

    for (;;)
    {
        ExAcquireFastMutex(&CoreWdiLock);

        if (CoreWdiState == CoreWdiLoaded)
        {
            CoreWdiReferences++;
            ExReleaseFastMutex(&CoreWdiLock);
            return TRUE;
        }

        if (CoreWdiState == CoreWdiUnloaded)
        {
            CoreWdiState = CoreWdiLoading;
            KeClearEvent(&CoreWdiSettled);
            ExReleaseFastMutex(&CoreWdiLock);
            break;
        }

        /* Someone else is loading or unloading it */
        ExReleaseFastMutex(&CoreWdiLock);
        KeWaitForSingleObject(&CoreWdiSettled, Executive, KernelMode, FALSE, NULL);
    }

    Status = ZwLoadDriver(&CoreWdiService);
    Loaded = NT_SUCCESS(Status) || Status == STATUS_IMAGE_ALREADY_LOADED;
    if (!Loaded)
        NDIS_DbgPrint(MIN_TRACE, ("Loading %wZ failed (0x%lx).\n", &CoreWdiService, Status));

    ExAcquireFastMutex(&CoreWdiLock);
    if (Loaded)
    {
        CoreWdiState = CoreWdiLoaded;
        CoreWdiReferences = 1;
        CoreWdiLoadedHere = NT_SUCCESS(Status);
    }
    else
    {
        CoreWdiState = CoreWdiUnloaded;
    }
    KeSetEvent(&CoreWdiSettled, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&CoreWdiLock);

    return Loaded;
}

/* The last reference unloads the upper edge, if NDIS was the one that loaded it */
static
VOID
CoreDereferenceWdi(VOID)
{
    BOOLEAN Unload;

    ExAcquireFastMutex(&CoreWdiLock);
    if (--CoreWdiReferences != 0)
    {
        ExReleaseFastMutex(&CoreWdiLock);
        return;
    }

    CoreWdiState = CoreWdiUnloading;
    KeClearEvent(&CoreWdiSettled);
    Unload = CoreWdiLoadedHere;
    CoreWdiLoadedHere = FALSE;
    ExReleaseFastMutex(&CoreWdiLock);

    if (Unload)
        ZwUnloadDriver(&CoreWdiService);

    ExAcquireFastMutex(&CoreWdiLock);
    CoreWdiState = CoreWdiUnloaded;
    KeSetEvent(&CoreWdiSettled, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&CoreWdiLock);
}

/**
 * @brief
 * Sets up miniport hooks and registers NDIS as a client of the hook NPI.
 * Without NMR, WDI miniports just fail to register.
 */
VOID
NTAPI
CoreHookInitialize(VOID)
{
    NTSTATUS Status;

    KeInitializeSpinLock(&CoreHookLock);
    ExInitializeFastMutex(&CoreWdiLock);
    KeInitializeEvent(&CoreWdiSettled, NotificationEvent, TRUE);
    CoreWdiState = CoreWdiUnloaded;

    Status = NmrRegisterClient(&CoreHookClientCharacteristics, NULL, &CoreHookClient);
    if (!NT_SUCCESS(Status))
        NDIS_DbgPrint(MIN_TRACE, ("No miniport hook client (0x%lx).\n", Status));
}

/* Registering a WLAN miniport */

/**
 * @brief
 * Registers a WLAN miniport driver under the WDI upper edge, loading the
 * upper edge first when nothing has yet.
 *
 * @param[in] DriverObject
 * The miniport's driver object.
 *
 * @param[in] RegistryPath
 * Its service key.
 *
 * @param[in] NdisDriverContext
 * The context its handlers get.
 *
 * @param[in] MiniportDriverCharacteristics
 * Its NDIS handlers, which the upper edge reaches through NDIS.
 *
 * @param[in] MiniportWdiCharacteristics
 * Its WDI handlers, for the upper edge.
 *
 * @param[out] NdisMiniportDriverHandle
 * The miniport driver.
 *
 * @return
 * NDIS_STATUS_SUCCESS, NDIS_STATUS_NOT_SUPPORTED when there is no upper edge,
 * or why the upper edge refused the driver.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMRegisterWdiMiniportDriver(
    PDRIVER_OBJECT DriverObject,
    PCUNICODE_STRING RegistryPath,
    NDIS_HANDLE NdisDriverContext,
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    PNDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS MiniportWdiCharacteristics,
    PNDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Unhooked;
    PNDIS_M_DRIVER_BLOCK Driver;
    PCORE_HOOK Hook;
    NDIS_STATUS Status;

    PAGED_CODE();

    *NdisMiniportDriverHandle = NULL;

    /* The upper edge reaches the miniport's own handlers through this copy */
    Unhooked = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Unhooked), NDIS_TAG);
    if (Unhooked == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Unhooked, sizeof(*Unhooked));
    RtlCopyMemory(Unhooked,
                  MiniportDriverCharacteristics,
                  min(MiniportDriverCharacteristics->Header.Size, sizeof(*Unhooked)));

    if (!CoreReferenceWdi())
    {
        ExFreePoolWithTag(Unhooked, NDIS_TAG);
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    Hook = CoreHookAcquire(NdisHookTypeWdi);
    if (Hook == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("The WDI upper edge did not attach.\n"));
        CoreDereferenceWdi();
        ExFreePoolWithTag(Unhooked, NDIS_TAG);
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    Status = Hook->Dispatch.RegisterWdiDriver(Hook->ProviderBindingContext,
                                              DriverObject,
                                              RegistryPath,
                                              NdisDriverContext,
                                              MiniportDriverCharacteristics,
                                              MiniportWdiCharacteristics,
                                              NdisMiniportDriverHandle);
    if (Status == NDIS_STATUS_SUCCESS && *NdisMiniportDriverHandle == NULL)
        Status = NDIS_STATUS_FAILURE;

    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("The WDI upper edge refused the driver (0x%x).\n", Status));
        ExReleaseRundownProtection(&Hook->Rundown);
        CoreDereferenceWdi();
        ExFreePoolWithTag(Unhooked, NDIS_TAG);
        return Status;
    }

    /* The hook reference stays with the driver until it deregisters */
    Driver = *NdisMiniportDriverHandle;
    Driver->UnhookedCharacteristics = Unhooked;
    Driver->HookType = NdisHookTypeWdi;
    Driver->HookRegistered = TRUE;

    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Deregisters a WLAN miniport driver from the WDI upper edge.
 *
 * @param[in] NdisMiniportDriverHandle
 * The miniport driver, from NdisMRegisterWdiMiniportDriver.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMDeregisterWdiMiniportDriver(
    NDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS_M_DRIVER_BLOCK Driver = NdisMiniportDriverHandle;
    PCORE_HOOK Hook = &CoreHooks[NdisHookTypeWdi];

    PAGED_CODE();

    if (Driver->HookType != NdisHookTypeWdi || !Driver->HookRegistered)
        return;

    Driver->HookRegistered = FALSE;

    /* The upper edge deregisters the NDIS driver, and with it the driver block */
    Hook->Dispatch.DeregisterWdiDriver(Hook->ProviderBindingContext, Driver, Driver->HookDriverHandle);

    ExReleaseRundownProtection(&Hook->Rundown);
    CoreDereferenceWdi();
}
