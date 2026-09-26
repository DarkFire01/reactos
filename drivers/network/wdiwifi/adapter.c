/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bringing a WLAN adapter up through WDI and taking it down again
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "wdiwifi.h"
#include <wditypes.h>

#define NDEBUG
#include <debug.h>

/* Adapter registry keywords, with their defaults and limits */
typedef struct _WDI_KNOB
{
    PCWSTR Keyword;
    ULONG Default;
    ULONG Minimum;
    ULONG Maximum;
    SIZE_T Offset;
    BOOLEAN IsBoolean;
} WDI_KNOB;

static const WDI_KNOB WdiKnobs[] =
{
    { L"TaskTimeout", 30000, 10000, 60000, FIELD_OFFSET(WDI_ADAPTER, TaskTimeout), FALSE },
    { L"CommandTimeout", 10000, 250, 30000, FIELD_OFFSET(WDI_ADAPTER, CommandTimeout), FALSE },
    { L"UnreachableThreshold", 3000, 500, 10000, FIELD_OFFSET(WDI_ADAPTER, UnreachableThreshold), FALSE },
    { L"SoftwareRadioOff", 0, 0, 1, FIELD_OFFSET(WDI_ADAPTER, SoftwareRadioOff), TRUE }
};

static
NDIS_STATUS
WdiOpenConfiguration(
    _In_ PWDI_ADAPTER Adapter,
    _Out_ PNDIS_HANDLE ConfigurationHandle)
{
    NDIS_CONFIGURATION_OBJECT Object;

    RtlZeroMemory(&Object, sizeof(Object));
    Object.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    Object.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    Object.Header.Size = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    Object.NdisHandle = Adapter->MiniportAdapterHandle;

    return NdisOpenConfigurationEx(&Object, ConfigurationHandle);
}

static
VOID
WdiReadKnobs(
    _In_ PWDI_ADAPTER Adapter)
{
    PNDIS_CONFIGURATION_PARAMETER Parameter;
    NDIS_HANDLE Configuration = NULL;
    NDIS_STRING Keyword;
    NDIS_STATUS Status;
    ULONG Value;
    ULONG i;

    if (WdiOpenConfiguration(Adapter, &Configuration) != NDIS_STATUS_SUCCESS)
        Configuration = NULL;

    for (i = 0; i < RTL_NUMBER_OF(WdiKnobs); i++)
    {
        Value = WdiKnobs[i].Default;

        if (Configuration != NULL)
        {
            RtlInitUnicodeString(&Keyword, WdiKnobs[i].Keyword);
            NdisReadConfiguration(&Status, &Parameter, Configuration, &Keyword, NdisParameterHexInteger);
            if (Status == NDIS_STATUS_SUCCESS &&
                Parameter->ParameterData.IntegerData >= WdiKnobs[i].Minimum &&
                Parameter->ParameterData.IntegerData <= WdiKnobs[i].Maximum)
            {
                Value = Parameter->ParameterData.IntegerData;
            }
        }

        if (WdiKnobs[i].IsBoolean)
            *(PBOOLEAN)((PUCHAR)Adapter + WdiKnobs[i].Offset) = (BOOLEAN)Value;
        else
            *(PULONG)((PUCHAR)Adapter + WdiKnobs[i].Offset) = Value;
    }

    if (Configuration != NULL)
        NdisCloseConfiguration(Configuration);
}

/* A NetworkAddress override only counts when it is a locally administered unicast address */
static
BOOLEAN
WdiReadConfiguredAddress(
    _In_ PWDI_ADAPTER Adapter,
    _Out_ PWDI_MAC_ADDRESS Address)
{
    static const UCHAR Zero[6] = { 0 };
    NDIS_HANDLE Configuration;
    NDIS_STATUS Status;
    PUCHAR NetworkAddress;
    UINT Length;
    BOOLEAN Valid = FALSE;

    if (WdiOpenConfiguration(Adapter, &Configuration) != NDIS_STATUS_SUCCESS)
        return FALSE;

    NdisReadNetworkAddress(&Status, (PVOID *)&NetworkAddress, &Length, Configuration);
    if (Status == NDIS_STATUS_SUCCESS &&
        Length == sizeof(Address->Address) &&
        (NetworkAddress[0] & 0x03) == 0x02 &&
        RtlCompareMemory(NetworkAddress, Zero, sizeof(Zero)) != sizeof(Zero))
    {
        RtlCopyMemory(Address->Address, NetworkAddress, sizeof(Address->Address));
        Valid = TRUE;
    }

    NdisCloseConfiguration(Configuration);
    return Valid;
}

