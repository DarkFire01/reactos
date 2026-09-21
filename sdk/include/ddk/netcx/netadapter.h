/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The network adapter object
 */

#pragma once

#include <netcx/netadaptercxtypes.h>
#include <netcx/netpacketqueue.h>
#include <netcx/netreceivescaling.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_ADAPTER_PAUSE_FUNCTION_TYPE {
    NetAdapterPauseFunctionTypeUnsupported = NdisPauseFunctionsUnsupported,
    NetAdapterPauseFunctionTypeSendOnly = NdisPauseFunctionsSendOnly,
    NetAdapterPauseFunctionTypeReceiveOnly = NdisPauseFunctionsReceiveOnly,
    NetAdapterPauseFunctionTypeSendAndReceive = NdisPauseFunctionsSendAndReceive,
    NetAdapterPauseFunctionTypeUnknown = NdisPauseFunctionsUnknown,
} NET_ADAPTER_PAUSE_FUNCTION_TYPE;

typedef enum _NET_ADAPTER_AUTO_NEGOTIATION_FLAGS {
    NetAdapterAutoNegotiationFlagNone = 0,
    NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated = NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED,
    NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated = NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED,
    NetAdapterAutoNegotiationFlagDuplexAutoNegotiated = NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED,
    NetAdapterAutoNegotiationFlagPauseFunctionsAutoNegotiated = NDIS_LINK_STATE_PAUSE_FUNCTIONS_AUTO_NEGOTIATED,
} NET_ADAPTER_AUTO_NEGOTIATION_FLAGS;

typedef enum _NET_MEMORY_MAPPING_REQUIREMENT {
    NetMemoryMappingRequirementNone = 0,
    NetMemoryMappingRequirementDmaMapped,
} NET_MEMORY_MAPPING_REQUIREMENT;

DEFINE_ENUM_FLAG_OPERATORS(NET_ADAPTER_PAUSE_FUNCTION_TYPE);
DEFINE_ENUM_FLAG_OPERATORS(NET_ADAPTER_AUTO_NEGOTIATION_FLAGS);

typedef
_Function_class_(EVT_NET_ADAPTER_CREATE_TXQUEUE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_ADAPTER_CREATE_TXQUEUE(
    _In_ NETADAPTER Adapter,
    _Inout_ NETTXQUEUE_INIT *TxQueueInit);

typedef EVT_NET_ADAPTER_CREATE_TXQUEUE *PFN_NET_ADAPTER_CREATE_TXQUEUE;

typedef
_Function_class_(EVT_NET_ADAPTER_CREATE_RXQUEUE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_ADAPTER_CREATE_RXQUEUE(
    _In_ NETADAPTER Adapter,
    _Inout_ NETRXQUEUE_INIT *RxQueueInit);

typedef EVT_NET_ADAPTER_CREATE_RXQUEUE *PFN_NET_ADAPTER_CREATE_RXQUEUE;

typedef struct _NET_ADAPTER_LINK_LAYER_ADDRESS
{
    USHORT Length;
    UCHAR Address[NDIS_MAX_PHYS_ADDRESS_LENGTH];
} NET_ADAPTER_LINK_LAYER_ADDRESS;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress,
    _In_range_(1,NDIS_MAX_PHYS_ADDRESS_LENGTH) USHORT Length,
    _In_reads_bytes_(Length) PCUCHAR AddressBuffer)
{
    RtlZeroMemory(LinkLayerAddress, sizeof(NET_ADAPTER_LINK_LAYER_ADDRESS));
    NT_ASSERTMSG("Failed: 0 < Length <= NDIS_MAX_PHYS_ADDRESS_LENGTH",
                 (Length != 0) && (Length <= NDIS_MAX_PHYS_ADDRESS_LENGTH));
    LinkLayerAddress->Length = Length;

    RtlCopyMemory(LinkLayerAddress->Address,
                  AddressBuffer,
                  Length < sizeof(LinkLayerAddress->Address) ?
                    Length :
                    sizeof(LinkLayerAddress->Address));
}

typedef struct _NET_ADAPTER_LINK_LAYER_CAPABILITIES {

    ULONG                           Size;
    ULONG64                         MaxTxLinkSpeed;
    ULONG64                         MaxRxLinkSpeed;
} NET_ADAPTER_LINK_LAYER_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_LINK_LAYER_CAPABILITIES *LinkLayerCapabilities,
    _In_ ULONG64 MaxTxLinkSpeed,
    _In_ ULONG64 MaxRxLinkSpeed)
{
    RtlZeroMemory(LinkLayerCapabilities, sizeof(NET_ADAPTER_LINK_LAYER_CAPABILITIES));
    LinkLayerCapabilities->Size = sizeof(NET_ADAPTER_LINK_LAYER_CAPABILITIES);
    LinkLayerCapabilities->MaxTxLinkSpeed = MaxTxLinkSpeed;
    LinkLayerCapabilities->MaxRxLinkSpeed = MaxRxLinkSpeed;
}

