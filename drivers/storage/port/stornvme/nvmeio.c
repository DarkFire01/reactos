/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Read, write and flush translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Finds the page aligned area of a request context.
 *
 * The context is one page larger than it needs to be so that a whole page can
 * be found inside it. A page of virtual memory is one page of physical memory,
 * which is what a list of region pages has to live in.
 */
static
PULONGLONG
NvmpPrpList(
    _In_ PNVME_REQUEST_CONTEXT Context)
{
    ULONG_PTR Address = (ULONG_PTR)(Context + 1);

    return (PULONGLONG)ROUND_TO_PAGES(Address);
}


/**
 * @brief Pulls the block address and count out of a read or write command.
 */
static
BOOLEAN
NvmpReadWriteParameters(
    _In_ PCDB Cdb,
    _Out_ PULONGLONG BlockAddress,
    _Out_ PULONG BlockCount)
{
    switch (Cdb->CDB6GENERIC.OperationCode)
    {
        case SCSIOP_READ6:
        case SCSIOP_WRITE6:
            *BlockAddress = ((ULONGLONG)(Cdb->CDB6READWRITE.LogicalBlockMsb1 & 0x1F) << 16) |
                            ((ULONGLONG)Cdb->CDB6READWRITE.LogicalBlockMsb0 << 8) |
                            Cdb->CDB6READWRITE.LogicalBlockLsb;

            /* A count of zero means 256 blocks in the six byte form */
            *BlockCount = Cdb->CDB6READWRITE.TransferBlocks;
            if (*BlockCount == 0)
                *BlockCount = 256;
            return TRUE;

        case SCSIOP_READ:
        case SCSIOP_WRITE:
        {
            ULONG Address;
            USHORT Count;

            REVERSE_BYTES(&Address, &Cdb->CDB10.LogicalBlockByte0);
            REVERSE_BYTES_SHORT(&Count, &Cdb->CDB10.TransferBlocksMsb);

            *BlockAddress = Address;
            *BlockCount = Count;
            return TRUE;
        }

        case SCSIOP_READ12:
        case SCSIOP_WRITE12:
        {
            ULONG Address;
            ULONG Count;

            REVERSE_BYTES(&Address, &Cdb->CDB12.LogicalBlock);
            REVERSE_BYTES(&Count, &Cdb->CDB12.TransferLength);

            *BlockAddress = Address;
            *BlockCount = Count;
            return TRUE;
        }

        case SCSIOP_READ16:
        case SCSIOP_WRITE16:
        {
            ULONGLONG Address;
            ULONG Count;

            REVERSE_BYTES_QUAD(&Address, &Cdb->CDB16.LogicalBlock);
            REVERSE_BYTES(&Count, &Cdb->CDB16.TransferLength);

            *BlockAddress = Address;
            *BlockCount = Count;
            return TRUE;
        }

        default:
            return FALSE;
    }
}


/**
 * @brief Describes a transfer to the controller as physical region pages.
 *
 * The first entry may begin part way into a page. Everything after it has to
 * start on a page boundary, which is what the scatter gather list from
 * storport already guarantees for a transfer of more than one element.
 */
