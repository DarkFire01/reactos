/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MDL fragment extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_MDL
{
    MDL *Mdl;
} NET_FRAGMENT_MDL;

C_ASSERT(sizeof(NET_FRAGMENT_MDL) == sizeof(PVOID));

#define NET_FRAGMENT_EXTENSION_MDL_NAME         L"ms_fragment_mdl"
#define NET_FRAGMENT_EXTENSION_MDL_VERSION_1    1U

#ifdef __cplusplus
}
#endif