typedef
_Function_class_(EVT_NET_ADAPTER_RETURN_RX_BUFFER)
_IRQL_requires_same_
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_RETURN_RX_BUFFER(
    _In_ NETADAPTER Adapter,
    _In_ NET_FRAGMENT_RETURN_CONTEXT_HANDLE RxReturnContext);

typedef EVT_NET_ADAPTER_RETURN_RX_BUFFER *PFN_NET_ADAPTER_RETURN_RX_BUFFER;

typedef struct _NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES
{
    ULONG Size;
    BOOLEAN ArpOffload;
    SIZE_T MaximumOffloadCount;
} NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES *Capabilities,
    _In_ SIZE_T MaximumOffloadCount)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->ArpOffload = TRUE;
    Capabilities->MaximumOffloadCount = MaximumOffloadCount;
}

typedef struct _NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES
{
    ULONG Size;
    BOOLEAN NSOffload;
    SIZE_T MaximumOffloadCount;
} NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES *Capabilities,
    _In_ SIZE_T MaximumOffloadCount)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->NSOffload = TRUE;
    Capabilities->MaximumOffloadCount = MaximumOffloadCount;
}

typedef struct _NET_ADAPTER_WAKE_BITMAP_CAPABILITIES
{
    ULONG Size;
    BOOLEAN BitmapPattern;
    SIZE_T MaximumPatternCount;
    SIZE_T MaximumPatternSize;
} NET_ADAPTER_WAKE_BITMAP_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_WAKE_BITMAP_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_BITMAP_CAPABILITIES *Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES
{
    ULONG Size;
    BOOLEAN MediaConnect;
    BOOLEAN MediaDisconnect;
} NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES *Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES
{
    ULONG Size;
    BOOLEAN MagicPacket;
} NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES *Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES
{
    ULONG Size;
    BOOLEAN PacketFilterMatch;
} NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES *Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

#define NET_ADAPTER_FRAGMENT_DEFAULT_ALIGNMENT 1

typedef struct _NET_ADAPTER_DMA_CAPABILITIES
{
    ULONG Size;

    WDFDMAENABLER DmaEnabler;

    PHYSICAL_ADDRESS MaximumPhysicalAddress;

    WDF_TRI_STATE CacheEnabled;

    NODE_REQUIREMENT PreferredNode;

} NET_ADAPTER_DMA_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_DMA_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities,
    _In_ WDFDMAENABLER DmaEnabler)
{
    RtlZeroMemory(DmaCapabilities, sizeof(NET_ADAPTER_DMA_CAPABILITIES));
    DmaCapabilities->Size = sizeof(NET_ADAPTER_DMA_CAPABILITIES);
    DmaCapabilities->DmaEnabler = DmaEnabler;
    DmaCapabilities->CacheEnabled = WdfUseDefault;
    DmaCapabilities->PreferredNode = MM_ANY_NODE_OK;
}

typedef enum _NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE
{

    NetRxFragmentBufferAllocationModeSystem = 0,

    NetRxFragmentBufferAllocationModeDriver,
} NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE;

typedef enum _NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE
{

    NetRxFragmentBufferAttachmentModeSystem = 0,

    NetRxFragmentBufferAttachmentModeDriver,
} NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE;

