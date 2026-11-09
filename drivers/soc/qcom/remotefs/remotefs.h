/*
 * PROJECT:     ReactOS Qualcomm remote file system server
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/qmiipc.h>
#include <qcom/qmisvc.h>

#define RFS_TAG                     'sfRQ'

/* The modem talks to us as QMI service 0x1C, remote file system */
#define RFS_SERVICE_ID              0x1C
#define RFS_SERVICE_VERSION         1
#define RFS_SERVICE_INSTANCE        0

/* Request message ids the modem sends, from the reference dispatch */
#define RFS_MSG_IOVEC               0x20
#define RFS_MSG_ALLOC_BUFFER        0x21
#define RFS_MSG_GET_DEV_ERROR       0x22
#define RFS_MSG_OPEN                0x23
#define RFS_MSG_CLOSE               0x24
#define RFS_MSG_WRITE               0x25
#define RFS_MSG_READ                0x26

/* Three shared windows from _CRS: the main RMT buffer and two more */
#define RFS_REGION_COUNT            3

typedef struct _RFS_REGION
{
    PHYSICAL_ADDRESS Physical;
    PVOID Base;
    ULONG Length;
} RFS_REGION, *PRFS_REGION;

typedef struct _RFS_DEVICE
{
    WDFDEVICE WdfDevice;

    /* QMI transport over the router and the service we expose on it */
    PQMI_TRANSPORT Transport;
    QMI_SERVICE Service;
    BOOLEAN ServiceUp;

    /* Shared memory the modem hands us iovec offsets into */
    RFS_REGION Regions[RFS_REGION_COUNT];
    ULONG RegionCount;

    /* Backing store: a raw partition when FileBased is 0, else a directory */
    BOOLEAN FileBased;
    ULONG PartitionNumber;
    WCHAR StorageBuffer[64];
    UNICODE_STRING StoragePath;
    HANDLE Storage;
} RFS_DEVICE, *PRFS_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RFS_DEVICE, RfsGetDevice)

/* service.c */
NTSTATUS
RfsRequest(
    _In_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Inout_ PQMI_ENCODER Response,
    _In_opt_ PVOID Context);
