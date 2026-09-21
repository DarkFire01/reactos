/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the receive coalescing extension types
 */

#pragma once

#include <net/rsctypes.h>

#define NET_PACKET_EXTENSION_RSC_VERSION_1_SIZE            sizeof(NET_PACKET_RSC)
#define NET_PACKET_EXTENSION_RSC_TIMESTAMP_VERSION_1_SIZE  sizeof(NET_PACKET_RSC_TIMESTAMP)