static
BOOLEAN
NvmpBuildPrp(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_REQUEST_CONTEXT Context,
    _In_ PSTOR_SCATTER_GATHER_LIST ScatterGather,
    _In_ PNVME_COMMAND Command)
{
    PULONGLONG List;
    PHYSICAL_ADDRESS ListAddress;
    ULONGLONG Address;
    ULONG Element;
    ULONG Offset;
    ULONG Count = 0;
    ULONG Length;

    List = NvmpPrpList(Context);

    /*
     * Walk every page each element covers, since one element may well span
     * several of them.
     */
    for (Element = 0; Element < ScatterGather->NumberOfElements; Element++)
    {
        Address = ScatterGather->List[Element].PhysicalAddress.QuadPart;
        Length = ScatterGather->List[Element].Length;

        /* Only the very first page of the transfer may carry an offset */
        Offset = (ULONG)(Address & (Adapter->PageSize - 1));
        if (Offset != 0 && Count != 0)
        {
            DPRINT1("Transfer element %lu is not page aligned\n", Element);
            return FALSE;
        }

        while (Length != 0)
        {
            ULONG Step = Adapter->PageSize - Offset;

            if (Step > Length)
                Step = Length;

            if (Count >= NVME_MAX_PRP_ENTRIES)
            {
                DPRINT1("Transfer needs more than %lu region pages\n",
                        (ULONG)NVME_MAX_PRP_ENTRIES);
                return FALSE;
            }

            List[Count++] = Address;

            Address += Step;
            Length -= Step;
            Offset = 0;
        }
    }

    if (Count == 0)
    {
        DPRINT1("Transfer describes no memory\n");
        return FALSE;
    }

    Command->PRP1 = List[0];

    if (Count == 1)
    {
        Command->PRP2 = 0;
        return TRUE;
    }

    if (Count == 2)
    {
        Command->PRP2 = List[1];
        return TRUE;
    }

    /*
     * With more than two pages the rest go in a list of their own, which the
     * second entry then points at.
     */
    ListAddress = StorPortGetPhysicalAddress(Adapter, NULL, &List[1], &Length);
    if (ListAddress.QuadPart == 0 || Length < (Count - 1) * sizeof(ULONGLONG))
    {
        DPRINT1("Region page list is not addressable\n");
        return FALSE;
    }

    Command->PRP2 = ListAddress.QuadPart;

    return TRUE;
}


/**
 * @brief Turns a read or write into the matching NVM command.
 */
static
BOOLEAN
NvmpBuildReadWrite(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ PNVME_NAMESPACE Namespace,
    _In_ PNVME_REQUEST_CONTEXT Context,
    _In_ PCDB Cdb)
{
    PNVME_COMMAND Command = &Context->Command;
    PSTOR_SCATTER_GATHER_LIST ScatterGather;
    ULONGLONG BlockAddress;
    ULONG BlockCount;
    BOOLEAN Write;

    if (!NvmpReadWriteParameters(Cdb, &BlockAddress, &BlockCount))
        return FALSE;

    if (BlockCount == 0)
    {
        /* A transfer of no blocks is not an error, it simply moves nothing */
        SrbSetDataTransferLength(Srb, 0);
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    if (BlockAddress + BlockCount > Namespace->BlockCount)
    {
        NvmpCompleteWithSense(Adapter, Srb, SCSI_SENSE_ILLEGAL_REQUEST,
                              SCSI_ADSENSE_ILLEGAL_BLOCK, 0);
        return TRUE;
    }

    switch (Cdb->CDB6GENERIC.OperationCode)
    {
        case SCSIOP_WRITE6:
        case SCSIOP_WRITE:
        case SCSIOP_WRITE12:
        case SCSIOP_WRITE16:
            Write = TRUE;
            break;

        default:
            Write = FALSE;
            break;
    }

    RtlZeroMemory(Command, sizeof(*Command));

    Command->CDW0.OPC = Write ? NVME_NVM_COMMAND_WRITE : NVME_NVM_COMMAND_READ;
    Command->NSID = Namespace->NamespaceId;
    Command->u.READWRITE.LBALOW = (ULONG)BlockAddress;
    Command->u.READWRITE.LBAHIGH = (ULONG)(BlockAddress >> 32);

    /* The block count is reported one less than the blocks moved */
    Command->u.READWRITE.CDW12.NLB = (USHORT)(BlockCount - 1);

    ScatterGather = StorPortGetScatterGatherList(Adapter, Srb);
    if (ScatterGather == NULL)
    {
        DPRINT1("Request has no scatter gather list\n");
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }

    if (!NvmpBuildPrp(Adapter, Context, ScatterGather, Command))
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }

    return TRUE;
}


