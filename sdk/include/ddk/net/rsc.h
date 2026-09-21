/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive segment coalescing packet extension accessors
 */

#pragma once

#include <net/rsctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_PACKET_RSC *
NetExtensionGetPacketRsc(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_RSC *)NetExtensionGetData(Extension, Index);
}

FORCEINLINE
NET_PACKET_RSC_TIMESTAMP *
NetExtensionGetPacketRscTimestamp(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_RSC_TIMESTAMP *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
