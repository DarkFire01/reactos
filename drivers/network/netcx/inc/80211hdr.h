/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IEEE 802.11 MAC header
 *
 * Only the leading part every frame type shares. Bit fields are in the order
 * the standard puts them in on the wire, least significant bit first.
 */

#pragma once

#include <pshpack1.h>

typedef struct DOT11_FRAME_CTRL
{
    USHORT Version : 2;
    USHORT Type : 2;
    USHORT Subtype : 4;
    USHORT ToDS : 1;
    USHORT FromDS : 1;
    USHORT MoreFrag : 1;
    USHORT Retry : 1;
    USHORT PwrMgt : 1;
    USHORT MoreData : 1;
    USHORT WEP : 1;
    USHORT Order : 1;
} DOT11_FRAME_CTRL, *PDOT11_FRAME_CTRL;

/* Address1 is the receiver, which on transmit is the peer the frame is for. */
typedef struct DOT11_MAC_HEADER
{
    DOT11_FRAME_CTRL FrameControl;
    USHORT DurationID;
    UCHAR Address1[6];
} DOT11_MAC_HEADER, *PDOT11_MAC_HEADER;

#include <poppack.h>
