/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Adapter interface the adapter half offers the translator
 *
 * Member order follows the drop's initializer in adapter/nxadapter.cpp. The
 * 26100 pdb has GetMdlForDriverAllocatedMemory after ReturnRxBuffer, which
 * this revision of the drop does not implement.
 */

#pragma once

#include <NetClientTypes.h>
#include <NetClientBuffer.h>
#include <NetClientQueue.h>
#include <net/returncontexttypes.h>
#include <executioncontext.h>
#include <executioncontextdispatch.h>
#include <pktmonclnt.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_CLIENT_ADAPTER_OFFLOAD_LAYER3_FLAGS
{
    NetClientAdapterOffloadLayer3FlagIPv4NoOptions = 0x1,
    NetClientAdapterOffloadLayer3FlagIPv4WithOptions = 0x2,
    NetClientAdapterOffloadLayer3FlagIPv6NoExtensions = 0x4,
    NetClientAdapterOffloadLayer3FlagIPv6WithExtensions = 0x8
} NET_CLIENT_ADAPTER_OFFLOAD_LAYER3_FLAGS;

typedef enum _NET_CLIENT_ADAPTER_OFFLOAD_LAYER4_FLAGS
{
    NetClientAdapterOffloadLayer4FlagTcpNoOptions = 0x1,
    NetClientAdapterOffloadLayer4FlagTcpWithOptions = 0x2,
    NetClientAdapterOffloadLayer4FlagUdp = 0x4
} NET_CLIENT_ADAPTER_OFFLOAD_LAYER4_FLAGS;

typedef enum _NET_CLIENT_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS
{
    NetClientAdapterOffloadIeee8021PriorityTaggingFlag = 0x1,
    NetClientAdapterOffloadIeee8021VlanTaggingFlag = 0x2
} NET_CLIENT_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(NET_CLIENT_ADAPTER_OFFLOAD_LAYER3_FLAGS);
DEFINE_ENUM_FLAG_OPERATORS(NET_CLIENT_ADAPTER_OFFLOAD_LAYER4_FLAGS);
DEFINE_ENUM_FLAG_OPERATORS(NET_CLIENT_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS);

typedef struct DECLSPEC_ALIGN(2) _NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    BOOLEAN IPv4;
    BOOLEAN Tcp;
    BOOLEAN Udp;
} NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES;

typedef struct _NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    NET_CLIENT_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_CLIENT_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    USHORT Layer3HeaderOffsetLimit;
    USHORT Layer4HeaderOffsetLimit;
} NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES;

typedef struct _NET_CLIENT_OFFLOAD_GSO_CAPABILITIES
{
    ULONG Size;
    NET_CLIENT_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_CLIENT_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    USHORT Layer4HeaderOffsetLimit;
    SIZE_T MaximumOffloadSize;
    SIZE_T MinimumSegmentCount;
} NET_CLIENT_OFFLOAD_GSO_CAPABILITIES;

typedef struct _NET_CLIENT_OFFLOAD_RSC_CAPABILITIES
{
    ULONG Size;
    BOOLEAN IPv4;
    BOOLEAN IPv6;
    BOOLEAN Udp;
    BOOLEAN TcpTimestampOption;
} NET_CLIENT_OFFLOAD_RSC_CAPABILITIES;

typedef struct _NET_CLIENT_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES
{
    ULONG Size;
    NET_CLIENT_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS Flags;
} NET_CLIENT_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES;

typedef enum _NET_CLIENT_ADAPTER_RECEIVE_SCALING_HASH_TYPE
{
    NetClientAdapterReceiveScalingHashTypeNone = 0x0,
    NetClientAdapterReceiveScalingHashTypeToeplitz = 0x1
} NET_CLIENT_ADAPTER_RECEIVE_SCALING_HASH_TYPE;

typedef enum _NET_CLIENT_ADAPTER_RECEIVE_SCALING_PROTOCOL_TYPE
{
    NetClientAdapterReceiveScalingProtocolTypeNone = 0x0,
    NetClientAdapterReceiveScalingProtocolTypeIPv4 = 0x1,
    NetClientAdapterReceiveScalingProtocolTypeIPv4Options = 0x2,
    NetClientAdapterReceiveScalingProtocolTypeIPv6 = 0x4,
    NetClientAdapterReceiveScalingProtocolTypeIPv6Extensions = 0x8,
    NetClientAdapterReceiveScalingProtocolTypeTcp = 0x10,
    NetClientAdapterReceiveScalingProtocolTypeUdp = 0x20
} NET_CLIENT_ADAPTER_RECEIVE_SCALING_PROTOCOL_TYPE;

DEFINE_ENUM_FLAG_OPERATORS(NET_CLIENT_ADAPTER_RECEIVE_SCALING_HASH_TYPE);
DEFINE_ENUM_FLAG_OPERATORS(NET_CLIENT_ADAPTER_RECEIVE_SCALING_PROTOCOL_TYPE);

