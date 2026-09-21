/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fragment return context extension accessor
 */

#pragma once

#include <net/returncontexttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_FRAGMENT_RETURN_CONTEXT *
NetExtensionGetFragmentReturnContext(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_RETURN_CONTEXT *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
