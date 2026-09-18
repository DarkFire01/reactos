/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Common header file
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _UASPSTOR_H_
#define _UASPSTOR_H_

#include <ntddk.h>
#include <storport.h>
#include <srbhelper.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <usb200.h>

#include "uas.h"

/* Memory tags */
#define TAG_UASP            'psaU'

/*
 * A command is named by its tag, and on a device with streams the tag is also
 * the stream every transfer of that command rides on. Tag zero is reserved,
 * so the first command uses tag one.
 */
#define UASP_FIRST_TAG      1

/*
 * How many commands may be outstanding at once. A device offers a stream
 * count of its own and the controller grants what it can, so this is only
 * the ceiling.
 */
#define UASP_MAX_REQUESTS   32

/* A device without streams answers one command at a time */
#define UASP_NO_STREAM_REQUESTS 1

/* Enough for a status information unit of either layout, and its sense data */
#define UASP_STATUS_IU_LENGTH   1024

/* What the class layer may ask for in one command */
#define UASP_MAX_TRANSFER_LENGTH (128 * 1024)

/* Targets and logical units this driver reports to the class layer */
#define UASP_MAX_LUN        16

/* How long a command started from inside the driver may take */
#define UASP_INTERNAL_TIMEOUT_MS 10000

/* How long to wait for a synchronous USB request during startup */
#define UASP_USB_TIMEOUT_MS 5000

/* The transfers one command is built out of */
typedef enum _UASP_TRANSFER_KIND
{
    UaspTransferCommand = 0,
    UaspTransferData,
    UaspTransferStatus,
    UaspTransferReady,
    UaspTransferMax
} UASP_TRANSFER_KIND;

typedef enum _UASP_QUEUE_STATE
{
    UaspQueueStopped = 0,
    UaspQueueRunning
} UASP_QUEUE_STATE;

struct _UASP_ADAPTER_EXTENSION;
struct _UASP_REQUEST;

typedef
VOID
(*PUASP_TRANSFER_COMPLETION)(
    _In_ struct _UASP_REQUEST *Request,
    _In_ NTSTATUS Status);

/*
 * One transfer of one command. The IRP and the request block are reused for
 * the life of the adapter, so nothing is allocated on the path a read or a
 * write takes.
 */
typedef struct _UASP_TRANSFER
{
    struct _UASP_REQUEST *Request;
    PIRP Irp;
    PURB Urb;

    /* The pipe the transfer went out on, which is the one a stall halts */
    USBD_PIPE_HANDLE ResetPipe;

    PUASP_TRANSFER_COMPLETION Completion;
} UASP_TRANSFER, *PUASP_TRANSFER;

/*
 * One command in flight. There is one of these per tag, and a tag is only
 * handed out again once every transfer of the command it carried has
 * answered.
 */
typedef struct _UASP_REQUEST
{
    struct _UASP_ADAPTER_EXTENSION *Adapter;

    /* The tag this request is known by, and the stream it uses */
    USHORT Tag;

    /* The request the class layer asked for, or NULL when nothing is running */
    PVOID Srb;

    /* Transfers yet to answer. The command is done when this reaches zero. */
    LONG Outstanding;

    /* Set while the tag is handed out */
    BOOLEAN Busy;

    /* What the class layer is told once every transfer has answered */
    UCHAR SrbStatus;

    /* The data stage of this command, when it has one */
    BOOLEAN HasData;
    BOOLEAN DataIn;
    PVOID DataBuffer;
    PMDL DataMdl;
    ULONG DataLength;
    ULONG DataTransferred;

    UASP_TRANSFER Transfer[UaspTransferMax];

    /* The stream pipes of this tag, when the device has streams */
    USBD_PIPE_HANDLE DataInStream;
    USBD_PIPE_HANDLE DataOutStream;
    USBD_PIPE_HANDLE StatusStream;

    /* The command going out, and whatever comes back on the status pipe */
    PUAS_COMMAND_IU CommandIu;
    PVOID StatusIu;
    PVOID ReadyIu;
} UASP_REQUEST, *PUASP_REQUEST;

/*
 * Per device state. Storport hands this out as the device extension, so
 * everything the miniport knows about one device hangs off it.
 */
