/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the data buffer fragment extension
 *
 * This is the earlier shape of the extension that the net memory one replaced.
 * The handle is an index into the pool the framework allocated the buffer from.
 */

#pragma once

#include <net/databuffer.h>
#include <net/ring.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_DATA_BUFFER
{
    PVOID Handle;
} NET_FRAGMENT_DATA_BUFFER;

C_ASSERT(sizeof(NET_FRAGMENT_DATA_BUFFER) == sizeof(PVOID));

#define NET_FRAGMENT_EXTENSION_DATA_BUFFER_NAME           L"ms_fragment_databuffer"
#define NET_FRAGMENT_EXTENSION_DATA_BUFFER_VERSION_1      1U
#define NET_FRAGMENT_EXTENSION_DATA_BUFFER_VERSION_1_SIZE sizeof(NET_FRAGMENT_DATA_BUFFER)

FORCEINLINE
NET_FRAGMENT_DATA_BUFFER *
NetExtensionGetFragmentDataBuffer(
    _In_ const NET_EXTENSION *Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_DATA_BUFFER *)NetExtensionGetData(Extension, Index);
}

/* Elements of the data buffer ring are indices into the pool that owns the buffers. */
FORCEINLINE
SIZE_T *
NetRingGetDataBufferAtIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return (SIZE_T *)NetRingGetElementAtIndex(Ring, Index);
}

#ifdef __cplusplus
}
#endif
