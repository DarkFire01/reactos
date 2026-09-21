/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Buffer manager entry point handed to the translator
 *
 * Only pool creation is exported. The rest of the pool operations are
 * reached through the dispatch table it returns.
 */

#pragma once

#include <NetClientBuffer.h>

EXTERN_C_START

_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
NTSTATUS
NetClientCreateBufferPool(
    _In_ NET_CLIENT_BUFFER_POOL_CONFIG *BufferPoolConfig,
    _Out_ NET_CLIENT_BUFFER_POOL *Pool,
    _Out_ NET_CLIENT_BUFFER_POOL_DISPATCH const **BufferPoolDispatch);

EXTERN_C_END
