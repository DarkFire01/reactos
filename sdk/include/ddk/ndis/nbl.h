/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER and NET_BUFFER_LIST definitions
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_DEFAULT                0x80

struct _NET_BUFFER;
struct _NET_BUFFER_LIST;
struct _NET_BUFFER_LIST_CONTEXT;
struct _NET_BUFFER_SHARED_MEMORY;

/*
 * Slot indices into NET_BUFFER_LIST::NetBufferListInfo, part of the miniport ABI.
 * 64 bit builds carry the switch and GFT slots in the middle, x86 gets them at
 * the end from 6.82 on.
 */
typedef enum _NDIS_NET_BUFFER_LIST_INFO
{
    TcpIpChecksumNetBufferListInfo,
    TcpOffloadBytesTransferred = TcpIpChecksumNetBufferListInfo,
    IPsecOffloadV1NetBufferListInfo,
#if NDIS_SUPPORT_NDIS61
    IPsecOffloadV2NetBufferListInfo = IPsecOffloadV1NetBufferListInfo,
#endif
    TcpLargeSendNetBufferListInfo,
    TcpReceiveNoPush = TcpLargeSendNetBufferListInfo,
    ClassificationHandleNetBufferListInfo,
    Ieee8021QNetBufferListInfo,
    NetBufferListCancelId,
    MediaSpecificInformation,
    NetBufferListFrameType,
    NetBufferListProtocolId = NetBufferListFrameType,
    NetBufferListHashValue,
    NetBufferListHashInfo,
    WfpNetBufferListInfo,
#if NDIS_SUPPORT_NDIS61
    IPsecOffloadV2TunnelNetBufferListInfo,
    IPsecOffloadV2HeaderNetBufferListInfo,
#endif
#if NDIS_SUPPORT_NDIS620
    NetBufferListCorrelationId,
    NetBufferListFilteringInfo,
    MediaSpecificInformationEx,
    NblOriginalInterfaceIfIndex,
    NblReAuthWfpFlowContext = NblOriginalInterfaceIfIndex,
    TcpReceiveBytesTransferred,
    NrtNameResolutionId = TcpReceiveBytesTransferred,
#if NDIS_SUPPORT_NDIS684
    UdpRecvSegCoalesceOffloadInfo = TcpReceiveBytesTransferred,
#endif
#if NDIS_SUPPORT_NDIS630
#if defined(_AMD64_) || defined(_ARM64_)
    SwitchForwardingReserved,
    SwitchForwardingDetail,
    VirtualSubnetInfo,
#endif
    IMReserved,
    TcpRecvSegCoalesceInfo,
#if NDIS_SUPPORT_NDIS683
    UdpSegmentationOffloadInfo = TcpRecvSegCoalesceInfo,
#endif
    RscTcpTimestampDelta,
    TcpSendOffloadsSupplementalNetBufferListInfo = RscTcpTimestampDelta,
#if NDIS_SUPPORT_NDIS650
#if defined(_AMD64_) || defined(_ARM64_)
    GftOffloadInformation,
    GftFlowEntryId,
#endif
#if NDIS_SUPPORT_NDIS680
    NetBufferListInfoReserved3,
#ifndef _WIN64
    NetBufferListInfoReserved4,
#endif
#endif
#endif
#endif
#endif
#if NDIS_SUPPORT_NDIS682
#if !defined(_AMD64_) && !defined(_ARM64_)
    SwitchForwardingReserved,
    SwitchForwardingDetail_b0_to_b31,
    SwitchForwardingDetail_b32_to_b63,
    VirtualSubnetInfo,
#endif
#endif
#if NDIS_WRAPPER == 1
    NetBufferListInfoReserved1,
    NetBufferListInfoReserved2,
#endif
    MaxNetBufferListInfo
} NDIS_NET_BUFFER_LIST_INFO, *PNDIS_NET_BUFFER_LIST_INFO;

typedef struct _NET_BUFFER_DATA
{
    struct _NET_BUFFER *Next;
    PMDL CurrentMdl;
    ULONG CurrentMdlOffset;
    union
    {
        ULONG DataLength;
        SIZE_T stDataLength;
    };
    PMDL MdlChain;
    ULONG DataOffset;
} NET_BUFFER_DATA, *PNET_BUFFER_DATA;

typedef union _NET_BUFFER_HEADER
{
    NET_BUFFER_DATA NetBufferData;
    SLIST_HEADER Link;
} NET_BUFFER_HEADER, *PNET_BUFFER_HEADER;

typedef struct DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) _NET_BUFFER
{
    union
    {
        struct
        {
            struct _NET_BUFFER *Next;
            PMDL CurrentMdl;
            ULONG CurrentMdlOffset;
            union
            {
                ULONG DataLength;
                SIZE_T stDataLength;
            };
            PMDL MdlChain;
            ULONG DataOffset;
        };
        NET_BUFFER_HEADER NetBufferHeader;
    };
    USHORT ChecksumBias;
    USHORT Reserved;
    NDIS_HANDLE NdisPoolHandle;
    PVOID NdisReserved[2];
    PVOID ProtocolReserved[6];
    PVOID MiniportReserved[4];
    NDIS_PHYSICAL_ADDRESS DataPhysicalAddress;
    union
    {
        struct _NET_BUFFER_SHARED_MEMORY *SharedMemoryInfo;
        PSCATTER_GATHER_LIST ScatterGatherList;
    };
} NET_BUFFER, *PNET_BUFFER;

