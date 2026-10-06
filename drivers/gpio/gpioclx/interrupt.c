/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Interrupt enable, mask and acknowledge
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GPIO interrupt is not a line: the controller raises one interrupt for a
 * whole bank and the pin that caused it is read back out of the bank's active
 * register. Turning that into something the kernel can connect is the work
 * this file does the client-facing half of.
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Arms one pin as an interrupt source.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pin is in.
 *
 * @param[in] PinNumber
 * The pin, as an offset within that bank.
 *
 * @param[in] InterruptMode
 * Edge or level.
 *
 * @param[in] Polarity
 * Which edge, or which level.
 *
 * @param[in] PullConfiguration
 * What the pad should pull to while nothing drives it.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxEnableInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE InterruptMode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ UCHAR PullConfiguration)
{
    GPIO_ENABLE_INTERRUPT_PARAMETERS Parameters;
    PGCX_PIN Pin;
    NTSTATUS Status;

    if (Controller->Registration.CLIENT_EnableInterrupt == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;
    Parameters.InterruptMode = InterruptMode;
    Parameters.Polarity = Polarity;
    Parameters.PullConfiguration = PullConfiguration;

    Status = Controller->Registration.CLIENT_EnableInterrupt(
                 GcxClientContext(Controller), &Parameters);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * The armed mask is what the demultiplexer narrows a bank's active
     * register against, so a pin only becomes visible to it once the client
     * has actually armed the pad.
     */
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
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pin is in.
 *
 * @param[in] PinNumber
 * The pin, as an offset within that bank.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxDisableInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    GPIO_DISABLE_INTERRUPT_PARAMETERS Parameters;
    PGCX_PIN Pin;
    NTSTATUS Status;

    if (Controller->Registration.CLIENT_DisableInterrupt == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;

    Status = Controller->Registration.CLIENT_DisableInterrupt(
                 GcxClientContext(Controller), &Parameters);

    /*
     * The registration goes whether or not the client managed to quieten the
     * pad: a pin that keeps asserting after this must not still reach a target
     * that has stopped expecting it.
     */
    Controller->Banks[BankId].EnabledMask &= ~(1ULL << PinNumber);

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin != NULL)
    {
        RtlZeroMemory(Pin, sizeof(*Pin));
    }

    return Status;
}

/**
 * @brief
 * Masks a set of a bank's interrupts.
 *
 * The client reports the ones it could not mask rather than failing the lot,
 * so a caller can tell which pins are still live.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank.
 *
 * @param[in] PinMask
 * One bit per pin in that bank.
 *
 * @param[out] FailedMask
 * Receives the pins that stayed unmasked.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxMaskInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 PinMask,
    _Out_ PULONG64 FailedMask)
{
    GPIO_MASK_INTERRUPT_PARAMETERS Parameters;
    NTSTATUS Status;

    *FailedMask = 0;

    if (Controller->Registration.CLIENT_MaskInterrupts == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinMask = PinMask;

    Status = Controller->Registration.CLIENT_MaskInterrupts(
                 GcxClientContext(Controller), &Parameters);

    *FailedMask = Parameters.FailedMask;

    return Status;
}

/**
 * @brief
 * Unmasks one pin's interrupt.
 *
 * Unmask takes the enable block rather than the mask one: the reference passes
 * a single pin here, not a mask, which is why the two are not symmetric.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pin is in.
 *
 * @param[in] PinNumber
 * The pin, as an offset within that bank.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxUnmaskInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    GPIO_ENABLE_INTERRUPT_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_UnmaskInterrupt == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumber = PinNumber;

    return Controller->Registration.CLIENT_UnmaskInterrupt(
               GcxClientContext(Controller), &Parameters);
}

/**
 * @brief
 * Asks which of a bank's armed interrupts are asserted.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank.
 *
 * @param[in] EnabledMask
 * The pins that are armed, so the client need not look at the rest.
 *
 * @param[out] ActiveMask
 * Receives the ones that are asserted.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxQueryActiveInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 EnabledMask,
    _Out_ PULONG64 ActiveMask)
{
    GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Parameters;
    NTSTATUS Status;

    *ActiveMask = 0;

    if (Controller->Registration.CLIENT_QueryActiveInterrupts == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.EnabledMask = EnabledMask;

    Status = Controller->Registration.CLIENT_QueryActiveInterrupts(
                 GcxClientContext(Controller), &Parameters);

    *ActiveMask = Parameters.ActiveMask;

    return Status;
}

/**
 * @brief
 * Acknowledges a set of a bank's asserted interrupts.
 *
 * Skipped entirely on a controller whose active register clears itself when it
 * is read, because the query above already acknowledged them.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank.
 *
 * @param[in] ClearMask
 * One bit per pin to acknowledge.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxClearActiveInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 ClearMask)
{
    GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Parameters;

    if (Controller->Information.Flags.ActiveInterruptsAutoClearOnRead)
    {
        return STATUS_SUCCESS;
    }

    if (Controller->Registration.CLIENT_ClearActiveInterrupts == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.ClearActiveMask = ClearMask;

    return Controller->Registration.CLIENT_ClearActiveInterrupts(
               GcxClientContext(Controller), &Parameters);
}
