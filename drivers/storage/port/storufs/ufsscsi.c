/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SCSI request translation to Command UPIU and completion
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

static
USHORT
UfsBuildPrdt(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PUCHAR Ucd)
{
    PSTOR_SCATTER_GATHER_LIST ScatterGather;
    PUFS_PRDT_ENTRY Prdt = (PUFS_PRDT_ENTRY)(Ucd + UFS_UCD_PRDT_OFFSET);
    ULONG Element;

    ScatterGather = StorPortGetScatterGatherList(Adapter, Srb);
    if (ScatterGather == NULL || ScatterGather->NumberOfElements == 0)
        return 0;

    if (ScatterGather->NumberOfElements > UFS_PRDT_ENTRIES)
    {
        DPRINT1("UFS: scatter gather list too long (%lu)\n",
                ScatterGather->NumberOfElements);
        return 0;
    }

    for (Element = 0; Element < ScatterGather->NumberOfElements; Element++)
    {
        ULONGLONG Address = ScatterGather->List[Element].PhysicalAddress.QuadPart;
        ULONG Length = ScatterGather->List[Element].Length;

        Prdt[Element].BaseLower = (ULONG)Address;
        Prdt[Element].BaseUpper = (ULONG)(Address >> 32);
        Prdt[Element].Reserved = 0;
        Prdt[Element].ByteCount = Length - 1;
    }

    return (USHORT)ScatterGather->NumberOfElements;
}

BOOLEAN
UfsSubmitScsi(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUFS_COMMAND_UPIU Command;
    PUCHAR Ucd;
    PCDB Cdb;
    ULONG Flags;
    ULONG DataLength;
    ULONG Direction;
    USHORT PrdtEntries = 0;
    UCHAR Lun;
    LONG Slot;

    SrbGetPathTargetLun(Srb, NULL, NULL, &Lun);
    Flags = SrbGetSrbFlags(Srb);
    DataLength = SrbGetDataTransferLength(Srb);

    Slot = UfsAllocSlot(Adapter);
    if (Slot < 0)
    {
        StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_BUSY);
        return TRUE;
    }

    Ucd = UfsSlotUcd(Adapter, Slot);
    RtlZeroMemory(Ucd, UFS_UCD_SIZE);

    Command = (PUFS_COMMAND_UPIU)Ucd;
    Command->TransactionType = UFS_UPIU_COMMAND;
    Command->Lun = Lun;
    Command->TaskTag = (UCHAR)Slot;
    Command->CommandSetType = 0;
    Command->ExpectedDataLength = RtlUlongByteSwap(DataLength);

    if (Flags & SRB_FLAGS_DATA_IN)
    {
        Command->Flags = UFS_UPIU_FLAG_READ;
        Direction = UFS_UTRD_DD_READ;
    }
    else if (Flags & SRB_FLAGS_DATA_OUT)
    {
        Command->Flags = UFS_UPIU_FLAG_WRITE;
        Direction = UFS_UTRD_DD_WRITE;
    }
    else
    {
        Direction = UFS_UTRD_DD_NODATA;
    }

    Cdb = SrbGetCdb(Srb);
    if (Cdb != NULL)
        RtlCopyMemory(Command->Cdb, Cdb, sizeof(Command->Cdb));

    if (Direction != UFS_UTRD_DD_NODATA && DataLength != 0)
    {
        PrdtEntries = UfsBuildPrdt(Adapter, Srb, Ucd);
        if (PrdtEntries == 0)
        {
            UfsFreeSlot(Adapter, Slot);
            StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
    }

    UfsBuildUtrd(Adapter, Slot, Direction, PrdtEntries);

    Adapter->SlotSrb[Slot] = Srb;
    UfsRingDoorbell(Adapter, Slot);

    return TRUE;
}

static
VOID
UfsCompleteSlot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUFS_RESPONSE_UPIU Response;
    PUCHAR Ucd;
    PVOID SenseBuffer;
    UCHAR Ocs;
    UCHAR SrbStatus;
    UCHAR ScsiStatus = SCSISTAT_GOOD;

    Adapter->SlotSrb[Slot] = NULL;

    Ucd = UfsSlotUcd(Adapter, Slot);
    Ocs = (UCHAR)(Adapter->UtrdList[Slot].Dw2Ocs & 0xFF);
    Response = (PUFS_RESPONSE_UPIU)(Ucd + UFS_UCD_RESP_OFFSET);

    /* The UTP layer status (OCS) covers transport delivery; the SCSI status in
       the response UPIU carries the command result, as in the UFS/SCSI model. */
    if (Ocs != UFS_OCS_SUCCESS)
    {
        DPRINT1("UFS: slot %lu transport error, OCS 0x%02x\n", Slot, Ocs);
        SrbStatus = SRB_STATUS_ERROR;
    }
    else
    {
        ScsiStatus = Response->Status;
        if (ScsiStatus == SCSISTAT_GOOD)
        {
            SrbStatus = SRB_STATUS_SUCCESS;
            SrbSetDataTransferLength(Srb, SrbGetDataTransferLength(Srb));
        }
        else
        {
            SrbStatus = SRB_STATUS_ERROR;
            SenseBuffer = SrbGetSenseInfoBuffer(Srb);
            if (SenseBuffer != NULL)
            {
                ULONG SenseLength = SrbGetSenseInfoBufferLength(Srb);
                USHORT Available = RtlUshortByteSwap(Response->SenseDataLength);

                if (Available > sizeof(Response->SenseData))
                    Available = sizeof(Response->SenseData);
                if (Available > SenseLength)
                    Available = (USHORT)SenseLength;

                RtlCopyMemory(SenseBuffer, Response->SenseData, Available);
                SrbStatus = SRB_STATUS_ERROR | SRB_STATUS_AUTOSENSE_VALID;
            }
        }
    }

    SrbSetScsiStatus(Srb, ScsiStatus);
    UfsFreeSlot(Adapter, Slot);
    StorUfsCompleteSrb(Adapter, Srb, SrbStatus);
}

BOOLEAN
UfsScanCompletions(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    PSCSI_REQUEST_BLOCK Srb;
    ULONG Status;
    ULONG Slot;

    Status = UfsRead(Adapter, UFS_REG_IS);
    if (Status == 0)
        return FALSE;

    /* Acknowledge everything we just observed (write 1 to clear). */
    UfsWrite(Adapter, UFS_REG_IS, Status);

    if (Status & UFS_IS_FATAL_MASK)
        DPRINT1("UFS: fatal interrupt status 0x%08lx\n", Status);

    /*
     * A slot is done once the controller has written its OCS.  The descriptor
     * starts at OCS_INVALID, so an in-flight slot never looks complete before
     * the hardware finishes, which keeps submission and completion unserialized.
     */
    for (Slot = 0; Slot < Adapter->Nutrs; Slot++)
    {
        Srb = Adapter->SlotSrb[Slot];
        if (Srb != NULL &&
            (UCHAR)(Adapter->UtrdList[Slot].Dw2Ocs & 0xFF) != UFS_OCS_INVALID)
        {
            UfsCompleteSlot(Adapter, Slot, Srb);
        }
    }

    return TRUE;
}