typedef struct _NET_BUFFER_LIST_CONTEXT
{
    struct _NET_BUFFER_LIST_CONTEXT *Next;
    USHORT Size;
    USHORT Offset;
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) UCHAR ContextData[];
} NET_BUFFER_LIST_CONTEXT, *PNET_BUFFER_LIST_CONTEXT;

typedef struct _NET_BUFFER_LIST_DATA
{
    struct _NET_BUFFER_LIST *Next;
    PNET_BUFFER FirstNetBuffer;
} NET_BUFFER_LIST_DATA, *PNET_BUFFER_LIST_DATA;

typedef union _NET_BUFFER_LIST_HEADER
{
    NET_BUFFER_LIST_DATA NetBufferListData;
    SLIST_HEADER Link;
} NET_BUFFER_LIST_HEADER, *PNET_BUFFER_LIST_HEADER;

typedef struct _NET_BUFFER_LIST
{
    union
    {
        struct
        {
            struct _NET_BUFFER_LIST *Next;
            PNET_BUFFER FirstNetBuffer;
        };
        NET_BUFFER_LIST_HEADER NetBufferListHeader;
    };
    PNET_BUFFER_LIST_CONTEXT Context;
    struct _NET_BUFFER_LIST *ParentNetBufferList;
    NDIS_HANDLE NdisPoolHandle;
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) PVOID NdisReserved[2];
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) PVOID ProtocolReserved[4];
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) PVOID MiniportReserved[2];
    PVOID Scratch;
    NDIS_HANDLE SourceHandle;
    ULONG NblFlags;
    LONG ChildRefCount;
    ULONG Flags;
    union
    {
        NDIS_STATUS Status;
        ULONG NdisReserved2;
    };
    PVOID NetBufferListInfo[MaxNetBufferListInfo];
} NET_BUFFER_LIST, *PNET_BUFFER_LIST;

/* Asked for at least *BufferSize bytes; may hand back more and says how many */
typedef PMDL
(NTAPI NET_BUFFER_ALLOCATE_MDL_HANDLER)(
    _Inout_ PULONG BufferSize);

typedef NET_BUFFER_ALLOCATE_MDL_HANDLER *PNET_BUFFER_ALLOCATE_MDL_HANDLER;

typedef VOID
(NTAPI NET_BUFFER_FREE_MDL_HANDLER)(
    _In_ PMDL Mdl);

typedef NET_BUFFER_FREE_MDL_HANDLER *PNET_BUFFER_FREE_MDL_HANDLER;

typedef struct _NET_BUFFER_POOL_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    ULONG PoolTag;
    ULONG DataSize;
} NET_BUFFER_POOL_PARAMETERS, *PNET_BUFFER_POOL_PARAMETERS;

typedef struct _NET_BUFFER_LIST_POOL_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    UCHAR ProtocolId;
    BOOLEAN fAllocateNetBuffer;
    USHORT ContextSize;
    ULONG PoolTag;
    ULONG DataSize;
} NET_BUFFER_LIST_POOL_PARAMETERS, *PNET_BUFFER_LIST_POOL_PARAMETERS;

#define NET_BUFFER_POOL_PARAMETERS_REVISION_1           1
#define NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1      1

#define NDIS_SIZEOF_NET_BUFFER_POOL_PARAMETERS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NET_BUFFER_POOL_PARAMETERS, DataSize)

#define NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NET_BUFFER_LIST_POOL_PARAMETERS, DataSize)

/* NET_BUFFER_LIST::NblFlags, split by who owns which bits */
#define NBL_FLAGS_PROTOCOL_RESERVED             0xFFF00000
#define NBL_FLAGS_SCRATCH                       0x000F0000
#define NBL_FLAGS_MINIPORT_RESERVED             0x0000F000
#define NBL_FLAGS_NDIS_RESERVED                 0x00000FFF

#define NDIS_NBL_FLAGS_SEND_READ_ONLY           0x00000001
#define NDIS_NBL_FLAGS_RECV_READ_ONLY           0x00000002
#define NDIS_NBL_FLAGS_HD_SPLIT                 0x00000100
#define NDIS_NBL_FLAGS_IS_IPV4                  0x00000200
#define NDIS_NBL_FLAGS_IS_IPV6                  0x00000400
#define NDIS_NBL_FLAGS_IS_TCP                   0x00000800
#define NDIS_NBL_FLAGS_IS_UDP                   0x00001000
#define NDIS_NBL_FLAGS_SPLIT_AT_UPPER_LAYER_PROTOCOL_HEADER  0x00002000
#define NDIS_NBL_FLAGS_SPLIT_AT_UPPER_LAYER_PROTOCOL_PAYLOAD 0x00004000
#define NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET       0x00008000

/*
 * Clone flags. The allocate and the free side test the same bit, so a clone
 * built over the original MDLs has to be freed with the flag set as well or
 * the original's MDLs are freed out from under it.
 */
#define NDIS_CLONE_FLAGS_USE_ORIGINAL_MDLS      0x00000002

#ifdef __cplusplus
}
#endif
