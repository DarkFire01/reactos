/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Records exchanged between RIL clients, the MSRil broker and the RIL host
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Interface the broker exposes to RIL clients while a host is attached */
DEFINE_GUID(GUID_DEVINTERFACE_RIL_CLIENT,
    0x47ca0628, 0x71c1, 0x4be1, 0x8f, 0xfb, 0x50, 0x32, 0x01, 0xfe, 0xad, 0x27);

/* Interface registered by the OEM RIL component (see the vendor WMRil INF) */
DEFINE_GUID(GUID_DEVINTERFACE_RILHOST,
    0x44c09290, 0x1d8e, 0x4942, 0xba, 0xf3, 0x63, 0x73, 0x41, 0x3f, 0xa4, 0x1f);

#define RIL_RECORD_COMMAND          3
#define RIL_RECORD_RESPONSE         4
#define RIL_RECORD_NOTIFICATION     5

#include <pshpack4.h>

/* Client to host */
typedef struct _RIL_COMMAND_RECORD
{
    ULONG Type;
    ULONG Command;
    ULONGLONG Context;
} RIL_COMMAND_RECORD, *PRIL_COMMAND_RECORD;

/* Host to client, answer to one command */
typedef struct _RIL_RESPONSE_RECORD
{
    ULONG Type;
    ULONG ResultType;
    ULONG Command;
    ULONGLONG Context;
} RIL_RESPONSE_RECORD, *PRIL_RESPONSE_RECORD;

/* Host to client, unsolicited */
typedef struct _RIL_NOTIFICATION_RECORD
{
    ULONG Type;
    ULONG Code;
} RIL_NOTIFICATION_RECORD, *PRIL_NOTIFICATION_RECORD;

#include <poppack.h>

C_ASSERT(sizeof(RIL_COMMAND_RECORD) == 16);
C_ASSERT(sizeof(RIL_RESPONSE_RECORD) == 20);
C_ASSERT(sizeof(RIL_NOTIFICATION_RECORD) == 8);

/* Largest record either side accepts */
#define RIL_RECORD_MAX_LENGTH       (64 * 1024)

/*
 * Host side of the broker. The host opens \\.\MSRil<N>\Host, the clients open
 * the GUID_DEVINTERFACE_RIL_CLIENT interface of the same device.
 */
#define MSRIL_HOST_FILE_NAME        L"\\Host"

#define FILE_DEVICE_MSRIL           0x8A52

/* Out: MSRIL_HOST_EVENT followed by the client record. Pends until an event exists. */
#define IOCTL_MSRIL_HOST_GET_EVENT \
    CTL_CODE(FILE_DEVICE_MSRIL, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

/* In: MSRIL_HOST_DELIVERY followed by the record for that client */
#define IOCTL_MSRIL_HOST_DELIVER \
    CTL_CODE(FILE_DEVICE_MSRIL, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

typedef enum _MSRIL_HOST_EVENT_TYPE
{
    MsRilClientOpened = 1,
    MsRilClientClosed,
    MsRilClientRecord
} MSRIL_HOST_EVENT_TYPE;

typedef struct _MSRIL_HOST_EVENT
{
    ULONG EventType;
    ULONG ClientId;
    ULONG ProcessId;
    ULONG DataLength;
} MSRIL_HOST_EVENT, *PMSRIL_HOST_EVENT;

typedef struct _MSRIL_HOST_DELIVERY
{
    ULONG ClientId;
    ULONG DataLength;
} MSRIL_HOST_DELIVERY, *PMSRIL_HOST_DELIVERY;
