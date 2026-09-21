/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Virtual address fragment extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_VIRTUAL_ADDRESS
{
    PVOID VirtualAddress;
} NET_FRAGMENT_VIRTUAL_ADDRESS;

#ifndef WDFAPI
DECLARE_HANDLE(WDFMEMORY);
#endif

typedef struct _NET_FRAGMENT_WDFMEMORY
{
    WDFMEMORY WdfMemory;
} NET_FRAGMENT_WDFMEMORY;

C_ASSERT(sizeof(NET_FRAGMENT_VIRTUAL_ADDRESS) == sizeof(PVOID));

#define NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME         L"ms_fragment_virtualaddress"
#define NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1    1U
#define NET_FRAGMENT_EXTENSION_WDFMEMORY_NAME               L"ms_fragment_wdfmemory"
#define NET_FRAGMENT_EXTENSION_WDFMEMORY_VERSION_1          1U

#ifdef __cplusplus
}
#endif
