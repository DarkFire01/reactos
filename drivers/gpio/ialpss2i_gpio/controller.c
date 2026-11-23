/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller lifetime and bank context
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ialpss2i_gpio.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * Maps one window per community, in resource order, and checks for the single interrupt.
 */
static
NTSTATUS
NTAPI
IGpioMapResources(
    _Inout_ PIGPIO_CONTROLLER Controller,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Interrupts = 0;
    ULONG Windows = 0;
    ULONG Count;
    ULONG Index;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Descriptor == NULL)
            continue;

        if (Descriptor->Type == CmResourceTypeInterrupt)
        {
            Interrupts++;
            continue;
        }

        if (Descriptor->Type != CmResourceTypeMemory)
            continue;

        if (Windows >= Controller->Soc->CommunityCount)
        {
            DPRINT1("iaLPSS2 GPIO: more windows than communities\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Controller->Window[Windows] = MmMapIoSpace(Descriptor->u.Memory.Start,
                                                   Descriptor->u.Memory.Length,
                                                   MmNonCached);
        if (Controller->Window[Windows] == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Controller->WindowLength[Windows] = Descriptor->u.Memory.Length;
        Windows++;
    }

    if (Windows != Controller->Soc->CommunityCount || Interrupts != 1)
    {
        DPRINT1("iaLPSS2 GPIO: %lu windows, %lu interrupts\n", Windows, Interrupts);
        return STATUS_INVALID_HW_PROFILE;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioPrepareController(
    WDFDEVICE Device,
    PVOID Context,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated)
{
    PIGPIO_CONTROLLER Controller = Context;
    const IGPIO_SOC *Soc;
    USHORT Bank;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    RtlZeroMemory(Controller, sizeof(*Controller));
    Controller->Device = Device;
    KeInitializeSpinLock(&Controller->RegisterLock);

    /* A borrowed geometry would drive the wrong pads */
    Soc = IGpioFindSoc(Device);
    if (Soc == NULL)
        return STATUS_INVALID_HW_PROFILE;

    Controller->Soc = Soc;

    Status = IGpioMapResources(Controller, ResourcesTranslated);
    if (!NT_SUCCESS(Status))
        goto Fail;

    for (Bank = 0; Bank < Soc->GroupCount; Bank++)
    {
        Controller->Banks[Bank].Pads = ExAllocatePoolZero(NonPagedPoolNx,
                                                          Soc->Groups[Bank].PadCount * sizeof(IGPIO_PAD),
                                                          IGPIO_POOL_TAG);
        if (Controller->Banks[Bank].Pads == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Fail;
        }
    }

    /* Every bank but the last is full width */
    Controller->TotalPins = (USHORT)(Soc->PinsPerBank * (Soc->GroupCount - 1) +
                                     Soc->Groups[Soc->GroupCount - 1].PadCount);
    return STATUS_SUCCESS;

Fail:
    IGpioReleaseController(Device, Context);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioReleaseController(
    WDFDEVICE Device,
    PVOID Context)
{
    PIGPIO_CONTROLLER Controller = Context;
    ULONG Index;

    UNREFERENCED_PARAMETER(Device);

    for (Index = 0; Index < IGPIO_MAX_COMMUNITIES; Index++)
    {
        if (Controller->Window[Index] == NULL)
            continue;

        MmUnmapIoSpace(Controller->Window[Index], Controller->WindowLength[Index]);
        Controller->Window[Index] = NULL;
        Controller->WindowLength[Index] = 0;
    }

    for (Index = 0; Index < IGPIO_MAX_GROUPS; Index++)
    {
        if (Controller->Banks[Index].Pads == NULL)
            continue;

        ExFreePoolWithTag(Controller->Banks[Index].Pads, IGPIO_POOL_TAG);
        Controller->Banks[Index].Pads = NULL;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Disarms every pad of a bank and drops whatever it latched.
 */
static
VOID
NTAPI
IGpioQuietBank(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    const IGPIO_REGISTER_MAP *Map = &Controller->Soc->Map;

    WRITE_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Map->InterruptEnable), 0);
    WRITE_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Map->InterruptStatus), MAXULONG);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioStartController(
    PVOID Context,
    BOOLEAN RestoreContext,
    WDF_POWER_DEVICE_STATE PreviousPowerState)
{
    GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters;
    PIGPIO_CONTROLLER Controller = Context;
    PIGPIO_BANK Bank;
    BANK_ID BankId;

    UNREFERENCED_PARAMETER(PreviousPowerState);

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    for (BankId = 0; BankId < Controller->Soc->GroupCount; BankId++)
    {
        if (RestoreContext)
        {
            Parameters.BankId = BankId;
            IGpioRestoreBankHardwareContext(Context, &Parameters);
            continue;
        }

        /* Coming up cold nothing is connected yet */
        IGpioQuietBank(Controller, BankId);

        Bank = &Controller->Banks[BankId];
        RtlZeroMemory(Bank->Pads, Controller->Soc->Groups[BankId].PadCount * sizeof(IGPIO_PAD));
        Bank->Saved = FALSE;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioStopController(
    PVOID Context,
    BOOLEAN SaveContext,
    WDF_POWER_DEVICE_STATE TargetState)
{
    GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters;
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId;

    UNREFERENCED_PARAMETER(TargetState);

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    for (BankId = 0; BankId < Controller->Soc->GroupCount; BankId++)
    {
        if (SaveContext)
        {
            Parameters.BankId = BankId;
            IGpioSaveBankHardwareContext(Context, &Parameters);
        }
        else
        {
            IGpioQuietBank(Controller, BankId);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioQueryControllerBasicInformation(
    PVOID Context,
    PCLIENT_CONTROLLER_BASIC_INFORMATION Information)
{
    PIGPIO_CONTROLLER Controller = Context;

    Information->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Information->Size = sizeof(*Information);
    Information->TotalPins = Controller->TotalPins;
    Information->NumberOfPinsPerBank = Controller->Soc->PinsPerBank;

    /* Pads have neither debounce nor dual edge logic GpioClx could rely on */
    Information->Flags.AsULONG = 0;
    Information->Flags.MemoryMappedController = 1;
    Information->Flags.EmulateDebouncing = 1;
    Information->Flags.EmulateActiveBoth = 1;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Records the configuration of the pads in use before the bank loses power.
 *
 * A pad muxed to a native function is left alone.
 */
_Use_decl_annotations_
VOID
NTAPI
IGpioSaveBankHardwareContext(
    PVOID Context,
    PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = Parameters->BankId;
    PIGPIO_BANK Bank;
    PIGPIO_PAD Pad;
    PIN_NUMBER Pin;
    PULONG Config;
    ULONG Config0;

    if (!IGpioIsBankValid(Controller, BankId))
        return;

    Bank = &Controller->Banks[BankId];

    for (Pin = 0; Pin < Controller->Soc->Groups[BankId].PadCount; Pin++)
    {
        Pad = &Bank->Pads[Pin];
        if (Pad->Use == 0)
            continue;

        Config = IGpioPadRegister(Controller, BankId, Pin);
        Config0 = READ_REGISTER_ULONG(Config);
        if ((Config0 & IGPIO_CFG0_MODE_MASK) != 0)
            continue;

        Pad->SavedConfig0 = Config0;
        Pad->SavedConfig1 = READ_REGISTER_ULONG(Config + 1);
        Pad->Saved = TRUE;
    }

    Bank->SavedEnable = READ_REGISTER_ULONG(IGpioGroupRegister(Controller,
                                                               BankId,
                                                               Controller->Soc->Map.InterruptEnable));
    Bank->Saved = TRUE;
}

/**
 * @brief
 * Writes back what the matching save recorded, the enable word last.
 */
_Use_decl_annotations_
VOID
NTAPI
IGpioRestoreBankHardwareContext(
    PVOID Context,
    PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = Parameters->BankId;
    PIGPIO_BANK Bank;
    PIGPIO_PAD Pad;
    PIN_NUMBER Pin;
    PULONG Config;

    if (!IGpioIsBankValid(Controller, BankId))
        return;

    Bank = &Controller->Banks[BankId];
    if (!Bank->Saved)
        return;

    for (Pin = 0; Pin < Controller->Soc->Groups[BankId].PadCount; Pin++)
    {
        Pad = &Bank->Pads[Pin];
        if (!Pad->Saved)
            continue;

        /* Termination before mode, so the pad never floats as a GPIO */
        Config = IGpioPadRegister(Controller, BankId, Pin);
        WRITE_REGISTER_ULONG(Config + 1, Pad->SavedConfig1);
        WRITE_REGISTER_ULONG(Config, Pad->SavedConfig0);
        Pad->Saved = FALSE;
    }

    WRITE_REGISTER_ULONG(IGpioGroupRegister(Controller, BankId, Controller->Soc->Map.InterruptEnable),
                         Bank->SavedEnable);
    Bank->Saved = FALSE;
}
