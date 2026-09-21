/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Generic segmentation offload packet extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_PACKET_GSO
{
    union
    {
        struct
        {
            UINT32 Mss : 20;
            UINT32 Reserved0 : 12;
        } TCP;
        struct
        {
            UINT32 Mss : 20;
            UINT32 Reserved0 : 12;
        } UDP;
    } DUMMYUNIONNAME;
} NET_PACKET_GSO;

C_ASSERT(sizeof(NET_PACKET_GSO) == 4);

#define NET_PACKET_EXTENSION_GSO_NAME       L"ms_packet_gso"
#define NET_PACKET_EXTENSION_GSO_VERSION_1  1U

#ifdef __cplusplus
}
#endif
