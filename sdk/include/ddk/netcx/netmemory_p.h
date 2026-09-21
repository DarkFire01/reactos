/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the memory collection object
 *
 * A memory collection is a pool of fragment buffers the driver draws from
 * directly instead of through a buffer queue.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NETMEMORYCOLLECTION);

typedef struct _NET_MEMORY_COLLECTION_CONFIG
{
    ULONG Size;
    SIZE_T BuffersCountHint;
} NET_MEMORY_COLLECTION_CONFIG;

typedef struct _NET_MEMORY_CONFIG
{
    ULONG Size;
    PVOID BufferAddress;
    SIZE_T BufferLength;
} NET_MEMORY_CONFIG;

#ifdef __cplusplus
}
#endif
