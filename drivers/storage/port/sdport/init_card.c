/*
 * PROJECT:     ReactOS Storage Stack / SD host controller port driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SD/eMMC card identification sequence
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Standard SD/MMC command indices. */
#define SD_CMD_GO_IDLE_STATE      0
#define SD_CMD_SEND_OP_COND       1
#define SD_CMD_ALL_SEND_CID       2
#define SD_CMD_SEND_RELATIVE_ADDR 3
#define SD_CMD_SELECT_CARD        7
#define SD_CMD_SEND_IF_COND       8
#define SD_CMD_SEND_CSD           9
#define SD_CMD_SET_BLOCKLEN       16
#define SD_CMD_APP_CMD            55
#define SD_ACMD_SD_SEND_OP_COND   41

#define SD_IF_COND_ARGUMENT       0x000001AA
#define SD_OCR_BUSY               0x80000000
#define SD_OCR_HIGH_CAPACITY      0x40000000
#define SD_ACMD41_VOLTAGE_WINDOW  0x00FF8000
#define SD_ACMD41_HCS             0x40000000

#define SD_DEFAULT_BLOCK_LENGTH   512

static
NTSTATUS
SdPortBusOperation(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ SDPORT_BUS_OPERATION_TYPE Type,
    _In_ ULONG Parameter)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    SDPORT_BUS_OPERATION BusOperation;

    if (InitData->IssueBusOperation == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&BusOperation, sizeof(BusOperation));
    BusOperation.Type = Type;
    BusOperation.Parameters.Parameter = Parameter;
    return InitData->IssueBusOperation(Slot->PrivateExtension, &BusOperation);
}

static
NTSTATUS
SdPortCommand(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ UCHAR Index,
    _In_ ULONG Argument,
    _In_ SDPORT_RESPONSE_TYPE ResponseType,
    _Out_writes_opt_(4) PULONG Response)
{
    SDPORT_REQUEST Request;
    NTSTATUS Status;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestCommand;
    Request.Data.Command.Index = Index;
    Request.Data.Command.Argument = Argument;
    Request.Data.Command.ResponseType = ResponseType;
    Request.Data.Command.TransferType = SdTransferTypeCommandOnly;

    Status = SdPortIssueSynchronousRequest(Slot, &Request);
    if (Response != NULL)
        RtlCopyMemory(Response, Request.ResponseBuffer, sizeof(Request.ResponseBuffer));

    return Status;
}

static
NTSTATUS
SdPortNegotiateVoltage(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ BOOLEAN Version2)
{
    ULONG Response[4];
    ULONG Argument;
    ULONG Attempt;
    NTSTATUS Status;

    Argument = SD_ACMD41_VOLTAGE_WINDOW;
    if (Version2)
        Argument |= SD_ACMD41_HCS;

    for (Attempt = 0; Attempt < 1000; Attempt++)
    {
        Status = SdPortCommand(Slot, SD_CMD_APP_CMD, 0, SdResponseTypeR1, NULL);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = SdPortCommand(Slot, SD_ACMD_SD_SEND_OP_COND, Argument, SdResponseTypeR3, Response);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Response[0] & SD_OCR_BUSY)
        {
            Slot->HighCapacity = (Response[0] & SD_OCR_HIGH_CAPACITY) ? TRUE : FALSE;
            return STATUS_SUCCESS;
        }

        SdPortWait(1000);
    }

    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS
NTAPI
SdPortInitializeCard(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    ULONG Response[4];
    BOOLEAN Version2;
    NTSTATUS Status;

    DPRINT1("sdport: initializing card in slot %u\n", Slot->SlotNumber);

    if (InitData->GetCardDetectState != NULL &&
        !InitData->GetCardDetectState(Slot->PrivateExtension))
    {
        DPRINT1("sdport: no card present in slot %u\n", Slot->SlotNumber);
        Slot->Present = FALSE;
        return STATUS_NO_MEDIA_IN_DEVICE;
    }

    SdPortBusOperation(Slot, SdResetHost, 0);
    SdPortBusOperation(Slot, SdSetVoltage, 33);
    SdPortBusOperation(Slot, SdSetBusWidth, SdBusWidth1Bit);
    SdPortBusOperation(Slot, SdSetClock, 400);

    Status = SdPortCommand(Slot, SD_CMD_GO_IDLE_STATE, 0, SdResponseTypeNone, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SdPortCommand(Slot, SD_CMD_SEND_IF_COND, SD_IF_COND_ARGUMENT, SdResponseTypeR6, Response);
    Version2 = (NT_SUCCESS(Status) && (Response[0] & 0xFF) == (SD_IF_COND_ARGUMENT & 0xFF));

    Status = SdPortNegotiateVoltage(Slot, Version2);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SdPortCommand(Slot, SD_CMD_ALL_SEND_CID, 0, SdResponseTypeR2, Response);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Slot->Cid, Response, sizeof(Slot->Cid));

    Status = SdPortCommand(Slot, SD_CMD_SEND_RELATIVE_ADDR, 0, SdResponseTypeR6, Response);
    if (!NT_SUCCESS(Status))
        return Status;
    Slot->Rca = (USHORT)(Response[0] >> 16);

    Status = SdPortCommand(Slot, SD_CMD_SEND_CSD, (ULONG)Slot->Rca << 16, SdResponseTypeR2, Response);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Slot->Csd, Response, sizeof(Slot->Csd));

    Status = SdPortCommand(Slot, SD_CMD_SELECT_CARD, (ULONG)Slot->Rca << 16, SdResponseTypeR1B, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Slot->HighCapacity)
        SdPortCommand(Slot, SD_CMD_SET_BLOCKLEN, SD_DEFAULT_BLOCK_LENGTH, SdResponseTypeR1, NULL);

    SdPortBusOperation(Slot, SdSetClock, 25000);

    Slot->BusWidth = SdBusWidth1Bit;
    Slot->ClockFrequencyKhz = 25000;
    Slot->Present = TRUE;
    Slot->CardInitialized = TRUE;
    DPRINT1("sdport: card initialized, RCA 0x%04x, high capacity %u\n",
            Slot->Rca, Slot->HighCapacity);
    return STATUS_SUCCESS;
}
