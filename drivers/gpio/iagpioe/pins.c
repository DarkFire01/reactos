/*
 * PROJECT:     ReactOS Intel Cherryview GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IO pins and pad interrupts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "iagpioe.h"

#define NDEBUG
#include <debug.h>

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvConnectIoPins(
    PVOID Context,
    PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParameters)
{
    ULONG Index;

    if (!Context || !ConnectParameters)
        return STATUS_INVALID_PARAMETER;

    /* Pull, drive strength and debounce are left as firmware set them */
    if ((ConnectParameters->ConnectMode != ConnectModeInput) &&
        (ConnectParameters->ConnectMode != ConnectModeOutput))
    {
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < ConnectParameters->PinCount; Index++)
    {
        ChvSetPadDirection(Context,
                           ConnectParameters->BankId,
                           ConnectParameters->PinNumberTable[Index],
                           ConnectParameters->ConnectMode == ConnectModeOutput);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvDisconnectIoPins(
    PVOID Context,
    PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParameters)
{
    ULONG Index;

    if (!Context || !DisconnectParameters)
        return STATUS_INVALID_PARAMETER;

    /* A released pin goes back to a safe input */
    if (DisconnectParameters->DisconnectFlags.PreserveConfiguration)
        return STATUS_SUCCESS;

    for (Index = 0; Index < DisconnectParameters->PinCount; Index++)
    {
        ChvSetPadDirection(Context,
                           DisconnectParameters->BankId,
                           DisconnectParameters->PinNumberTable[Index],
                           FALSE);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvReadGpioPins(
    PVOID Context,
    PGPIO_READ_PINS_PARAMETERS ReadParameters)
{
    PCHV_CONTROLLER Controller = Context;
    PUCHAR Buffer;
    PIN_NUMBER Pin;
    ULONG Index;

    if (!Controller || !ReadParameters)
        return STATUS_INVALID_PARAMETER;

    /* Values are packed one bit per table entry */
    Buffer = ReadParameters->Buffer;
    RtlZeroMemory(Buffer, (ReadParameters->PinCount + 7) / 8);

    for (Index = 0; Index < ReadParameters->PinCount; Index++)
    {
        Pin = ReadParameters->PinNumberTable[Index];
        if (!ChvIsPinValid(Controller, ReadParameters->BankId, Pin))
            continue;

        if (ChvReadRegister(Controller, ChvPadOffset(Controller, ReadParameters->BankId, Pin)) &
            CHV_CTRL0_RX_STATE)
        {
            Buffer[Index / 8] |= (UCHAR)(1 << (Index % 8));
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvWriteGpioPins(
    PVOID Context,
    PGPIO_WRITE_PINS_PARAMETERS WriteParameters)
{
    PUCHAR Buffer;
    ULONG Index;

    if (!Context || !WriteParameters)
        return STATUS_INVALID_PARAMETER;

    Buffer = WriteParameters->Buffer;
    for (Index = 0; Index < WriteParameters->PinCount; Index++)
    {
        ChvWritePadOutput(Context,
                          WriteParameters->BankId,
                          WriteParameters->PinNumberTable[Index],
                          (Buffer[Index / 8] >> (Index % 8)) & 1);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes a pad an input that raises its line with the requested trigger.
 */
static
VOID
ChvArmPad(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity)
{
    ChvSetPadDirection(Controller, BankId, PinNumber, FALSE);
    ChvAckLine(Controller, BankId, PinNumber);
    ChvSetPadTrigger(Controller, BankId, PinNumber, Mode, Polarity);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvEnableInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParameters)
{
    PCHV_CONTROLLER Controller = Context;

    if (!Controller || !EnableParameters || (EnableParameters->Polarity == InterruptPolarityUnknown))
        return STATUS_INVALID_PARAMETER;

    if (!ChvIsPinValid(Controller, EnableParameters->BankId, EnableParameters->PinNumber))
        return STATUS_SUCCESS;

    GPIO_CLX_AcquireInterruptLock(Context, EnableParameters->BankId);
    ChvArmPad(Controller,
              EnableParameters->BankId,
              EnableParameters->PinNumber,
              EnableParameters->InterruptMode,
              EnableParameters->Polarity);
    GPIO_CLX_ReleaseInterruptLock(Context, EnableParameters->BankId);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvDisableInterrupt(
    PVOID Context,
    PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParameters)
{
    PCHV_CONTROLLER Controller = Context;
    BANK_ID Bank;
    PIN_NUMBER Pin;

    if (!Controller || !DisableParameters)
        return STATUS_INVALID_PARAMETER;

    Bank = DisableParameters->BankId;
    Pin = DisableParameters->PinNumber;
    if (!ChvIsPinValid(Controller, Bank, Pin))
        return STATUS_SUCCESS;

    /* The line stays unmasked, other pads may still share it */
    GPIO_CLX_AcquireInterruptLock(Context, Bank);
    ChvSetPadDirection(Controller, Bank, Pin, FALSE);
    ChvClearPadTrigger(Controller, Bank, Pin);
    ChvAckLine(Controller, Bank, Pin);
    GPIO_CLX_ReleaseInterruptLock(Context, Bank);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvReconfigureInterrupt(
    PVOID Context,
    PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParameters)
{
    PCHV_CONTROLLER Controller = Context;

    if (!Controller || !ReconfigureParameters || (ReconfigureParameters->Polarity == InterruptPolarityUnknown))
        return STATUS_INVALID_PARAMETER;

    if (ChvIsPinValid(Controller, ReconfigureParameters->BankId, ReconfigureParameters->PinNumber))
    {
        ChvArmPad(Controller,
                  ReconfigureParameters->BankId,
                  ReconfigureParameters->PinNumber,
                  ReconfigureParameters->InterruptMode,
                  ReconfigureParameters->Polarity);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvUnmaskInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS InterruptParameters)
{
    PCHV_CONTROLLER Controller = Context;

    if (!Controller || !InterruptParameters)
        return STATUS_INVALID_PARAMETER;

    if ((InterruptParameters->InterruptMode == LevelSensitive) ||
        (InterruptParameters->InterruptMode == Latched))
    {
        ChvSetLineMask(Controller, InterruptParameters->BankId, InterruptParameters->PinNumber, TRUE);
    }

    ChvAckLine(Controller, InterruptParameters->BankId, InterruptParameters->PinNumber);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvMaskInterrupts(
    PVOID Context,
    PGPIO_MASK_INTERRUPT_PARAMETERS MaskParameters)
{
    PCHV_CONTROLLER Controller = Context;
    PIN_NUMBER Pin;

    if (!Controller || !MaskParameters)
        return STATUS_INVALID_PARAMETER;

    /* Mask every line first so nothing refires while the rest are acknowledged */
    for (Pin = 0; Controller->Community && Pin < Controller->Community->PinsPerBank; Pin++)
    {
        if (MaskParameters->PinMask & (1ULL << Pin))
            ChvSetLineMask(Controller, MaskParameters->BankId, Pin, FALSE);
    }

    for (Pin = 0; Controller->Community && Pin < Controller->Community->PinsPerBank; Pin++)
    {
        if (MaskParameters->PinMask & (1ULL << Pin))
            ChvAckLine(Controller, MaskParameters->BankId, Pin);
    }

    MaskParameters->FailedMask = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvQueryActiveInterrupts(
    PVOID Context,
    PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParameters)
{
    PCHV_CONTROLLER Controller = Context;
    BANK_ID Bank;
    PIN_NUMBER Pin;
    ULONG Trigger;
    ULONG64 Active = 0;

    if (!Controller || !QueryActiveParameters)
        return STATUS_INVALID_PARAMETER;

    Bank = QueryActiveParameters->BankId;
    for (Pin = 0; Controller->Community && Pin < Controller->Community->PinsPerBank; Pin++)
    {
        if (!(QueryActiveParameters->EnabledMask & (1ULL << Pin)) ||
            !ChvIsPinValid(Controller, Bank, Pin))
        {
            continue;
        }

        /* A pad with no trigger cannot be the source, even on a shared line */
        Trigger = ChvReadRegister(Controller, ChvPadOffset(Controller, Bank, Pin) + sizeof(ULONG)) &
                  CHV_CTRL1_INT_MASK;
        if ((Trigger < CHV_CTRL1_INT_FALLING) || (Trigger > CHV_CTRL1_INT_LEVEL))
            continue;

        if (ChvIsLinePending(Controller, Bank, Pin))
            Active |= 1ULL << Pin;
    }

    QueryActiveParameters->ActiveMask = Active;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvClearActiveInterrupts(
    PVOID Context,
    PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParameters)
{
    PCHV_CONTROLLER Controller = Context;
    PIN_NUMBER Pin;

    if (!Controller || !ClearParameters)
        return STATUS_INVALID_PARAMETER;

    for (Pin = 0; Controller->Community && Pin < Controller->Community->PinsPerBank; Pin++)
    {
        if (ClearParameters->ClearActiveMask & (1ULL << Pin))
            ChvAckLine(Controller, ClearParameters->BankId, Pin);
    }

    ClearParameters->FailedClearMask = 0;
    return STATUS_SUCCESS;
}
