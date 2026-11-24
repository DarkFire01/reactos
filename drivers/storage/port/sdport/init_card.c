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
#define MMC_CMD_SWITCH            6
#define SD_CMD_SELECT_CARD        7
#define SD_CMD_SEND_IF_COND       8
#define MMC_CMD_SEND_EXT_CSD      8
#define SD_CMD_SEND_CSD           9
#define SD_CMD_SEND_STATUS        13
#define SD_CMD_SET_BLOCKLEN       16
#define SD_CMD_APP_CMD            55
#define SD_ACMD_SD_SEND_OP_COND   41

#define SD_IF_COND_ARGUMENT       0x000001AA
#define SD_IF_COND_ECHO_MASK      0x00000FFF
#define SD_OCR_BUSY               0x80000000
#define SD_OCR_HIGH_CAPACITY      0x40000000
#define SD_ACMD41_VOLTAGE_WINDOW  0x00FF8000
#define SD_ACMD41_HCS             0x40000000

/* MMC OCR access mode lives in bits 30:29, the 2.7-3.6V window in bits 23:15. */
#define MMC_OCR_ACCESS_MODE_MASK  0x60000000
#define MMC_OCR_ACCESS_SECTOR     0x40000000
#define MMC_OCR_VOLTAGE_WINDOW    0x00FF8000

/* MMC devices take their relative address from the host. */
#define MMC_HOST_RCA              0x0001

/* Extended CSD byte offsets. */
#define EXT_CSD_ERASE_GROUP_DEF   175
#define EXT_CSD_BUS_WIDTH         183
#define EXT_CSD_HS_TIMING         185
#define EXT_CSD_POWER_CLASS       187
#define EXT_CSD_REV               192
#define EXT_CSD_DEVICE_TYPE       196
#define EXT_CSD_PWR_CL_52_360     202
#define EXT_CSD_PWR_CL_26_360     203
#define EXT_CSD_SEC_COUNT         212
#define EXT_CSD_HC_WP_GRP_SIZE    221
#define EXT_CSD_HC_ERASE_GRP_SIZE 224

#define EXT_CSD_DEVICE_TYPE_HS_52 0x02
#define EXT_CSD_BUS_WIDTH_1       0
#define EXT_CSD_BUS_WIDTH_4       1
#define EXT_CSD_BUS_WIDTH_8       2

/* CMD6 access mode that writes the value byte into the indexed field. */
#define MMC_SWITCH_WRITE_BYTE     3

/* R1 card status fields. */
#define SD_STATUS_SWITCH_ERROR    0x00000080
#define SD_STATUS_READY_FOR_DATA  0x00000100
#define SD_STATUS_STATE_SHIFT     9
#define SD_STATUS_STATE_MASK      0xF
#define SD_STATE_TRANSFER         4

#define SD_DEFAULT_BLOCK_LENGTH   512

