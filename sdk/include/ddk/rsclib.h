/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Software receive segment coalescing library
 *
 * The coalescing context is opaque to callers, so its shape is ours to pick.
 */

#pragma once

#include <ndis.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RSCLIBCoalesceNBL Flags */
#define RSCLIB_FLAG_ALLOW_DEFERRED_CHECKSUM     0x00000001

typedef struct _RSCLIB_SERIAL_COALESCING_CONTEXT
{
    PVOID Cookie;
    NET_BUFFER_LIST *PendingNblChain;
    ULONG PendingNblCount;
    ULONG Reserved;
} RSCLIB_SERIAL_COALESCING_CONTEXT, *PRSCLIB_SERIAL_COALESCING_CONTEXT;

typedef struct _RSCLIB_STATS
{
    USHORT CoalescedSegments;
    USHORT GeneratedSCUs;
    ULONG CoalescedTransportPayloadBytes;
} RSCLIB_STATS, *PRSCLIB_STATS;

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RSCLIBInitializeContext(
    _In_ PVOID Cookie,
    _Out_ RSCLIB_SERIAL_COALESCING_CONTEXT *Context);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RSCLIBReadAndResetStats(
    _Inout_ RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    _Out_ RSCLIB_STATS *Stats);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RSCLIBCoalesceNBL(
    _Inout_ RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    _In_ UCHAR IpHeaderOffset,
    _In_ ULONG Flags,
    _In_ ULONG Reserved,
    _In_ NET_BUFFER_LIST *NetBufferList,
    _Inout_ NET_BUFFER_LIST ***TailNbl,
    _Inout_ ULONG *NumberOfNbls);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RSCLIBFlushContext(
    _Inout_ RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    _Inout_ NET_BUFFER_LIST ***TailNbl,
    _Inout_ ULONG *NumberOfNbls);

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
RscLibExUtilMatchCoalescedNBLCookie(
    _In_ const NET_BUFFER_LIST *NetBufferList,
    _In_ const VOID *Cookie);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RscLibExUncoalesceNBL(
    _In_ NET_BUFFER_LIST *NetBufferList,
    _In_ const VOID *Cookie,
    _Outptr_ NET_BUFFER_LIST **OutputNblChainTail,
    _Out_ ULONG *NumberOfNbls);

#ifdef __cplusplus
}
#endif
