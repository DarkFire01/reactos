/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Miniport life cycle and status through the miniport core
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/* Packets the core lays over NET_BUFFERs, per adapter and direction */
#define CORE_PACKET_POOL_SIZE 512

/**
 * @brief
 * Sets up the core block of an adapter that was just created.
 *
 * @param[in] Adapter
 * The new adapter.
 */
VOID
NTAPI
CoreInitializeAdapterBlock(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;

    KeInitializeSpinLock(&Core->Lock);
    KeInitializeEvent(&Core->SendsDrained, NotificationEvent, TRUE);
    InitializeListHead(&Core->OidQueue);
    InitializeListHead(&Core->PortList);
    Core->State = CoreMiniportHalted;
}

/**
 * @brief
 * Records the general attributes of an adapter, as set by a 6.x miniport or
 * built by the NDIS 5 shim.
 *
 * @param[in] Adapter
 * The adapter being described.
 *
 * @param[in] General
 * The attributes.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or the reason the attributes cannot be used.
 */
NDIS_STATUS
NTAPI
CoreSetGeneralAttributes(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES General)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    PNDIS_OID OidList = NULL;

    if (General->MacAddressLength > NDIS_MAX_PHYS_ADDRESS_LENGTH)
        return NDIS_STATUS_INVALID_PARAMETER;

    /* Only Ethernet framing reaches the protocols in this tree */
    if (General->MediaType != NdisMedium802_3)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;

    if (Adapter->NdisMiniportBlock.DriverHandle->Ndis6Driver &&
        General->PhysicalMediumType == NdisPhysicalMediumWirelessLan)
    {
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (General->SupportedOidList != NULL && General->SupportedOidListLength != 0)
    {
        OidList = ExAllocatePoolWithTag(NonPagedPool, General->SupportedOidListLength, NDIS_TAG);
        if (OidList == NULL)
            return NDIS_STATUS_RESOURCES;

        RtlCopyMemory(OidList, General->SupportedOidList, General->SupportedOidListLength);
    }

    if (Core->SupportedOidList != NULL)
        ExFreePoolWithTag(Core->SupportedOidList, NDIS_TAG);

    Core->SupportedOidList = OidList;
    Core->SupportedOidListLength = (OidList != NULL) ? General->SupportedOidListLength : 0;

    Core->MediaType = General->MediaType;
    Core->PhysicalMediumType = General->PhysicalMediumType;
    Core->MtuSize = General->MtuSize;
    Core->MaxXmitLinkSpeed = General->MaxXmitLinkSpeed;
    Core->MaxRcvLinkSpeed = General->MaxRcvLinkSpeed;
    Core->MacOptions |= General->MacOptions;
    Core->SupportedPacketFilters = General->SupportedPacketFilters;
    Core->MaxMulticastListSize = General->MaxMulticastListSize;
    Core->SupportedStatistics = General->SupportedStatistics;
    Core->DataBackFillSize = General->DataBackFillSize;
    Core->ContextBackFillSize = General->ContextBackFillSize;
    Core->AccessType = General->AccessType;
    Core->ConnectionType = General->ConnectionType;

    /* Ethernet lookahead is capped at 512 */
    Core->LookaheadSize = min(General->LookaheadSize, 512);
    Core->CurrentLookahead = Core->LookaheadSize;

    Core->MacAddressLength = General->MacAddressLength;
    RtlCopyMemory(Core->PermanentMacAddress, General->PermanentMacAddress, General->MacAddressLength);
    RtlCopyMemory(Core->CurrentMacAddress, General->CurrentMacAddress, General->MacAddressLength);

    if (General->RecvScaleCapabilities != NULL)
    {
        RtlCopyMemory(&Core->RecvScaleCapabilities,
                      General->RecvScaleCapabilities,
                      sizeof(Core->RecvScaleCapabilities));
    }

    Core->LinkState.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Core->LinkState.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    Core->LinkState.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    Core->LinkState.MediaConnectState = General->MediaConnectState;
    Core->LinkState.MediaDuplexState = General->MediaDuplexState;
    Core->LinkState.XmitLinkSpeed = General->XmitLinkSpeed;
    Core->LinkState.RcvLinkSpeed = General->RcvLinkSpeed;
    Core->LinkState.AutoNegotiationFlags = General->AutoNegotiationFlags;

    Core->GeneralAttributesSet = TRUE;

    return NDIS_STATUS_SUCCESS;
}