/* Bus clock rates in kHz. */
#define SD_CLOCK_IDENTIFY         400
#define SD_CLOCK_DEFAULT          25000
#define MMC_CLOCK_LEGACY          20000
#define MMC_CLOCK_DEFAULT         26000
#define MMC_CLOCK_HIGH_SPEED      52000

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
VOID
SdPortSetClock(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ ULONG FrequencyKhz)
{
    if (NT_SUCCESS(SdPortBusOperation(Slot, SdSetClock, FrequencyKhz)))
        Slot->ClockFrequencyKhz = FrequencyKhz;
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
SdPortGoIdle(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    NTSTATUS Status;

    Status = SdPortCommand(Slot, SD_CMD_GO_IDLE_STATE, 0, SdResponseTypeNone, NULL);
    if (NT_SUCCESS(Status))
        SdPortWait(10000);

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

/* CMD1 loop. A ready MMC device reports its access mode in the returned OCR. */
static
NTSTATUS
SdPortMmcSendOpCond(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    ULONG Response[4];
    ULONG Attempt;
    ULONG Ocr;
    NTSTATUS Status;

    for (Attempt = 0; Attempt < 500; Attempt++)
    {
        Status = SdPortCommand(Slot,
                               SD_CMD_SEND_OP_COND,
                               MMC_OCR_ACCESS_SECTOR | MMC_OCR_VOLTAGE_WINDOW,
                               SdResponseTypeR3,
                               Response);
        if (!NT_SUCCESS(Status))
            return Status;

        Ocr = Response[0];
        if (Ocr & SD_OCR_BUSY)
        {
            DPRINT1("sdport: MMC OCR 0x%08lx after %lu polls\n", Ocr, Attempt + 1);
            SdPortWait(2000);

            if ((Ocr & MMC_OCR_VOLTAGE_WINDOW) != MMC_OCR_VOLTAGE_WINDOW)
                return STATUS_NOT_SUPPORTED;

            Slot->HighCapacity = ((Ocr & MMC_OCR_ACCESS_MODE_MASK) == MMC_OCR_ACCESS_SECTOR);
            return STATUS_SUCCESS;
        }

        SdPortWait(2000);
    }

    return STATUS_DEVICE_NOT_READY;
}

/* Poll CMD13 until the card is back in the transfer state and ready for data. */
static
NTSTATUS
SdPortWaitForTransferState(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    ULONG Response[4];
    ULONG Attempt;
    ULONG State;
    NTSTATUS Status;

    for (Attempt = 0; Attempt < 1000; Attempt++)
    {
        Status = SdPortCommand(Slot,
                               SD_CMD_SEND_STATUS,
                               (ULONG)Slot->Rca << 16,
                               SdResponseTypeR1,
                               Response);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Response[0] & SD_STATUS_SWITCH_ERROR)
            return STATUS_INVALID_DEVICE_REQUEST;

        State = (Response[0] >> SD_STATUS_STATE_SHIFT) & SD_STATUS_STATE_MASK;
        if ((Response[0] & SD_STATUS_READY_FOR_DATA) && State == SD_STATE_TRANSFER)
            return STATUS_SUCCESS;

        SdPortWait(1000);
    }

    return STATUS_IO_TIMEOUT;
}

/* CMD6 write of a single extended CSD byte, then wait out the busy phase. */
static
NTSTATUS
SdPortMmcSwitch(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ UCHAR Index,
    _In_ UCHAR Value)
{
    ULONG Argument;
    NTSTATUS Status;

    Argument = ((ULONG)MMC_SWITCH_WRITE_BYTE << 24) | ((ULONG)Index << 16) | ((ULONG)Value << 8);

    Status = SdPortCommand(Slot, MMC_CMD_SWITCH, Argument, SdResponseTypeR1B, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    return SdPortWaitForTransferState(Slot);
}

static
NTSTATUS
SdPortMmcReadExtCsd(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Out_writes_bytes_(SDPORT_EXT_CSD_LENGTH) PUCHAR Buffer)
{
    SDPORT_REQUEST Request;

    RtlZeroMemory(Buffer, SDPORT_EXT_CSD_LENGTH);
    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestCommandWithTransfer;
    Request.Data.Command.Index = MMC_CMD_SEND_EXT_CSD;
    Request.Data.Command.Argument = (ULONG)Slot->Rca << 16;
    Request.Data.Command.ResponseType = SdResponseTypeR1;
    Request.Data.Command.TransferType = SdTransferTypeSingleBlock;
    Request.Data.Command.TransferDirection = SdTransferDirectionRead;
    Request.Data.Command.TransferMethod = SdTransferMethodPio;
    Request.Data.Command.BlockSize = SDPORT_EXT_CSD_LENGTH;
    Request.Data.Command.BlockCount = 1;
    Request.Data.Command.Length = SDPORT_EXT_CSD_LENGTH;
    Request.DataBuffer = Buffer;
    Request.DataLength = SDPORT_EXT_CSD_LENGTH;

    return SdPortIssueSynchronousRequest(Slot, &Request);
}

/* Read the extended CSD back over the current bus setup and compare fields that never change. */
static
BOOLEAN
SdPortMmcVerifyBus(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Out_writes_bytes_(SDPORT_EXT_CSD_LENGTH) PUCHAR Scratch,
    _In_ UCHAR WidthCode)
{
    NTSTATUS Status;

    Status = SdPortMmcReadExtCsd(Slot, Scratch);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("sdport: MMC bus check read failed 0x%08lx\n", Status);
        return FALSE;
    }

    if (Scratch[EXT_CSD_REV] != Slot->ExtCsd[EXT_CSD_REV] ||
        Scratch[EXT_CSD_DEVICE_TYPE] != Slot->ExtCsd[EXT_CSD_DEVICE_TYPE] ||
        !RtlEqualMemory(&Scratch[EXT_CSD_SEC_COUNT], &Slot->ExtCsd[EXT_CSD_SEC_COUNT], sizeof(ULONG)))
    {
        DPRINT1("sdport: MMC bus check data mismatch\n");
        return FALSE;
    }

    if (WidthCode != EXT_CSD_BUS_WIDTH_1 && Scratch[EXT_CSD_BUS_WIDTH] != WidthCode)
    {
        DPRINT1("sdport: MMC bus width field %u, expected %u\n",
                Scratch[EXT_CSD_BUS_WIDTH], WidthCode);
        return FALSE;
    }

    return TRUE;
}

/* High speed timing first, then the widest data bus that passes a read back. */
static
NTSTATUS
SdPortMmcConfigureBus(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Out_writes_bytes_(SDPORT_EXT_CSD_LENGTH) PUCHAR Scratch)
{
    static const SDPORT_BUS_WIDTH Widths[] = { SdBusWidth8Bit, SdBusWidth4Bit };
    BOOLEAN HighSpeed = FALSE;
    SDPORT_BUS_WIDTH Width;
    UCHAR WidthCode;
    UCHAR PowerClass;
    ULONG Index;
    NTSTATUS Status;

    if ((Slot->ExtCsd[EXT_CSD_DEVICE_TYPE] & EXT_CSD_DEVICE_TYPE_HS_52) &&
        Slot->Capabilities.Flags.HighSpeed)
    {
        Status = SdPortMmcSwitch(Slot, EXT_CSD_HS_TIMING, 1);
        if (NT_SUCCESS(Status))
        {
            SdPortBusOperation(Slot, SdSetBusSpeed, SdBusSpeedHigh);
            HighSpeed = TRUE;
        }
        else
        {
            DPRINT1("sdport: MMC high speed switch failed 0x%08lx\n", Status);
        }
    }

    SdPortSetClock(Slot, HighSpeed ? MMC_CLOCK_HIGH_SPEED : MMC_CLOCK_DEFAULT);

    if (!SdPortMmcVerifyBus(Slot, Scratch, EXT_CSD_BUS_WIDTH_1))
    {
        if (!HighSpeed)
            return STATUS_DEVICE_DATA_ERROR;

        DPRINT1("sdport: MMC high speed unusable, falling back to default timing\n");
        SdPortSetClock(Slot, MMC_CLOCK_DEFAULT);
        SdPortMmcSwitch(Slot, EXT_CSD_HS_TIMING, 0);
        SdPortBusOperation(Slot, SdSetBusSpeed, SdBusSpeedNormal);
        HighSpeed = FALSE;

        if (!SdPortMmcVerifyBus(Slot, Scratch, EXT_CSD_BUS_WIDTH_1))
            return STATUS_DEVICE_DATA_ERROR;
    }

    Slot->BusSpeed = HighSpeed ? SdBusSpeedHigh : SdBusSpeedNormal;
    Slot->BusWidth = SdBusWidth1Bit;

    for (Index = 0; Index < RTL_NUMBER_OF(Widths); Index++)
    {
        Width = Widths[Index];
        if (Width == SdBusWidth8Bit && !Slot->Capabilities.Flags.BusWidth8Bit)
            continue;

        WidthCode = (Width == SdBusWidth8Bit) ? EXT_CSD_BUS_WIDTH_8 : EXT_CSD_BUS_WIDTH_4;

        /* The power class nibble for 8 bit is the high one, 4 bit the low one */
        PowerClass = Slot->ExtCsd[HighSpeed ? EXT_CSD_PWR_CL_52_360 : EXT_CSD_PWR_CL_26_360];
        PowerClass = (Width == SdBusWidth8Bit) ? (PowerClass >> 4) : (PowerClass & 0xF);
        if (PowerClass != 0)
            SdPortMmcSwitch(Slot, EXT_CSD_POWER_CLASS, PowerClass);

        Status = SdPortMmcSwitch(Slot, EXT_CSD_BUS_WIDTH, WidthCode);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("sdport: MMC %u bit switch failed 0x%08lx\n", Width, Status);
            continue;
        }

        SdPortBusOperation(Slot, SdSetBusWidth, Width);
        if (SdPortMmcVerifyBus(Slot, Scratch, WidthCode))
        {
            Slot->BusWidth = (UCHAR)Width;
            return STATUS_SUCCESS;
        }

        DPRINT1("sdport: MMC %u bit bus failed read back, reverting to 1 bit\n", Width);
        SdPortMmcSwitch(Slot, EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_1);
        SdPortBusOperation(Slot, SdSetBusWidth, SdBusWidth1Bit);
    }

    /* Stay on the 1 bit bus that was already proven */
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdPortInitializeSdMemory(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ BOOLEAN Version2)
{
    ULONG Response[4];
    NTSTATUS Status;

    Slot->CardType = SdCardTypeSdMemory;

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

    SdPortSetClock(Slot, SD_CLOCK_DEFAULT);
    Slot->BusWidth = SdBusWidth1Bit;
    Slot->BusSpeed = SdBusSpeedNormal;
    return STATUS_SUCCESS;
}

/* Runs after a successful CMD1, with the device in the ready state. */
static
NTSTATUS
SdPortInitializeMmc(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    ULONG Response[4];
    PUCHAR Scratch;
    UCHAR SpecVersion;
    ULONG SectorCount;
    NTSTATUS Status;

    Slot->CardType = SdCardTypeMmc;

    Status = SdPortCommand(Slot, SD_CMD_ALL_SEND_CID, 0, SdResponseTypeR2, Response);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Slot->Cid, Response, sizeof(Slot->Cid));

    Status = SdPortCommand(Slot,
                           SD_CMD_SEND_RELATIVE_ADDR,
                           (ULONG)MMC_HOST_RCA << 16,
                           SdResponseTypeR1,
                           NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Slot->Rca = MMC_HOST_RCA;

    Status = SdPortCommand(Slot, SD_CMD_SEND_CSD, (ULONG)Slot->Rca << 16, SdResponseTypeR2, Response);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Slot->Csd, Response, sizeof(Slot->Csd));

    /* SPEC_VERS is CSD bits 125:122 */
    SpecVersion = (Slot->Csd[0] >> 2) & 0xF;

    Status = SdPortCommand(Slot, SD_CMD_SELECT_CARD, (ULONG)Slot->Rca << 16, SdResponseTypeR1B, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    SdPortCommand(Slot, SD_CMD_SET_BLOCKLEN, SD_DEFAULT_BLOCK_LENGTH, SdResponseTypeR1, NULL);

    DPRINT1("sdport: MMC spec version %u, sector mode %u\n", SpecVersion, Slot->HighCapacity);

    if (SpecVersion < 4)
    {
        /* Sector addressing implies a capacity only the extended CSD can describe */
        if (Slot->HighCapacity)
            return STATUS_NOT_SUPPORTED;

        SdPortSetClock(Slot, MMC_CLOCK_LEGACY);
        Slot->BusWidth = SdBusWidth1Bit;
        Slot->BusSpeed = SdBusSpeedNormal;
        return STATUS_SUCCESS;
    }

    Status = SdPortMmcReadExtCsd(Slot, Slot->ExtCsd);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("sdport: EXT_CSD read failed 0x%08lx\n", Status);
        return Status;
    }
    Slot->ExtCsdValid = TRUE;

    RtlCopyMemory(&SectorCount, &Slot->ExtCsd[EXT_CSD_SEC_COUNT], sizeof(SectorCount));
    DPRINT1("sdport: EXT_CSD rev %u, device type 0x%02x, sectors %lu\n",
            Slot->ExtCsd[EXT_CSD_REV], Slot->ExtCsd[EXT_CSD_DEVICE_TYPE], SectorCount);

    if (Slot->ExtCsd[EXT_CSD_HC_ERASE_GRP_SIZE] != 0 && Slot->ExtCsd[EXT_CSD_HC_WP_GRP_SIZE] != 0)
    {
        Status = SdPortMmcSwitch(Slot, EXT_CSD_ERASE_GROUP_DEF, 1);
        if (!NT_SUCCESS(Status))
            DPRINT1("sdport: ERASE_GROUP_DEF switch failed 0x%08lx\n", Status);
    }

    Scratch = ExAllocatePoolWithTag(NonPagedPool, SDPORT_EXT_CSD_LENGTH, SDPORT_POOL_TAG);
    if (Scratch == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = SdPortMmcConfigureBus(Slot, Scratch);
    ExFreePoolWithTag(Scratch, SDPORT_POOL_TAG);
    return Status;
}

NTSTATUS
NTAPI
SdPortInitializeCard(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    ULONG Response[4];
    NTSTATUS Status;

    DPRINT1("sdport: initializing card in slot %u\n", Slot->SlotNumber);

    Slot->CardInitialized = FALSE;
    Slot->CardType = SdCardTypeUnknown;
    Slot->HighCapacity = FALSE;
    Slot->ExtCsdValid = FALSE;
    Slot->Rca = 0;
    RtlZeroMemory(Slot->Cid, sizeof(Slot->Cid));
    RtlZeroMemory(Slot->Csd, sizeof(Slot->Csd));

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
    SdPortSetClock(Slot, SD_CLOCK_IDENTIFY);
    Slot->BusWidth = SdBusWidth1Bit;
    Slot->BusSpeed = SdBusSpeedNormal;

    /* Give the supply and the identification clock time to settle */
    SdPortWait(10000);

    Status = SdPortGoIdle(Slot);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("sdport: GO_IDLE_STATE failed 0x%08lx\n", Status);
        return Status;
    }

    Status = SdPortCommand(Slot, SD_CMD_SEND_IF_COND, SD_IF_COND_ARGUMENT, SdResponseTypeR6, Response);
    if (NT_SUCCESS(Status) && (Response[0] & SD_IF_COND_ECHO_MASK) == SD_IF_COND_ARGUMENT)
    {
        DPRINT1("sdport: SD 2.0 interface condition accepted\n");
        Status = SdPortInitializeSdMemory(Slot, TRUE);
    }
    else
    {
        /* No SD 2.0 reply, so probe for MMC before treating it as an SD 1.x card */
        Status = SdPortGoIdle(Slot);
        if (NT_SUCCESS(Status))
            Status = SdPortMmcSendOpCond(Slot);

        if (NT_SUCCESS(Status))
        {
            Status = SdPortInitializeMmc(Slot);
        }
        else
        {
            DPRINT1("sdport: no MMC reply 0x%08lx, trying SD 1.x\n", Status);
            Slot->HighCapacity = FALSE;
            Status = SdPortGoIdle(Slot);
            if (NT_SUCCESS(Status))
                Status = SdPortInitializeSdMemory(Slot, FALSE);
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("sdport: card initialization failed 0x%08lx, type %u\n", Status, Slot->CardType);
        Slot->CardType = SdCardTypeUnknown;
        Slot->ExtCsdValid = FALSE;
        return Status;
    }

    Slot->Present = TRUE;
    Slot->CardInitialized = TRUE;
    DPRINT1("sdport: %s initialized, RCA 0x%04x, high capacity %u, %u bit, %lu kHz\n",
            (Slot->CardType == SdCardTypeMmc) ? "MMC" : "SD card",
            Slot->Rca,
            Slot->HighCapacity,
            Slot->BusWidth,
            Slot->ClockFrequencyKhz);
    return STATUS_SUCCESS;
}
