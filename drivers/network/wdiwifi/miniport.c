/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The NDIS miniport the upper edge registers in front of a WLAN miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "wdiwifi.h"

#define NDEBUG
#include <debug.h>

static MINIPORT_INITIALIZE WdiMpInitialize;
static MINIPORT_HALT WdiMpHalt;
static MINIPORT_SET_OPTIONS WdiMpSetOptions;
static MINIPORT_PAUSE WdiMpPause;
static MINIPORT_RESTART WdiMpRestart;
static MINIPORT_OID_REQUEST WdiMpOidRequest;
static MINIPORT_CANCEL_OID_REQUEST WdiMpCancelOidRequest;
static MINIPORT_DIRECT_OID_REQUEST WdiMpDirectOidRequest;
static MINIPORT_CANCEL_DIRECT_OID_REQUEST WdiMpCancelDirectOidRequest;
static MINIPORT_SEND_NET_BUFFER_LISTS WdiMpSendNetBufferLists;
static MINIPORT_RETURN_NET_BUFFER_LISTS WdiMpReturnNetBufferLists;
static MINIPORT_CANCEL_SEND WdiMpCancelSend;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY WdiMpDevicePnPEventNotify;
static MINIPORT_SHUTDOWN WdiMpShutdown;
static MINIPORT_RESET WdiMpReset;

static NDIS_WDI_OPEN_ADAPTER_COMPLETE WdiOpenAdapterComplete;
static NDIS_WDI_CLOSE_ADAPTER_COMPLETE WdiCloseAdapterComplete;
static NDIS_WDI_IDLE_NOTIFICATION_CONFIRM WdiIdleNotificationConfirm;
static NDIS_WDI_IDLE_NOTIFICATION_COMPLETE WdiIdleNotificationComplete;

/* The WLAN miniport a wrapper call is for */

static
PWDI_MINIPORT
WdiFindMiniport(
    _In_opt_ NDIS_HANDLE DriverContext,
    _In_opt_ NDIS_HANDLE NdisDriverHandle)
{
    PWDI_MINIPORT Miniport;
    PWDI_MINIPORT Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    for (Entry = WdiGlobals.Miniports.Flink; Entry != &WdiGlobals.Miniports; Entry = Entry->Flink)
    {
        Miniport = CONTAINING_RECORD(Entry, WDI_MINIPORT, Link);
        if ((DriverContext != NULL && Miniport->DriverContext == DriverContext) ||
            (NdisDriverHandle != NULL && Miniport->NdisDriverHandle == NdisDriverHandle))
        {
            Found = Miniport;
            break;
        }
    }
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    return Found;
}

static
VOID
WdiUnlinkMiniport(
    _In_ PWDI_MINIPORT Miniport)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    RemoveEntryList(&Miniport->Link);
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);
}

/* Registration, handed over by NDIS */

