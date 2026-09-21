/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Virtual address fragment extension accessors
 */

#pragma once

#include <net/virtualaddresstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
NET_FRAGMENT_VIRTUAL_ADDRESS *
NetExtensionGetFragmentVirtualAddress(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_VIRTUAL_ADDRESS *)NetExtensionGetData(Extension, Index);
}

FORCEINLINE
PVOID
NetExtensionGetFragmentVirtualAddressOffset(
    _In_ const NET_FRAGMENT *Fragment,
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    const NET_FRAGMENT_VIRTUAL_ADDRESS *VirtualAddress =
        NetExtensionGetFragmentVirtualAddress(Extension, Index);

    return (PUCHAR)VirtualAddress->VirtualAddress + Fragment->Offset;
}

FORCEINLINE
NET_FRAGMENT_WDFMEMORY *
NetExtensionGetFragmentWdfMemory(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_WDFMEMORY *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
