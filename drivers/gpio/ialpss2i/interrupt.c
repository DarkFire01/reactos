/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pad interrupt configuration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Two registers do the work. The pad's own PADCFG0 says what counts as an
 * event - level or edge, and which way round - and the community's GPI_IE and
 * GPI_IS arrays carry one bit per pad for whether it is armed and whether it
 * has fired.
 */

#include "gpiopriv.h"

/**
 * @brief
 * Sets or clears one bit in a community's per-group dword array.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group whose dword to touch.
 *
 * @param[in] BlockOffset
 * Which array, as a community-relative offset.
 *
 * @param[in] PinMask
 * The pads to change.
 *
 * @param[in] Set
 * TRUE to set those bits, FALSE to clear them.
 */
static
VOID
GpioUpdateGroupMask(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG BlockOffset,
    _In_ ULONG PinMask,
    _In_ BOOLEAN Set)
{
    PULONG Register;
    ULONG Value;

    Register = GpioGroupRegister(Controller, BankId, BlockOffset);
    if (Register == NULL)
    {
        return;
    }

    Value = READ_REGISTER_ULONG(Register);
    if (Set)
    {
        Value |= PinMask;
    }
    else
    {
        Value &= ~PinMask;
    }

    WRITE_REGISTER_ULONG(Register, Value);
}

/**
 * @brief
 * Tells a pad what counts as an interrupt.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group the pad is in.
 *
 * @param[in] PinNumber
 * The pad's index within it.
 *
 * @param[in] InterruptMode
 * Edge or level.
 *
 * @param[in] Polarity
 * Which edge, or which level.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a pad that does not exist or
 * a polarity the pad cannot express.
 */