/* Bring-up steps */

static
NDIS_STATUS
WdiAllocate(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS InitParameters)
{
    PWDI_MINIPORT Miniport = Adapter->Miniport;
    NDIS_MINIPORT_ADAPTER_ATTRIBUTES Attributes;
    NDIS_STATUS Status;

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.RegistrationAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    Attributes.RegistrationAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Attributes.RegistrationAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Attributes.RegistrationAttributes.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_PAUSE_ON_SUSPEND;
    Attributes.RegistrationAttributes.InterfaceType = NdisInterfaceInternal;

    /* The IHV is told the newest version both sides know, never older than the floor */
    if (Miniport->Wdi.WdiVersion > WDI_UPPER_EDGE_FLOOR_VERSION)
        Miniport->InitParameters.WdiVersion = min(Miniport->Wdi.WdiVersion, WDI_UPPER_EDGE_VERSION);
    else
        Miniport->InitParameters.WdiVersion = WDI_UPPER_EDGE_FLOOR_VERSION;

    Status = Miniport->Wdi.AllocateAdapterHandler(Adapter->MiniportAdapterHandle,
                                                  Miniport->OwnDriverContext ? NULL : Miniport->DriverContext,
                                                  InitParameters,
                                                  &Miniport->InitParameters,
                                                  &Attributes.RegistrationAttributes);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AllocateAdapter failed (0x%x)\n", Status);
        return Status;
    }

    Status = NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle, &Attributes);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("The IHV's registration attributes were refused (0x%x)\n", Status);
        return Status;
    }

    Adapter->MiniportAdapterContext = Attributes.RegistrationAttributes.MiniportAdapterContext;
    Adapter->Progress |= WDI_PROGRESS_ALLOCATED;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
WdiOpen(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS InitParameters)
{
    NDIS_STATUS Status;

    /* Success means pending; the IHV reports the outcome through OpenAdapterComplete */
    KeClearEvent(&Adapter->OpenCloseDone);
    Status = Adapter->Miniport->Wdi.OpenAdapterHandler(Adapter->MiniportAdapterContext, InitParameters);
    if (Status == NDIS_STATUS_SUCCESS)
    {
        KeWaitForSingleObject(&Adapter->OpenCloseDone, Executive, KernelMode, FALSE, NULL);
        Status = Adapter->OpenCloseStatus;
    }

    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("OpenAdapter failed (0x%x)\n", Status);
        return Status;
    }

    Adapter->Progress |= WDI_PROGRESS_OPENED;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
WdiInitializeDataPath(
    _In_ PWDI_ADAPTER Adapter)
{
    PNDIS_MINIPORT_WDI_DATA_HANDLERS Handlers = &Adapter->DataHandlers;
    NDIS_STATUS Status;

    RtlZeroMemory(Handlers, sizeof(*Handlers));
    Handlers->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_WDI_DATA_HANDLERS;
    Handlers->Header.Revision = NDIS_OBJECT_TYPE_MINIPORT_WDI_DATA_HANDLERS_REVISION_2;
    Handlers->Header.Size = NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_2;

    Status = Adapter->Miniport->Wdi.TalTxRxInitializeHandler(Adapter->MiniportAdapterContext,
                                                              Adapter,
                                                              &Adapter->DataApi,
                                                              &Adapter->TalTxRx,
                                                              Handlers,
                                                              &Adapter->FrameExtraSpace);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("TalTxRxInitialize failed (0x%x)\n", Status);
        return Status;
    }

    if (Handlers->Header.Type != NDIS_OBJECT_TYPE_MINIPORT_WDI_DATA_HANDLERS ||
        Handlers->Header.Revision == 0 ||
        Handlers->Header.Size < NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_1)
    {
        DPRINT1("Data handlers type 0x%x revision %u size %u\n",
                Handlers->Header.Type, Handlers->Header.Revision, Handlers->Header.Size);
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    /* Revision 2 came with WDI 1.1 */
    if ((Adapter->PeerVersion <= WDI_VERSION_1_1_0) != (Handlers->Header.Revision < 2))
    {
        DPRINT1("Data handlers revision %u does not fit WDI 0x%lx\n",
                Handlers->Header.Revision, Adapter->PeerVersion);
    }

    Adapter->Progress |= WDI_PROGRESS_DATAPATH_READY;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
WdiQueryCapabilities(
    _In_ PWDI_ADAPTER Adapter)
{
    WDI_MESSAGE Response;
    NDIS_STATUS Status;

    Status = WdiSendCommand(Adapter, WDI_GET_ADAPTER_CAPABILITIES, WDI_PORT_ID_ADAPTER, NULL, 0, FALSE, &Response);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        WdiFreeMessage(&Response);
        return Status;
    }

    Status = WdiParseCapabilities(Adapter->PeerVersion,
                                  Response.Buffer + sizeof(WDI_MESSAGE_HEADER),
                                  Response.Length - sizeof(WDI_MESSAGE_HEADER),
                                  &Adapter->Caps);
    WdiFreeMessage(&Response);
    return Status;
}

