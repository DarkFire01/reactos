/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Software receive segment coalescing library, stubbed
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * RSCLIB is linked from a Microsoft internal source tree. It is in no WDK and
 * not in the shipping netadaptercx.sys, so there is nothing to port from.
 *
 * Coalescing is an optimisation, so passing every chain through untouched is
 * correct, just slower. Each entry point says so on the debug port because a
 * silent pass through looks exactly like hardware RSC being unavailable.
 */

#include "NxXlatPrecomp.hpp"

#include <rsclib.h>

#define NDEBUG
#include <debug.h>

_Use_decl_annotations_
VOID
RSCLIBInitializeContext(
    PVOID Cookie,
    RSCLIB_SERIAL_COALESCING_CONTEXT *Context)
{
    DPRINT1("RSCLIB stub: RSCLIBInitializeContext, receive coalescing is not implemented\n");

    RtlZeroMemory(Context, sizeof(*Context));
    Context->Cookie = Cookie;
}

_Use_decl_annotations_
VOID
RSCLIBReadAndResetStats(
    RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    RSCLIB_STATS *Stats)
{
    UNREFERENCED_PARAMETER(Context);

    RtlZeroMemory(Stats, sizeof(*Stats));
}

/*
 * Nothing is held back, so the chain the caller handed over is appended as it
 * stands and the context stays empty.
 */
_Use_decl_annotations_
VOID
RSCLIBCoalesceNBL(
    RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    UCHAR IpHeaderOffset,
    ULONG Flags,
    ULONG Reserved,
    NET_BUFFER_LIST *NetBufferList,
    NET_BUFFER_LIST ***TailNbl,
    ULONG *NumberOfNbls)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(IpHeaderOffset);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Reserved);

    DPRINT1("RSCLIB stub: RSCLIBCoalesceNBL, the chain is passed through uncoalesced\n");

    NetBufferList->Next = nullptr;
    **TailNbl = NetBufferList;
    *TailNbl = &NetBufferList->Next;
    *NumberOfNbls += 1;
}

_Use_decl_annotations_
VOID
RSCLIBFlushContext(
    RSCLIB_SERIAL_COALESCING_CONTEXT *Context,
    NET_BUFFER_LIST ***TailNbl,
    ULONG *NumberOfNbls)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(TailNbl);
    UNREFERENCED_PARAMETER(NumberOfNbls);
}

_Use_decl_annotations_
BOOLEAN
RscLibExUtilMatchCoalescedNBLCookie(
    const NET_BUFFER_LIST *NetBufferList,
    const VOID *Cookie)
{
    UNREFERENCED_PARAMETER(NetBufferList);
    UNREFERENCED_PARAMETER(Cookie);

    /* Nothing was coalesced here, so no chain can be ours to take apart */
    return FALSE;
}

_Use_decl_annotations_
VOID
RscLibExUncoalesceNBL(
    NET_BUFFER_LIST *NetBufferList,
    const VOID *Cookie,
    NET_BUFFER_LIST **OutputNblChainTail,
    ULONG *NumberOfNbls)
{
    UNREFERENCED_PARAMETER(Cookie);

    DPRINT1("RSCLIB stub: RscLibExUncoalesceNBL called for a chain we did not coalesce\n");

    *OutputNblChainTail = NetBufferList;
    *NumberOfNbls = 1;
}
