/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Common header file
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _STORNVME_H_
#define _STORNVME_H_

#include <ntddk.h>
#include <storport.h>
#include <srbhelper.h>
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

/* Entries in the admin queues. The specification caps this at 4096. */
#define NVME_ADMIN_QUEUE_DEPTH  64

/*
 * Namespaces become logical units, so the count is what a single byte LUN can
 * name. A controller with more than this keeps the rest to itself.
 */
#define NVME_MAX_NAMESPACES     255

/*
 * Nothing larger is offered to the class layer, whatever the controller
 * says. The limit is set so a transfer never needs more region pages than
 * one page of them can name, even when it begins part way into a page.
 */
#define NVME_MAX_TRANSFER_LENGTH (1024 * 1024)

/* The one IO queue pair this driver drives, and how deep it is willing to go */
#define NVME_IO_QUEUE_ID        1
#define NVME_IO_QUEUE_DEPTH     1024

/*
 * A page of region page entries describes a transfer of up to this many pages
 * beyond the first two, which are named by the command itself.
 */
#define NVME_MAX_PRP_ENTRIES    (PAGE_SIZE / sizeof(ULONGLONG))

/* What a single command may be asked to carry */
#define NVME_MAX_OUTSTANDING    NVME_IO_QUEUE_DEPTH

/* One namespace, as far as this driver is concerned */
typedef struct _NVME_NAMESPACE
{
    ULONG NamespaceId;
    ULONGLONG BlockCount;
    ULONG BlockSize;
    ULONG BlockShift;
    BOOLEAN Present;
} NVME_NAMESPACE, *PNVME_NAMESPACE;

/* How long to wait between looks at CSTS while the controller settles */
#define NVME_POLL_INTERVAL_US   10000

/* How long an admin command run during startup may take */
#define NVME_ADMIN_TIMEOUT_MS   60000

/*
 * One submission and completion queue working as a pair. The admin pair is
 * queue zero; every IO pair gets an identifier of its own.
 */
typedef struct _NVME_QUEUE_PAIR
{
    PNVME_COMMAND SubmissionQueue;
    PHYSICAL_ADDRESS SubmissionAddress;
    ULONG SubmissionTail;

    PNVME_COMPLETION_ENTRY CompletionQueue;
    PHYSICAL_ADDRESS CompletionAddress;
    ULONG CompletionHead;

    /*
     * The controller flips the phase bit of an entry when it writes one, so
     * the host can tell a fresh completion from a stale one without the
     * queue being cleared. It starts at one and inverts on every wrap.
     */
    ULONG Phase;

    ULONG Depth;
    USHORT QueueId;
} NVME_QUEUE_PAIR, *PNVME_QUEUE_PAIR;

/*
 * What storport keeps for us alongside each request. The region page list
 * needs a page of its own, so the context is asked for one page more than it
 * uses and the list is taken from the aligned part of that.
 */
typedef struct _NVME_REQUEST_CONTEXT
{
    PVOID Srb;
    NVME_COMMAND Command;
} NVME_REQUEST_CONTEXT, *PNVME_REQUEST_CONTEXT;

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

    /* The admin queue pair, which carries every command before IO starts */
    NVME_QUEUE_PAIR AdminQueue;

    /* The queue pair every read and write goes through */
    NVME_QUEUE_PAIR IoQueue;
    ULONG IoQueueDepth;

    /* Handed out so a completion can be matched to the command it answers */
    USHORT AdminCommandId;

    /* Contiguous memory the queues and the scratch buffer were carved out of */
    PVOID QueueMemory;
    PHYSICAL_ADDRESS QueueMemoryAddress;
    ULONG QueueMemorySize;

    /*
     * One page the controller can write results into. Only used while
     * bringing the adapter up, where commands run one at a time.
     */
    PVOID ScratchBuffer;
    PHYSICAL_ADDRESS ScratchAddress;

    /* What the controller said about itself */
    ULONG NamespaceCount;
    UCHAR SerialNumber[20];
    UCHAR ModelNumber[40];
    UCHAR FirmwareRevision[8];

    /* Set when the controller has a write cache that needs flushing */
    BOOLEAN VolatileWriteCache;

    /* Indexed by namespace identifier less one */
    NVME_NAMESPACE Namespaces[NVME_MAX_NAMESPACES];

    /* Outstanding requests, indexed by the command identifier they carry */
    PNVME_REQUEST_CONTEXT Requests[NVME_MAX_OUTSTANDING];

    /*
     * Identifiers nothing is using, taken from the top. The controller answers
     * in whatever order it likes, so an identifier is only free again once its
     * own completion arrives.
     */
    USHORT FreeCommandIds[NVME_MAX_OUTSTANDING];
    ULONG FreeCommandCount;


    /* Set while running as part of a crash dump or hibernation stack */
    BOOLEAN DumpMode;

    /* Set once storport told us the adapter is on message interrupts */
    BOOLEAN MessageInterrupts;
} NVME_ADAPTER_EXTENSION, *PNVME_ADAPTER_EXTENSION;

/*
 * Doorbells sit after the register block, two per queue, spaced by a stride
 * the controller reports. The submission tail comes first, then the
 * completion head.
 */
FORCEINLINE
PULONG
NvmpSubmissionDoorbell(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG QueueId)
{
    return (PULONG)((PUCHAR)Adapter->Doorbells +
                    ((2 * QueueId) * Adapter->DoorbellStride));
}

FORCEINLINE
PULONG
NvmpCompletionDoorbell(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG QueueId)
{
    return (PULONG)((PUCHAR)Adapter->Doorbells +
                    (((2 * QueueId) + 1) * Adapter->DoorbellStride));
}

/* nvmectrl.c */

BOOLEAN
NvmpDisableController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

BOOLEAN
NvmpEnableController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

BOOLEAN
NvmpCreateAdminQueues(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo);

BOOLEAN
NvmpStartController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo);

/* nvmequeue.c */

VOID
NvmpSubmitCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue,
    _In_ PNVME_COMMAND Command);

BOOLEAN
NvmpNextCompletion(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue,
    _Out_ PNVME_COMPLETION_ENTRY Completion);

BOOLEAN
NvmpIssueAdminCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_COMMAND Command,
    _Out_opt_ PNVME_COMPLETION_ENTRY Completion);

/* nvmeid.c */

BOOLEAN
NvmpIdentifyController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

BOOLEAN
NvmpIdentifyNamespace(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NamespaceId,
    _Out_ PNVME_NAMESPACE Namespace);

BOOLEAN
NvmpEnumerateNamespaces(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

/* nvmeioq.c */

BOOLEAN
NvmpCreateIoQueues(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

VOID
NvmpCompleteRequest(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus);

/* nvmeio.c */

VOID
NvmpInitializeCommandIds(
    _In_ PNVME_ADAPTER_EXTENSION Adapter);

BOOLEAN
NvmpBuildFlush(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb);

BOOLEAN
NvmpBuildCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb);

VOID
NvmpPostCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb);

VOID
NvmpCompleteFromEntry(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_COMPLETION_ENTRY Completion);

/* nvmescsi.c */

PNVME_NAMESPACE
NvmpNamespaceFromLun(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Lun);

BOOLEAN
NvmpSetSenseData(
    _In_ PVOID Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode,
    _In_ UCHAR AdditionalSenseCodeQualifier);

VOID
NvmpCompleteWithSense(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode,
    _In_ UCHAR AdditionalSenseCodeQualifier);

BOOLEAN
NvmpTranslateScsi(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb);

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