static
NDIS_STATUS
WdiConfigure(
    _In_ PWDI_ADAPTER Adapter)
{
    WDI_MAC_ADDRESS Address;
    PCWDI_MAC_ADDRESS Configured = NULL;
    NDIS_STATUS Status;
    PUCHAR Tlvs;
    ULONG Length;

    if (WdiReadConfiguredAddress(Adapter, &Address))
        Configured = &Address;

    Length = WdiBuildAdapterConfiguration(Adapter, Configured, NULL);
    Tlvs = ExAllocatePoolWithTag(PagedPool, Length, WDI_TAG);
    if (Tlvs == NULL)
        return NDIS_STATUS_RESOURCES;

    WdiBuildAdapterConfiguration(Adapter, Configured, Tlvs);
    Status = WdiSendCommand(Adapter, WDI_SET_ADAPTER_CONFIGURATION, WDI_PORT_ID_ADAPTER, Tlvs, Length, FALSE, NULL);

    ExFreePoolWithTag(Tlvs, WDI_TAG);
    return Status;
}

static
NDIS_STATUS
WdiStartDataPath(
    _In_ PWDI_ADAPTER Adapter)
{
    WDI_TXRX_TARGET_CONFIGURATION Target;
    TAL_TXRX_PARAMETERS Parameters;
    PWDI_CAPABILITIES Caps = &Adapter->Caps;
    NDIS_STATUS Status;

    Status = WdiCreateFrameLookaside(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    /* The port count is left for the IHV to decide */
    RtlZeroMemory(&Target, sizeof(Target));
    Target.TxRxParams.TxRxCapabilities.InterconnectType = Caps->InterconnectType;
    Target.TxRxParams.TxRxCapabilities.TransmitCapabilities.TargetPriorityQueueing = Caps->TxTargetPriorityQueueing;
    Target.TxRxParams.TxRxCapabilities.TransmitCapabilities.MaxScatterGatherElementsPerFrame = Caps->TxMaxScatterGatherElements;
    Target.TxRxParams.TxRxCapabilities.TransmitCapabilities.ExplicitSendCompleteFlagRequired = Caps->TxExplicitSendComplete;
    Target.TxRxParams.TxRxCapabilities.TransmitCapabilities.MinEffectiveSize = Caps->TxMinEffectiveFrameSize;
    Target.TxRxParams.TxRxCapabilities.TransmitCapabilities.FrameSizeGranularity = Caps->TxFrameSizeGranularity;
    Target.TxRxParams.TxRxCapabilities.ReceiveCapabilities.RxTxForwarding = Caps->RxTxForwarding;
    Target.TxRxParams.TxRxCapabilities.ReceiveCapabilities.MaxThroughput = Caps->RxMaxThroughput;
    Target.MaxNumPeers = Caps->MaxNumPeers;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Status = Adapter->DataHandlers.TalTxRxStartHandler(Adapter->TalTxRx, &Target, &Parameters);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("TalTxRxStart failed (0x%x)\n", Status);
        WdiDeleteFrameLookaside(Adapter);
        return Status;
    }

    Adapter->MaxOutstandingTransfers = Parameters.MaxOutstandingTransfers;
    Adapter->Progress |= WDI_PROGRESS_DATAPATH_STARTED;
    return NDIS_STATUS_SUCCESS;
}