/**
 * @brief
 * Takes over a WLAN miniport's registration: the NDIS miniport is registered
 * on the IHV's driver object with the upper edge's handlers, and the IHV's own
 * handlers are kept for the upper edge to call.
 *
 * @return
 * NDIS_STATUS_SUCCESS, STATUS_INVALID_PARAMETER for a WDI version the upper
 * edge does not take, NDIS_STATUS_BAD_VERSION for an IHV older than NDIS 6.50,
 * or why NDIS refused the registration.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiRegisterDriver(
    PVOID ProviderBindingContext,
    PDRIVER_OBJECT DriverObject,
    PCUNICODE_STRING RegistryPath,
    NDIS_HANDLE MiniportDriverContext,
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    PNDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS MiniportWdiCharacteristics,
    PNDIS_HANDLE NdisMiniportDriverHandle)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Wrapper;
    PWDI_MINIPORT Miniport;
    NDIS_STATUS Status;
    ULONG Version;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(ProviderBindingContext);

    *NdisMiniportDriverHandle = NULL;

    /* WDI 1.1.1 through 1.1.3 were never shipped */
    Version = MiniportWdiCharacteristics->WdiVersion;
    if (Version < WDI_VERSION_1_0 ||
        (Version > WDI_VERSION_1_1_0 && Version < WDI_VERSION_1_1_4))
    {
        DPRINT1("WDI version 0x%lx is not supported\n", Version);
        return STATUS_INVALID_PARAMETER;
    }

    Miniport = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Miniport), WDI_TAG);
    if (Miniport == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Miniport, sizeof(*Miniport));
    RtlCopyMemory(&Miniport->Ndis,
                  MiniportDriverCharacteristics,
                  min(MiniportDriverCharacteristics->Header.Size, sizeof(Miniport->Ndis)));
    RtlCopyMemory(&Miniport->Wdi,
                  MiniportWdiCharacteristics,
                  min(MiniportWdiCharacteristics->Header.Size, sizeof(Miniport->Wdi)));

    /* The IHV's OID handlers are only reached through NDIS */
    Miniport->Ndis.OidRequestHandler = NULL;
    Miniport->Ndis.CancelOidRequestHandler = NULL;
    Miniport->Ndis.DirectOidRequestHandler = NULL;
    Miniport->Ndis.CancelDirectOidRequestHandler = NULL;

    Miniport->DriverObject = DriverObject;
    Miniport->DriverContext = MiniportDriverContext;
    if (Miniport->DriverContext == NULL)
    {
        Miniport->DriverContext = Miniport;
        Miniport->OwnDriverContext = TRUE;
    }

    Miniport->InitParameters.Header.Type = NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS;
    Miniport->InitParameters.Header.Revision = NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS_REVISION_1;
    Miniport->InitParameters.Header.Size = NDIS_SIZEOF_WDI_INIT_PARAMETERS_REVISION_1;
    Miniport->InitParameters.WdiVersion = WDI_UPPER_EDGE_VERSION;
    Miniport->InitParameters.OpenAdapterCompleteHandler = WdiOpenAdapterComplete;
    Miniport->InitParameters.CloseAdapterCompleteHandler = WdiCloseAdapterComplete;
    Miniport->InitParameters.UeIdleNotificationConfirm = WdiIdleNotificationConfirm;
    Miniport->InitParameters.UeIdleNotificationComplete = WdiIdleNotificationComplete;

    RtlZeroMemory(&Wrapper, sizeof(Wrapper));
    RtlCopyMemory(&Wrapper,
                  MiniportDriverCharacteristics,
                  min(MiniportDriverCharacteristics->Header.Size, sizeof(Wrapper)));
    Wrapper.InitializeHandlerEx = WdiMpInitialize;
    Wrapper.HaltHandlerEx = WdiMpHalt;
    Wrapper.SetOptionsHandler = WdiMpSetOptions;
    Wrapper.PauseHandler = WdiMpPause;
    Wrapper.RestartHandler = WdiMpRestart;
    Wrapper.OidRequestHandler = WdiMpOidRequest;
    Wrapper.SendNetBufferListsHandler = WdiMpSendNetBufferLists;
    Wrapper.ReturnNetBufferListsHandler = WdiMpReturnNetBufferLists;
    Wrapper.CancelSendHandler = WdiMpCancelSend;
    Wrapper.DevicePnPEventNotifyHandler = WdiMpDevicePnPEventNotify;
    Wrapper.ShutdownHandlerEx = WdiMpShutdown;
    Wrapper.CheckForHangHandlerEx = NULL;
    Wrapper.ResetHandlerEx = WdiMpReset;
    Wrapper.CancelOidRequestHandler = WdiMpCancelOidRequest;
    Wrapper.DirectOidRequestHandler = WdiMpDirectOidRequest;
    Wrapper.CancelDirectOidRequestHandler = WdiMpCancelDirectOidRequest;

    if (Miniport->Ndis.MajorNdisVersion < 6 ||
        (Miniport->Ndis.MajorNdisVersion == 6 && Miniport->Ndis.MinorNdisVersion < 50))
    {
        DPRINT1("NDIS %u.%u miniport with WDI 0x%lx is too old\n",
                Miniport->Ndis.MajorNdisVersion, Miniport->Ndis.MinorNdisVersion, Version);
        ExFreePoolWithTag(Miniport, WDI_TAG);
        return NDIS_STATUS_BAD_VERSION;
    }

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    InsertTailList(&WdiGlobals.Miniports, &Miniport->Link);
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    Status = NdisMRegisterMiniportDriver(DriverObject,
                                         (PUNICODE_STRING)RegistryPath,
                                         Miniport->DriverContext,
                                         &Wrapper,
                                         &Miniport->NdisDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("Registering the NDIS miniport failed (0x%x)\n", Status);
        WdiUnlinkMiniport(Miniport);
        ExFreePoolWithTag(Miniport, WDI_TAG);
        return Status;
    }

    DPRINT1("WLAN miniport NDIS %u.%u WDI 0x%lx registered\n",
            Miniport->Ndis.MajorNdisVersion, Miniport->Ndis.MinorNdisVersion, Version);

    *NdisMiniportDriverHandle = Miniport->NdisDriverHandle;
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
WdiDeregisterDriver(
    PVOID ProviderBindingContext,
    NDIS_HANDLE NdisMiniportDriverHandle,
    NDIS_HANDLE HookDriverHandle)
{
    PWDI_MINIPORT Miniport;

    UNREFERENCED_PARAMETER(ProviderBindingContext);
    UNREFERENCED_PARAMETER(HookDriverHandle);

    Miniport = WdiFindMiniport(NULL, NdisMiniportDriverHandle);
    if (Miniport == NULL)
        return;

    NdisMDeregisterMiniportDriver(Miniport->NdisDriverHandle);
    Miniport->NdisDriverHandle = NULL;

    WdiUnlinkMiniport(Miniport);
    ExFreePoolWithTag(Miniport, WDI_TAG);
}

