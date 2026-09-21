/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.1Q packet extension accessor
 */

#pragma once

#include <net/ieee8021qtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_PACKET_IEEE8021Q *
NetExtensionGetPacketIeee8021Q(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_IEEE8021Q *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
