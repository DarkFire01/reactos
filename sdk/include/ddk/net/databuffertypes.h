/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Net memory fragment extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID NET_MEMORY_ID;

typedef struct _NET_FRAGMENT_NET_MEMORY
{
    NET_MEMORY_ID NetMemoryId;
} NET_FRAGMENT_NET_MEMORY;

C_ASSERT(sizeof(NET_FRAGMENT_NET_MEMORY) == sizeof(NET_MEMORY_ID));

#define NET_FRAGMENT_EXTENSION_NET_MEMORY_NAME      L"ms_fragment_netmemory"
#define NET_FRAGMENT_EXTENSION_NET_MEMORY_VERSION_1 1U

#ifdef __cplusplus
}
#endif