static
NTSTATUS
GpioConfigurePadForInterrupt(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE InterruptMode,
    _In_ KINTERRUPT_POLARITY Polarity)
{
    PULONG Pad;
    ULONG Value;

    Pad = GpioPadAddress(Controller, BankId, PinNumber);
    if (Pad == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Value = READ_REGISTER_ULONG(Pad);

    /* An interrupt source is an input in GPIO mode, and must not drive */
    Value &= ~PADCFG0_PMODE_MASK;
    Value &= ~PADCFG0_GPIORXDIS;
    Value |= PADCFG0_GPIOTXDIS;

    Value &= ~(PADCFG0_RXEVCFG_MASK | PADCFG0_RXINV);

    if (InterruptMode == LevelSensitive)
    {
        Value |= PADCFG0_RXEVCFG_LEVEL;

        /*
         * The pad only detects a high level, so an active-low source is
         * handled by inverting what it sees rather than by a second mode.
         */
        if (Polarity == InterruptActiveLow)
        {
            Value |= PADCFG0_RXINV;
        }
    }
    else
    {
        switch (Polarity)
        {
            case InterruptActiveHigh:
                /* A rising edge */
                Value |= PADCFG0_RXEVCFG_EDGE;
                break;

            case InterruptActiveLow:
                /* A falling edge, which the pad reaches by inverting */
                Value |= PADCFG0_RXEVCFG_EDGE | PADCFG0_RXINV;
                break;

            case InterruptActiveBoth:
                Value |= PADCFG0_RXEVCFG_EDGE_BOTH;
                break;

            default:
                return STATUS_INVALID_PARAMETER;
        }
    }

    WRITE_REGISTER_ULONG(Pad, Value);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Arms one pad as an interrupt source.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] EnableParameters
 * The pad, and what should trigger it.
 *
 * @return
 * STATUS_SUCCESS, or the failure from configuring the pad.
 */
NTSTATUS
NTAPI
GpioEnableInterrupt(
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PGPIO_PIN_STATE PinState;
    NTSTATUS Status;

    Status = GpioConfigurePadForInterrupt(Controller,
                                          EnableParameters->BankId,
                                          EnableParameters->PinNumber,
                                          EnableParameters->InterruptMode,
                                          EnableParameters->Polarity);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * Acknowledge anything the pad latched while it was being configured, so
     * arming it does not immediately deliver a stale event.
     */
    GpioUpdateGroupMask(Controller, EnableParameters->BankId,
                        GPIO_COMMUNITY_GPI_STATUS,
                        1UL << EnableParameters->PinNumber, TRUE);

    GpioUpdateGroupMask(Controller, EnableParameters->BankId,
                        GPIO_COMMUNITY_GPI_ENABLE,
                        1UL << EnableParameters->PinNumber, TRUE);

    /* An armed pad has to be restored across a power transition as much as a
     * connected one, so it counts as claimed too. */
    PinState = GpioPinState(Controller, EnableParameters->BankId,
                            EnableParameters->PinNumber);
    if (PinState != NULL)
    {
        PinState->InUse = TRUE;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Changes what an already armed pad triggers on.
 *
 * The reference reaches the same helper the enable path uses, differing only
 * in that it leaves the armed mask alone (:5423): the pad stays armed
 * throughout, so the caller does not have to disable and re-enable it just to
 * swap an edge for a level.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] ReconfigureParameters
 * The pad, and what should trigger it from now on.
 *
 * @return
 * STATUS_SUCCESS, or the failure from configuring the pad.
 */
NTSTATUS
NTAPI
GpioReconfigureInterrupt(
    _In_ PVOID Context,
    _In_ PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParameters)
{
    return GpioConfigurePadForInterrupt((PGPIO_CONTROLLER)Context,
                                        ReconfigureParameters->BankId,
                                        ReconfigureParameters->PinNumber,
                                        ReconfigureParameters->InterruptMode,
                                        ReconfigureParameters->Polarity);
}

/**
 * @brief
 * Stops one pad being an interrupt source.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] DisableParameters
 * The pad.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioDisableInterrupt(
    _In_ PVOID Context,
    _In_ PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Pad;
    ULONG Value;

    GpioUpdateGroupMask(Controller, DisableParameters->BankId,
                        GPIO_COMMUNITY_GPI_ENABLE,
                        1UL << DisableParameters->PinNumber, FALSE);

    /* Stop the pad detecting anything, so it cannot latch while disarmed */
    Pad = GpioPadAddress(Controller, DisableParameters->BankId,
                         DisableParameters->PinNumber);
    if (Pad != NULL)
    {
        Value = READ_REGISTER_ULONG(Pad);
        Value &= ~PADCFG0_RXEVCFG_MASK;
        Value |= PADCFG0_RXEVCFG_DISABLED;
        WRITE_REGISTER_ULONG(Pad, Value);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Masks a set of a group's interrupts.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] MaskParameters
 * The pads to mask, and where to report the ones that would not.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a group that does not exist.
 */
NTSTATUS
NTAPI
GpioMaskInterrupts(
    _In_ PVOID Context,
    _In_ PGPIO_MASK_INTERRUPT_PARAMETERS MaskParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;

    if (GpioGroupRegister(Controller, MaskParameters->BankId,
                          GPIO_COMMUNITY_GPI_ENABLE) == NULL)
    {
        MaskParameters->FailedMask = MaskParameters->PinMask;
        return STATUS_INVALID_PARAMETER;
    }

    GpioUpdateGroupMask(Controller, MaskParameters->BankId,
                        GPIO_COMMUNITY_GPI_ENABLE,
                        (ULONG)MaskParameters->PinMask, FALSE);

    MaskParameters->FailedMask = 0;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Unmasks one pad's interrupt.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] UnmaskParameters
 * The pad. Unmask is handed the enable block, not the mask one.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioUnmaskInterrupt(
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS UnmaskParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;

    GpioUpdateGroupMask(Controller, UnmaskParameters->BankId,
                        GPIO_COMMUNITY_GPI_ENABLE,
                        1UL << UnmaskParameters->PinNumber, TRUE);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reports which of a group's armed pads have fired.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in,out] QueryActiveParameters
 * Carries the armed mask in, and the asserted mask out.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a group that does not exist.
 */
NTSTATUS
NTAPI
GpioQueryActiveInterrupts(
    _In_ PVOID Context,
    _Inout_ PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Status;

    Status = GpioGroupRegister(Controller, QueryActiveParameters->BankId,
                               GPIO_COMMUNITY_GPI_STATUS);
    if (Status == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * The status register latches every pad that fired, armed or not, so it is
     * narrowed to what the caller actually armed.
     */
    QueryActiveParameters->ActiveMask =
        READ_REGISTER_ULONG(Status) & QueryActiveParameters->EnabledMask;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Acknowledges a set of a group's fired pads.
 *
 * The status bits are write-one-to-clear, so only the named pads are touched:
 * writing the whole register back would drop anything that fired in between.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] ClearParameters
 * The pads to acknowledge.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a group that does not exist.
 */
NTSTATUS
NTAPI
GpioClearActiveInterrupts(
    _In_ PVOID Context,
    _In_ PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Status;

    Status = GpioGroupRegister(Controller, ClearParameters->BankId,
                               GPIO_COMMUNITY_GPI_STATUS);
    if (Status == NULL)
    {
        ClearParameters->FailedClearMask = ClearParameters->ClearActiveMask;
        return STATUS_INVALID_PARAMETER;
    }

    WRITE_REGISTER_ULONG(Status, (ULONG)ClearParameters->ClearActiveMask);

    ClearParameters->FailedClearMask = 0;

    return STATUS_SUCCESS;
}
