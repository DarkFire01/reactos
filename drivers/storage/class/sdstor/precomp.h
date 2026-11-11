/*
 * PROJECT:     ReactOS Storage Stack / SD storage class driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Internal definitions
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _SDSTOR_PRECOMP_H_
#define _SDSTOR_PRECOMP_H_

#include <ntddk.h>
#include <ntstrsafe.h>
#include <scsi.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <sdport.h>

#define SDSTOR_POOL_TAG 'rtdS'
#define SDSTOR_BLOCK_LENGTH 512

#define SDSTOR_CARD_PRESENT        0x00000001
#define SDSTOR_CARD_SD             0x00000002
#define SDSTOR_CARD_MMC            0x00000004
#define SDSTOR_CARD_HIGH_CAPACITY  0x00000008
#define SDSTOR_CARD_WRITE_PROTECT  0x00000010

typedef enum _SDSTOR_EXTENSION_TYPE
{
    SdstorExtensionFdo = 1,
    SdstorExtensionPdo = 2
} SDSTOR_EXTENSION_TYPE;

typedef struct _SDSTOR_CARD_DATA
{
    ULONG Flags;
    USHORT Rca;
    ULONG BlockLength;
    ULONGLONG BlockCount;
    ULONGLONG CapacityBytes;
    UCHAR Cid[16];
    UCHAR Csd[16];
    UCHAR VendorId[8];
    UCHAR ProductId[16];
    UCHAR ProductRevision[4];
    UCHAR SerialNumber[4];
} SDSTOR_CARD_DATA, *PSDSTOR_CARD_DATA;

typedef struct _SDSTOR_FDO_EXTENSION
{
    SDSTOR_EXTENSION_TYPE ExtensionType;
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT LowerDevice;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT ChildPdo;
    IO_REMOVE_LOCK RemoveLock;
    SDSTOR_CARD_DATA Card;
    BOOLEAN Started;
} SDSTOR_FDO_EXTENSION, *PSDSTOR_FDO_EXTENSION;

typedef struct _SDSTOR_PDO_EXTENSION
{
    SDSTOR_EXTENSION_TYPE ExtensionType;
    PDEVICE_OBJECT Self;
    PSDSTOR_FDO_EXTENSION FdoExtension;
    BOOLEAN Claimed;
} SDSTOR_PDO_EXTENSION, *PSDSTOR_PDO_EXTENSION;

/* sdstor.c */
DRIVER_ADD_DEVICE SdStorAddDevice;
DRIVER_UNLOAD SdStorUnload;

NTSTATUS
NTAPI
SdStorSubmitRequest(
    _In_ PDEVICE_OBJECT LowerDevice,
    _Inout_ PSDPORT_REQUEST Request);

NTSTATUS
NTAPI
SdStorGetProperty(
    _In_ PDEVICE_OBJECT LowerDevice,
    _In_ SDPORT_PROPERTY PropertyId,
    _Out_writes_bytes_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength);

/* sdcard.c */
NTSTATUS
NTAPI
SdStorQueryCardData(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension);

/* pnp.c */
NTSTATUS
NTAPI
SdStorDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdStorDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdStorDispatchWmi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

/* scsi.c */
NTSTATUS
NTAPI
SdStorDispatchScsi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdStorDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

#endif /* _SDSTOR_PRECOMP_H_ */
