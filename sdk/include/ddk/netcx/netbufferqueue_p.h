/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the buffer queue object
 *
 * A buffer queue hands the driver empty buffers on a ring owned by an
 * execution context, for devices that fill receive buffers out of order.
 */

#pragma once

#include <netcx/netexecutioncontext_p.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef
_Function_class_(EVT_NET_BUFFER_QUEUE_POST_AND_DRAIN)
_IRQL_requires_same_
VOID
NTAPI
EVT_NET_BUFFER_QUEUE_POST_AND_DRAIN(
    _In_ NETBUFFERQUEUE BufferQueue);

typedef EVT_NET_BUFFER_QUEUE_POST_AND_DRAIN *PFN_NET_BUFFER_QUEUE_POST_AND_DRAIN;

typedef
_Function_class_(EVT_NET_BUFFER_QUEUE_RETURN_BUFFERS)
_IRQL_requires_same_
VOID
NTAPI
EVT_NET_BUFFER_QUEUE_RETURN_BUFFERS(
    _In_ NETBUFFERQUEUE BufferQueue);

typedef EVT_NET_BUFFER_QUEUE_RETURN_BUFFERS *PFN_NET_BUFFER_QUEUE_RETURN_BUFFERS;

typedef
_Function_class_(EVT_NET_BUFFER_QUEUE_CANCEL)
_IRQL_requires_same_
VOID
NTAPI
EVT_NET_BUFFER_QUEUE_CANCEL(
    _In_ NETBUFFERQUEUE BufferQueue);

typedef EVT_NET_BUFFER_QUEUE_CANCEL *PFN_NET_BUFFER_QUEUE_CANCEL;

typedef struct _NET_BUFFER_QUEUE_CONFIG
{
    ULONG Size;
    SIZE_T BufferSize;
    SIZE_T BufferAlignment;
    PFN_NET_BUFFER_QUEUE_POST_AND_DRAIN EvtPostAndDrain;
    PFN_NET_BUFFER_QUEUE_RETURN_BUFFERS EvtReturnBuffers;
    PFN_NET_BUFFER_QUEUE_CANCEL EvtCancel;
    NET_ADAPTER_DMA_CAPABILITIES *DmaCapabilities;
} NET_BUFFER_QUEUE_CONFIG;

FORCEINLINE
VOID
NTAPI
NET_BUFFER_QUEUE_CONFIG_INIT(
    _Out_ NET_BUFFER_QUEUE_CONFIG *Config,
    _In_ SIZE_T BufferSize,
    _In_ SIZE_T BufferAlignment)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->BufferSize = BufferSize;
    Config->BufferAlignment = BufferAlignment;
}

typedef
_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETBUFFERQUEUECREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_ CONST NET_BUFFER_QUEUE_CONFIG *Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Outptr_ NETBUFFERQUEUE *BufferQueue);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetBufferQueueCreate(
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_ CONST NET_BUFFER_QUEUE_CONFIG *Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Outptr_ NETBUFFERQUEUE *BufferQueue)
{
    return ((PFN_NETBUFFERQUEUECREATE)NetFunctions[NetBufferQueueCreateTableIndex])(
        NetDriverGlobals, ExecutionContext, Config, Attributes, BufferQueue);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETBUFFERQUEUEGETEXTENSION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETBUFFERQUEUE BufferQueue,
    _In_ CONST NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetBufferQueueGetExtension(
    _In_ NETBUFFERQUEUE BufferQueue,
    _In_ CONST NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension)
{
    ((PFN_NETBUFFERQUEUEGETEXTENSION)NetFunctions[NetBufferQueueGetExtensionTableIndex])(
        NetDriverGlobals, BufferQueue, Query, Extension);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_RING *
(NTAPI *PFN_NETBUFFERQUEUEGETRING)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETBUFFERQUEUE BufferQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_RING *
NTAPI
NetBufferQueueGetRing(
    _In_ NETBUFFERQUEUE BufferQueue)
{
    return ((PFN_NETBUFFERQUEUEGETRING)NetFunctions[NetBufferQueueGetRingTableIndex])(
        NetDriverGlobals, BufferQueue);
}

#ifdef __cplusplus
}
#endif
