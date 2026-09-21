/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Net memory fragment extension accessor
 */

#pragma once

#include <net/databuffertypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_FRAGMENT_NET_MEMORY *
NetExtensionGetFragmentNetMemory(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_NET_MEMORY *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
