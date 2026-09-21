/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the virtual address fragment extension types
 */

#pragma once

#include <net/virtualaddresstypes.h>

#define NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1_SIZE  sizeof(NET_FRAGMENT_VIRTUAL_ADDRESS)
#define NET_FRAGMENT_EXTENSION_WDFMEMORY_VERSION_1_SIZE        sizeof(NET_FRAGMENT_WDFMEMORY)
