/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fragment return context extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NET_FRAGMENT_RETURN_CONTEXT_HANDLE);

typedef struct _NET_FRAGMENT_RETURN_CONTEXT
{
    NET_FRAGMENT_RETURN_CONTEXT_HANDLE Handle;
} NET_FRAGMENT_RETURN_CONTEXT;

C_ASSERT(sizeof(NET_FRAGMENT_RETURN_CONTEXT) == sizeof(NET_FRAGMENT_RETURN_CONTEXT_HANDLE));

#define NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_NAME      L"ms_fragment_returncontext"
#define NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_VERSION_1 1U

#ifdef __cplusplus
}
#endif
