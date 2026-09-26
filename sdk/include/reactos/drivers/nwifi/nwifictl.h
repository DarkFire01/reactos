/*
 * PROJECT:     ReactOS Native WiFi filter
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The control device the WLAN service reaches adapters through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Opened as \\.\nativewifip\{interface GUID} to reach one adapter */
#define NWIFI_DEVICE_NAME       L"\\Device\\nativewifip"
#define NWIFI_DOS_DEVICE_NAME   L"\\DosDevices\\nativewifip"
#define NWIFI_WIN32_NAME        L"\\\\.\\nativewifip"

#define NWIFI_CTL_CODE(_Function) \
    CTL_CODE(FILE_DEVICE_NETWORK, 0x800 + (_Function), METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* An NWIFI_OID_REQUEST in and out, in the one buffer */
#define IOCTL_NWIFI_OID_REQUEST         NWIFI_CTL_CODE(1)

/* The oldest queued NWIFI_INDICATION, or STATUS_NO_MORE_ENTRIES */
#define IOCTL_NWIFI_GET_INDICATION      NWIFI_CTL_CODE(2)

/* NWIFI_OID_REQUEST::RequestType */
#define NWIFI_REQUEST_QUERY     0
#define NWIFI_REQUEST_SET       1
#define NWIFI_REQUEST_METHOD    2

typedef struct _NWIFI_OID_REQUEST
{
    ULONG RequestType;
    ULONG Oid;

    /* How much of Data goes in, and how much room it has for the answer */
    ULONG InputLength;
    ULONG OutputLength;

    /* Filled in on the way out; Status is the NDIS_STATUS of the request */
    ULONG Status;
    ULONG BytesWritten;
    ULONG BytesRead;
    ULONG BytesNeeded;

    UCHAR Data[ANYSIZE_ARRAY];
} NWIFI_OID_REQUEST, *PNWIFI_OID_REQUEST;

/* A dot11 status indication the adapter made, with its buffer */
typedef struct _NWIFI_INDICATION
{
    ULONG StatusCode;
    ULONG Length;
    UCHAR Data[ANYSIZE_ARRAY];
} NWIFI_INDICATION, *PNWIFI_INDICATION;