typedef struct _NET_ADAPTER_RX_CAPABILITIES
{
    ULONG Size;

    NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE AllocationMode;

    NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE AttachmentMode;

    UINT32 FragmentRingNumberOfElementsHint;

    SIZE_T MaximumFrameSize;

    SIZE_T MaximumNumberOfQueues;

    union
    {

        struct
        {
            PFN_NET_ADAPTER_RETURN_RX_BUFFER EvtAdapterReturnRxBuffer;
        }
        DUMMYSTRUCTNAME;

        struct
        {

            NET_MEMORY_MAPPING_REQUIREMENT MappingRequirement;

            SIZE_T FragmentBufferAlignment;

            NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities;
        }
        DUMMYSTRUCTNAME2;
    }
    DUMMYUNIONNAME;

} NET_ADAPTER_RX_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_RX_CAPABILITIES_INIT_DRIVER_MANAGED(
    _Out_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities,
    _In_ PFN_NET_ADAPTER_RETURN_RX_BUFFER EvtAdapterReturnRxBuffer,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(RxCapabilities, sizeof(NET_ADAPTER_RX_CAPABILITIES));
    RxCapabilities->Size = sizeof(NET_ADAPTER_RX_CAPABILITIES);
    RxCapabilities->FragmentBufferAlignment = NET_ADAPTER_FRAGMENT_DEFAULT_ALIGNMENT;
    RxCapabilities->MaximumFrameSize = MaximumFrameSize;
    RxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;

    RxCapabilities->AllocationMode = NetRxFragmentBufferAllocationModeDriver;
    RxCapabilities->AttachmentMode = NetRxFragmentBufferAttachmentModeDriver;
    RxCapabilities->EvtAdapterReturnRxBuffer = EvtAdapterReturnRxBuffer;
}

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(
    _Out_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(RxCapabilities, sizeof(NET_ADAPTER_RX_CAPABILITIES));
    RxCapabilities->Size = sizeof(NET_ADAPTER_RX_CAPABILITIES);
    RxCapabilities->FragmentBufferAlignment = NET_ADAPTER_FRAGMENT_DEFAULT_ALIGNMENT;
    RxCapabilities->MaximumFrameSize = MaximumFrameSize;
    RxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;

    RxCapabilities->AllocationMode = NetRxFragmentBufferAllocationModeSystem;
    RxCapabilities->AttachmentMode = NetRxFragmentBufferAttachmentModeSystem;
    RxCapabilities->MappingRequirement = NetMemoryMappingRequirementNone;
}

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
    _Out_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities,
    _In_ NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(RxCapabilities, sizeof(NET_ADAPTER_RX_CAPABILITIES));
    RxCapabilities->Size = sizeof(NET_ADAPTER_RX_CAPABILITIES);
    RxCapabilities->FragmentBufferAlignment = NET_ADAPTER_FRAGMENT_DEFAULT_ALIGNMENT;
    RxCapabilities->MaximumFrameSize = MaximumFrameSize;
    RxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;

    RxCapabilities->AllocationMode = NetRxFragmentBufferAllocationModeSystem;
    RxCapabilities->AttachmentMode = NetRxFragmentBufferAttachmentModeSystem;
    RxCapabilities->MappingRequirement = NetMemoryMappingRequirementDmaMapped;
    RxCapabilities->DmaCapabilities = DmaCapabilities;
}

typedef struct _NET_ADAPTER_TX_CAPABILITIES
{
    ULONG Size;

    NET_MEMORY_MAPPING_REQUIREMENT MappingRequirement;

    SIZE_T PayloadBackfill;

    SIZE_T MaximumNumberOfFragments;

    SIZE_T FragmentBufferAlignment;

    UINT32 FragmentRingNumberOfElementsHint;

    SIZE_T MaximumNumberOfQueues;

    NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities;

} NET_ADAPTER_TX_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_TX_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_TX_CAPABILITIES *TxCapabilities,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(TxCapabilities, sizeof(NET_ADAPTER_TX_CAPABILITIES));
    TxCapabilities->Size = sizeof(NET_ADAPTER_TX_CAPABILITIES);
    TxCapabilities->FragmentBufferAlignment = NET_ADAPTER_FRAGMENT_DEFAULT_ALIGNMENT;
    TxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;
    TxCapabilities->MaximumNumberOfFragments = (SIZE_T)-1;
}

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(
    _Out_ NET_ADAPTER_TX_CAPABILITIES *TxCapabilities,
    _In_ NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    NET_ADAPTER_TX_CAPABILITIES_INIT(
        TxCapabilities,
        MaximumNumberOfQueues);

    TxCapabilities->DmaCapabilities = DmaCapabilities;
    TxCapabilities->MappingRequirement = NetMemoryMappingRequirementDmaMapped;
}