/* Pause and restart */

/*
 * Wait for NdisMPauseComplete or NdisMRestartComplete when the handler pended,
 * or take the status it returned directly.
 */
static
NDIS_STATUS
CoreWaitForOperation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PKEVENT Event,
    _In_ NDIS_STATUS Status)
{
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
        return Adapter->Core.OperationStatus;
    }

    Adapter->Core.OperationEvent = NULL;
    return Status;
}

static
NDIS_STATUS
CoreRestart(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_MINIPORT_RESTART_PARAMETERS Parameters;
    NDIS_RESTART_GENERAL_ATTRIBUTES General;
    PNDIS_RESTART_ATTRIBUTES Attributes;
    ULONG AttributesSize;
    NDIS_STATUS Status;
    KEVENT Event;
    KIRQL OldIrql;

    if (Core->State != CoreMiniportPaused)
        return NDIS_STATUS_FAILURE;

    /* The general restart attributes ride in the Data area of one attribute entry */
    AttributesSize = FIELD_OFFSET(NDIS_RESTART_ATTRIBUTES, Data) + sizeof(General);
    Attributes = ExAllocatePoolWithTag(NonPagedPool, AttributesSize, NDIS_TAG);
    if (Attributes == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(&General, sizeof(General));
    General.Header.Type = NDIS_OBJECT_TYPE_RESTART_GENERAL_ATTRIBUTES;
    General.Header.Revision = NDIS_RESTART_GENERAL_ATTRIBUTES_REVISION_1;
    General.Header.Size = NDIS_SIZEOF_RESTART_GENERAL_ATTRIBUTES_REVISION_1;
    General.MtuSize = Core->MtuSize;
    General.MaxXmitLinkSpeed = Core->MaxXmitLinkSpeed;
    General.MaxRcvLinkSpeed = Core->MaxRcvLinkSpeed;
    General.LookaheadSize = Core->LookaheadSize;
    General.MacOptions = Core->MacOptions;
    General.SupportedPacketFilters = Core->SupportedPacketFilters;
    General.MaxMulticastListSize = Core->MaxMulticastListSize;
    General.AccessType = Core->AccessType;
    General.ConnectionType = Core->ConnectionType;
    General.SupportedStatistics = Core->SupportedStatistics;
    General.DataBackFillSize = Core->DataBackFillSize;
    General.ContextBackFillSize = Core->ContextBackFillSize;
    General.SupportedOidList = Core->SupportedOidList;
    General.SupportedOidListLength = Core->SupportedOidListLength;

    RtlZeroMemory(Attributes, AttributesSize);
    Attributes->Oid = OID_GEN_MINIPORT_RESTART_ATTRIBUTES;
    Attributes->DataLength = sizeof(General);
    RtlCopyMemory(Attributes->Data, &General, sizeof(General));

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NDIS_MINIPORT_RESTART_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_MINIPORT_RESTART_PARAMETERS_REVISION_1;
    Parameters.RestartAttributes = Attributes;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->OperationEvent = &Event;
    Core->State = CoreMiniportRestarting;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    Status = Core->Dispatch->RestartHandler(CORE_DISPATCH_CONTEXT(Adapter), &Parameters);
    Status = CoreWaitForOperation(Adapter, &Event, Status);

    ExFreePoolWithTag(Attributes, NDIS_TAG);

    /* A miniport that failed to restart stays paused and sends keep failing */
    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->State = (Status == NDIS_STATUS_SUCCESS) ? CoreMiniportRunning : CoreMiniportPaused;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (Status != NDIS_STATUS_SUCCESS)
        NDIS_DbgPrint(MIN_TRACE, ("MiniportRestart failed with 0x%x\n", Status));

    return Status;
}

static
VOID
CorePause(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_MINIPORT_PAUSE_PARAMETERS Parameters;
    NDIS_STATUS Status;
    KEVENT Event;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    if (Core->State != CoreMiniportRunning)
    {
        KeReleaseSpinLock(&Core->Lock, OldIrql);
        return;
    }

    /* From here on new sends are refused */
    Core->State = CoreMiniportPausing;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    /* Anything already handed to the miniport finishes before it is paused */
    CoreWaitForSends(Adapter);

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NDIS_MINIPORT_PAUSE_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_MINIPORT_PAUSE_PARAMETERS_REVISION_1;
    Parameters.PauseReason = NDIS_PAUSE_NDIS_INTERNAL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Core->OperationEvent = &Event;

    Status = Core->Dispatch->PauseHandler(CORE_DISPATCH_CONTEXT(Adapter), &Parameters);
    CoreWaitForOperation(Adapter, &Event, Status);

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->State = CoreMiniportPaused;
    KeReleaseSpinLock(&Core->Lock, OldIrql);
}

/**
 * @brief
 * Keeps an adapter's data path paused for a reason, pausing it if it runs.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Reason
 * One CORE_PAUSE_* bit.
 */
VOID
NTAPI
CoreHoldPaused(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG Reason)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->PauseReasons |= Reason;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    CorePause(Adapter);
}

