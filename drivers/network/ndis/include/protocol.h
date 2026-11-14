/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS library
 * FILE:        ndis/protocol.h
 * PURPOSE:     Definitions for routines used by NDIS protocol drivers
 */

#pragma once

typedef struct _PROTOCOL_BINDING {
    LIST_ENTRY                    ListEntry;        /* Entry on global list */
    KSPIN_LOCK                    Lock;             /* Protecting spin lock */
    NDIS_PROTOCOL_CHARACTERISTICS Chars;            /* Characteristics */
    WORK_QUEUE_ITEM               WorkItem;         /* Work item */
    LIST_ENTRY                    AdapterListHead;  /* List of adapter bindings */
} PROTOCOL_BINDING, *PPROTOCOL_BINDING;

#define GET_PROTOCOL_BINDING(Handle)((PPROTOCOL_BINDING)Handle)


typedef struct _ADAPTER_BINDING {
    NDIS_OPEN_BLOCK NdisOpenBlock;                            /* NDIS defined fields */

    LIST_ENTRY        ListEntry;                /* Entry on global list */
    LIST_ENTRY        ProtocolListEntry;        /* Entry on protocol binding adapter list */
    LIST_ENTRY        AdapterListEntry;         /* Entry on logical adapter list */
    KSPIN_LOCK        Lock;                     /* Protecting spin lock */
    PPROTOCOL_BINDING ProtocolBinding;          /* Protocol that opened adapter */
    PLOGICAL_ADAPTER  Adapter;                  /* Adapter opened by protocol */
} ADAPTER_BINDING, *PADAPTER_BINDING;

typedef struct _NDIS_REQUEST_MAC_BLOCK {
    PVOID Unknown1;
    PNDIS_OPEN_BLOCK Binding;
    PVOID Unknown3;
    PVOID Unknown4;
} NDIS_REQUEST_MAC_BLOCK, *PNDIS_REQUEST_MAC_BLOCK;

#define GET_ADAPTER_BINDING(Handle)((PADAPTER_BINDING)Handle)


extern LIST_ENTRY ProtocolListHead;
extern KSPIN_LOCK ProtocolListLock;


/* pro5shim.c */

NDIS_STATUS
NTAPI
ProSend(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ PNDIS_PACKET Packet);

VOID
NTAPI
ProSendPackets(
    _In_ NDIS_HANDLE NdisBindingHandle,
    _In_reads_(NumberOfPackets) PPNDIS_PACKET PacketArray,
    _In_ UINT NumberOfPackets);

NDIS_STATUS
NTAPI
ProTransferData(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ NDIS_HANDLE MacReceiveContext,
    _In_ UINT ByteOffset,
    _In_ UINT BytesToTransfer,
    _Inout_ PNDIS_PACKET Packet,
    _Out_ PUINT BytesTransferred);

NDIS_STATUS
NTAPI
ProRequest(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ PNDIS_REQUEST NdisRequest);

NTSTATUS
NTAPI
NdisIPnPQueryStopDevice(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp);

NTSTATUS
NTAPI
NdisIPnPCancelStopDevice(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp);

VOID
NTAPI
ndisBindMiniportsToProtocol(OUT PNDIS_STATUS Status, IN PPROTOCOL_BINDING Protocol);

/* EOF */
