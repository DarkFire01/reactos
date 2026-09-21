/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Logical address fragment extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_LOGICAL_ADDRESS
{
    UINT64 LogicalAddress;
} NET_FRAGMENT_LOGICAL_ADDRESS;

C_ASSERT(sizeof(NET_FRAGMENT_LOGICAL_ADDRESS) == sizeof(UINT64));

#define NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME         L"ms_fragment_logicaladdress"
#define NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1    1U

#ifdef __cplusplus
}
#endif
