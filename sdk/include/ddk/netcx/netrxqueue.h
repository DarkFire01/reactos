/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive packet queue
 */

#pragma once

#include <netcx/netpacketqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _NETRXQUEUE_INIT;
typedef struct _NETRXQUEUE_INIT NETRXQUEUE_INIT;

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETRXQUEUECREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETRXQUEUE_INIT *NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *PacketQueue);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetRxQueueCreate(
    _Inout_ NETRXQUEUE_INIT *NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *PacketQueue)
{
    return ((PFN_NETRXQUEUECREATE)NetFunctions[NetRxQueueCreateTableIndex])(
        NetDriverGlobals, NetRxQueueInit, RxQueueAttributes, Configuration, PacketQueue);
}

typedef
_IRQL_requires_max_(HIGH_LEVEL)
VOID
(NTAPI *PFN_NETRXQUEUENOTIFYMORERECEIVEDPACKETSAVAILABLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(HIGH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetRxQueueNotifyMoreReceivedPacketsAvailable(
    _In_ NETPACKETQUEUE PacketQueue)
{
    ((PFN_NETRXQUEUENOTIFYMORERECEIVEDPACKETSAVAILABLE)
        NetFunctions[NetRxQueueNotifyMoreReceivedPacketsAvailableTableIndex])(
            NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
(NTAPI *PFN_NETRXQUEUEINITGETQUEUEID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRXQUEUE_INIT *NetRxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
ULONG
NTAPI
NetRxQueueInitGetQueueId(
    _In_ NETRXQUEUE_INIT *NetRxQueueInit)
{
    return ((PFN_NETRXQUEUEINITGETQUEUEID)
        NetFunctions[NetRxQueueInitGetQueueIdTableIndex])(NetDriverGlobals, NetRxQueueInit);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NET_RING_COLLECTION const *
(NTAPI *PFN_NETRXQUEUEGETRINGCOLLECTION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_RING_COLLECTION const *
NTAPI
NetRxQueueGetRingCollection(
    _In_ NETPACKETQUEUE PacketQueue)
{
    return ((PFN_NETRXQUEUEGETRINGCOLLECTION)
        NetFunctions[NetRxQueueGetRingCollectionTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETRXQUEUEGETEXTENSION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ const NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetRxQueueGetExtension(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ const NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension)
{
    ((PFN_NETRXQUEUEGETEXTENSION)NetFunctions[NetRxQueueGetExtensionTableIndex])(
        NetDriverGlobals, PacketQueue, Query, Extension);
}

#ifdef __cplusplus
}
#endif
