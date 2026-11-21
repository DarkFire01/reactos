/*
 * PROJECT:     ReactOS Cellular Data Adapter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <ndis.h>
#include <telephony/rmnetioctl.h>

#define RMNET_TAG                   'tnMR'

/* tcpip binds 802.3 only, so the raw IP of the modem travels in emulated Ethernet frames */
#define RMNET_ETH_HEADER            14
#define RMNET_ETH_ADDRESS           6
#define RMNET_ETHERTYPE_IPV4        0x0800
#define RMNET_ETHERTYPE_ARP         0x0806
#define RMNET_ETHERTYPE_IPV6        0x86DD
#define RMNET_DEFAULT_MTU           1500
#define RMNET_MAX_MTU               2048
#define RMNET_FRAME_MAX             (RMNET_ETH_HEADER + RMNET_MAX_MTU)
#define RMNET_LINK_SPEED            (150ULL * 1000 * 1000)

/* The IPA client interface, as qcipa850.sys publishes it on \Device\IPA */
#define RMNET_IPA_DEVICE            L"\\Device\\IPA"
#define RMNET_IPA_IOCTL_REGISTER    CTL_CODE(8, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
#define RMNET_IPA_INTERFACE_VERSION 1
#define RMNET_IPA_ENDPOINT_EMBEDDED 4
#define RMNET_IPA_FAMILY_IPV4       4
#define RMNET_IPA_FAMILY_IPV6       6
#define RMNET_IPA_RETRY_MS          2000

typedef
VOID
(NTAPI *PRMNET_IPA_RECEIVE)(
    _In_ PVOID Owner,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG Count,
    _In_ ULONG ReceiveFlags);

typedef
VOID
(NTAPI *PRMNET_IPA_SEND_COMPLETE)(
    _In_ PVOID Owner,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendCompleteFlags);

typedef
VOID
(NTAPI *PRMNET_IPA_STATE)(
    _In_ PVOID Owner);

/* The registration block, IPA writes the client id back into the first field */
typedef struct _RMNET_IPA_REGISTRATION
{
    ULONG ClientId;
    ULONG Limits[3];
    NDIS_HANDLE NdisHandle;
    PVOID Owner;
    PRMNET_IPA_RECEIVE Receive;
    PRMNET_IPA_SEND_COMPLETE SendComplete;
    PRMNET_IPA_STATE State;
} RMNET_IPA_REGISTRATION, *PRMNET_IPA_REGISTRATION;

C_ASSERT(sizeof(RMNET_IPA_REGISTRATION) == 56);

typedef
VOID
(NTAPI *PRMNET_IPA_SEND)(
    _In_ PNET_BUFFER_LIST NetBufferLists);

typedef
VOID
(NTAPI *PRMNET_IPA_RETURN)(
    _In_opt_ PVOID Reserved,
    _In_ PNET_BUFFER_LIST NetBufferLists);

typedef
NTSTATUS
(NTAPI *PRMNET_IPA_START)(VOID);

typedef
VOID
(NTAPI *PRMNET_IPA_QUERY_CLIENT)(
    _Out_writes_bytes_(64) PVOID Record,
    _In_ ULONG ClientId);

typedef
VOID
(NTAPI *PRMNET_IPA_DEREGISTER)(
    _In_ ULONG ClientId);

typedef
VOID
(NTAPI *PRMNET_IPA_SET_COALESCING)(
    _In_ ULONG ClientId,
    _In_ BOOLEAN Enable,
    _In_ ULONG Family);

typedef struct _RMNET_IPA_INTERFACE
{
    INTERFACE Header;
    PRMNET_IPA_SEND Send;
    PRMNET_IPA_RETURN Return;
    PRMNET_IPA_START Start;
    PRMNET_IPA_QUERY_CLIENT QueryClient;
    PRMNET_IPA_DEREGISTER Deregister;
    PRMNET_IPA_SET_COALESCING SetCoalescing;
} RMNET_IPA_INTERFACE, *PRMNET_IPA_INTERFACE;

C_ASSERT(sizeof(RMNET_IPA_INTERFACE) == 80);

typedef struct _RMNET_ADAPTER
{
    NDIS_HANDLE MiniportHandle;
    NDIS_SPIN_LOCK Lock;

    /* Frames to and from tcpip, and the raw packets to IPA */
    NDIS_HANDLE FramePool;
    NDIS_HANDLE PacketPool;

    UCHAR Address[RMNET_ETH_ADDRESS];
    UCHAR PeerAddress[RMNET_ETH_ADDRESS];
    ULONG PacketFilter;
    BOOLEAN Paused;
    BOOLEAN Halting;

    /* IPA client */
    PFILE_OBJECT IpaFile;
    PDEVICE_OBJECT IpaDevice;
    RMNET_IPA_INTERFACE Ipa;
    BOOLEAN IpaRegistered;
    ULONG ClientId;
    NDIS_HANDLE RetryTimer;
    LONG IpaSendsPending;

    /* The data session the RIL brought up */
    RMNET_LINK Link;
    BOOLEAN Connected;

    ULONG64 SentPackets;
    ULONG64 ReceivedPackets;
    ULONG64 DroppedPackets;
} RMNET_ADAPTER, *PRMNET_ADAPTER;

extern NDIS_HANDLE RmNetDriverHandle;
extern PRMNET_ADAPTER RmNetAdapter;

/* ipa.c */
VOID
NTAPI
RmNetIpaConnect(
    _In_ PRMNET_ADAPTER Adapter);

VOID
NTAPI
RmNetIpaDisconnect(
    _In_ PRMNET_ADAPTER Adapter);

VOID
NTAPI
RmNetIpaSetCoalescing(
    _In_ PRMNET_ADAPTER Adapter,
    _In_ BOOLEAN Enable);

BOOLEAN
NTAPI
RmNetIpaSend(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length);

/* datapath.c */
VOID
NTAPI
RmNetSendFrame(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length);

VOID
NTAPI
RmNetIndicatePacket(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length);

VOID
NTAPI
RmNetIndicateLinkState(
    _In_ PRMNET_ADAPTER Adapter);

/* control.c */
NDIS_STATUS
NTAPI
RmNetControlCreate(
    _In_ PRMNET_ADAPTER Adapter);

VOID
NTAPI
RmNetControlDelete(VOID);