/* A fresh port starts from its default MIB, which the IHV applies on a dot11 reset */
static
VOID
WdiResetPort(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PWDI_PORT Port)
{
    UCHAR Tlvs[8];
    UCHAR SetDefaultMib = TRUE;
    ULONG Length;

    Length = WdiTlvPut(Tlvs, WDI_TLV_DOT11_RESET_PARAMETERS, &SetDefaultMib, sizeof(SetDefaultMib));
    if (WdiSendCommand(Adapter, WDI_TASK_DOT11_RESET, Port->PortId, Tlvs, Length, TRUE, NULL) != NDIS_STATUS_SUCCESS)
        DPRINT1("Resetting port %u failed, going on\n", Port->PortId);
}

static
NDIS_STATUS
WdiCreatePort(
    _In_ PWDI_ADAPTER Adapter,
    _In_ UINT16 OpModeMask,
    _In_ NDIS_PORT_NUMBER NdisPortNumber)
{
    WDI_MESSAGE Completion;
    WDI_MAC_ADDRESS Address;
    WDI_PORT_ID PortId;
    NDIS_STATUS Status;
    PWDI_PORT Port = NULL;
    UCHAR Tlvs[16];
    ULONG Length;
    ULONG i;

    Length = WdiBuildCreatePort(OpModeMask, NdisPortNumber, Tlvs);
    Status = WdiSendCommand(Adapter, WDI_TASK_CREATE_PORT, WDI_PORT_ID_ADAPTER, Tlvs, Length, TRUE, &Completion);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        WdiFreeMessage(&Completion);
        return Status;
    }

    Status = WdiParsePortAttributes(Completion.Buffer + sizeof(WDI_MESSAGE_HEADER),
                                    Completion.Length - sizeof(WDI_MESSAGE_HEADER),
                                    &Address,
                                    &PortId);
    WdiFreeMessage(&Completion);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("Port creation reported no port attributes\n");
        return Status;
    }

    for (i = 0; i < RTL_NUMBER_OF(Adapter->Ports); i++)
    {
        if (!Adapter->Ports[i].InUse)
        {
            Port = &Adapter->Ports[i];
            break;
        }
    }

    if (Port == NULL)
        return NDIS_STATUS_RESOURCES;

    Port->InUse = TRUE;
    Port->PortId = PortId;
    Port->NdisPortNumber = NdisPortNumber;
    Port->OpModeMask = OpModeMask;
    Port->Address = Address;

    Adapter->DataHandlers.TalTxRxAddPortHandler(Adapter->TalTxRx, PortId, (WDI_OPERATION_MODE)OpModeMask);

    DPRINT1("Port %u for NDIS port %lu at %02x:%02x:%02x:%02x:%02x:%02x\n",
            PortId, NdisPortNumber,
            Address.Address[0], Address.Address[1], Address.Address[2],
            Address.Address[3], Address.Address[4], Address.Address[5]);

    WdiResetPort(Adapter, Port);
    return NDIS_STATUS_SUCCESS;
}

static
VOID
WdiDeletePort(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PWDI_PORT Port)
{
    UCHAR Tlvs[8];
    ULONG Length;

    Length = WdiTlvPut(Tlvs, WDI_TLV_DELETE_PORT_PARAMETERS, &Port->PortId, sizeof(Port->PortId));
    if (WdiSendCommand(Adapter, WDI_TASK_DELETE_PORT, WDI_PORT_ID_ADAPTER, Tlvs, Length, TRUE, NULL) != NDIS_STATUS_SUCCESS)
        DPRINT1("Deleting port %u failed\n", Port->PortId);

    Adapter->DataHandlers.TalTxRxDeletePortHandler(Adapter->TalTxRx, Port->PortId);
    Port->InUse = FALSE;
}