/**
 * @brief
 * Drops one reason to keep an adapter paused, restarting it when none is left.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Reason
 * One CORE_PAUSE_* bit.
 *
 * @return
 * The restart's status, or NDIS_STATUS_SUCCESS when there was nothing to restart.
 */
NDIS_STATUS
NTAPI
CoreReleasePaused(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG Reason)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    BOOLEAN Restart;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->PauseReasons &= ~Reason;
    Restart = (Core->PauseReasons == 0 && Core->State == CoreMiniportPaused);
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (!Restart)
        return NDIS_STATUS_SUCCESS;

    return CoreRestart(Adapter);
}

/* Unexpected completions from 6.50 and later miniports are driver bugs */
static
VOID
CoreCheckUnexpectedCompletion(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG_PTR Which)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    if (Driver->Ndis6Driver &&
        (Driver->Characteristics6.MajorNdisVersion > 6 ||
         (Driver->Characteristics6.MajorNdisVersion == 6 && Driver->Characteristics6.MinorNdisVersion >= 50)))
    {
        KeBugCheckEx(BUGCODE_NDIS_DRIVER, 0x1F, (ULONG_PTR)Adapter, Which, 0);
    }

    NDIS_DbgPrint(MIN_TRACE, ("Completion with nothing pending, ignored.\n"));
}

static
VOID
CoreCompleteOperation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS Status,
    _In_ ULONG_PTR Which)
{
    PKEVENT Event;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->Core.Lock, &OldIrql);
    Event = Adapter->Core.OperationEvent;
    Adapter->Core.OperationEvent = NULL;
    Adapter->Core.OperationStatus = Status;
    KeReleaseSpinLock(&Adapter->Core.Lock, OldIrql);

    if (Event != NULL)
        KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    else
        CoreCheckUnexpectedCompletion(Adapter, Which);
}

/**
 * @brief
 * Completes a MiniportPause that returned NDIS_STATUS_PENDING.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter that finished pausing.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMPauseComplete(
    NDIS_HANDLE MiniportAdapterHandle)
{
    CoreCompleteOperation((PLOGICAL_ADAPTER)MiniportAdapterHandle, NDIS_STATUS_SUCCESS, 1);
}

/**
 * @brief
 * Completes a MiniportRestart that returned NDIS_STATUS_PENDING.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter that finished restarting.
 *
 * @param[in] Status
 * Whether the restart worked.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMRestartComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_STATUS Status)
{
    CoreCompleteOperation((PLOGICAL_ADAPTER)MiniportAdapterHandle, Status, 2);
}

/* Initialize, halt and shutdown */

