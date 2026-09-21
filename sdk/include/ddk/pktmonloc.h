/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet monitor drop location codes
 *
 * Each code names the place in a component a packet was dropped. Only the
 * codes the class extension reports are listed.
 */

#pragma once

typedef enum _PKTMON_DROP_LOCATION
{
    PMLOC_NETCX_NETPACKET_LAYOUT_PARSE_FAIL = 0xE0009001,
    PMLOC_NETCX_SOFTWARE_CHECKSUM_FAILURE = 0xE0009002,
    PMLOC_NETCX_NIC_QUEUE_STOP = 0xE0009003,
    PMLOC_NETCX_INVALID_NETBUFFER_LENGTH = 0xE0009004,
    PMLOC_NETCX_NBL_LSO_FAILURE = 0xE0009005,
    PMLOC_NETCX_NBL_USO_FAILURE = 0xE0009006,
    PMLOC_NETCX_BUFFER_BOUNCE_FAILURE = 0xE0009007
} PKTMON_DROP_LOCATION;