/* The radio ends up the way the SoftwareRadioOff keyword asks */
static
VOID
WdiApplyRadioState(
    _In_ PWDI_ADAPTER Adapter)
{
    UCHAR Tlvs[8];
    UCHAR RadioOn;
    ULONG Length;

    if (Adapter->Caps.SoftwareRadioOn != Adapter->SoftwareRadioOff)
        return;

    RadioOn = !Adapter->SoftwareRadioOff;
    Length = WdiTlvPut(Tlvs, WDI_TLV_RADIO_STATE_PARAMETERS, &RadioOn, sizeof(RadioOn));
    if (WdiSendCommand(Adapter, WDI_TASK_SET_RADIO_STATE, WDI_PORT_ID_ADAPTER, Tlvs, Length, TRUE, NULL) != NDIS_STATUS_SUCCESS)
        DPRINT1("Turning the radio %s failed, going on\n", RadioOn ? "on" : "off");
}

static
NDIS_STATUS
WdiSetGeneralAttributes(
    _In_ PWDI_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_ATTRIBUTES Attributes;
    PNDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES General = &Attributes.GeneralAttributes;
    PWDI_CAPABILITIES Caps = &Adapter->Caps;
    ULONG i;

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    General->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    General->Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    General->Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;

    General->MediaType = NdisMediumNative802_11;
    General->PhysicalMediumType = NdisPhysicalMediumNative802_11;
    General->MtuSize = Caps->MtuSize;
    General->MaxXmitLinkSpeed = 1000ULL * Caps->MaxTxRate;
    General->XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    General->MaxRcvLinkSpeed = 1000ULL * Caps->MaxRxRate;
    General->RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;

    /* The link is down until the 802.11 side associates and says otherwise */
    General->MediaConnectState = MediaConnectStateDisconnected;
    General->MediaDuplexState = MediaDuplexStateFull;
    General->LookaheadSize = 220;
    General->MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                          NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                          NDIS_MAC_OPTION_NO_LOOPBACK;
    General->SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED |
                                      NDIS_PACKET_TYPE_MULTICAST |
                                      NDIS_PACKET_TYPE_ALL_MULTICAST |
                                      NDIS_PACKET_TYPE_BROADCAST;
    General->MaxMulticastListSize = Caps->MaxMulticastListSize;

    RtlCopyMemory(General->PermanentMacAddress, Caps->PermanentAddress.Address, sizeof(Caps->PermanentAddress.Address));
    for (i = 0; i < RTL_NUMBER_OF(Adapter->Ports); i++)
    {
        if (Adapter->Ports[i].InUse && Adapter->Ports[i].NdisPortNumber == NDIS_DEFAULT_PORT_NUMBER)
        {
            RtlCopyMemory(General->CurrentMacAddress,
                          Adapter->Ports[i].Address.Address,
                          sizeof(Adapter->Ports[i].Address.Address));
            General->MacAddressLength = sizeof(Adapter->Ports[i].Address.Address);
            break;
        }
    }

    General->AccessType = NET_IF_ACCESS_BROADCAST;
    General->DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    General->ConnectionType = NET_IF_CONNECTION_DEDICATED;
    General->IfType = IF_TYPE_IEEE80211;
    General->IfConnectorPresent = TRUE;
    General->DataBackFillSize = Caps->BackFillSize;
    General->ContextBackFillSize = Adapter->FrameSize;

    return NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle, &Attributes);
}

static
NDIS_STATUS
WdiStartOperation(
    _In_ PWDI_ADAPTER Adapter)
{
    MINIPORT_WDI_START_OPERATION_HANDLER StartOperation = Adapter->Miniport->Wdi.StartOperationHandler;
    NDIS_STATUS Status;

    if (StartOperation == NULL)
        return NDIS_STATUS_SUCCESS;

    /* Halt stops the IHV even when starting it failed */
    Status = StartOperation(Adapter->MiniportAdapterContext);
    Adapter->Progress |= WDI_PROGRESS_OPERATING;
    return Status;
}

