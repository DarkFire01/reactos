/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bank state and the controller interrupt
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Hands each asserted pin of one bank to whoever connected it.
 *
 * A pin nobody claims is masked, or a level triggered source would keep the
 * controller's line asserted forever.
 */
static
VOID
NTAPI
GcxDispatchBank(
    _In_ PGCX_CONTROLLER Controller,
    _In_ PGCX_BANK Bank,
    _In_ ULONG64 Asserted)
{
    ULONG64 Unclaimed = 0;
    ULONG64 Failed;
    PIN_NUMBER PinNumber;
    PGCX_PIN Pin;

    for (PinNumber = 0; Asserted != 0; PinNumber++, Asserted >>= 1)
    {
        if ((Asserted & 1) == 0)
            continue;

        Pin = &Bank->Pins[PinNumber];
        if ((Pin->Flags & GCX_PIN_INTERRUPT_ENABLED) != 0 && GcxDeliverPinInterrupt(Pin))
            continue;

        Unclaimed |= 1ULL << PinNumber;
    }

    if (Unclaimed != 0)
    {
        DPRINT1("GpioClx: bank %u pins 0x%I64x asserted with no taker\n", Bank->BankId, Unclaimed);
        GcxMaskInterrupts(Controller, Bank->BankId, Unclaimed, &Failed);
    }
}

/**
 * @brief
 * Demultiplexes the controller's line into per pin interrupts.
 *
 * Runs at DIRQL for a memory mapped controller and at PASSIVE_LEVEL otherwise,
 * matching where the client may be called. The work cannot be deferred to a DPC:
 * a level triggered controller line would fire again before the DPC could run.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
GcxEvtInterruptIsr(
    WDFINTERRUPT Interrupt,
    ULONG MessageID)
{
    PGCX_CONTROLLER Controller;
    BOOLEAN Claimed = FALSE;
    ULONG64 Asserted;
    PGCX_BANK Bank;
    USHORT Index;

    UNREFERENCED_PARAMETER(MessageID);

    Controller = GcxGetController(WdfInterruptGetDevice(Interrupt));
    if (Controller->State != GcxControllerReady || Controller->Banks == NULL)
        return FALSE;

    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        Bank = &Controller->Banks[Index];
        if (Bank->EnabledMask == 0)
            continue;

        if (!NT_SUCCESS(GcxQueryActiveInterrupts(Controller,
                                                 Bank->BankId,
                                                 Bank->EnabledMask,
                                                 &Asserted)))
        {
            continue;
        }

        /* A pin that asserted while masked stays latched for when it is unmasked */
        Asserted &= Bank->EnabledMask;
        if (Asserted == 0)
            continue;

        Claimed = TRUE;
        GcxClearActiveInterrupts(Controller, Bank->BankId, Asserted);
        GcxDispatchBank(Controller, Bank, Asserted);
    }

    return Claimed;
}

/**
 * @brief
 * Creates the per bank locks for GPIO_CLX_AcquireInterruptLock.
 */
static
NTSTATUS
NTAPI
GcxCreateBankLocks(
    _In_ PGCX_CONTROLLER Controller)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    USHORT Index;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Controller->BankMemory;

    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        if (Controller->Information.Flags.MemoryMappedController)
            Status = WdfSpinLockCreate(&Attributes, &Controller->Banks[Index].SpinLock);
        else
            Status = WdfWaitLockCreate(&Attributes, &Controller->Banks[Index].WaitLock);

        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Builds the bank array and connects the controller's interrupt.
 *
 * @param[in] Controller
 * The controller, with its geometry already known.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the controller coming up.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxInitializeBanks(
    PGCX_CONTROLLER Controller)
{
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    SIZE_T PinBytes;
    SIZE_T Length;
    PUCHAR Block;
    USHORT Index;
    NTSTATUS Status;

    /* The banks and all their pin entries live in one block owned by the device */
    PinBytes = Controller->Information.NumberOfPinsPerBank * sizeof(GCX_PIN);
    Length = Controller->BankCount * (sizeof(GCX_BANK) + PinBytes);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Controller->Device;

    Status = WdfMemoryCreate(&Attributes,
                             NonPagedPoolNx,
                             GCX_POOL_TAG,
                             Length,
                             &Controller->BankMemory,
                             (PVOID *)&Block);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Block, Length);
    Controller->Banks = (PGCX_BANK)Block;
    Block += Controller->BankCount * sizeof(GCX_BANK);

    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        Controller->Banks[Index].BankId = Index;
        Controller->Banks[Index].Pins = (PGCX_PIN)(Block + Index * PinBytes);
    }

    Status = GcxCreateBankLocks(Controller);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Without a line of its own the controller's pins are only good for I/O */
    if (Controller->InterruptRaw == NULL)
        return STATUS_SUCCESS;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, GcxEvtInterruptIsr, NULL);
    InterruptConfig.InterruptRaw = Controller->InterruptRaw;
    InterruptConfig.InterruptTranslated = Controller->InterruptTranslated;

    /* A controller behind a serial bus can only be touched at PASSIVE_LEVEL */
    if (!Controller->Information.Flags.MemoryMappedController)
    {
        InterruptConfig.PassiveHandling = TRUE;
        InterruptConfig.AutomaticSerialization = FALSE;
    }

    Status = WdfInterruptCreate(Controller->Device,
                                &InterruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &Controller->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: WdfInterruptCreate failed 0x%08lx\n", Status);
        Controller->Interrupt = NULL;
        return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drops the bank array once the hardware is released.
 */
_Use_decl_annotations_
VOID
NTAPI
GcxFreeBanks(
    PGCX_CONTROLLER Controller)
{
    /* KMDF owns an interrupt created in EvtDevicePrepareHardware and deletes it itself */
    Controller->Interrupt = NULL;

    Controller->Banks = NULL;
    Controller->BankCount = 0;

    if (Controller->BankMemory != NULL)
    {
        WdfObjectDelete(Controller->BankMemory);
        Controller->BankMemory = NULL;
    }
}