static
NDIS_STATUS
CoreAllocatePools(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NET_BUFFER_LIST_POOL_PARAMETERS Parameters;
    NDIS_STATUS Status;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    Parameters.fAllocateNetBuffer = TRUE;
    Parameters.PoolTag = NDIS_TAG;

    Core->SendNblPool = NdisAllocateNetBufferListPool(Adapter, &Parameters);
    Core->ReceiveNblPool = NdisAllocateNetBufferListPool(Adapter, &Parameters);
    if (Core->SendNblPool == NULL || Core->ReceiveNblPool == NULL)
        return NDIS_STATUS_RESOURCES;

    /* Past the protocol's own reserved space sits the packet state */
    NdisAllocatePacketPoolEx(&Status,
                             &Core->ReceivePacketPool,
                             CORE_PACKET_POOL_SIZE,
                             CORE_PACKET_POOL_SIZE,
                             PROTOCOL_RESERVED_SIZE_IN_PACKET + CORE_PACKET_STATE_SIZE);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    NdisAllocatePacketPoolEx(&Status,
                             &Core->SendPacketPool,
                             CORE_PACKET_POOL_SIZE,
                             CORE_PACKET_POOL_SIZE,
                             PROTOCOL_RESERVED_SIZE_IN_PACKET + CORE_PACKET_STATE_SIZE);
    return Status;
}

static
VOID
CoreFreeResources(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;

    if (Core->SendNblPool != NULL)
    {
        NdisFreeNetBufferListPool(Core->SendNblPool);
        Core->SendNblPool = NULL;
    }

    if (Core->ReceiveNblPool != NULL)
    {
        NdisFreeNetBufferListPool(Core->ReceiveNblPool);
        Core->ReceiveNblPool = NULL;
    }

    if (Core->ReceivePacketPool != NULL)
    {
        NdisFreePacketPool(Core->ReceivePacketPool);
        Core->ReceivePacketPool = NULL;
    }

    if (Core->SendPacketPool != NULL)
    {
        NdisFreePacketPool(Core->SendPacketPool);
        Core->SendPacketPool = NULL;
    }

    if (Core->SupportedOidList != NULL)
    {
        ExFreePoolWithTag(Core->SupportedOidList, NDIS_TAG);
        Core->SupportedOidList = NULL;
        Core->SupportedOidListLength = 0;
    }

    CoreFreePorts(Adapter);

    Core->GeneralAttributesSet = FALSE;
}

static
NDIS_STATUS
CoreCompleteInitialization(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_STATUS Status;

    if (!Core->GeneralAttributesSet)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Miniport set no general attributes.\n"));
        return NDIS_STATUS_FAILURE;
    }

    /* The NDIS 5 edges read these out of the miniport block */
    Adapter->NdisMiniportBlock.MediaType = Core->MediaType;
    Adapter->NdisMiniportBlock.MacOptions = Core->MacOptions;
    Adapter->NdisMiniportBlock.MaximumLookahead = Core->LookaheadSize;
    Adapter->NdisMiniportBlock.CurrentLookahead = Core->CurrentLookahead;
    Adapter->MediumHeaderSize = 14;
    Adapter->AddressLength = ETH_LENGTH_OF_ADDRESS;
    RtlCopyMemory(Adapter->Address.Type.Medium802_3,
                  Core->CurrentMacAddress,
                  min(Core->MacAddressLength, ETH_LENGTH_OF_ADDRESS));

    if (!EthCreateFilter(Core->MaxMulticastListSize,
                         Adapter->Address.Type.Medium802_3,
                         &Adapter->NdisMiniportBlock.EthDB))
    {
        return NDIS_STATUS_RESOURCES;
    }
    ((PETHI_FILTER)Adapter->NdisMiniportBlock.EthDB)->Miniport = (PNDIS_MINIPORT_BLOCK)Adapter;

    Status = CoreAllocatePools(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    /* Initialization leaves every miniport paused */
    Core->State = CoreMiniportPaused;

    if (Core->PauseReasons != 0)
        return NDIS_STATUS_SUCCESS;

    return CoreRestart(Adapter);
}

/**
 * @brief
 * Initializes an adapter through its miniport and brings it to running.
 *
 * @param[in] Adapter
 * The adapter being started.
 *
 * @param[in] WrapperContext
 * The configuration context an NDIS 5 miniport's MiniportInitialize gets.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why the adapter did not come up.
 */