typedef struct _NET_ADAPTER_LINK_STATE {

    ULONG                                Size;

    ULONG64                              TxLinkSpeed;
    ULONG64                              RxLinkSpeed;

    NET_IF_MEDIA_CONNECT_STATE           MediaConnectState;

    NET_IF_MEDIA_DUPLEX_STATE            MediaDuplexState;

    NET_ADAPTER_PAUSE_FUNCTION_TYPE          SupportedPauseFunctions;

    NET_ADAPTER_AUTO_NEGOTIATION_FLAGS   AutoNegotiationFlags;

} NET_ADAPTER_LINK_STATE;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_LINK_STATE_INIT(
    _Out_ NET_ADAPTER_LINK_STATE * LinkState,
    _In_ ULONG64 LinkSpeed,
    _In_ NET_IF_MEDIA_CONNECT_STATE MediaConnectState,
    _In_ NET_IF_MEDIA_DUPLEX_STATE MediaDuplexState,
    _In_ NET_ADAPTER_PAUSE_FUNCTION_TYPE SupportedPauseFunctions,
    _In_ NET_ADAPTER_AUTO_NEGOTIATION_FLAGS AutoNegotiationFlags)
{
    RtlZeroMemory(LinkState, sizeof(NET_ADAPTER_LINK_STATE));
    LinkState->Size = sizeof(NET_ADAPTER_LINK_STATE);
    LinkState->TxLinkSpeed = LinkSpeed;
    LinkState->RxLinkSpeed = LinkSpeed;
    LinkState->MediaConnectState = MediaConnectState;
    LinkState->MediaDuplexState = MediaDuplexState;
    LinkState->SupportedPauseFunctions = SupportedPauseFunctions;
    LinkState->AutoNegotiationFlags = AutoNegotiationFlags;
}

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(
    _Out_ NET_ADAPTER_LINK_STATE * LinkState)
{
    RtlZeroMemory(LinkState, sizeof(NET_ADAPTER_LINK_STATE));
    LinkState->Size = sizeof(NET_ADAPTER_LINK_STATE);

    LinkState->MediaConnectState = MediaConnectStateDisconnected;

    LinkState->TxLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    LinkState->RxLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    LinkState->MediaDuplexState = MediaDuplexStateUnknown;
    LinkState->SupportedPauseFunctions = NetAdapterPauseFunctionTypeUnsupported;
}

typedef struct _NET_ADAPTER_DATAPATH_CALLBACKS {

    ULONG                                     Size;

    PFN_NET_ADAPTER_CREATE_TXQUEUE            EvtAdapterCreateTxQueue;
    PFN_NET_ADAPTER_CREATE_RXQUEUE            EvtAdapterCreateRxQueue;

} NET_ADAPTER_DATAPATH_CALLBACKS;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_DATAPATH_CALLBACKS_INIT(
    _Out_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks,
    _In_ PFN_NET_ADAPTER_CREATE_TXQUEUE EvtAdapterCreateTxQueue,
    _In_ PFN_NET_ADAPTER_CREATE_RXQUEUE EvtAdapterCreateRxQueue)
{
    RtlZeroMemory(DatapathCallbacks,
        sizeof(NET_ADAPTER_DATAPATH_CALLBACKS));
    DatapathCallbacks->Size = sizeof(NET_ADAPTER_DATAPATH_CALLBACKS);
    DatapathCallbacks->EvtAdapterCreateTxQueue = EvtAdapterCreateTxQueue;
    DatapathCallbacks->EvtAdapterCreateRxQueue = EvtAdapterCreateRxQueue;
}

typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);

typedef EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM;