typedef struct _NET_CLIENT_ADAPTER_RECEIVE_SCALING_CAPABILITIES
{
    SIZE_T NumberOfIndirectionQueues;
    SIZE_T NumberOfIndirectionTableEntries;
} NET_CLIENT_ADAPTER_RECEIVE_SCALING_CAPABILITIES;

typedef struct _NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRY
{
    NET_CLIENT_QUEUE Queue;
    NTSTATUS Status;
    ULONG Index;
} NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRY;

typedef struct _NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRIES
{
    NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRY *Entries;
    SIZE_T Length;
} NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRIES;

typedef struct _NET_CLIENT_RECEIVE_SCALING_HASH_SECRET_KEY
{
    UINT8 const *Key;
    SIZE_T Length;
} NET_CLIENT_RECEIVE_SCALING_HASH_SECRET_KEY;

typedef struct _NET_CLIENT_RECEIVE_SCALING_HASH_INFO
{
    ULONG Size;
    NET_CLIENT_ADAPTER_RECEIVE_SCALING_HASH_TYPE HashType;
    NET_CLIENT_ADAPTER_RECEIVE_SCALING_PROTOCOL_TYPE ProtocolType;
} NET_CLIENT_RECEIVE_SCALING_HASH_INFO;

typedef struct _NET_CLIENT_ADAPTER_PROPERTIES
{
    NDIS_MEDIUM MediaType;
    NET_LUID NetLuid;
    BOOLEAN DriverIsVerifying;
    NDIS_HANDLE NdisAdapterHandle;
    PVOID NblDispatcher;
    PKTMON_LOWEREDGE_HANDLE PacketMonitorLowerEdge;
    PKTMON_COMPONENT_HANDLE PacketMonitorComponentContext;
    UNICODE_STRING const *Name;
    EXECUTION_CONTEXT_RUNTIME_KNOBS const *ExecutionContextKnobs;
    struct _EPROCESS *Process;
} NET_CLIENT_ADAPTER_PROPERTIES;

typedef struct DECLSPEC_ALIGN(8) _NET_CLIENT_ADAPTER_DATAPATH_CAPABILITIES
{
    NET_CLIENT_MEMORY_MANAGEMENT_MODE RxMemoryManagementMode;
    PVOID RxMemoryCollection;
    NET_CLIENT_MEMORY_CONSTRAINTS TxMemoryConstraints;
    NET_CLIENT_MEMORY_CONSTRAINTS RxMemoryConstraints;
    SIZE_T MaximumTxFragmentSize;
    SIZE_T MaximumRxFragmentSize;
    SIZE_T MaximumNumberOfTxFragments;
    SIZE_T TxPayloadBackfill;
    SIZE_T MaximumNumberOfTxQueues;
    SIZE_T MaximumNumberOfRxQueues;
    ULONG PreferredTxFragmentRingSize;
    ULONG PreferredRxFragmentRingSize;
    ULONG NominalMtu;
    ULONG MtuWithGso;
    ULONG MtuWithRsc;
    ULONG64 NominalMaxTxLinkSpeed;
    ULONG64 NominalMaxRxLinkSpeed;
    BOOLEAN FlushBuffers;
} NET_CLIENT_ADAPTER_DATAPATH_CAPABILITIES;

typedef struct DECLSPEC_ALIGN(4) _NET_CLIENT_ADAPTER_TX_DEMUX
{
    ULONG Size;
    NET_CLIENT_ADAPTER_TX_DEMUX_TYPE Type;
    UINT8 Range;
} NET_CLIENT_ADAPTER_TX_DEMUX;

typedef struct _NET_CLIENT_ADAPTER_TX_DEMUX_CONFIGURATION
{
    ULONG Size;
    NET_CLIENT_ADAPTER_TX_DEMUX const *Demux;
    SIZE_T Count;
} NET_CLIENT_ADAPTER_TX_DEMUX_CONFIGURATION;

typedef struct _NET_CLIENT_ADAPTER_RECEIVE_SCALING_DISPATCH
{
    NTSTATUS
    (*Enable)(
        _In_ NET_CLIENT_ADAPTER Adapter);

    VOID
    (*Disable)(
        _In_ NET_CLIENT_ADAPTER Adapter);

    NTSTATUS
    (*SetIndirectionEntries)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Inout_ NET_CLIENT_RECEIVE_SCALING_INDIRECTION_ENTRIES *IndirectionEntries);

    NTSTATUS
    (*SetHashSecretKey)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_RECEIVE_SCALING_HASH_SECRET_KEY const *HashSecretKey);

    NTSTATUS
    (*SetHashInfo)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_RECEIVE_SCALING_HASH_INFO const *HashInfo);
} NET_CLIENT_ADAPTER_RECEIVE_SCALING_DISPATCH;