/**
 * @brief Builds the command that pushes a namespace's write cache out.
 *
 * Reached both from the SCSI command that asks for it and from the request
 * functions storport sends when the system is going down.
 *
 * @return TRUE when the request is ready to be posted, FALSE when it has
 *         already been finished one way or another.
 */
BOOLEAN
NvmpBuildFlush(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb)
{
    PNVME_REQUEST_CONTEXT Context;
    PNVME_NAMESPACE Namespace;
    UCHAR Lun;

    SrbSetDataTransferLength(Srb, 0);

    /* Nothing to push out when the controller holds nothing back */
    if (!Adapter->VolatileWriteCache)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_SUCCESS);
        return FALSE;
    }

    Context = SrbGetMiniportContext(Srb);
    if (Context == NULL)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_ERROR);
        return FALSE;
    }

    SrbGetPathTargetLun(Srb, NULL, NULL, &Lun);

    Namespace = NvmpNamespaceFromLun(Adapter, Lun);
    if (Namespace == NULL)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_NO_DEVICE);
        return FALSE;
    }

    RtlZeroMemory(Context, sizeof(*Context));
    Context->Srb = Srb;

    Context->Command.CDW0.OPC = NVME_NVM_COMMAND_FLUSH;
    Context->Command.NSID = Namespace->NamespaceId;

    return TRUE;
}


/**
 * @brief Builds the command a request will be carried out by.
 *
 * @return TRUE when the request is ready to be posted, FALSE when it has
 *         already been finished one way or another.
 */
BOOLEAN
NvmpBuildCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb)
{
    PNVME_REQUEST_CONTEXT Context;
    PNVME_NAMESPACE Namespace;
    PCDB Cdb;
    UCHAR Lun;

    Context = SrbGetMiniportContext(Srb);
    if (Context == NULL)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_ERROR);
        return FALSE;
    }

    RtlZeroMemory(Context, sizeof(*Context));
    Context->Srb = Srb;

    Cdb = SrbGetCdb(Srb);
    SrbGetPathTargetLun(Srb, NULL, NULL, &Lun);

    Namespace = NvmpNamespaceFromLun(Adapter, Lun);
    if (Cdb == NULL || Namespace == NULL)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_NO_DEVICE);
        return FALSE;
    }

    switch (Cdb->CDB6GENERIC.OperationCode)
    {
        case SCSIOP_READ6:
        case SCSIOP_READ:
        case SCSIOP_READ12:
        case SCSIOP_READ16:
        case SCSIOP_WRITE6:
        case SCSIOP_WRITE:
        case SCSIOP_WRITE12:
        case SCSIOP_WRITE16:
            if (!NvmpBuildReadWrite(Adapter, Srb, Namespace, Context, Cdb))
            {
                NvmpCompleteWithSense(Adapter, Srb, SCSI_SENSE_ILLEGAL_REQUEST,
                                      SCSI_ADSENSE_INVALID_CDB, 0);
                return FALSE;
            }

            /* The helper finishes the request itself when it cannot proceed */
            return (SrbGetSrbStatus(Srb) == SRB_STATUS_PENDING);

        case SCSIOP_SYNCHRONIZE_CACHE:
        case SCSIOP_SYNCHRONIZE_CACHE16:
            return NvmpBuildFlush(Adapter, Srb);

        default:
            NvmpCompleteWithSense(Adapter, Srb, SCSI_SENSE_ILLEGAL_REQUEST,
                                  SCSI_ADSENSE_ILLEGAL_COMMAND, 0);
            return FALSE;
    }
}


/**
 * @brief Fills the pool of command identifiers.
 *
 * One identifier short of the queue depth, so that the submission tail can
 * never catch up with the head and make a full queue look like an empty one.
 */
VOID
NvmpInitializeCommandIds(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    ULONG Index;

    Adapter->FreeCommandCount = Adapter->IoQueueDepth - 1;

    for (Index = 0; Index < Adapter->FreeCommandCount; Index++)
        Adapter->FreeCommandIds[Index] = (USHORT)Index;
}


