/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Common header file
 */

#ifndef _STORNVME_H_
#define _STORNVME_H_

#include <ntddk.h>
#include <storport.h>
#include <nvme.h>

/* Memory tags */
#define TAG_NVME_QUEUE      'QvNS'
#define TAG_NVME_IDENTIFY   'IvNS'

/* The controller register block is the first access range */
#define NVME_BAR_REGISTERS  0

/*
 * CAP.TO counts half seconds, and the specification allows a controller the
 * whole of it to become ready or to shut down.
 */
#define NVME_TIMEOUT_UNIT_MS    500

/* Queue entry sizes are reported as powers of two */
#define NVME_SQ_ENTRY_SHIFT     6
#define NVME_CQ_ENTRY_SHIFT     4

/* A page is the unit the controller addresses memory in */
#define NVME_MIN_PAGE_SHIFT     12

/* Where the controller came from, for the sake of reporting it */
typedef enum _NVME_ADAPTER_STATE
{
    NvmeAdapterStopped = 0,
    NvmeAdapterFound,
    NvmeAdapterRunning,
    NvmeAdapterFailed
} NVME_ADAPTER_STATE;

/*
 * Per controller state. Storport hands this out as the device extension, so
 * everything the miniport needs about one controller hangs off it.
 */
typedef struct _NVME_ADAPTER_EXTENSION
{
    NVME_ADAPTER_STATE State;

    /* Mapped controller registers, and the doorbell array that follows them */
    PNVME_CONTROLLER_REGISTERS Registers;
    PULONG Doorbells;

    /* Cached at find time because reading CAP costs two register accesses */
    NVME_CONTROLLER_CAPABILITIES Capabilities;
    NVME_VERSION Version;

    /* Bytes between one doorbell and the next, from CAP.DSTRD */
    ULONG DoorbellStride;

    /* How long the controller may take to become ready, in milliseconds */
    ULONG ReadyTimeout;

    /* Page size the controller is configured for, and its shift */
    ULONG PageSize;
    ULONG PageShift;

    /* Largest transfer the controller accepts, in bytes */
    ULONG MaximumTransferLength;

    /* Entries in the admin queues */
    ULONG AdminQueueDepth;

    /* Set while running as part of a crash dump or hibernation stack */
    BOOLEAN DumpMode;

    /* Set once storport told us the adapter is on message interrupts */
    BOOLEAN MessageInterrupts;
} NVME_ADAPTER_EXTENSION, *PNVME_ADAPTER_EXTENSION;

/* stornvme.c */

ULONG
NTAPI
StorNvmeFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again);

BOOLEAN
NTAPI
StorNvmeInitialize(
    _In_ PVOID DeviceExtension);

BOOLEAN
NTAPI
StorNvmeBuildIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb);

BOOLEAN
NTAPI
StorNvmeStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb);

BOOLEAN
NTAPI
StorNvmeInterrupt(
    _In_ PVOID DeviceExtension);

BOOLEAN
NTAPI
StorNvmeMessageInterrupt(
    _In_ PVOID DeviceExtension,
    _In_ ULONG MessageId);

BOOLEAN
NTAPI
StorNvmeResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId);

SCSI_ADAPTER_CONTROL_STATUS
NTAPI
StorNvmeAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters);

SCSI_UNIT_CONTROL_STATUS
NTAPI
StorNvmeUnitControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_UNIT_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters);

#endif /* _STORNVME_H_ */