static
VOID
WdiLogInitFailure(
    _In_ PWDI_ADAPTER Adapter,
    _In_ WDI_INIT_STEP Step,
    _In_ NDIS_STATUS Status)
{
    ULONG ErrorCode;

    switch (Step)
    {
        case WdiInitNotStarted:     ErrorCode = NDIS_ERROR_CODE_OUT_OF_RESOURCES; break;
        case WdiInitAllocate:       ErrorCode = NDIS_ERROR_CODE_ADAPTER_NOT_FOUND; break;
        case WdiInitOpen:           ErrorCode = NDIS_ERROR_CODE_BAD_VERSION; break;
        case WdiInitCapabilities:   ErrorCode = NDIS_ERROR_CODE_INVALID_VALUE_FROM_ADAPTER; break;
        case WdiInitConfiguration:  ErrorCode = NDIS_ERROR_CODE_NETWORK_ADDRESS; break;
        case WdiInitDataPath:       ErrorCode = NDIS_ERROR_CODE_RECEIVE_SPACE_SMALL; break;
        case WdiInitCreatePort:     ErrorCode = NDIS_ERROR_CODE_RESOURCE_CONFLICT; break;
        case WdiInitRadioState:     ErrorCode = NDIS_ERROR_CODE_MISSING_CONFIGURATION_PARAMETER; break;
        case WdiInitAttributes:     ErrorCode = NDIS_ERROR_CODE_UNSUPPORTED_CONFIGURATION; break;
        case WdiInitStartOperation: ErrorCode = NDIS_ERROR_CODE_HARDWARE_FAILURE; break;
        default:                    ErrorCode = NDIS_ERROR_CODE_DRIVER_FAILURE; break;
    }

    DPRINT1("WDI bring-up failed at step %d (0x%x)\n", Step, Status);
    NdisWriteErrorLogEntry(Adapter->MiniportAdapterHandle, ErrorCode, 2, Step | 0x30000, Status);
}