/**
 * @brief Posts a prepared command to the queue it belongs on.
 */
VOID
NvmpPostCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb)
{
    PNVME_REQUEST_CONTEXT Context;
    PNVME_QUEUE_PAIR Queue = &Adapter->IoQueue;
    STOR_LOCK_HANDLE LockHandle;
    USHORT CommandId;

    Context = SrbGetMiniportContext(Srb);

    /*
     * The interrupt lock is the one that also shuts out the service
     * routine, which is the other place the outstanding request table and
     * the queue are touched.
     */
    StorPortAcquireSpinLock(Adapter, InterruptLock, NULL, &LockHandle);

    if (Adapter->FreeCommandCount == 0)
    {
        StorPortReleaseSpinLock(Adapter, &LockHandle);

        /* Nothing is lost, storport hands the request back later */
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_BUSY);
        return;
    }

    /* The identifier names the request when the controller answers */
    CommandId = Adapter->FreeCommandIds[--Adapter->FreeCommandCount];
    Context->Command.CDW0.CID = CommandId;

    Adapter->Requests[CommandId] = Context;

    NvmpSubmitCommand(Adapter, Queue, &Context->Command);

    StorPortReleaseSpinLock(Adapter, &LockHandle);
}


/**
 * @brief Says what a failed command should look like to the class layer.
 *
 * The status the controller reports is finer grained than the sense keys it
 * has to be told in, so several codes land on the same answer.
 */
static
VOID
NvmpSenseForStatus(
    _In_ NVME_COMMAND_STATUS Status,
    _Out_ PUCHAR SenseKey,
    _Out_ PUCHAR AdditionalSenseCode)
{
    *SenseKey = SCSI_SENSE_MEDIUM_ERROR;
    *AdditionalSenseCode = SCSI_ADSENSE_NO_SENSE;

    if (Status.SCT == NVME_STATUS_TYPE_MEDIA_ERROR)
    {
        switch (Status.SC)
        {
            case NVME_STATUS_NVM_WRITE_FAULT:
                *AdditionalSenseCode = SCSI_ADSENSE_WRITE_ERROR;
                break;

            case NVME_STATUS_NVM_UNRECOVERED_READ_ERROR:
            case NVME_STATUS_NVM_END_TO_END_GUARD_CHECK_ERROR:
            case NVME_STATUS_NVM_END_TO_END_APPLICATION_TAG_CHECK_ERROR:
            case NVME_STATUS_NVM_END_TO_END_REFERENCE_TAG_CHECK_ERROR:
                *AdditionalSenseCode = SCSI_ADSENSE_UNRECOVERED_ERROR;
                break;

            case NVME_STATUS_NVM_ACCESS_DENIED:
                *SenseKey = SCSI_SENSE_DATA_PROTECT;
                *AdditionalSenseCode = SCSI_ADSENSE_WRITE_PROTECT;
                break;

            default:
                break;
        }

        return;
    }

    if (Status.SCT != NVME_STATUS_TYPE_GENERIC_COMMAND)
    {
        /* Nothing else names a medium problem, so blame the request */
        *SenseKey = SCSI_SENSE_ILLEGAL_REQUEST;
        *AdditionalSenseCode = SCSI_ADSENSE_INVALID_CDB;
        return;
    }

    switch (Status.SC)
    {
        case NVME_STATUS_INVALID_COMMAND_OPCODE:
            *SenseKey = SCSI_SENSE_ILLEGAL_REQUEST;
            *AdditionalSenseCode = SCSI_ADSENSE_ILLEGAL_COMMAND;
            break;

        case NVME_STATUS_INVALID_FIELD_IN_COMMAND:
        case NVME_STATUS_INVALID_NAMESPACE_OR_FORMAT:
        case NVME_STATUS_PRP_OFFSET_INVALID:
            *SenseKey = SCSI_SENSE_ILLEGAL_REQUEST;
            *AdditionalSenseCode = SCSI_ADSENSE_INVALID_CDB;
            break;

        case NVME_STATUS_NVM_LBA_OUT_OF_RANGE:
            *SenseKey = SCSI_SENSE_ILLEGAL_REQUEST;
            *AdditionalSenseCode = SCSI_ADSENSE_ILLEGAL_BLOCK;
            break;

        case NVME_STATUS_NVM_NAMESPACE_NOT_READY:
        case NVME_STATUS_FORMAT_IN_PROGRESS:
        case NVME_STATUS_ADMIN_COMMAND_MEDIA_NOT_READY:
            *SenseKey = SCSI_SENSE_NOT_READY;
            *AdditionalSenseCode = SCSI_ADSENSE_LUN_NOT_READY;
            break;

        case NVME_STATUS_NAMESPACE_IS_WRITE_PROTECTED:
            *SenseKey = SCSI_SENSE_DATA_PROTECT;
            *AdditionalSenseCode = SCSI_ADSENSE_WRITE_PROTECT;
            break;

        case NVME_STATUS_INTERNAL_DEVICE_ERROR:
            *SenseKey = SCSI_SENSE_HARDWARE_ERROR;
            break;

        case NVME_STATUS_COMMAND_ABORT_REQUESTED:
        case NVME_STATUS_COMMAND_ABORTED_DUE_TO_SQ_DELETION:
        case NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_FUSED_COMMAND:
        case NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_MISSING_COMMAND:
        case NVME_STATUS_COMMAND_ABORTED_DUE_TO_POWER_LOSS_NOTIFICATION:
        case NVME_STATUS_COMMAND_ABORTED_DUE_TO_PREEMPT_ABORT:
            *SenseKey = SCSI_SENSE_ABORTED_COMMAND;
            break;

        default:
            break;
    }
}