typedef struct _UASP_ADAPTER_EXTENSION
{
    /* Where the miniport sits in the device stack */
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT LowerDeviceObject;

    USBD_HANDLE UsbdHandle;

    /* What the device said about itself */
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;

    /* The interface as the stack opened it, and the handle of that configuration */
    PUSBD_INTERFACE_INFORMATION InterfaceInformation;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;

    /* Which endpoint of the interface serves which purpose */
    UCHAR CommandPipeIndex;
    UCHAR StatusPipeIndex;
    UCHAR DataInPipeIndex;
    UCHAR DataOutPipeIndex;

    USBD_PIPE_HANDLE CommandPipe;
    USBD_PIPE_HANDLE StatusPipe;
    USBD_PIPE_HANDLE DataInPipe;
    USBD_PIPE_HANDLE DataOutPipe;

    /* What the endpoint descriptors offered, and what the controller granted */
    USHORT StreamsOffered;
    USHORT StreamCount;
    BOOLEAN StreamsOpen;

    /* Commands that may be outstanding, which is the size of the request array */
    USHORT RequestCount;

    ULONG MaximumTransferLength;

    /* Tags, and the state of the queue they are handed out from */
    KSPIN_LOCK QueueLock;
    UASP_QUEUE_STATE QueueState;
    PUASP_REQUEST Requests;
    USHORT NextFreeTag;

    /* Set once the device answered and requests may be issued */
    BOOLEAN Started;

    /* Set while the device is being taken away */
    BOOLEAN Removing;

    /* Memory the information units were carved out of */
    PVOID IuBuffer;
    ULONG IuBufferSize;
} UASP_ADAPTER_EXTENSION, *PUASP_ADAPTER_EXTENSION;

/*
 * Tags and information unit lengths travel big endian, which is the one
 * place this driver has to care about byte order.
 */
FORCEINLINE
VOID
UaspStoreBigEndian16(
    _Out_writes_bytes_(2) PUCHAR Destination,
    _In_ USHORT Value)
{
    Destination[0] = (UCHAR)(Value >> 8);
    Destination[1] = (UCHAR)Value;
}

FORCEINLINE
USHORT
UaspReadBigEndian16(
    _In_reads_bytes_(2) const UCHAR *Source)
{
    return (USHORT)((Source[0] << 8) | Source[1]);
}

/* uasdev.c */

NTSTATUS
UaspStartDevice(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

VOID
UaspStopDevice(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

/* uasurb.c */

NTSTATUS
UaspSendUrbSynchronously(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PURB Urb,
    _In_ BOOLEAN FromUsbdHandle);

NTSTATUS
UaspGetDescriptor(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR DescriptorType,
    _In_ UCHAR Index,
    _In_ ULONG Length,
    _Outptr_result_maybenull_ PVOID *Descriptor);

NTSTATUS
UaspResetPipe(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ USBD_PIPE_HANDLE Pipe);

VOID
UaspQueuePipeReset(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ USBD_PIPE_HANDLE Pipe);

BOOLEAN
UaspIsPipeStalled(
    _In_ USBD_STATUS UsbdStatus);

UCHAR
UaspTranslateStatus(
    _In_ NTSTATUS Status);

/* uasqueue.c */

NTSTATUS
UaspCreateRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

VOID
UaspDeleteRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

PUASP_REQUEST
UaspAcquireRequest(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

VOID
UaspReleaseRequest(
    _In_ PUASP_REQUEST Request);

VOID
UaspFreezeQueue(
    _In_ PUASP_ADAPTER_EXTENSION Adapter);

/* uasio.c */

VOID
UaspIssueCommand(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb);

VOID
UaspCompleteSrb(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus);

/* uaspstor.c */

ULONG
NTAPI
UaspFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PVOID LowerDevice,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _Out_ PBOOLEAN Again);

BOOLEAN
NTAPI
UaspInitialize(
    _In_ PVOID DeviceExtension);

BOOLEAN
NTAPI
UaspPassiveInitialize(
    _In_ PVOID DeviceExtension);

BOOLEAN
NTAPI
UaspStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb);

BOOLEAN
NTAPI
UaspResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId);

SCSI_ADAPTER_CONTROL_STATUS
NTAPI
UaspAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters);

VOID
NTAPI
UaspFreeAdapterResources(
    _In_ PVOID DeviceExtension);

#endif /* _UASPSTOR_H_ */