/* The NDIS miniport handlers */

static
NDIS_STATUS
NTAPI
WdiMpInitialize(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PWDI_MINIPORT Miniport;
    PWDI_ADAPTER Adapter;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    Miniport = WdiFindMiniport(MiniportDriverContext, NULL);
    if (Miniport == NULL)
        return NDIS_STATUS_FAILURE;

    Adapter = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Adapter), WDI_TAG);
    if (Adapter == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->Miniport = Miniport;
    Adapter->MiniportAdapterHandle = MiniportAdapterHandle;
    Adapter->PeerVersion = Miniport->Wdi.WdiVersion;

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    InsertTailList(&WdiGlobals.Adapters, &Adapter->Link);
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    Status = WdiInitializeAdapter(Adapter, MiniportInitParameters);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
        RemoveEntryList(&Adapter->Link);
        KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

        ExFreePoolWithTag(Adapter, WDI_TAG);
        return Status;
    }

    WdiGlobals.Ndis.SetAdapterContext(MiniportAdapterHandle, Adapter);
    return NDIS_STATUS_SUCCESS;
}

static
VOID
NTAPI
WdiMpHalt(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PWDI_ADAPTER Adapter;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(HaltAction);

    Adapter = WdiFindAdapterByContext(MiniportAdapterContext);
    if (Adapter == NULL)
        return;

    WdiHaltAdapter(Adapter);

    KeAcquireSpinLock(&WdiGlobals.ListLock, &OldIrql);
    RemoveEntryList(&Adapter->Link);
    KeReleaseSpinLock(&WdiGlobals.ListLock, OldIrql);

    ExFreePoolWithTag(Adapter, WDI_TAG);
}

static
NDIS_STATUS
NTAPI
WdiMpSetOptions(
    _In_ NDIS_HANDLE NdisDriverHandle,
    _In_ NDIS_HANDLE DriverContext)
{
    PWDI_MINIPORT Miniport;

    Miniport = WdiFindMiniport(NULL, NdisDriverHandle);
    if (Miniport == NULL || Miniport->Ndis.SetOptionsHandler == NULL)
        return NDIS_STATUS_SUCCESS;

    return Miniport->Ndis.SetOptionsHandler(NdisDriverHandle, DriverContext);
}

/* Pause and restart run the IHV's post handlers and finish from a work item */

static
VOID
NTAPI
WdiStateWorker(
    _In_ PVOID WorkItemContext,
    _In_ NDIS_HANDLE NdisIoWorkItemHandle)
{
    PWDI_ADAPTER Adapter = WorkItemContext;
    PWDI_MINIPORT Miniport = Adapter->Miniport;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(NdisIoWorkItemHandle);

    if (Adapter->Pausing)
    {
        if (Miniport->Wdi.PostPauseHandler != NULL)
        {
            Status = Miniport->Wdi.PostPauseHandler(Adapter->MiniportAdapterContext, Adapter->PauseParameters);
            if (Status != NDIS_STATUS_SUCCESS)
                DPRINT1("PostPause failed (0x%x)\n", Status);
        }

        Adapter->PauseParameters = NULL;
        NdisMPauseComplete(Adapter->MiniportAdapterHandle);
        return;
    }

    if (Miniport->Wdi.PostRestartHandler != NULL)
    {
        Status = Miniport->Wdi.PostRestartHandler(Adapter->MiniportAdapterContext, Adapter->RestartParameters);
        if (Status != NDIS_STATUS_SUCCESS)
            DPRINT1("PostRestart failed (0x%x)\n", Status);
    }

    Adapter->RestartParameters = NULL;
    NdisMRestartComplete(Adapter->MiniportAdapterHandle, Status);
}

