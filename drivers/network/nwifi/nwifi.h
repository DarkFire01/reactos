/*
 * PROJECT:     ReactOS Native WiFi filter
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ndis.h>
#include <windot11.h>
#include <drivers/nwifi/nwifictl.h>

#define NWIFI_TAG               'fiwN'

/* Room for the largest 802.11 MSDU plus its header and SNAP */
#define NWIFI_FRAME_SIZE        2400

/* Status indications kept for the WLAN service between its reads */
#define NWIFI_INDICATION_SLOTS  16
#define NWIFI_INDICATION_MAX    2048

#define DOT11_ADDRESS_LENGTH    6

/* Privacy exemptions kept from the list the WLAN service sets */
#define NWIFI_EXEMPTIONS_MAX    8

/* The context area of every list of ours: what it belongs to, and for a
   send the ExtSTA send context the miniport reads through the list info */
typedef struct _NWIFI_FRAME_CONTEXT
{
    PVOID Owner;
    DOT11_EXTSTA_SEND_CONTEXT SendContext;
} NWIFI_FRAME_CONTEXT, *PNWIFI_FRAME_CONTEXT;

#define NWIFI_FRAME_CONTEXT_SIZE \
    ALIGN_UP_BY(sizeof(NWIFI_FRAME_CONTEXT), MEMORY_ALLOCATION_ALIGNMENT)

#define NWIFI_FRAME_CONTEXT_OF(_Nbl) \
    ((PNWIFI_FRAME_CONTEXT)NET_BUFFER_LIST_CONTEXT_DATA_START(_Nbl))

typedef struct _NWIFI_QUEUED_INDICATION
{
    ULONG StatusCode;
    ULONG Length;
    UCHAR Data[NWIFI_INDICATION_MAX];
} NWIFI_QUEUED_INDICATION, *PNWIFI_QUEUED_INDICATION;

/* One per native 802.11 adapter the filter is attached to */
typedef struct _NWIFI_MODULE
{
    LIST_ENTRY ListEntry;
    NDIS_HANDLE FilterHandle;
    GUID InterfaceGuid;

    /* The control device's users, and every OID request of ours in flight */
    LONG References;
    KEVENT Unreferenced;

    KSPIN_LOCK Lock;
    BOOLEAN Running;

    /* Kept current by the dot11 association indications */
    BOOLEAN Associated;
    UCHAR Bssid[DOT11_ADDRESS_LENGTH];

    /* The privacy exemption list that last went down, under Lock */
    ULONG ExemptionCount;
    DOT11_PRIVACY_EXEMPTION Exemptions[NWIFI_EXEMPTIONS_MAX];

    /* Our own NET_BUFFER_LISTs, one per frame, still owned by another layer */
    NDIS_HANDLE NblPool;
    LONG Outstanding;
    KEVENT Drained;

    /* A ring of indications for IOCTL_NWIFI_GET_INDICATION, under Lock */
    ULONG IndicationHead;
    ULONG IndicationCount;
    PNWIFI_QUEUED_INDICATION Indications;
} NWIFI_MODULE, *PNWIFI_MODULE;

/* nwifi.c */

extern NDIS_HANDLE NwifiDriverHandle;

PNWIFI_MODULE
NTAPI
NwifiReferenceModule(
    _In_ const GUID *InterfaceGuid);

VOID
NTAPI
NwifiDereferenceModule(
    _In_ PNWIFI_MODULE Module);

/* xlate.c */

PNET_BUFFER_LIST
NTAPI
NwifiBuildNative(
    _In_ PNWIFI_MODULE Module,
    _In_ PNET_BUFFER NetBuffer);

PNET_BUFFER_LIST
NTAPI
NwifiBuildEthernet(
    _In_ PNWIFI_MODULE Module,
    _In_ PNET_BUFFER NetBuffer);

VOID
NTAPI
NwifiTrackStatus(
    _In_ PNWIFI_MODULE Module,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);

VOID
NTAPI
NwifiTrackExemptions(
    _In_ PNWIFI_MODULE Module,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

/* control.c */

NTSTATUS
NTAPI
NwifiCreateControlDevice(
    _In_ PDRIVER_OBJECT DriverObject);

VOID
NTAPI
NwifiDeleteControlDevice(VOID);

VOID
NTAPI
NwifiQueueIndication(
    _In_ PNWIFI_MODULE Module,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);