/**
 * @brief
 * Brings a WLAN adapter up: the IHV allocates and opens it, its data path is
 * initialized, capabilities are read and configuration set, the data path
 * starts, the station port is created, and the adapter describes itself to
 * NDIS before the IHV starts operating.
 *
 * @param[in] Adapter
 * The adapter, linked into the adapter list.
 *
 * @param[in] InitParameters
 * What NDIS passed MiniportInitializeEx.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or the failure of the step that stopped it. Whatever
 * was brought up by then has been taken down again.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiInitializeAdapter(
    PWDI_ADAPTER Adapter,
    PNDIS_MINIPORT_INIT_PARAMETERS InitParameters)
{
    WDI_INIT_STEP Step = WdiInitNotStarted;
    NDIS_STATUS Status;

    PAGED_CODE();

    WdiInitializeCommands(Adapter);
    WdiInitializeSendQueue(Adapter);
    KeInitializeEvent(&Adapter->OpenCloseDone, NotificationEvent, FALSE);
    KeInitializeEvent(&Adapter->ScanIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&Adapter->ConnectIdle, NotificationEvent, TRUE);
    KeInitializeSpinLock(&Adapter->BssLock);
    KeInitializeSpinLock(&Adapter->AssocLock);

    /* An open network until the dot11 OIDs ask for something else */
    Adapter->DesiredAuth = DOT11_AUTH_ALGO_80211_OPEN;
    Adapter->DesiredUnicastCipher = DOT11_CIPHER_ALGO_NONE;
    Adapter->DesiredMulticastCipher = DOT11_CIPHER_ALGO_NONE;

    WdiReadKnobs(Adapter);
    WdiSetDataApi(&Adapter->DataApi);

    Adapter->StateWorkItem = NdisAllocateIoWorkItem(Adapter->MiniportAdapterHandle);
    Adapter->ScanWorkItem = NdisAllocateIoWorkItem(Adapter->MiniportAdapterHandle);
    Adapter->ConnectWorkItem = NdisAllocateIoWorkItem(Adapter->MiniportAdapterHandle);
    if (Adapter->StateWorkItem == NULL ||
        Adapter->ScanWorkItem == NULL ||
        Adapter->ConnectWorkItem == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Failed;
    }

    Adapter->Progress |= WDI_PROGRESS_CONTROL_PATH;

    Step = WdiInitAllocate;
    Status = WdiAllocate(Adapter, InitParameters);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitOpen;
    Status = WdiOpen(Adapter, InitParameters);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitCapabilities;
    Status = WdiQueryCapabilities(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitConfiguration;
    Status = WdiConfigure(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Status = WdiInitializeDataPath(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Status = WdiStartDataPath(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitCreatePort;
    Status = WdiCreatePort(Adapter, WDI_OPERATION_MODE_STA, NDIS_DEFAULT_PORT_NUMBER);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitRadioState;
    WdiApplyRadioState(Adapter);

    Step = WdiInitAttributes;
    Status = WdiSetGeneralAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    Step = WdiInitStartOperation;
    Status = WdiStartOperation(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    DPRINT1("WLAN adapter up, WDI 0x%lx\n", Adapter->PeerVersion);
    WdiStartTestScan(Adapter);
    return NDIS_STATUS_SUCCESS;

Failed:
    WdiLogInitFailure(Adapter, Step, Status);
    WdiHaltAdapter(Adapter);
    return Status;
}

/**
 * @brief
 * Takes a WLAN adapter down, undoing whatever bring-up got through: the
 * IHV stops operating, ports go, the data path stops and is released, and
 * the IHV closes and frees the adapter.
 *
 * @param[in] Adapter
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiHaltAdapter(
    PWDI_ADAPTER Adapter)
{
    PWDI_MINIPORT Miniport = Adapter->Miniport;
    NDIS_STATUS Status;
    LONG i;

    PAGED_CODE();

    WdiWaitForScan(Adapter);
    KeWaitForSingleObject(&Adapter->ConnectIdle, Executive, KernelMode, FALSE, NULL);

    if (Adapter->Progress & WDI_PROGRESS_OPERATING)
    {
        if (Miniport->Wdi.StopOperationHandler != NULL)
            Miniport->Wdi.StopOperationHandler(Adapter->MiniportAdapterContext);
        Adapter->Progress &= ~WDI_PROGRESS_OPERATING;
    }

    for (i = RTL_NUMBER_OF(Adapter->Ports) - 1; i >= 0; i--)
    {
        if (Adapter->Ports[i].InUse)
            WdiDeletePort(Adapter, &Adapter->Ports[i]);
    }

    if (Adapter->Progress & WDI_PROGRESS_DATAPATH_STARTED)
    {
        Adapter->DataHandlers.TalTxRxStopHandler(Adapter->TalTxRx);
        WdiFlushSends(Adapter, NDIS_STATUS_MEDIA_DISCONNECTED);
        Adapter->Progress &= ~WDI_PROGRESS_DATAPATH_STARTED;
    }

    if (Adapter->Progress & WDI_PROGRESS_DATAPATH_READY)
    {
        Miniport->Wdi.TalTxRxDeinitializeHandler(Adapter->TalTxRx);
        Adapter->TalTxRx = NULL;
        Adapter->Progress &= ~WDI_PROGRESS_DATAPATH_READY;
    }

    WdiDeleteFrameLookaside(Adapter);
    RtlZeroMemory(&Adapter->DataHandlers, sizeof(Adapter->DataHandlers));

    if (Adapter->Progress & WDI_PROGRESS_OPENED)
    {
        /* Like opening, success means the IHV reports back when it is closed */
        KeClearEvent(&Adapter->OpenCloseDone);
        Status = Miniport->Wdi.CloseAdapterHandler(Adapter->MiniportAdapterContext);
        if (Status == NDIS_STATUS_SUCCESS)
        {
            KeWaitForSingleObject(&Adapter->OpenCloseDone, Executive, KernelMode, FALSE, NULL);
            Status = Adapter->OpenCloseStatus;
        }

        if (Status != NDIS_STATUS_SUCCESS)
            DPRINT1("CloseAdapter failed (0x%x)\n", Status);

        Adapter->Progress &= ~WDI_PROGRESS_OPENED;
    }

    if (Adapter->Progress & WDI_PROGRESS_ALLOCATED)
    {
        Miniport->Wdi.FreeAdapterHandler(Adapter->MiniportAdapterContext);
        Adapter->Progress &= ~WDI_PROGRESS_ALLOCATED;
    }

    if (Adapter->StateWorkItem != NULL)
    {
        NdisFreeIoWorkItem(Adapter->StateWorkItem);
        Adapter->StateWorkItem = NULL;
    }

    if (Adapter->ScanWorkItem != NULL)
    {
        NdisFreeIoWorkItem(Adapter->ScanWorkItem);
        Adapter->ScanWorkItem = NULL;
    }

    if (Adapter->ConnectWorkItem != NULL)
    {
        NdisFreeIoWorkItem(Adapter->ConnectWorkItem);
        Adapter->ConnectWorkItem = NULL;
    }

    Adapter->Progress = 0;
}