NDIS_STATUS
NTAPI
CoreInitializeAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_WRAPPER_CONTEXT WrapperContext)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_MINIPORT_INIT_PARAMETERS InitParameters;
    NDIS_STATUS Status;

    Core->Dispatch = Driver->Ndis6Driver ? &Driver->Characteristics6 : &Mp5ShimCharacteristics;
    Core->WrapperContext = WrapperContext;
    Core->State = CoreMiniportInitializing;

    RtlZeroMemory(&InitParameters, sizeof(InitParameters));
    InitParameters.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_INIT_PARAMETERS;
    InitParameters.Header.Revision = NDIS_MINIPORT_INIT_PARAMETERS_REVISION_1;
    InitParameters.Header.Size = NDIS_SIZEOF_MINIPORT_INIT_PARAMETERS_REVISION_1;
    InitParameters.MiniportAddDeviceContext = Core->AddDeviceContext;

    /* NDIS 6 hands over the partial list, which sits inside the full one */
    if (Adapter->NdisMiniportBlock.AllocatedResources != NULL)
    {
        InitParameters.AllocatedResources =
            &Adapter->NdisMiniportBlock.AllocatedResources->List[0].PartialResourceList;
    }

    Status = Core->Dispatch->InitializeHandlerEx(
        (NDIS_HANDLE)Adapter,
        Driver->Ndis6Driver ? Driver->MiniportDriverContext : (NDIS_HANDLE)Driver,
        &InitParameters);

    Core->WrapperContext = NULL;

    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Miniport initialization failed (0x%x).\n", Status));
        Core->State = CoreMiniportHalted;
        CoreFreeResources(Adapter);
        return Status;
    }

    Status = CoreCompleteInitialization(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        /* Paused or never restarted, the miniport still needs its halt */
        Core->Dispatch->HaltHandlerEx(CORE_DISPATCH_CONTEXT(Adapter), NdisHaltDeviceInitializationFailed);
        Core->State = CoreMiniportHalted;
        CoreFreeResources(Adapter);
    }

    return Status;
}

/**
 * @brief
 * Pauses an adapter and halts its miniport.
 *
 * @param[in] Adapter
 * The adapter going away.
 *
 * @param[in] HaltAction
 * Why, as passed on to MiniportHaltEx.
 */
VOID
NTAPI
CoreHaltAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PMINIPORT_CORE Core = &Adapter->Core;

    if (Core->State == CoreMiniportHalted)
        return;

    CorePause(Adapter);

    Core->Dispatch->HaltHandlerEx(CORE_DISPATCH_CONTEXT(Adapter), HaltAction);

    Core->State = CoreMiniportHalted;
    CoreFreeResources(Adapter);
}

/**
 * @brief
 * Tells a miniport the system is going down.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] ShutdownAction
 * Power off or bug check.
 */
VOID
NTAPI
CoreShutdownAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    if (Adapter->Core.State == CoreMiniportHalted || Adapter->Core.Dispatch == NULL)
        return;

    Adapter->Core.Dispatch->ShutdownHandlerEx(CORE_DISPATCH_CONTEXT(Adapter), ShutdownAction);
}

/* Status */

/**
 * @brief
 * The one path every status indication takes up from a miniport.
 *
 * NDIS 5 protocols only learn about link changes through media connect and
 * disconnect, and only on an actual transition. Statuses that only mean
 * something to NDIS 6 stop here.
 *
 * @param[in] Adapter
 * The indicating adapter.
 *
 * @param[in] StatusIndication
 * The status code and its buffer.
 */
