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

    /* A pending debounce must not unmask a pin that is going away */
    if (Pin->Debounce != NULL)
    {
        KeCancelTimer(&Pin->Debounce->Timer);
        KeFlushQueuedDpcs();
        ExFreePoolWithTag(Pin->Debounce, GCX_POOL_TAG);
    }

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

/**
 * @brief
 * Turns an emulated both edge pin around to wait for the opposite level.
 *
 * @remarks
 * Runs inside the controller interrupt. A client that refuses is asked
 * again a few times, then the pin keeps its old polarity.
 */
_Use_decl_annotations_
VOID
NTAPI
GcxFlipPinPolarity(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PIN_NUMBER PinNumber)
{
    GPIO_RECONFIGURE_INTERRUPTS_PARAMETERS Parameters;
    KINTERRUPT_POLARITY Next;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    PGCX_PIN Pin;
    ULONG Attempt;

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL || Controller->Registration.CLIENT_ReconfigureInterrupt == NULL)
        return;

    Next = (Pin->Polarity == InterruptActiveHigh) ? InterruptActiveLow : InterruptActiveHigh;

    for (Attempt = 0; Attempt < 5; Attempt++)
    {
        RtlZeroMemory(&Parameters, sizeof(Parameters));
        Parameters.BankId = BankId;
        Parameters.PinNumber = PinNumber;
        Parameters.InterruptMode = Pin->InterruptMode;
        Parameters.Polarity = Next;

        Status = Controller->Registration.CLIENT_ReconfigureInterrupt(GcxClientContext(Controller),
                                                                      &Parameters);
        if (NT_SUCCESS(Status))
        {
            Pin->Polarity = Next;
            return;
        }
    }

    DPRINT1("GpioClx: bank %u pin %u would not flip polarity, 0x%08lx\n", BankId, PinNumber, Status);
}

/**
 * @brief
 * Unmasks a pin whose debounce time has passed.
 */
static
VOID
NTAPI
GcxFinishDebounce(
    _In_ PGCX_DEBOUNCE Debounce)
{
    PGCX_CONTROLLER Controller = Debounce->Controller;
    PGCX_PIN Pin;

    WdfInterruptAcquireLock(Controller->Interrupt);
    Pin = GcxPin(Controller, Debounce->BankId, Debounce->PinNumber);
    if (Pin != NULL && (Pin->Flags & GCX_PIN_INTERRUPT_ENABLED) != 0)
        GcxUnmaskInterrupt(Controller, Debounce->BankId, Debounce->PinNumber);
    WdfInterruptReleaseLock(Controller->Interrupt);
}

static
VOID
NTAPI
GcxDebounceWorker(
    _In_ PVOID Parameter)
{
    GcxFinishDebounce(Parameter);
}

static
VOID
NTAPI
GcxDebounceDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PGCX_DEBOUNCE Debounce = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* A controller behind a serial bus can only be reached at PASSIVE_LEVEL */
    if (Debounce->Controller->Information.Flags.MemoryMappedController)
        GcxFinishDebounce(Debounce);
    else
        ExQueueWorkItem(&Debounce->WorkItem, DelayedWorkQueue);
}

/**
 * @brief
 * Sets up the timer that holds a pin masked while its input settles.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxArmDebounce(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PIN_NUMBER PinNumber)
{
    PGCX_DEBOUNCE Debounce;
    PGCX_PIN Pin;

    Pin = GcxPin(Controller, BankId, PinNumber);
    if (Pin == NULL)
        return STATUS_INVALID_PARAMETER;
    if (Pin->Debounce != NULL)
        return STATUS_SUCCESS;

    Debounce = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Debounce), GCX_POOL_TAG);
    if (Debounce == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Debounce->Controller = Controller;
    Debounce->BankId = BankId;
    Debounce->PinNumber = PinNumber;
    KeInitializeTimer(&Debounce->Timer);
    KeInitializeDpc(&Debounce->Dpc, GcxDebounceDpc, Debounce);
    ExInitializeWorkItem(&Debounce->WorkItem, GcxDebounceWorker, Debounce);

    Pin->Debounce = Debounce;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Masks a pin that just fired and lets its bounces pass before it may fire again.
 *
 * @remarks
 * The timeout is in hundredths of a millisecond, the ACPI GpioInt unit.
 */
_Use_decl_annotations_
VOID
NTAPI
GcxStartDebounce(
    PGCX_CONTROLLER Controller,
    PGCX_PIN Pin)
{
    LARGE_INTEGER DueTime;
    ULONG64 Failed;

    if (Pin->Debounce == NULL || Pin->DebounceTimeout == 0)
        return;

    GcxMaskInterrupts(Controller, Pin->Debounce->BankId, 1ULL << Pin->Debounce->PinNumber, &Failed);
    DueTime.QuadPart = -(LONGLONG)Pin->DebounceTimeout * 100;
    KeSetTimer(&Pin->Debounce->Timer, DueTime, &Pin->Debounce->Dpc);
}
