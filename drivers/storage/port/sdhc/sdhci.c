/*
 * PROJECT:     ReactOS Storage Stack / SD Standard Host Controller miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared SD Host Controller (SDHCI) register core
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "sdhci.h"

#define NDEBUG
#include <debug.h>

FORCEINLINE
UCHAR
SdhciReadUchar(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset)
{
    return SdPortReadRegisterUchar(Extension->BaseAddress, Offset);
}

FORCEINLINE
USHORT
SdhciReadUshort(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset)
{
    return SdPortReadRegisterUshort(Extension->BaseAddress, Offset);
}

FORCEINLINE
ULONG
SdhciReadUlong(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset)
{
    return SdPortReadRegisterUlong(Extension->BaseAddress, Offset);
}

FORCEINLINE
VOID
SdhciWriteUchar(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset, _In_ UCHAR Value)
{
    SdPortWriteRegisterUchar(Extension->BaseAddress, Offset, Value);
}

FORCEINLINE
VOID
SdhciWriteUshort(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset, _In_ USHORT Value)
{
    SdPortWriteRegisterUshort(Extension->BaseAddress, Offset, Value);
}

FORCEINLINE
VOID
SdhciWriteUlong(_In_ PSDHCI_EXTENSION Extension, _In_ ULONG Offset, _In_ ULONG Value)
{
    SdPortWriteRegisterUlong(Extension->BaseAddress, Offset, Value);
}

static
VOID
SdhciEnableInterrupts(
    _In_ PSDHCI_EXTENSION Extension)
{
    SdhciWriteUshort(Extension, SDHCI_NORMAL_INT_ENABLE, 0xFFFF);
    SdhciWriteUshort(Extension, SDHCI_ERROR_INT_ENABLE, 0xFFFF);
    SdhciWriteUshort(Extension, SDHCI_NORMAL_SIGNAL_ENABLE,
                     SDHCI_INT_CMD_COMPLETE | SDHCI_INT_XFER_COMPLETE |
                     SDHCI_INT_BUFFER_WRITE_RDY | SDHCI_INT_BUFFER_READ_RDY |
                     SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE | SDHCI_INT_ERROR);
    SdhciWriteUshort(Extension, SDHCI_ERROR_SIGNAL_ENABLE, 0xFFFF);
}

static
NTSTATUS
SdhciReset(
    _In_ PSDHCI_EXTENSION Extension,
    _In_ UCHAR Mask)
{
    ULONG Timeout;

    SdhciWriteUchar(Extension, SDHCI_SOFTWARE_RESET, Mask);

    for (Timeout = 0; Timeout < 100000; Timeout++)
    {
        if ((SdhciReadUchar(Extension, SDHCI_SOFTWARE_RESET) & Mask) == 0)
        {
            /* RESET_ALL clears the interrupt enable registers, so reinstate
               them or the status bits never latch. */
            if (Mask & SDHCI_RESET_ALL)
                SdhciEnableInterrupts(Extension);
            return STATUS_SUCCESS;
        }

        SdPortWait(10);
    }

    return STATUS_IO_TIMEOUT;
}

