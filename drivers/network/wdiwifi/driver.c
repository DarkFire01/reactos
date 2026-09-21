/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry and the miniport hook NDIS reaches the upper edge through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ndis.h>
#include <netioddk.h>
#include <dot11wdi.h>
#include <initguid.h>
#include <drivers/ndis/ndishook.h>
#include "wdiwifi.h"

#define NDEBUG
#include <debug.h>

/* The layouts x64 WLAN miniports are built against */
#ifdef _WIN64
C_ASSERT(sizeof(NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS) == 0x70);
C_ASSERT(NDIS_SIZEOF_WDI_INIT_PARAMETERS_REVISION_1 == 0x28);
C_ASSERT(NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_1 == 0xC4);
C_ASSERT(NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_2 == 0xCC);
C_ASSERT(NDIS_SIZEOF_WDI_DATA_API_REVISION_2 == 0x8C);
C_ASSERT(sizeof(WDI_FRAME_METADATA) == 0x60);
C_ASSERT(sizeof(WDI_TXRX_TARGET_CONFIGURATION) == 0x1C);
C_ASSERT(FIELD_OFFSET(WDI_TXRX_TARGET_CONFIGURATION, MaxNumPeers) == 0x19);
C_ASSERT(NDIS_SIZEOF_OID_REQUEST_REVISION_1 == 0xEC);
C_ASSERT(NDIS_SIZEOF_OID_REQUEST_REVISION_2 == 0xF8);
C_ASSERT(NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2 == 0xE0);
C_ASSERT(NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1 == 0x1C);
#endif

WDI_GLOBALS WdiGlobals;

/* {EB004A28-9B1A-11D4-9123-0050047759BC} */
static const NPI_MODULEID WdiModuleId =
{
    sizeof(NPI_MODULEID),
    MIT_GUID,
    { { 0xEB004A28, 0x9B1A, 0x11D4, { 0x91, 0x23, 0x00, 0x50, 0x04, 0x77, 0x59, 0xBC } } }
};

static const NDIS_HOOK_PROVIDER_CHARACTERISTICS WdiHookCharacteristics =
{
    NdisHookTypeWdi
};

static NPI_PROVIDER_ATTACH_CLIENT_FN WdiAttachClient;
static NPI_PROVIDER_DETACH_CLIENT_FN WdiDetachClient;
static NPI_PROVIDER_CLEANUP_BINDING_CONTEXT_FN WdiCleanupBindingContext;

static const NPI_PROVIDER_CHARACTERISTICS WdiProviderCharacteristics =
{
    0,
    sizeof(NPI_PROVIDER_CHARACTERISTICS),
    WdiAttachClient,
    WdiDetachClient,
    WdiCleanupBindingContext,
    {
        1,
        sizeof(NPI_REGISTRATION_INSTANCE),
        &NDIS_HOOK_NPI_ID,
        &WdiModuleId,
        0,
        &WdiHookCharacteristics
    }
};

static NDIS_HOOK_OID_REQUEST_COMPLETE WdiHookOidRequestComplete;
static NDIS_HOOK_INDICATE_STATUS WdiHookIndicateStatus;

static const NDIS_HOOK_PROVIDER_DISPATCH WdiProviderDispatch =
{
    WdiRegisterDriver,
    WdiDeregisterDriver,
    WdiHookOidRequestComplete,
    WdiHookOidRequestComplete,
    WdiHookIndicateStatus
};

/* Adapters, looked up the way NDIS names them */

_Use_decl_annotations_
PWDI_ADAPTER
NTAPI
WdiFindAdapterByHandle(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PWDI_ADAPTER Adapter;
    PWDI_ADAPTER Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    for (Entry = WdiGlobals.Adapters.Flink; Entry != &WdiGlobals.Adapters; Entry = Entry->Flink)
    {
        Adapter = CONTAINING_RECORD(Entry, WDI_ADAPTER, Link);
        if (Adapter->MiniportAdapterHandle == MiniportAdapterHandle)
        {
            Found = Adapter;
            break;
        }
    }
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    return Found;
}

_Use_decl_annotations_
PWDI_ADAPTER
NTAPI
WdiFindAdapterByContext(
    NDIS_HANDLE MiniportAdapterContext)
{
    PWDI_ADAPTER Adapter;
    PWDI_ADAPTER Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    for (Entry = WdiGlobals.Adapters.Flink; Entry != &WdiGlobals.Adapters; Entry = Entry->Flink)
    {
        Adapter = CONTAINING_RECORD(Entry, WDI_ADAPTER, Link);
        if (Adapter->MiniportAdapterContext == MiniportAdapterContext)
        {
            Found = Adapter;
            break;
        }
    }
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    return Found;
}