VOID
NTAPI
CoreIndicateStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PNDIS_LINK_STATE LinkState;
    NET_IF_MEDIA_CONNECT_STATE Previous;

    if (StatusIndication->StatusCode != NDIS_STATUS_LINK_STATE)
    {
        /* The NDIS 5 status codes all sit below the first NDIS 6 one */
        if (StatusIndication->StatusCode < NDIS_STATUS_LINK_STATE)
        {
            Pro5IndicateStatus(Adapter,
                               StatusIndication->StatusCode,
                               StatusIndication->StatusBuffer,
                               StatusIndication->StatusBufferSize);
        }
        return;
    }

    if (StatusIndication->StatusBuffer == NULL ||
        StatusIndication->StatusBufferSize < NDIS_SIZEOF_LINK_STATE_REVISION_1)
    {
        return;
    }

    LinkState = (PNDIS_LINK_STATE)StatusIndication->StatusBuffer;
    Previous = Adapter->Core.LinkState.MediaConnectState;

    Adapter->Core.LinkState.MediaConnectState = LinkState->MediaConnectState;
    Adapter->Core.LinkState.MediaDuplexState = LinkState->MediaDuplexState;
    Adapter->Core.LinkState.XmitLinkSpeed = LinkState->XmitLinkSpeed;
    Adapter->Core.LinkState.RcvLinkSpeed = LinkState->RcvLinkSpeed;
    Adapter->Core.LinkState.PauseFunctions = LinkState->PauseFunctions;
    Adapter->Core.LinkState.AutoNegotiationFlags = LinkState->AutoNegotiationFlags;

    if (LinkState->MediaConnectState == MediaConnectStateConnected &&
        Previous != MediaConnectStateConnected)
    {
        Pro5IndicateStatus(Adapter, NDIS_STATUS_MEDIA_CONNECT, NULL, 0);
    }
    else if (LinkState->MediaConnectState == MediaConnectStateDisconnected &&
             Previous == MediaConnectStateConnected)
    {
        Pro5IndicateStatus(Adapter, NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
    }
}

/**
 * @brief
 * Indicates a status of NDIS's own that carries no data, such as the start
 * and end of a reset.
 *
 * @param[in] Adapter
 * The adapter the status is about.
 *
 * @param[in] StatusCode
 * The status.
 */
VOID
NTAPI
CoreIndicateStatusCode(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS StatusCode)
{
    NDIS_STATUS_INDICATION Indication;

    RtlZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    Indication.SourceHandle = Adapter;
    Indication.PortNumber = NDIS_DEFAULT_PORT_NUMBER;
    Indication.StatusCode = StatusCode;

    CoreIndicateStatus(Adapter, &Indication);
}


/* Reset and hang detection */

/**
 * @brief
 * Resets a miniport through whichever reset handler it has.
 *
 * @param[in] Adapter
 * The adapter to reset.
 *
 * @param[out] AddressingReset
 * Set when NDIS has to restore the addressing state afterwards.
 *
 * @return
 * The miniport's reset status.
 */
NDIS_STATUS
NTAPI
MiniCallResetHandler(
    _In_ PLOGICAL_ADAPTER Adapter,
    _Out_ PBOOLEAN AddressingReset)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    if (Driver->Ndis6Driver)
    {
        *AddressingReset = FALSE;

        if (Driver->Characteristics6.ResetHandlerEx == NULL)
            return NDIS_STATUS_NOT_SUPPORTED;

        return Driver->Characteristics6.ResetHandlerEx(
            Adapter->NdisMiniportBlock.MiniportAdapterContext,
            AddressingReset);
    }

    *AddressingReset = TRUE;
    return Driver->MiniportCharacteristics.ResetHandler(
        AddressingReset,
        Adapter->NdisMiniportBlock.MiniportAdapterContext);
}

/**
 * @brief
 * Asks a miniport whether it is hung, through whichever handler it has.
 *
 * @param[in] Adapter
 * The adapter to check.
 *
 * @return
 * TRUE if the miniport reports it is hung.
 */
BOOLEAN
NTAPI
MiniCallCheckForHangHandler(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    if (Adapter->Core.State != CoreMiniportRunning)
        return FALSE;

    if (Driver->Ndis6Driver)
    {
        if (Driver->Characteristics6.CheckForHangHandlerEx == NULL)
            return FALSE;

        return Driver->Characteristics6.CheckForHangHandlerEx(
            Adapter->NdisMiniportBlock.MiniportAdapterContext);
    }

    if (Driver->MiniportCharacteristics.CheckForHangHandler == NULL)
        return FALSE;

    return Driver->MiniportCharacteristics.CheckForHangHandler(
        Adapter->NdisMiniportBlock.MiniportAdapterContext);
}

/**
 * @brief
 * Lets a miniport ask for its own reset.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter to reset.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMResetMiniport(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;

    /* The reset runs later from the adapter's work queue */
    MiniQueueWorkItem(Adapter, NdisWorkItemResetRequested, NULL, FALSE);
    MiniWorkItemComplete(Adapter, NdisWorkItemResetRequested);
}