static
NTSTATUS
SdhciSetClock(
    _In_ PSDHCI_EXTENSION Extension,
    _In_ ULONG FrequencyKhz)
{
    ULONG BaseKhz = Extension->BaseClockKhz;
    ULONG Divisor;
    USHORT Select;
    USHORT ClockControl;
    ULONG Timeout;

    if (BaseKhz == 0 || FrequencyKhz == 0)
        return STATUS_INVALID_PARAMETER;

    SdhciWriteUshort(Extension, SDHCI_CLOCK_CONTROL, 0);

    if (FrequencyKhz >= BaseKhz)
    {
        Select = 0;
    }
    else
    {
        for (Divisor = 2; Divisor < 2048; Divisor <<= 1)
        {
            if ((BaseKhz / Divisor) <= FrequencyKhz)
                break;
        }
        Select = (USHORT)(Divisor >> 1);
    }

    ClockControl = (USHORT)(((Select & 0xFF) << 8) | ((Select & 0x300) >> 2));
    ClockControl |= SDHCI_CLOCK_INTERNAL_EN;
    SdhciWriteUshort(Extension, SDHCI_CLOCK_CONTROL, ClockControl);

    for (Timeout = 0; Timeout < 100000; Timeout++)
    {
        if (SdhciReadUshort(Extension, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INTERNAL_STABLE)
            break;

        SdPortWait(10);
    }

    ClockControl |= SDHCI_CLOCK_SD_EN;
    SdhciWriteUshort(Extension, SDHCI_CLOCK_CONTROL, ClockControl);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdhciGetSlotCount(
    _In_ PVOID Argument,
    _Out_ PUCHAR SlotCount)
{
    UNREFERENCED_PARAMETER(Argument);

    *SlotCount = 1;
    return STATUS_SUCCESS;
}

VOID
NTAPI
SdhciGetSlotCapabilities(
    _In_ PVOID Extension,
    _Out_ PSDPORT_CAPABILITIES Capabilities)
{
    PSDHCI_EXTENSION Ext = Extension;
    ULONG Caps;
    ULONG BaseClock;
    ULONG MaxBlock;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Caps = SdhciReadUlong(Ext, SDHCI_CAPABILITIES);

    Ext->SpecVersion = (UCHAR)(SdhciReadUshort(Ext, SDHCI_HOST_VERSION) & 0xFF);

    if (Ext->SpecVersion >= 2)
        BaseClock = (Caps >> 8) & 0xFF;
    else
        BaseClock = (Caps >> 8) & 0x3F;

    Ext->BaseClockKhz = BaseClock * 1000;
    Capabilities->BaseClockFrequencyKhz = Ext->BaseClockKhz;

    switch ((Caps >> 16) & 0x3)
    {
        case 0: MaxBlock = 512; break;
        case 1: MaxBlock = 1024; break;
        case 2: MaxBlock = 2048; break;
        default: MaxBlock = 512; break;
    }

    Capabilities->MaximumBlockSize = MaxBlock;
    Capabilities->MaximumBlockCount = 0xFFFF;
    Capabilities->MaximumOutstandingRequests = 1;

    Capabilities->Flags.BusWidth8Bit = (Caps & (1 << 18)) ? 1 : 0;
    Capabilities->Flags.HighSpeed = (Caps & (1 << 21)) ? 1 : 0;
    Capabilities->Flags.Voltage33V = (Caps & (1 << 24)) ? 1 : 0;
    Capabilities->Flags.Voltage30V = (Caps & (1 << 25)) ? 1 : 0;
    Capabilities->Flags.Voltage18V = (Caps & (1 << 26)) ? 1 : 0;
    Capabilities->PioTransferMaxThreshold = MaxBlock;
}

NTSTATUS
NTAPI
SdhciInitialize(
    _In_ PVOID Extension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode)
{
    PSDHCI_EXTENSION Ext = Extension;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PhysicalBase);
    UNREFERENCED_PARAMETER(Length);

    Ext->BaseAddress = VirtualBase;
    Ext->CrashdumpMode = CrashdumpMode;

    if (Ext->BaseClockKhz == 0)
    {
        SDPORT_CAPABILITIES Capabilities;
        SdhciGetSlotCapabilities(Ext, &Capabilities);
    }

    if (Ext->VendorInitialize != NULL)
    {
        Status = Ext->VendorInitialize(Ext);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = SdhciReset(Ext, SDHCI_RESET_ALL);
    if (!NT_SUCCESS(Status))
        return Status;

    SdhciWriteUchar(Ext, SDHCI_TIMEOUT_CONTROL, 0x0E);

    SdhciEnableInterrupts(Ext);

    DPRINT1("sdhci: initialized, base clock %lu kHz, spec version %u\n",
            Ext->BaseClockKhz, Ext->SpecVersion);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdhciIssueBusOperation(
    _In_ PVOID Extension,
    _In_ PSDPORT_BUS_OPERATION BusOperation)
{
    PSDHCI_EXTENSION Ext = Extension;
    UCHAR Control;
    UCHAR Power;

    switch (BusOperation->Type)
    {
        case SdResetHost:
        case SdResetHw:
            return SdhciReset(Ext, SDHCI_RESET_ALL);

        case SdSetClock:
            return SdhciSetClock(Ext, BusOperation->Parameters.FrequencyKhz);

        case SdSetVoltage:
            SdhciWriteUchar(Ext, SDHCI_POWER_CONTROL, 0);
            if (BusOperation->Parameters.Voltage >= 33)
                Power = SDHCI_POWER_33;
            else if (BusOperation->Parameters.Voltage >= 30)
                Power = SDHCI_POWER_30;
            else
                Power = SDHCI_POWER_18;
            SdhciWriteUchar(Ext, SDHCI_POWER_CONTROL, Power | SDHCI_POWER_ON);
            return STATUS_SUCCESS;

        case SdSetBusWidth:
            Control = SdhciReadUchar(Ext, SDHCI_HOST_CONTROL1);
            Control &= ~(SDHCI_CTRL1_DATA_4BIT | SDHCI_CTRL1_DATA_8BIT);
            if (BusOperation->Parameters.BusWidth == SdBusWidth8Bit)
                Control |= SDHCI_CTRL1_DATA_8BIT;
            else if (BusOperation->Parameters.BusWidth == SdBusWidth4Bit)
                Control |= SDHCI_CTRL1_DATA_4BIT;
            SdhciWriteUchar(Ext, SDHCI_HOST_CONTROL1, Control);
            return STATUS_SUCCESS;

        case SdSetBusSpeed:
            Control = SdhciReadUchar(Ext, SDHCI_HOST_CONTROL1);
            if (BusOperation->Parameters.BusSpeed >= SdBusSpeedHigh)
                Control |= SDHCI_CTRL1_HIGH_SPEED;
            else
                Control &= ~SDHCI_CTRL1_HIGH_SPEED;
            SdhciWriteUchar(Ext, SDHCI_HOST_CONTROL1, Control);
            return STATUS_SUCCESS;

        case SdSetSignalingVoltage:
        case SdExecuteTuning:
        default:
            return STATUS_SUCCESS;
    }
}

BOOLEAN
NTAPI
SdhciGetCardDetectState(
    _In_ PVOID Extension)
{
    PSDHCI_EXTENSION Ext = Extension;

    return (SdhciReadUlong(Ext, SDHCI_PRESENT_STATE) & SDHCI_STATE_CARD_INSERTED) ? TRUE : FALSE;
}

BOOLEAN
NTAPI
SdhciGetWriteProtectState(
    _In_ PVOID Extension)
{
    PSDHCI_EXTENSION Ext = Extension;

    return (SdhciReadUlong(Ext, SDHCI_PRESENT_STATE) & SDHCI_STATE_WRITE_ENABLED) ? FALSE : TRUE;
}

BOOLEAN
NTAPI
SdhciInterrupt(
    _In_ PVOID Extension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN CardChange,
    _Out_ PBOOLEAN SdioInterrupt,
    _Out_ PBOOLEAN Tuning)
{
    PSDHCI_EXTENSION Ext = Extension;
    USHORT Normal;
    USHORT Error;

    *Events = 0;
    *Errors = 0;
    *CardChange = FALSE;
    *SdioInterrupt = FALSE;
    *Tuning = FALSE;

    Normal = SdhciReadUshort(Ext, SDHCI_NORMAL_INT_STATUS);
    if (Normal == 0)
        return FALSE;

    Error = SdhciReadUshort(Ext, SDHCI_ERROR_INT_STATUS);

    *Events = Normal;
    *Errors = Error;
    *CardChange = (Normal & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE)) ? TRUE : FALSE;
    *SdioInterrupt = (Normal & SDHCI_INT_CARD) ? TRUE : FALSE;
    *Tuning = (Normal & SDHCI_INT_RETUNE) ? TRUE : FALSE;

    /* Write-1-to-clear the handled status bits. */
    if (Error != 0)
        SdhciWriteUshort(Ext, SDHCI_ERROR_INT_STATUS, Error);
    SdhciWriteUshort(Ext, SDHCI_NORMAL_INT_STATUS, Normal);

    return TRUE;
}

static
USHORT
SdhciCommandRegister(
    _In_ PSDPORT_COMMAND Command)
{
    USHORT Value = (USHORT)Command->Index << 8;

    switch (Command->ResponseType)
    {
        case SdResponseTypeR2:
            Value |= SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC_CHECK;
            break;

        case SdResponseTypeR3:
        case SdResponseTypeR4:
            Value |= SDHCI_CMD_RESP_48;
            break;

        case SdResponseTypeR1B:
        case SdResponseTypeR5B:
            Value |= SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
            break;

        case SdResponseTypeNone:
            Value |= SDHCI_CMD_RESP_NONE;
            break;

        default:
            Value |= SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
            break;
    }

    if (Command->TransferType != SdTransferTypeNone &&
        Command->TransferType != SdTransferTypeCommandOnly)
    {
        Value |= SDHCI_CMD_DATA_PRESENT;
    }

    return Value;
}

NTSTATUS
NTAPI
SdhciIssueRequest(
    _In_ PVOID Extension,
    _In_ PSDPORT_REQUEST Request)
{
    PSDHCI_EXTENSION Ext = Extension;
    PSDPORT_COMMAND Command = &Request->Data.Command;
    ULONG Timeout;
    USHORT TransferMode = 0;
    USHORT CommandValue;
    BOOLEAN HasData;

    HasData = (Command->TransferType != SdTransferTypeNone &&
               Command->TransferType != SdTransferTypeCommandOnly);

    for (Timeout = 0; Timeout < 100000; Timeout++)
    {
        ULONG State = SdhciReadUlong(Ext, SDHCI_PRESENT_STATE);
        if (!(State & SDHCI_STATE_CMD_INHIBIT) &&
            (!HasData || !(State & SDHCI_STATE_DAT_INHIBIT)))
        {
            break;
        }
        SdPortWait(10);
    }

    Ext->CurrentRequest = Request;
    Ext->DataPointer = NULL;
    Ext->DataRemaining = 0;

    if (HasData)
    {
        Ext->DataPointer = Request->DataBuffer;
        Ext->DataRemaining = Command->BlockCount * Command->BlockSize;
        Ext->DataRead = (Command->TransferDirection == SdTransferDirectionRead);

        SdhciWriteUshort(Ext, SDHCI_BLOCK_SIZE, (USHORT)(Command->BlockSize & 0xFFF));
        SdhciWriteUshort(Ext, SDHCI_BLOCK_COUNT, (USHORT)Command->BlockCount);

        if (Ext->DataRead)
            TransferMode |= SDHCI_TRANSFER_READ;
        if (Command->BlockCount > 1)
            TransferMode |= SDHCI_TRANSFER_MULTI_BLOCK | SDHCI_TRANSFER_BLKCNT_EN;

        SdhciWriteUshort(Ext, SDHCI_TRANSFER_MODE, TransferMode);
    }
    else
    {
        SdhciWriteUshort(Ext, SDHCI_TRANSFER_MODE, 0);
    }

    CommandValue = SdhciCommandRegister(Command);

    SdhciWriteUlong(Ext, SDHCI_ARGUMENT, Command->Argument);
    SdhciWriteUshort(Ext, SDHCI_COMMAND, CommandValue);

    return STATUS_PENDING;
}

VOID
NTAPI
SdhciGetResponse(
    _In_ PVOID Extension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer)
{
    PSDHCI_EXTENSION Ext = Extension;
    PUCHAR Output = ResponseBuffer;
    ULONG Resp[4];

    if (Command->ResponseType == SdResponseTypeR2)
    {
        Resp[0] = SdhciReadUlong(Ext, SDHCI_RESPONSE + 0);
        Resp[1] = SdhciReadUlong(Ext, SDHCI_RESPONSE + 4);
        Resp[2] = SdhciReadUlong(Ext, SDHCI_RESPONSE + 8);
        Resp[3] = SdhciReadUlong(Ext, SDHCI_RESPONSE + 12);

        /* The 120-bit CID/CSD lives in RESP[119:0]; shift left 8 to place it at
           bit positions [127:8] and emit most significant byte first. */
        Output[0] = (UCHAR)(Resp[3] >> 16);
        Output[1] = (UCHAR)(Resp[3] >> 8);
        Output[2] = (UCHAR)(Resp[3]);
        Output[3] = (UCHAR)(Resp[2] >> 24);
        Output[4] = (UCHAR)(Resp[2] >> 16);
        Output[5] = (UCHAR)(Resp[2] >> 8);
        Output[6] = (UCHAR)(Resp[2]);
        Output[7] = (UCHAR)(Resp[1] >> 24);
        Output[8] = (UCHAR)(Resp[1] >> 16);
        Output[9] = (UCHAR)(Resp[1] >> 8);
        Output[10] = (UCHAR)(Resp[1]);
        Output[11] = (UCHAR)(Resp[0] >> 24);
        Output[12] = (UCHAR)(Resp[0] >> 16);
        Output[13] = (UCHAR)(Resp[0] >> 8);
        Output[14] = (UCHAR)(Resp[0]);
        Output[15] = 0;
    }
    else
    {
        *(PULONG)ResponseBuffer = SdhciReadUlong(Ext, SDHCI_RESPONSE + 0);
    }
}

VOID
NTAPI
SdhciToggleEvents(
    _In_ PVOID Extension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable)
{
    PSDHCI_EXTENSION Ext = Extension;
    USHORT Signal = SdhciReadUshort(Ext, SDHCI_NORMAL_SIGNAL_ENABLE);

    if (Enable)
        Signal |= (USHORT)EventMask;
    else
        Signal &= ~(USHORT)EventMask;

    SdhciWriteUshort(Ext, SDHCI_NORMAL_SIGNAL_ENABLE, Signal);
}

VOID
NTAPI
SdhciClearEvents(
    _In_ PVOID Extension,
    _In_ ULONG EventMask)
{
    PSDHCI_EXTENSION Ext = Extension;

    SdhciWriteUshort(Ext, SDHCI_NORMAL_INT_STATUS, (USHORT)EventMask);
}

static
VOID
SdhciDrainPio(
    _In_ PSDHCI_EXTENSION Ext)
{
    while (Ext->DataRemaining >= sizeof(ULONG))
    {
        ULONG State = SdhciReadUlong(Ext, SDHCI_PRESENT_STATE);

        if (Ext->DataRead)
        {
            if (!(State & SDHCI_STATE_BUFFER_READ))
                break;
            *(PULONG)Ext->DataPointer = SdhciReadUlong(Ext, SDHCI_BUFFER_DATA_PORT);
        }
        else
        {
            if (!(State & SDHCI_STATE_BUFFER_WRITE))
                break;
            SdhciWriteUlong(Ext, SDHCI_BUFFER_DATA_PORT, *(PULONG)Ext->DataPointer);
        }

        Ext->DataPointer += sizeof(ULONG);
        Ext->DataRemaining -= sizeof(ULONG);
    }
}

VOID
NTAPI
SdhciRequestDpc(
    _In_ PVOID Extension,
    _In_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors)
{
    PSDHCI_EXTENSION Ext = Extension;
    BOOLEAN HasData;

    if (Ext->CurrentRequest != Request)
        return;

    HasData = (Request->Data.Command.TransferType != SdTransferTypeNone &&
               Request->Data.Command.TransferType != SdTransferTypeCommandOnly);

    if (Errors != 0)
    {
        SdhciReset(Ext, SDHCI_RESET_CMD);
        SdhciReset(Ext, SDHCI_RESET_DAT);
        Ext->CurrentRequest = NULL;
        SdPortCompleteRequest(Request, STATUS_DEVICE_DATA_ERROR);
        return;
    }

    if (HasData &&
        (Events & (SDHCI_INT_BUFFER_READ_RDY | SDHCI_INT_BUFFER_WRITE_RDY | SDHCI_INT_CMD_COMPLETE)))
    {
        SdhciDrainPio(Ext);
    }

    if (!HasData && (Events & SDHCI_INT_CMD_COMPLETE))
    {
        Ext->CurrentRequest = NULL;
        Request->BytesTransferred = 0;
        SdPortCompleteRequest(Request, STATUS_SUCCESS);
        return;
    }

    if (HasData && (Events & SDHCI_INT_XFER_COMPLETE))
    {
        SdhciDrainPio(Ext);
        Ext->CurrentRequest = NULL;
        Request->BytesTransferred = Request->Data.Command.BlockCount * Request->Data.Command.BlockSize;
        SdPortCompleteRequest(Request, STATUS_SUCCESS);
        return;
    }
}

VOID
NTAPI
SdhciFillInitializationData(
    _Out_ PSDPORT_INITIALIZATION_DATA InitData)
{
    RtlZeroMemory(InitData, sizeof(*InitData));
    InitData->StructureSize = sizeof(SDPORT_INITIALIZATION_DATA);
    InitData->GetSlotCount = SdhciGetSlotCount;
    InitData->GetSlotCapabilities = SdhciGetSlotCapabilities;
    InitData->Initialize = SdhciInitialize;
    InitData->IssueBusOperation = SdhciIssueBusOperation;
    InitData->GetCardDetectState = SdhciGetCardDetectState;
    InitData->GetWriteProtectState = SdhciGetWriteProtectState;
    InitData->Interrupt = SdhciInterrupt;
    InitData->IssueRequest = SdhciIssueRequest;
    InitData->GetResponse = SdhciGetResponse;
    InitData->ToggleEvents = SdhciToggleEvents;
    InitData->ClearEvents = SdhciClearEvents;
    InitData->RequestDpc = SdhciRequestDpc;
    InitData->PrivateExtensionSize = sizeof(SDHCI_EXTENSION);
}