/* What NDIS routes to the upper edge */

static
VOID
NTAPI
WdiHookOidRequestComplete(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    PWDI_ADAPTER Adapter;

    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookAdapterHandle);

    /* Requests the upper edge sent carry themselves as their id */
    Adapter = WdiFindAdapterByHandle(MiniportAdapterHandle);
    if (Adapter != NULL && OidRequest->RequestId == OidRequest)
    {
        WdiOidRequestComplete(Adapter, OidRequest, Status);
        return;
    }

    WdiGlobals.Ndis.RawOidRequestComplete(MiniportAdapterHandle, OidRequest, Status);
}

static
VOID
NTAPI
WdiHookIndicateStatus(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PWDI_ADAPTER Adapter;

    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookAdapterHandle);

    Adapter = WdiFindAdapterByHandle(MiniportAdapterHandle);
    if (Adapter != NULL &&
        (StatusIndication->StatusCode & WDI_INDICATION_PREFIX) == WDI_INDICATION_PREFIX)
    {
        WdiIndication(Adapter, StatusIndication);
        return;
    }

    WdiGlobals.Ndis.RawIndicateStatus(MiniportAdapterHandle, StatusIndication);
}

/* NDIS binds as the client of the hook NPI */

static
NTSTATUS
NTAPI
WdiAttachClient(
    _In_ HANDLE NmrBindingHandle,
    _In_ PVOID ProviderContext,
    _In_ PNPI_REGISTRATION_INSTANCE ClientRegistrationInstance,
    _In_ PVOID ClientBindingContext,
    _In_ CONST VOID *ClientDispatch,
    _Out_ PVOID *ProviderBindingContext,
    _Out_ CONST VOID **ProviderDispatch)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(ClientRegistrationInstance);

    KeAcquireSpinLock(&WdiGlobals.BindingLock, &OldIrql);
    if (WdiGlobals.NmrBinding != NULL)
    {
        Status = STATUS_ALREADY_REGISTERED;
    }
    else
    {
        WdiGlobals.NmrBinding = NmrBindingHandle;
        WdiGlobals.ClientBindingContext = ClientBindingContext;
        RtlCopyMemory(&WdiGlobals.Ndis, ClientDispatch, sizeof(WdiGlobals.Ndis));
    }
    KeReleaseSpinLock(&WdiGlobals.BindingLock, OldIrql);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NDIS is already bound to the WDI upper edge\n");
        return Status;
    }

    *ProviderBindingContext = ProviderContext;
    *ProviderDispatch = &WdiProviderDispatch;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WdiDetachClient(
    _In_ PVOID ProviderBindingContext)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(ProviderBindingContext);

    /* NDIS waits out the drivers it registered through the hook before it lets go */
    KeAcquireSpinLock(&WdiGlobals.BindingLock, &OldIrql);
    WdiGlobals.NmrBinding = NULL;
    KeReleaseSpinLock(&WdiGlobals.BindingLock, OldIrql);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WdiCleanupBindingContext(
    _In_ PVOID ProviderBindingContext)
{
    UNREFERENCED_PARAMETER(ProviderBindingContext);
}

/* Loading and unloading */

static
VOID
NTAPI
WdiUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverObject);

    if (WdiGlobals.NmrProvider == NULL)
        return;

    Status = NmrDeregisterProvider(WdiGlobals.NmrProvider);
    if (Status == STATUS_PENDING)
        NmrWaitForProviderDeregisterComplete(WdiGlobals.NmrProvider);

    WdiGlobals.NmrProvider = NULL;
}

/**
 * @brief
 * Registers the upper edge as the provider of the miniport hook NPI, which
 * NDIS attaches to before it hands over a WLAN miniport's registration.
 *
 * @param[in] DriverObject
 * This driver.
 *
 * @param[in] RegistryPath
 * Its service key.
 *
 * @return
 * STATUS_SUCCESS, or why NMR refused the provider.
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&WdiGlobals, sizeof(WdiGlobals));
    WdiGlobals.DriverObject = DriverObject;
    KeInitializeSpinLock(&WdiGlobals.BindingLock);
    KeInitializeSpinLock(&WdiGlobals.ListLock);
    InitializeListHead(&WdiGlobals.Miniports);
    InitializeListHead(&WdiGlobals.Adapters);

    DriverObject->DriverUnload = WdiUnload;

    Status = NmrRegisterProvider(&WdiProviderCharacteristics, &WdiGlobals, &WdiGlobals.NmrProvider);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Registering the miniport hook provider failed (0x%lx)\n", Status);
        WdiGlobals.NmrProvider = NULL;
    }

    return Status;
}
