/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Generic segmentation offload packet extension accessor
 */

#pragma once

#include <net/gsotypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_PACKET_GSO *
NetExtensionGetPacketGso(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_GSO *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