static
NDIS_STATUS
NTAPI
WdiMpPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL)
        return NDIS_STATUS_FAILURE;

    Adapter->Pausing = TRUE;
    Adapter->PauseParameters = PauseParameters;
    NdisQueueIoWorkItem(Adapter->StateWorkItem, WdiStateWorker, Adapter);
    return NDIS_STATUS_PENDING;
}

static
NDIS_STATUS
NTAPI
WdiMpRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL)
        return NDIS_STATUS_FAILURE;

    Adapter->Pausing = FALSE;
    Adapter->RestartParameters = RestartParameters;
    NdisQueueIoWorkItem(Adapter->StateWorkItem, WdiStateWorker, Adapter);
    return NDIS_STATUS_PENDING;
}

/* Requests from above; native 802.11 OIDs are not translated yet */

static
NDIS_STATUS
NTAPI
WdiMpOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL)
        return NDIS_STATUS_FAILURE;

    return WdiHandleOidRequest(Adapter, OidRequest);
}

static
VOID
NTAPI
WdiMpCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}

static
NDIS_STATUS
NTAPI
WdiMpDirectOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);

    DPRINT("Direct OID 0x%08lx not handled\n", OidRequest->DATA.QUERY_INFORMATION.Oid);
    return NDIS_STATUS_NOT_SUPPORTED;
}

static
VOID
NTAPI
WdiMpCancelDirectOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}

/* Data from above; nothing is sent until the data path is translated */

static
VOID
NTAPI
WdiMpSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    UNREFERENCED_PARAMETER(PortNumber);

    if (Adapter == NULL)
        return;

    WdiQueueSend(Adapter, NetBufferLists, SendFlags);
}

static
VOID
NTAPI
WdiMpReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(ReturnFlags);
}

static
VOID
NTAPI
WdiMpCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}

/* PnP, shutdown and reset go to the IHV */

static
VOID
NTAPI
WdiMpDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL)
        return;

    if (NetDevicePnPEvent->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved)
        Adapter->SurpriseRemoved = TRUE;

    if (Adapter->Miniport->Ndis.DevicePnPEventNotifyHandler != NULL)
        Adapter->Miniport->Ndis.DevicePnPEventNotifyHandler(Adapter->MiniportAdapterContext, NetDevicePnPEvent);
}

static
VOID
NTAPI
WdiMpShutdown(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL)
        return;

    /* No command goes down once the system is going away */
    InterlockedIncrement(&Adapter->ShutDown);

    if (Adapter->Miniport->Ndis.ShutdownHandlerEx != NULL)
        Adapter->Miniport->Ndis.ShutdownHandlerEx(Adapter->MiniportAdapterContext, ShutdownAction);
}

static
NDIS_STATUS
NTAPI
WdiMpReset(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _Out_ PBOOLEAN AddressingReset)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByContext(MiniportAdapterContext);

    if (Adapter == NULL || Adapter->Miniport->Ndis.ResetHandlerEx == NULL)
    {
        *AddressingReset = TRUE;
        return NDIS_STATUS_SUCCESS;
    }

    return Adapter->Miniport->Ndis.ResetHandlerEx(Adapter->MiniportAdapterContext, AddressingReset);
}

/* Completions the IHV reports through the WDI init parameters */

static
VOID
NTAPI
WdiOpenAdapterComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS CompletionStatus)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByHandle(MiniportAdapterHandle);

    if (Adapter == NULL)
        return;

    Adapter->OpenCloseStatus = CompletionStatus;
    KeSetEvent(&Adapter->OpenCloseDone, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
WdiCloseAdapterComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS CompletionStatus)
{
    PWDI_ADAPTER Adapter = WdiFindAdapterByHandle(MiniportAdapterHandle);

    if (Adapter == NULL)
        return;

    Adapter->OpenCloseStatus = CompletionStatus;
    KeSetEvent(&Adapter->OpenCloseDone, IO_NO_INCREMENT, FALSE);
}

/* Selective suspend is not used, the adapter stays in D0 */

static
VOID
NTAPI
WdiIdleNotificationConfirm(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_DEVICE_POWER_STATE DeviceIdlePowerState)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);

    DPRINT("Idle confirm for D%d ignored\n", DeviceIdlePowerState - 1);
}

static
VOID
NTAPI
WdiIdleNotificationComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);
}
