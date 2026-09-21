/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Transmit packet queue
 */

#pragma once

#include <netcx/netpacketqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _NETTXQUEUE_INIT;
typedef struct _NETTXQUEUE_INIT NETTXQUEUE_INIT;

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETTXQUEUECREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETTXQUEUE_INIT *NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *PacketQueue);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetTxQueueCreate(
    _Inout_ NETTXQUEUE_INIT *NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *PacketQueue)
{
    return ((PFN_NETTXQUEUECREATE)NetFunctions[NetTxQueueCreateTableIndex])(
        NetDriverGlobals, NetTxQueueInit, TxQueueAttributes, Configuration, PacketQueue);
}

typedef
_IRQL_requires_max_(HIGH_LEVEL)
VOID
(NTAPI *PFN_NETTXQUEUENOTIFYMORECOMPLETEDPACKETSAVAILABLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(HIGH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetTxQueueNotifyMoreCompletedPacketsAvailable(
    _In_ NETPACKETQUEUE PacketQueue)
{
    ((PFN_NETTXQUEUENOTIFYMORECOMPLETEDPACKETSAVAILABLE)
        NetFunctions[NetTxQueueNotifyMoreCompletedPacketsAvailableTableIndex])(
            NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
(NTAPI *PFN_NETTXQUEUEINITGETQUEUEID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETTXQUEUE_INIT *NetTxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
ULONG
NTAPI
NetTxQueueInitGetQueueId(
    _In_ NETTXQUEUE_INIT *NetTxQueueInit)
{
    return ((PFN_NETTXQUEUEINITGETQUEUEID)
        NetFunctions[NetTxQueueInitGetQueueIdTableIndex])(NetDriverGlobals, NetTxQueueInit);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NET_RING_COLLECTION const *
(NTAPI *PFN_NETTXQUEUEGETRINGCOLLECTION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_RING_COLLECTION const *
NTAPI
NetTxQueueGetRingCollection(
    _In_ NETPACKETQUEUE PacketQueue)
{
    return ((PFN_NETTXQUEUEGETRINGCOLLECTION)
        NetFunctions[NetTxQueueGetRingCollectionTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETTXQUEUEGETEXTENSION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ const NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetTxQueueGetExtension(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ const NET_EXTENSION_QUERY *Query,
    _Out_ NET_EXTENSION *Extension)
{
    ((PFN_NETTXQUEUEGETEXTENSION)NetFunctions[NetTxQueueGetExtensionTableIndex])(
        NetDriverGlobals, PacketQueue, Query, Extension);
}

#ifdef __cplusplus
}
#endif
