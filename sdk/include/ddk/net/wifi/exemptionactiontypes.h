/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.11 exemption action packet extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_PACKET_WIFI_EXEMPTION_ACTION
{
    UINT8 ExemptionAction;
} NET_PACKET_WIFI_EXEMPTION_ACTION;

C_ASSERT(sizeof(NET_PACKET_WIFI_EXEMPTION_ACTION) == sizeof(UINT8));

#define NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_NAME         L"ms_packet_wifiexemptionaction"
#define NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_VERSION_1    1U

#ifdef __cplusplus
}
#endif
