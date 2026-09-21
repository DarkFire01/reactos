/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A packet within a packet ring
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_PACKET_LAYER2_TYPE
{
    NetPacketLayer2TypeUnspecified,
    NetPacketLayer2TypeNull,
    NetPacketLayer2TypeEthernet,
    NetPacketLayer2TypeIeee80211
} NET_PACKET_LAYER2_TYPE;

typedef enum _NET_PACKET_LAYER3_TYPE
{
    NetPacketLayer3TypeUnspecified,
    NetPacketLayer3TypeIPv4UnspecifiedOptions,
    NetPacketLayer3TypeIPv4WithOptions,
    NetPacketLayer3TypeIPv4NoOptions,
    NetPacketLayer3TypeIPv6UnspecifiedExtensions,
    NetPacketLayer3TypeIPv6WithExtensions,
    NetPacketLayer3TypeIPv6NoExtensions
} NET_PACKET_LAYER3_TYPE;

typedef enum _NET_PACKET_LAYER4_TYPE
{
    NetPacketLayer4TypeUnspecified,
    NetPacketLayer4TypeTcp,
    NetPacketLayer4TypeUdp,
    NetPacketLayer4TypeIPFragment,
    NetPacketLayer4TypeIPNotFragment
} NET_PACKET_LAYER4_TYPE;

#include <pshpack1.h>
typedef struct _NET_PACKET_LAYOUT
{
    UINT16 Layer2HeaderLength : 7;
    UINT16 Layer3HeaderLength : 9;
    UINT8 Layer4HeaderLength : 8;
    UINT8 Layer2Type : 4;
    UINT8 Layer3Type : 4;
    UINT8 Layer4Type : 4;
    UINT8 Reserved0 : 4;
} NET_PACKET_LAYOUT;
#include <poppack.h>

C_ASSERT(sizeof(NET_PACKET_LAYOUT) == 5);

typedef struct _NET_PACKET
{
    UINT32 FragmentIndex;
    UINT16 FragmentCount;
    NET_PACKET_LAYOUT Layout;
    UINT8 Ignore : 1;
    UINT8 Scratch : 1;
    UINT8 Reserved1 : 6;
} NET_PACKET;

C_ASSERT(sizeof(NET_PACKET) == 12);

FORCEINLINE
BOOLEAN
NetPacketIsIpv4(
    _In_ const NET_PACKET *Packet)
{
    return Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv4NoOptions ||
           Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv4UnspecifiedOptions ||
           Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv4WithOptions;
}

FORCEINLINE
BOOLEAN
NetPacketIsIpv6(
    _In_ const NET_PACKET *Packet)
{
    return Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv6NoExtensions ||
           Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv6UnspecifiedExtensions ||
           Packet->Layout.Layer3Type == NetPacketLayer3TypeIPv6WithExtensions;
}

FORCEINLINE
PVOID
NetPacketGetExtension(
    _In_ const NET_PACKET *Packet,
    _In_ SIZE_T Offset)
{
    return (PVOID)((PUCHAR)Packet + Offset);
}

#ifdef __cplusplus
}
#endif