typedef struct _NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES
{

    ULONG Size;

    BOOLEAN IPv4;

    BOOLEAN Tcp;

    BOOLEAN Udp;

    PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM
        EvtAdapterOffloadSetChecksum;

} NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES *ChecksumCapabilities,
    _In_ BOOLEAN IPv4,
    _In_ BOOLEAN Tcp,
    _In_ BOOLEAN Udp,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM EvtAdapterOffloadSetChecksum)
{
    RtlZeroMemory(ChecksumCapabilities, sizeof(NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES));
    ChecksumCapabilities->Size = sizeof(NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES);

    ChecksumCapabilities->IPv4 = IPv4;
    ChecksumCapabilities->Tcp = Tcp;
    ChecksumCapabilities->Udp = Udp;
    ChecksumCapabilities->EvtAdapterOffloadSetChecksum = EvtAdapterOffloadSetChecksum;
}

typedef struct _NET_ADAPTER_WAKE_REASON_PACKET
{
    ULONG Size;
    ULONG PatternId;
    ULONG OriginalPacketSize;
    WDFMEMORY WakePacket;
} NET_ADAPTER_WAKE_REASON_PACKET;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_WAKE_REASON_PACKET_INIT(
    _Out_ NET_ADAPTER_WAKE_REASON_PACKET *Reason)
{
    RtlZeroMemory(Reason, sizeof(*Reason));
    Reason->Size = sizeof(*Reason);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER_INIT *
(NTAPI *PFN_NETADAPTERINITALLOCATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTER_INIT *
NTAPI
NetAdapterInitAllocate(
    _In_ WDFDEVICE Device)
{
    return ((PFN_NETADAPTERINITALLOCATE) NetFunctions[NetAdapterInitAllocateTableIndex])(NetDriverGlobals, Device);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERINITFREE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterInitFree(
    _In_ NETADAPTER_INIT *AdapterInit)
{
    ((PFN_NETADAPTERINITFREE) NetFunctions[NetAdapterInitFreeTableIndex])(NetDriverGlobals, AdapterInit);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERINITSETDATAPATHCALLBACKS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS *DatapathCallbacks);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterInitSetDatapathCallbacks(
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS *DatapathCallbacks)
{
    ((PFN_NETADAPTERINITSETDATAPATHCALLBACKS) NetFunctions[NetAdapterInitSetDatapathCallbacksTableIndex])(NetDriverGlobals, AdapterInit, DatapathCallbacks);
}

typedef
    _Must_inspect_result_ _IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTERCREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *AdapterAttributes,
    _Out_ NETADAPTER *Adapter);

    _Must_inspect_result_ _IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetAdapterCreate(
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *AdapterAttributes,
    _Out_ NETADAPTER *Adapter)
{
    return ((PFN_NETADAPTERCREATE) NetFunctions[NetAdapterCreateTableIndex])(NetDriverGlobals, AdapterInit, AdapterAttributes, Adapter);
}

typedef
    _Must_inspect_result_ _IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTERSTART)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

    _Must_inspect_result_ _IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetAdapterStart(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERSTART) NetFunctions[NetAdapterStartTableIndex])(NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSTOP)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterStop(
    _In_ NETADAPTER Adapter)
{
    ((PFN_NETADAPTERSTOP) NetFunctions[NetAdapterStopTableIndex])(NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKLAYERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES *LinkLayerCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetLinkLayerCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES *LinkLayerCapabilities)
{
    ((PFN_NETADAPTERSETLINKLAYERCAPABILITIES) NetFunctions[NetAdapterSetLinkLayerCapabilitiesTableIndex])(NetDriverGlobals, Adapter, LinkLayerCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKLAYERMTUSIZE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetLinkLayerMtuSize(
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize)
{
    ((PFN_NETADAPTERSETLINKLAYERMTUSIZE) NetFunctions[NetAdapterSetLinkLayerMtuSizeTableIndex])(NetDriverGlobals, Adapter, MtuSize);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERPOWEROFFLOADSETARPCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterPowerOffloadSetArpCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERPOWEROFFLOADSETARPCAPABILITIES) NetFunctions[NetAdapterPowerOffloadSetArpCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERPOWEROFFLOADSETNSCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterPowerOffloadSetNSCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERPOWEROFFLOADSETNSCAPABILITIES) NetFunctions[NetAdapterPowerOffloadSetNSCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETBITMAPCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_BITMAP_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterWakeSetBitmapCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_BITMAP_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERWAKESETBITMAPCAPABILITIES) NetFunctions[NetAdapterWakeSetBitmapCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETMEDIACHANGECAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterWakeSetMediaChangeCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERWAKESETMEDIACHANGECAPABILITIES) NetFunctions[NetAdapterWakeSetMediaChangeCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETMAGICPACKETCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterWakeSetMagicPacketCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERWAKESETMAGICPACKETCAPABILITIES) NetFunctions[NetAdapterWakeSetMagicPacketCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETPACKETFILTERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterWakeSetPacketFilterCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTERWAKESETPACKETFILTERCAPABILITIES) NetFunctions[NetAdapterWakeSetPacketFilterCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETDATAPATHCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES *TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetDataPathCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES *TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities)
{
    ((PFN_NETADAPTERSETDATAPATHCAPABILITIES) NetFunctions[NetAdapterSetDataPathCapabilitiesTableIndex])(NetDriverGlobals, Adapter, TxCapabilities, RxCapabilities);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKSTATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE *State);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetLinkState(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE *State)
{
    ((PFN_NETADAPTERSETLINKSTATE) NetFunctions[NetAdapterSetLinkStateTableIndex])(NetDriverGlobals, Adapter, State);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_LUID
(NTAPI *PFN_NETADAPTERGETNETLUID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NET_LUID
NTAPI
NetAdapterGetNetLuid(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETNETLUID) NetFunctions[NetAdapterGetNetLuidTableIndex])(NetDriverGlobals, Adapter);
}

typedef
    _Must_inspect_result_ _IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTEROPENCONFIGURATION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration);

    _Must_inspect_result_ _IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetAdapterOpenConfiguration(
    _In_ NETADAPTER Adapter,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration)
{
    return ((PFN_NETADAPTEROPENCONFIGURATION) NetFunctions[NetAdapterOpenConfigurationTableIndex])(NetDriverGlobals, Adapter, ConfigurationAttributes, Configuration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETPERMANENTLINKLAYERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetPermanentLinkLayerAddress(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress)
{
    ((PFN_NETADAPTERSETPERMANENTLINKLAYERADDRESS) NetFunctions[NetAdapterSetPermanentLinkLayerAddressTableIndex])(NetDriverGlobals, Adapter, LinkLayerAddress);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETCURRENTLINKLAYERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetCurrentLinkLayerAddress(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress)
{
    ((PFN_NETADAPTERSETCURRENTLINKLAYERADDRESS) NetFunctions[NetAdapterSetCurrentLinkLayerAddressTableIndex])(NetDriverGlobals, Adapter, LinkLayerAddress);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES *HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETCHECKSUMCAPABILITIES) NetFunctions[NetAdapterOffloadSetChecksumCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMIPV4ENABLED) NetFunctions[NetOffloadIsChecksumIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMTCPENABLED) NetFunctions[NetOffloadIsChecksumTcpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMUDPENABLED) NetFunctions[NetOffloadIsChecksumUdpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERREPORTWAKEREASONPACKET)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_REASON_PACKET *Reason);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterReportWakeReasonPacket(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_WAKE_REASON_PACKET *Reason)
{
    ((PFN_NETADAPTERREPORTWAKEREASONPACKET) NetFunctions[NetAdapterReportWakeReasonPacketTableIndex])(NetDriverGlobals, Adapter, Reason);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERREPORTWAKEREASONMEDIACHANGE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_IF_MEDIA_CONNECT_STATE Reason);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterReportWakeReasonMediaChange(
    _In_ NETADAPTER Adapter,
    _In_ NET_IF_MEDIA_CONNECT_STATE Reason)
{
    ((PFN_NETADAPTERREPORTWAKEREASONMEDIACHANGE) NetFunctions[NetAdapterReportWakeReasonMediaChangeTableIndex])(NetDriverGlobals, Adapter, Reason);
}

#ifdef __cplusplus
}
#endif
