/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet monitor client library
 *
 * A network component links this in to report packets and drops to the
 * packet monitor. The implementation is sdk/lib/drivers/pktmonclnt.
 */

#pragma once

#include <ndis.h>
#include <netioddk.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PKTMON_EDGE_LOWER   L"Lower"

typedef enum _PKTMON_DIRECTION_TAG
{
    PktMonDirTag_Unspecified = 0,
    PktMonDirTag_In,
    PktMonDirTag_Out,
    PktMonDirTag_Rx,
    PktMonDirTag_Tx,
    PktMonDirTag_Ingress,
    PktMonDirTag_Egress
} PKTMON_DIRECTION_TAG;

typedef enum _PKTMON_DIRECTION
{
    PktMonDir_In = 1,
    PktMonDir_Out
} PKTMON_DIRECTION;

typedef enum _PKTMON_COMPONENT_TYPE
{
    PktMonComp_Ndis = 1,
    PktMonComp_Miniport,
    PktMonComp_Filter,
    PktMonComp_Protocol,
    PktMonComp_VmsVmNic,
    PktMonComp_VmsMiniport,
    PktMonComp_VmsExtMiniport,
    PktMonComp_VmsProtocolNic,
    PktMonComp_NetVsc,
    PktMonComp_HTTP,
    PktMonComp_IpInterface,
    PktMonComp_Slbmux,
    PktMonComp_Ipsec,
    PktMonComp_NetCx,
    PktMonComp_HTTPMessage
} PKTMON_COMPONENT_TYPE;

typedef enum _PKTMON_COMPONENT_PROPERTY_ID
{
    PktMonCompProp_IfIndex = 1,
    PktMonCompProp_MiniportIfIndex,
    PktMonCompProp_LowerIfIndex,
    PktMonCompProp_IfGuid,
    PktMonCompProp_NdisMedium,
    PktMonCompProp_PhysAddress,
    PktMonCompProp_EtherType,
    PktMonCompProp_OptDataPath,
    PktMonCompProp_NdisObject,
    PktMonCompProp_VMSwitchName,
    PktMonCompProp_VmsExtIfIndex,
    PktMonCompProp_LowestIfIndex,
    PktMonCompProp_IpAddress,
    PktMonCompProp_IpIfIndex,
    PktMonCompProp_Vsid,
    PktMonCompProp_Vlan,
    PktMonCompProp_CompartmentId,
    PktMonCompProp_Max
} PKTMON_COMPONENT_PROPERTY_ID;

typedef enum _PKTMON_PACKET_TYPE
{
    PktMonPayload_Unknown = 0,
    PktMonPayload_Ethernet,
    PktMonPayload_WiFi,
    PktMonPayload_IP,
    PktMonPayload_HTTP,
    PktMonPayload_TCP,
    PktMonPayload_UDP,
    PktMonPayload_ARP,
    PktMonPayload_ICMP,
    PktMonPayload_ESP,
    PktMonPayload_AH,
    PktMonPayload_L4Payload
} PKTMON_PACKET_TYPE;

/* Only the reasons the class extension reports are listed. */
typedef enum _PKTMON_DROP_REASON
{
    PktMonDrop_NetCx_NetPacketLayoutParseFailure = 0x5DD,
    PktMonDrop_NetCx_SoftwareChecksumFailure = 0x5DE,
    PktMonDrop_NetCx_NicQueueStop = 0x5DF,
    PktMonDrop_NetCx_InvalidNetBufferLength = 0x5E0,
    PktMonDrop_NetCx_LSOFailure = 0x5E1,
    PktMonDrop_NetCx_USOFailure = 0x5E2,
    PktMonDrop_NetCx_BufferBounceFailureAndPacketIgnore = 0x5E3
} PKTMON_DROP_REASON;

DECLARE_HANDLE(PKTMON_LOWEREDGE_HANDLE);
DECLARE_HANDLE(PKTMON_COMPONENT_HANDLE);

/* Owned by the caller, filled in by the library. */
typedef struct DECLSPEC_ALIGN(8) _PKTMON_COMPONENT_CONTEXT
{
    LIST_ENTRY ListLink;
    LIST_ENTRY EdgeList;
    LONG EdgeCount;
    PVOID CompHandle;
    PKTMON_COMPONENT_TYPE CompType;
    PKTMON_PACKET_TYPE PacketType;
    LONG FlowEnabled : 1;
    LONG DropEnabled : 1;
} PKTMON_COMPONENT_CONTEXT, *PPKTMON_COMPONENT_CONTEXT;

typedef struct DECLSPEC_ALIGN(8) _PKTMON_EDGE_CONTEXT
{
    LIST_ENTRY ListLink;
    PVOID EdgeHandle;
    PKTMON_COMPONENT_CONTEXT *CompContext;
    PKTMON_PACKET_TYPE PacketType;
} PKTMON_EDGE_CONTEXT, *PPKTMON_EDGE_CONTEXT;

struct _PKTMON_PACKET_HEADER_INFO;
struct _PKTMON_PROVIDER_DISPATCH;

/* Runs when the packet monitor comes or goes, so components can re-register. */
typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(*PKTMON_CLIENT_ENUMERATE_CALLBACK)(
    VOID);

