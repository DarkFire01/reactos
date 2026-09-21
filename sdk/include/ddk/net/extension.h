/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet ring extension accessors
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_EXTENSION_TYPE
{
    NetExtensionTypePacket = 1,
    NetExtensionTypeFragment,
    NetExtensionTypeBuffer
} NET_EXTENSION_TYPE;

/* Reserved[0] is the base of the extension data, Reserved[1] its stride. */
typedef struct _NET_EXTENSION
{
    PVOID Reserved[4];
    union
    {
        BOOLEAN Enabled;
        PVOID Reserved1;
    } DUMMYUNIONNAME;
} NET_EXTENSION;

#ifdef _WIN64
C_ASSERT(sizeof(NET_EXTENSION) == 40);
#else
C_ASSERT(sizeof(NET_EXTENSION) == 20);
#endif

FORCEINLINE
PVOID
NetExtensionGetData(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (PVOID)((PUCHAR)Extension->Reserved[0] + (SIZE_T)Extension->Reserved[1] * Index);
}

#ifdef __cplusplus
}
#endif
