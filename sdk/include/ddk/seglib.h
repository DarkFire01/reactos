/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Software segmentation and checksum library
 *
 * The fallback that performs LSO, USO and checksum in software when the
 * hardware cannot. Microsoft links it from an internal source tree, so
 * sdk/lib/drivers/seglib is a stub that announces itself and fails.
 */

#pragma once

#include <batchinglib.h>

#define SEGLIB_OFFLOAD_PERFORM_NONE     0x00000000
#define SEGLIB_OFFLOAD_PERFORM_LSO      0x00000001
#define SEGLIB_OFFLOAD_PERFORM_USO      0x00000002
#define SEGLIB_OFFLOAD_PERFORM_CSO      0x00000004

#define SEGLIB_OFFLOAD_OVERRIDE_INFO_CSO_SET    0x00000001

#ifdef __cplusplus

enum class SEGLIB_NBL_DIRECTION
{
    TxtoTx,
    RxtoRx
};

typedef struct _SEGLIB_CONTEXT
{
    BATCHING_LIB_CONTEXT *BatchingContext;
    PVOID Reserved;
} SEGLIB_CONTEXT;

typedef struct _SEGLIB_CHKSUM_OFFLOAD_INFO
{
    bool PerformChecksumOffload;
} SEGLIB_CHKSUM_OFFLOAD_INFO;

typedef struct _SEGLIB_OFFLOAD_OVERRIDE_INFO
{
    ULONG OffloadDataSet;
    SEGLIB_CHKSUM_OFFLOAD_INFO ChecksumOffloadInfo;
    SEGLIB_NBL_DIRECTION NblDirection;
} SEGLIB_OFFLOAD_OVERRIDE_INFO;

struct _NET_BUFFER_LIST;
struct _NET_BUFFER;

VOID
SegLibInitialize(
    _In_ BATCHING_LIB_CONTEXT *BatchingContext,
    _Out_ SEGLIB_CONTEXT *Context);

VOID
SegLibUninitialize(
    _Inout_ SEGLIB_CONTEXT *Context);

NTSTATUS
SegLibCreateMultiNbNblClone(
    _In_ SEGLIB_CONTEXT *Context,
    _In_ struct _NET_BUFFER_LIST *SourceNbl,
    _In_ bool Reserved1,
    _In_ UINT8 Layer3HeaderOffset,
    _In_ SEGLIB_OFFLOAD_OVERRIDE_INFO *OverrideInfo,
    _In_opt_ PVOID Reserved2,
    _In_opt_ PVOID Reserved3,
    _In_ ULONG Reserved4,
    _In_ ULONG Reserved5,
    _Out_ struct _NET_BUFFER_LIST **OutputNbl,
    _Out_ ULONG *DataOffset,
    _Out_ ULONG *OffloadsPerformed);

NTSTATUS
SegLibCreateMultiNbNblUsoClone(
    _In_ SEGLIB_CONTEXT *Context,
    _In_ struct _NET_BUFFER_LIST *SourceNbl,
    _In_ bool Reserved1,
    _In_ UINT8 Layer3HeaderOffset,
    _In_ SEGLIB_OFFLOAD_OVERRIDE_INFO *OverrideInfo,
    _In_opt_ PVOID Reserved2,
    _In_opt_ PVOID Reserved3,
    _In_ ULONG Reserved4,
    _In_ ULONG Reserved5,
    _Out_ struct _NET_BUFFER_LIST **OutputNbl,
    _Out_ ULONG *DataOffset,
    _Out_ ULONG *OffloadsPerformed);

VOID
SegLibFreeMultiNbNblClone(
    _In_ struct _NET_BUFFER_LIST *NetBufferList,
    _In_ ULONG DataOffset);

NTSTATUS
SegLibDeferredChecksumPacket(
    _In_ BATCHING_BUFFER_CONTEXT *BufferContext,
    _In_ struct _NET_BUFFER *NetBuffer,
    _In_ NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO const &ChecksumInfo,
    _In_ UINT8 Layer3HeaderOffset,
    _In_ UINT16 Layer4HeaderOffset,
    _In_ ULONG IsIPv6);

#endif /* __cplusplus */
