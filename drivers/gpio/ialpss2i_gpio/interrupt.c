/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pad interrupt programming
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ialpss2i_gpio.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * Takes the register lock above every interrupt GpioClx may call in from.
 */
static
KIRQL
NTAPI
IGpioLockRegisters(
    _In_ PIGPIO_CONTROLLER Controller)
{
    KIRQL OldIrql;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&Controller->RegisterLock);
    return OldIrql;
}

static
VOID
NTAPI
IGpioUnlockRegisters(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(&Controller->RegisterLock);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * The trigger bits of the first pad dword for a mode and polarity.
 *
 * @return
 * FALSE when the combination cannot be programmed.
 */
static
BOOLEAN
NTAPI
IGpioTriggerBits(
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _Out_ PULONG Bits)
{
    if (Mode == Latched)
    {
        if (Polarity == InterruptActiveHigh)
            *Bits = IGPIO_CFG0_TRIGGER_EDGE;
        else if (Polarity == InterruptActiveLow)
            *Bits = IGPIO_CFG0_TRIGGER_EDGE | IGPIO_CFG0_RX_INVERT;
        else if (Polarity == InterruptActiveBoth)
            *Bits = IGPIO_CFG0_TRIGGER_BOTH;
        else
            return FALSE;

        return TRUE;
    }

    if (Mode == LevelSensitive)
    {
        if (Polarity == InterruptActiveHigh)
            *Bits = 0;
        else if (Polarity == InterruptActiveLow)
            *Bits = IGPIO_CFG0_RX_INVERT;
        else
            return FALSE;

        return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Programs a pad as an interrupt source and arms it.
 *
 * The pad has to be owned by the GPIO driver and in GPIO mode; firmware sets
 * both up for every pad it hands out as GpioInt.
 */
static
NTSTATUS
NTAPI
IGpioProgramInterrupt(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ UCHAR PullConfiguration,
    _In_ BOOLEAN ClearLatched)
{
    const IGPIO_REGISTER_MAP *Map = &Controller->Soc->Map;
    ULONG Mask = 1UL << PinNumber;
    PULONG Enable;
    PULONG Config;
    ULONG Config0;
    ULONG Config1;
    ULONG Owners;
    ULONG Trigger;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    Config = IGpioPadRegister(Controller, BankId, PinNumber);

    OldIrql = IGpioLockRegisters(Controller);

    Config0 = READ_REGISTER_ULONG(Config);
    Config1 = READ_REGISTER_ULONG(Config + 1);
    Owners = READ_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Map->HostOwnership));

    if ((Owners & Mask) == 0 || (Config0 & IGPIO_CFG0_MODE_MASK) != 0)
    {
        DPRINT1("iaLPSS2 GPIO: bank %u pad %u is not a GPIO driver pad\n", BankId, PinNumber);
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (!IGpioTriggerBits(Mode, Polarity, &Trigger))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Config0 &= ~(IGPIO_CFG0_TRIGGER_MASK | IGPIO_CFG0_RX_INVERT);
    Config0 |= Trigger;

    /* Input only, with the event routed to the GPIO status register */
    Config0 &= ~(IGPIO_CFG0_RX_DISABLE | IGPIO_CFG0_ROUTE_MASK);
    Config0 |= IGPIO_CFG0_TX_DISABLE;

    Config1 = IGpioApplyPull(Config1, PullConfiguration);

    WRITE_REGISTER_ULONG(Config + 1, Config1);
    WRITE_REGISTER_ULONG(Config, Config0);

    if (ClearLatched)
        WRITE_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Map->InterruptStatus), Mask);

    Enable = IGpioGroupRegister(Controller, BankId, Map->InterruptEnable);
    WRITE_REGISTER_ULONG(Enable, READ_REGISTER_ULONG(Enable) | Mask);

Exit:
    IGpioUnlockRegisters(Controller, OldIrql);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioEnableInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = EnableParameters->BankId;
    PIN_NUMBER Pin = EnableParameters->PinNumber;
    NTSTATUS Status;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    GPIO_CLX_AcquireInterruptLock(Context, BankId);

    if (!IGpioIsPinValid(Controller, BankId, Pin))
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        Status = IGpioProgramInterrupt(Controller,
                                       BankId,
                                       Pin,
                                       EnableParameters->InterruptMode,
                                       EnableParameters->Polarity,
                                       EnableParameters->PullConfiguration,
                                       TRUE);
        if (NT_SUCCESS(Status))
            Controller->Banks[BankId].Pads[Pin].Use |= IGPIO_PAD_INTERRUPT;
    }

    GPIO_CLX_ReleaseInterruptLock(Context, BankId);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioDisableInterrupt(
    PVOID Context,
    PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = DisableParameters->BankId;
    PIN_NUMBER Pin = DisableParameters->PinNumber;
    const IGPIO_REGISTER_MAP *Map;
    PULONG Enable;
    ULONG Owners;
    ULONG Config0;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    Map = &Controller->Soc->Map;

    GPIO_CLX_AcquireInterruptLock(Context, BankId);

    if (!IGpioIsPinValid(Controller, BankId, Pin))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Owners = READ_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Map->HostOwnership));
    Config0 = READ_REGISTER_ULONG(IGpioPadRegister(Controller, BankId, Pin));
    if ((Owners & (1UL << Pin)) == 0 || (Config0 & IGPIO_CFG0_MODE_MASK) != 0)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    OldIrql = IGpioLockRegisters(Controller);
    Enable = IGpioGroupRegister(Controller, BankId, Map->InterruptEnable);
    WRITE_REGISTER_ULONG(Enable, READ_REGISTER_ULONG(Enable) & ~(1UL << Pin));
    IGpioUnlockRegisters(Controller, OldIrql);

    Controller->Banks[BankId].Pads[Pin].Use &= ~IGPIO_PAD_INTERRUPT;