/* The library's attachment to the provider. Enabled is set while one is attached. */
typedef struct _PKTMON_CLIENT_CONTEXT
{
    PVOID NmrClientHandle;
    PEX_RUNDOWN_REF_CACHE_AWARE RundownRef;
    BOOLEAN Enabled;
    PKTMON_CLIENT_ENUMERATE_CALLBACK EnumComponents;
    PKTMON_CLIENT_ENUMERATE_CALLBACK CleanupComponents;
    VOID (*NotifyComponent)(PKTMON_COMPONENT_CONTEXT *CompContext);
    PVOID ProviderContext;
    struct _PKTMON_PROVIDER_DISPATCH *ProviderDispatch;
} PKTMON_CLIENT_CONTEXT;

extern PKTMON_CLIENT_CONTEXT PktMon;

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
PktMonClientInitializeEx(
    _In_ const NPI_MODULEID *ModuleId,
    _In_opt_ PKTMON_CLIENT_ENUMERATE_CALLBACK EnumerateAndRegister,
    _In_opt_ PKTMON_CLIENT_ENUMERATE_CALLBACK EnumerateAndUnregister);

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
PktMonClientUninitialize(
    VOID);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
PktMonClientComponentRegister(
    _Inout_ PKTMON_COMPONENT_CONTEXT *CompContext,
    _In_ PUNICODE_STRING Name,
    _In_ PUNICODE_STRING Description,
    _In_ PKTMON_COMPONENT_TYPE Type,
    _In_ NDIS_MEDIUM MediaType,
    _In_ PKTMON_DIRECTION_TAG DirTagIn,
    _In_ PKTMON_DIRECTION_TAG DirTagOut);

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
PktMonClientComponentUnregister(
    _Inout_ PKTMON_COMPONENT_CONTEXT *CompContext);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
PktMonClientAddEdge(
    _Inout_ PKTMON_COMPONENT_CONTEXT *CompContext,
    _In_ PUNICODE_STRING Name,
    _In_ PKTMON_DIRECTION_TAG DirTagIn,
    _In_ PKTMON_DIRECTION_TAG DirTagOut,
    _In_ NDIS_MEDIUM MediaType,
    _Out_ PKTMON_EDGE_CONTEXT *EdgeContext);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
PktMonClientSetCompProperty(
    _In_ PKTMON_COMPONENT_CONTEXT *CompContext,
    _In_ PKTMON_COMPONENT_PROPERTY_ID Id,
    _In_reads_bytes_(Size) PVOID Value,
    _In_ USHORT Size);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
PktMonClientNblLog(
    _In_ PKTMON_EDGE_CONTEXT *EdgeContext,
    _In_ NET_BUFFER_LIST *NetBufferList,
    _In_ PKTMON_PACKET_TYPE PacketType,
    _In_opt_ struct _PKTMON_PACKET_HEADER_INFO *PacketHeaderInfo,
    _In_ BOOLEAN UseOnlyFirstNbl,
    _In_ PKTMON_DIRECTION Direction);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
PktMonClientNblDrop(
    _In_ PKTMON_COMPONENT_CONTEXT *CompContext,
    _In_ NET_BUFFER_LIST *NetBufferList,
    _In_ PKTMON_PACKET_TYPE PacketType,
    _In_opt_ struct _PKTMON_PACKET_HEADER_INFO *PacketHeaderInfo,
    _In_ BOOLEAN UseOnlyFirstNbl,
    _In_ PKTMON_DIRECTION Direction,
    _In_ PKTMON_DROP_REASON DropReason,
    _In_ ULONG LocationCode);

/*
 * Report one NBL, or a whole chain as NDIS sees it. Nothing is called unless a
 * provider is attached and has turned on flow or drop reporting for the
 * component. These expand to a braced statement, so callers may leave off the
 * trailing semicolon.
 */
#define PKTMON_FLOW_ENABLED(EdgeContext) \
    (PktMon.Enabled && (EdgeContext)->CompContext != NULL && (EdgeContext)->CompContext->FlowEnabled)

#define PKTMON_LOG_NBL(EdgeContext, Nbl, PacketType, UseOnlyFirstNbl, Direction) \
    if (PKTMON_FLOW_ENABLED(EdgeContext)) \
    { \
        PktMonClientNblLog((EdgeContext), (Nbl), (PacketType), NULL, (UseOnlyFirstNbl), (Direction)); \
    }

#define PKTMON_LOG_NBL_NDIS(EdgeContext, Nbl, Direction) \
    if (PKTMON_FLOW_ENABLED(EdgeContext)) \
    { \
        PktMonClientNblLog((EdgeContext), (Nbl), (EdgeContext)->PacketType, NULL, FALSE, (Direction)); \
    }

#define PKTMON_DROP_NBL(CompContext, Nbl, Direction, DropReason, Location) \
    if (PktMon.Enabled && (CompContext)->DropEnabled) \
    { \
        PktMonClientNblDrop((CompContext), (Nbl), (CompContext)->PacketType, NULL, TRUE, \
                            (Direction), (DropReason), (ULONG)(Location)); \
    }

#ifdef __cplusplus
}
#endif
