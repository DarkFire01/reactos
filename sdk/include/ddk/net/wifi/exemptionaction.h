/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.11 exemption action packet extension accessor
 */

#pragma once

#include <net/wifi/exemptionactiontypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_PACKET_WIFI_EXEMPTION_ACTION *
WifiExtensionGetExemptionAction(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_WIFI_EXEMPTION_ACTION *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
