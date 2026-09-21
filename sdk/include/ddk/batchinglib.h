/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Batched buffer operation library
 *
 * Microsoft links this from an internal source tree. It is in no WDK and not
 * in the shipping netadaptercx.sys either, so only the shape the callers need
 * is declared here. sdk/lib/drivers/seglib is a stub that announces itself.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _BATCHING_LIB_CONTEXT
{
    PVOID Reserved;
} BATCHING_LIB_CONTEXT;

typedef struct _BATCHING_BUFFER_CONTEXT
{
    BATCHING_LIB_CONTEXT *Context;
    PVOID Reserved;
} BATCHING_BUFFER_CONTEXT;

VOID
BLInitialize(
    _Inout_ BATCHING_LIB_CONTEXT *Context);

VOID
BLUninitialize(
    _Inout_ BATCHING_LIB_CONTEXT *Context);

VOID
BLInitializeBatchOpContext(
    _In_ BATCHING_LIB_CONTEXT *Context,
    _Out_ BATCHING_BUFFER_CONTEXT *BufferContext,
    _In_ BOOLEAN Reserved1,
    _In_ BOOLEAN Reserved2,
    _In_opt_ PVOID Reserved3);

VOID
BLFlushBatchOpContext(
    _Inout_ BATCHING_BUFFER_CONTEXT *BufferContext);

#ifdef __cplusplus
}
#endif