Exit:
    GPIO_CLX_ReleaseInterruptLock(Context, BankId);
    return Status;
}

/**
 * @brief
 * Re-arms a pad, keeping whatever it latched while masked.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioUnmaskInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS InterruptParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = InterruptParameters->BankId;
    PIN_NUMBER Pin = InterruptParameters->PinNumber;
    NTSTATUS Status;

    if (!IGpioIsBankValid(Controller, BankId) || !IGpioIsPinValid(Controller, BankId, Pin))
        return STATUS_INVALID_PARAMETER;

    Status = IGpioProgramInterrupt(Controller,
                                   BankId,
                                   Pin,
                                   InterruptParameters->InterruptMode,
                                   InterruptParameters->Polarity,
                                   InterruptParameters->PullConfiguration,
                                   FALSE);
    if (NT_SUCCESS(Status))
        Controller->Banks[BankId].Pads[Pin].Use |= IGPIO_PAD_INTERRUPT;

    return Status;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioMaskInterrupts(
    PVOID Context,
    PGPIO_MASK_INTERRUPT_PARAMETERS MaskParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = MaskParameters->BankId;
    PULONG Enable;
    KIRQL OldIrql;

    MaskParameters->FailedMask = MaskParameters->PinMask;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_SUCCESS;

    Enable = IGpioGroupRegister(Controller, BankId, Controller->Soc->Map.InterruptEnable);

    OldIrql = IGpioLockRegisters(Controller);
    WRITE_REGISTER_ULONG(Enable, READ_REGISTER_ULONG(Enable) & ~(ULONG)MaskParameters->PinMask);
    IGpioUnlockRegisters(Controller, OldIrql);

    MaskParameters->FailedMask = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioQueryActiveInterrupts(
    PVOID Context,
    PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = QueryActiveParameters->BankId;
    ULONG Latched;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    Latched = READ_REGISTER_ULONG(IGpioGroupRegister(Controller,
                                                     BankId,
                                                     Controller->Soc->Map.InterruptStatus));
    QueryActiveParameters->ActiveMask = QueryActiveParameters->EnabledMask & Latched;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioQueryEnabledInterrupts(
    PVOID Context,
    PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS QueryEnabledParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = QueryEnabledParameters->BankId;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    QueryEnabledParameters->EnabledMask =
        READ_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Controller->Soc->Map.InterruptEnable));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Acknowledges latched pads; the status register is write one to clear.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioClearActiveInterrupts(
    PVOID Context,
    PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = ClearParameters->BankId;

    ClearParameters->FailedClearMask = ClearParameters->ClearActiveMask;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_SUCCESS;

    WRITE_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Controller->Soc->Map.InterruptStatus),
                         (ULONG)ClearParameters->ClearActiveMask);

    ClearParameters->FailedClearMask = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Changes the trigger of an armed pad, used to emulate both edges.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioReconfigureInterrupt(
    PVOID Context,
    PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = ReconfigureParameters->BankId;
    PIN_NUMBER Pin = ReconfigureParameters->PinNumber;

    if (!IGpioIsBankValid(Controller, BankId) || !IGpioIsPinValid(Controller, BankId, Pin))
        return STATUS_INVALID_PARAMETER;

    return IGpioProgramInterrupt(Controller,
                                 BankId,
                                 Pin,
                                 ReconfigureParameters->InterruptMode,
                                 ReconfigureParameters->Polarity,
                                 GPIO_PIN_PULL_CONFIGURATION_DEFAULT,
                                 TRUE);
}
