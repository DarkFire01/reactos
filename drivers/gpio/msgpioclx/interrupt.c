/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Interrupt enable, mask and acknowledge
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Arms one pin as an interrupt source.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_SUPPORTED, STATUS_INVALID_PARAMETER or the client's failure.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxEnableInterrupt(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PIN_NUMBER PinNumber,
    KINTERRUPT_MODE InterruptMode,
    KINTERRUPT_POLARITY Polarity,
    UCHAR PullConfiguration)
{
    GPIO_ENABLE_INTERRUPT_PARAMETERS Parameters;
    PGCX_PIN Pin;
    NTSTATUS Status;

    if (Controller->Registration.CLIENT_EnableInterrupt == NULL)
        return STATUS_NOT_SUPPORTED;

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;
    Parameters.InterruptMode = InterruptMode;
    Parameters.Polarity = Polarity;
    Parameters.PullConfiguration = PullConfiguration;

    Status = Controller->Registration.CLIENT_EnableInterrupt(GcxClientContext(Controller),
                                                             &Parameters);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Only once the pad is armed may the demultiplexer see it */
    Pin->InterruptMode = InterruptMode;
    Pin->Polarity = Polarity;
    Pin->Flags |= GCX_PIN_INTERRUPT_ENABLED;
    Controller->Banks[BankId].EnabledMask |= 1ULL << PinNumber;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops one pin being an interrupt source.
 *
 * The pin is dropped from the armed set even if the client fails, so a pad that
 * keeps asserting can never reach a target that disconnected.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxDisableInterrupt(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PIN_NUMBER PinNumber)
{
    GPIO_DISABLE_INTERRUPT_PARAMETERS Parameters;
    PGCX_PIN Pin;

    if (Controller->Registration.CLIENT_DisableInterrupt == NULL)
        return STATUS_NOT_SUPPORTED;

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL)
        return STATUS_INVALID_PARAMETER;

    Controller->Banks[BankId].EnabledMask &= ~(1ULL << PinNumber);
    RtlZeroMemory(Pin, sizeof(*Pin));

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;
    return Controller->Registration.CLIENT_DisableInterrupt(GcxClientContext(Controller),
                                                            &Parameters);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxMaskInterrupts(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    ULONG64 PinMask,
    PULONG64 FailedMask)
{
    GPIO_MASK_INTERRUPT_PARAMETERS Parameters;
    NTSTATUS Status;

    *FailedMask = PinMask;

    if (Controller->Registration.CLIENT_MaskInterrupts == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinMask = PinMask;

    Status = Controller->Registration.CLIENT_MaskInterrupts(GcxClientContext(Controller),
                                                            &Parameters);
    *FailedMask = Parameters.FailedMask;
    return Status;
}

/**
 * @brief
 * Unmasks one pin. The client is handed the enable block for this, not the mask one.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxUnmaskInterrupt(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PIN_NUMBER PinNumber)
{
    GPIO_ENABLE_INTERRUPT_PARAMETERS Parameters;
    PGCX_PIN Pin;

    if (Controller->Registration.CLIENT_UnmaskInterrupt == NULL)
        return STATUS_NOT_SUPPORTED;

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;
    Parameters.InterruptMode = Pin->InterruptMode;
    Parameters.Polarity = Pin->Polarity;
    return Controller->Registration.CLIENT_UnmaskInterrupt(GcxClientContext(Controller),
                                                           &Parameters);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxQueryActiveInterrupts(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    ULONG64 EnabledMask,
    PULONG64 ActiveMask)
{
    GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Parameters;
    NTSTATUS Status;

    *ActiveMask = 0;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.EnabledMask = EnabledMask;

    Status = Controller->Registration.CLIENT_QueryActiveInterrupts(GcxClientContext(Controller),
                                                                   &Parameters);
    if (NT_SUCCESS(Status))
        *ActiveMask = Parameters.ActiveMask;

    return Status;
}

/**
 * @brief
 * Acknowledges asserted pins, unless reading the status already did.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxClearActiveInterrupts(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    ULONG64 ClearMask)
{
    GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Parameters;

    if (Controller->Information.Flags.ActiveInterruptsAutoClearOnRead)
        return STATUS_SUCCESS;

    if (Controller->Registration.CLIENT_ClearActiveInterrupts == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.ClearActiveMask = ClearMask;
    return Controller->Registration.CLIENT_ClearActiveInterrupts(GcxClientContext(Controller),
                                                                 &Parameters);
}
