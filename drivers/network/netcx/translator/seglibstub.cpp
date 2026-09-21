/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Software segmentation and checksum library, stubbed
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * SegLib and its batching helper are linked from a Microsoft internal source
 * tree. They are in no WDK and not in the shipping netadaptercx.sys, so there
 * is nothing to port from.
 *
 * Every entry point here says so on the debug port rather than failing
 * quietly, because the callers treat a failure as "the hardware cannot do
 * this offload" and carry on. Without the announcement, software LSO, USO and
 * checksum would simply never happen and the only symptom would be lost
 * throughput.
 *
 * Replacing this means implementing real segmentation over an NBL chain and
 * the TCP, UDP and IP checksums.
 */

#include "NxXlatPrecomp.hpp"

#include <BatchingLib.h>
#include <SegLib.h>

#define NDEBUG
#include <debug.h>

VOID
BLInitialize(
    BATCHING_LIB_CONTEXT *Context)
{
    DPRINT1("SegLib stub: BLInitialize, batched buffer operations are not implemented\n");

    if (Context != nullptr)
        Context->Reserved = nullptr;
}

VOID
BLUninitialize(
    BATCHING_LIB_CONTEXT *Context)
{
    UNREFERENCED_PARAMETER(Context);

    DPRINT1("SegLib stub: BLUninitialize\n");
}

VOID
BLInitializeBatchOpContext(
    BATCHING_LIB_CONTEXT *Context,
    BATCHING_BUFFER_CONTEXT *BufferContext,
    BOOLEAN Reserved1,
    BOOLEAN Reserved2,
    PVOID Reserved3)
{
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);

    DPRINT1("SegLib stub: BLInitializeBatchOpContext\n");

    if (BufferContext != nullptr)
    {
        BufferContext->Context = Context;
        BufferContext->Reserved = nullptr;
    }
}

VOID
BLFlushBatchOpContext(
    BATCHING_BUFFER_CONTEXT *BufferContext)
{
    UNREFERENCED_PARAMETER(BufferContext);

    DPRINT1("SegLib stub: BLFlushBatchOpContext\n");
}

VOID
SegLibInitialize(
    BATCHING_LIB_CONTEXT *BatchingContext,
    SEGLIB_CONTEXT *Context)
{
    DPRINT1("SegLib stub: SegLibInitialize, software segmentation is not implemented\n");

    if (Context != nullptr)
    {
        Context->BatchingContext = BatchingContext;
        Context->Reserved = nullptr;
    }
}

VOID
SegLibUninitialize(
    SEGLIB_CONTEXT *Context)
{
    UNREFERENCED_PARAMETER(Context);

    DPRINT1("SegLib stub: SegLibUninitialize\n");
}

/*
 * The caller asserts that the requested offload was performed, so returning
 * success with no output would bugcheck. Failing is the honest answer.
 */
NTSTATUS
SegLibCreateMultiNbNblClone(
    SEGLIB_CONTEXT *Context,
    struct _NET_BUFFER_LIST *SourceNbl,
    bool Reserved1,
    UINT8 Layer3HeaderOffset,
    SEGLIB_OFFLOAD_OVERRIDE_INFO *OverrideInfo,
    PVOID Reserved2,
    PVOID Reserved3,
    ULONG Reserved4,
    ULONG Reserved5,
    struct _NET_BUFFER_LIST **OutputNbl,
    ULONG *DataOffset,
    ULONG *OffloadsPerformed)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SourceNbl);
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Layer3HeaderOffset);
    UNREFERENCED_PARAMETER(OverrideInfo);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);
    UNREFERENCED_PARAMETER(Reserved5);

    DPRINT1("SegLib stub: SegLibCreateMultiNbNblClone, software LSO is not implemented\n");

    if (OutputNbl != nullptr)
        *OutputNbl = nullptr;
    if (DataOffset != nullptr)
        *DataOffset = 0;
    if (OffloadsPerformed != nullptr)
        *OffloadsPerformed = SEGLIB_OFFLOAD_PERFORM_NONE;

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
SegLibCreateMultiNbNblUsoClone(
    SEGLIB_CONTEXT *Context,
    struct _NET_BUFFER_LIST *SourceNbl,
    bool Reserved1,
    UINT8 Layer3HeaderOffset,
    SEGLIB_OFFLOAD_OVERRIDE_INFO *OverrideInfo,
    PVOID Reserved2,
    PVOID Reserved3,
    ULONG Reserved4,
    ULONG Reserved5,
    struct _NET_BUFFER_LIST **OutputNbl,
    ULONG *DataOffset,
    ULONG *OffloadsPerformed)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SourceNbl);
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Layer3HeaderOffset);
    UNREFERENCED_PARAMETER(OverrideInfo);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);
    UNREFERENCED_PARAMETER(Reserved5);

    DPRINT1("SegLib stub: SegLibCreateMultiNbNblUsoClone, software USO is not implemented\n");

    if (OutputNbl != nullptr)
        *OutputNbl = nullptr;
    if (DataOffset != nullptr)
        *DataOffset = 0;
    if (OffloadsPerformed != nullptr)
        *OffloadsPerformed = SEGLIB_OFFLOAD_PERFORM_NONE;

    return STATUS_NOT_IMPLEMENTED;
}

VOID
SegLibFreeMultiNbNblClone(
    struct _NET_BUFFER_LIST *NetBufferList,
    ULONG DataOffset)
{
    UNREFERENCED_PARAMETER(NetBufferList);
    UNREFERENCED_PARAMETER(DataOffset);

    DPRINT1("SegLib stub: SegLibFreeMultiNbNblClone, nothing was ever cloned\n");
}

NTSTATUS
SegLibDeferredChecksumPacket(
    BATCHING_BUFFER_CONTEXT *BufferContext,
    struct _NET_BUFFER *NetBuffer,
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO const &ChecksumInfo,
    UINT8 Layer3HeaderOffset,
    UINT16 Layer4HeaderOffset,
    ULONG IsIPv6)
{
    UNREFERENCED_PARAMETER(BufferContext);
    UNREFERENCED_PARAMETER(NetBuffer);
    UNREFERENCED_PARAMETER(ChecksumInfo);
    UNREFERENCED_PARAMETER(Layer3HeaderOffset);
    UNREFERENCED_PARAMETER(Layer4HeaderOffset);
    UNREFERENCED_PARAMETER(IsIPv6);

    DPRINT1("SegLib stub: SegLibDeferredChecksumPacket, software checksum is not implemented\n");

    return STATUS_NOT_IMPLEMENTED;
}