/**
 * @brief Turns a completion into the result of the request that caused it.
 */
VOID
NvmpCompleteFromEntry(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_COMPLETION_ENTRY Completion)
{
    PNVME_REQUEST_CONTEXT Context;
    USHORT CommandId = Completion->DW3.CID;
    UCHAR SenseKey;
    UCHAR AdditionalSenseCode;
    PVOID Srb;

    if (CommandId >= Adapter->IoQueueDepth)
    {
        DPRINT1("Completion names command %u, out of range\n", CommandId);
        return;
    }

    Context = Adapter->Requests[CommandId];
    if (Context == NULL)
    {
        DPRINT1("Completion names command %u, which is not outstanding\n", CommandId);
        return;
    }

    Adapter->Requests[CommandId] = NULL;
    Adapter->FreeCommandIds[Adapter->FreeCommandCount++] = CommandId;
    Srb = Context->Srb;

    if (Completion->DW3.Status.SC == NVME_STATUS_SUCCESS_COMPLETION &&
        Completion->DW3.Status.SCT == NVME_STATUS_TYPE_GENERIC_COMMAND)
    {
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    DPRINT1("Command %u failed, type %u code 0x%02x\n",
            CommandId, Completion->DW3.Status.SCT, Completion->DW3.Status.SC);

    SrbSetDataTransferLength(Srb, 0);

    /* A reservation is reported by the status alone, with no sense behind it */
    if (Completion->DW3.Status.SCT == NVME_STATUS_TYPE_GENERIC_COMMAND &&
        Completion->DW3.Status.SC == NVME_STATUS_NVM_RESERVATION_CONFLICT)
    {
        SrbSetScsiStatus(Srb, SCSISTAT_RESERVATION_CONFLICT);
        NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_ERROR);
        return;
    }

    NvmpSenseForStatus(Completion->DW3.Status, &SenseKey, &AdditionalSenseCode);
    NvmpCompleteWithSense(Adapter, Srb, SenseKey, AdditionalSenseCode, 0);
}