typedef struct _NET_CLIENT_ADAPTER_OFFLOAD_DISPATCH
{
    VOID (*GetChecksumHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES *HardwareCapabilities);

    VOID (*GetChecksumDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES *DefaultCapabilities);

    VOID (*SetChecksumActiveCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES const *ActiveCapabilities);

    VOID (*GetGsoHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES *HardwareCapabilities);

    VOID (*GetGsoSoftwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES *SoftwareCapabilities);

    VOID (*GetLsoDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES *DefaultCapabilities);

    VOID (*GetUsoDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES *DefaultCapabilities);

    VOID (*SetGsoActiveCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES const *LsoActiveCapabilities,
        _In_ NET_CLIENT_OFFLOAD_GSO_CAPABILITIES const *UsoActiveCapabilities);

    VOID (*GetRscHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_RSC_CAPABILITIES *HardwareCapabilities);

    VOID (*GetRscSoftwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_RSC_CAPABILITIES *SoftwareCapabilities);

    VOID (*GetRscDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_RSC_CAPABILITIES *DefaultCapabilities);

    VOID (*SetRscActiveCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_OFFLOAD_RSC_CAPABILITIES const *ActiveCapabilities);

    VOID (*GetIeee8021qTagHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES *HardwareCapabilities);

    VOID (*GetTxChecksumHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES *HardwareCapabilities);

    VOID (*GetTxChecksumSoftwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES *SoftwareCapabilities);

    VOID (*GetTxIPv4ChecksumDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES *DefaultCapabilities);

    VOID (*GetTxTcpChecksumDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES *DefaultCapabilities);

    VOID (*GetTxUdpChecksumDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES *DefaultCapabilities);

    VOID (*SetTxChecksumActiveCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES const *ActiveIPv4Capabilities,
        _In_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES const *ActiveTcpCapabilities,
        _In_ NET_CLIENT_OFFLOAD_TX_CHECKSUM_CAPABILITIES const *ActiveUdpCapabilities);

    VOID (*GetRxChecksumHardwareCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES *HardwareCapabilities);

    VOID (*GetRxChecksumDefaultCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES *DefaultCapabilities);

    VOID (*SetRxChecksumActiveCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_OFFLOAD_CHECKSUM_CAPABILITIES const *ActiveCapabilities);
} NET_CLIENT_ADAPTER_OFFLOAD_DISPATCH;

typedef struct _NET_CLIENT_ADAPTER_DISPATCH
{
    ULONG Size;

    VOID
    (*SetDeviceFailed)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NTSTATUS Status);

    VOID
    (*GetProperties)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_ADAPTER_PROPERTIES *Properties);

    VOID
    (*GetDatapathCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_ADAPTER_DATAPATH_CAPABILITIES *Capabilities);

    VOID
    (*GetReceiveScalingCapabilities)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_ADAPTER_RECEIVE_SCALING_CAPABILITIES *Capabilities);

    VOID
    (*GetTxDemuxConfiguration)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _Out_ NET_CLIENT_ADAPTER_TX_DEMUX_CONFIGURATION *Configuration);

    NTSTATUS
    (*CreateTxQueue)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ PVOID ClientContext,
        _In_ NET_CLIENT_QUEUE_NOTIFY_DISPATCH const *ClientDispatch,
        _In_ NET_CLIENT_QUEUE_CONFIG const *ClientQueueConfig,
        _Out_ NET_CLIENT_QUEUE *AdapterQueue,
        _Out_ NET_CLIENT_QUEUE_DISPATCH const **AdapterDispatch,
        _Out_ NET_EXECUTION_CONTEXT *ExecutionContext,
        _Out_ NET_EXECUTION_CONTEXT_DISPATCH const **ExecutionContextDispatch);

    NTSTATUS
    (*CreateRxQueue)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ PVOID ClientContext,
        _In_ NET_CLIENT_QUEUE_NOTIFY_DISPATCH const *ClientDispatch,
        _In_ NET_CLIENT_QUEUE_CONFIG const *ClientQueueConfig,
        _Out_ NET_CLIENT_QUEUE *AdapterQueue,
        _Out_ NET_CLIENT_QUEUE_DISPATCH const **AdapterDispatch,
        _Out_ NET_EXECUTION_CONTEXT *ExecutionContext,
        _Out_ NET_EXECUTION_CONTEXT_DISPATCH const **ExecutionContextDispatch);

    VOID
    (*DestroyQueue)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_QUEUE Queue);

    VOID
    (*ReturnRxBuffer)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_FRAGMENT_RETURN_CONTEXT_HANDLE RxBufferReturnContext);

    NTSTATUS
    (*RegisterExtension)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_EXTENSION const *Extension);

    NTSTATUS
    (*QueryExtension)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_EXTENSION const *Extension);

    NTSTATUS
    (*SetReceiveFilter)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ ULONG PacketFilter,
        _In_ SIZE_T MulticastAddressCount,
        _In_reads_(MulticastAddressCount) IF_PHYSICAL_ADDRESS const *MulticastAddressList);

    SIZE_T
    (*WifiTxPeerDemux)(
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_EUI48_ADDRESS const *Address);

    NET_CLIENT_ADAPTER_RECEIVE_SCALING_DISPATCH ReceiveScalingDispatch;
    NET_CLIENT_ADAPTER_OFFLOAD_DISPATCH OffloadDispatch;
} NET_CLIENT_ADAPTER_DISPATCH;

#ifdef __cplusplus
}
#endif
