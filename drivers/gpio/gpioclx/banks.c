/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bank state, the controller interrupt, and the device interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GPIO controller raises one interrupt for a whole bank, so the pin that
 * caused it has to be read back out of the bank's active register and turned
 * into something a consumer can be told about. That demultiplexing is the only
 * reason this layer exists.
 */

#define NDEBUG
#include "gpioclxp.h"

/* For RtlUnicodeStringPrintf, used to name the controller */
#include <ntstrsafe.h>

/**
 * @brief
 * Runs at device IRQL when the controller's line asserts.
 *
 * The work is deferred: reading a bank's active register means touching the
 * client, which is not something to do at this IRQL when the DPC can do it.
 * All the ISR establishes is that the interrupt was ours.
 *
 * @param[in] Interrupt
 * The controller's interrupt object.
 *
 * @param[in] MessageID
 * Unused. A GPIO controller's interrupt is a line, not a message.
 *
 * @return
 * TRUE when the interrupt belonged to this controller.
 */
BOOLEAN
NTAPI
GcxEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    PGCX_CONTROLLER Controller;

    UNREFERENCED_PARAMETER(MessageID);

    Controller = GcxGetController(WdfInterruptGetDevice(Interrupt));
    if (Controller == NULL || Controller->State != GcxControllerReady)
    {
        return FALSE;
    }

    WdfInterruptQueueDpcForIsr(Interrupt);

    return TRUE;
}

/**
 * @brief
 * Works out which pins asserted and acknowledges them.
 *
 * Every bank is asked, because the controller's one line says nothing about
 * which of them raised it.
 *
 * @param[in] Interrupt
 * The controller's interrupt object.
 *
 * @param[in] AssociatedObject
 * The device the interrupt belongs to.
 */
VOID
NTAPI
GcxEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PGCX_CONTROLLER Controller;
    PGCX_BANK Bank;
    ULONG64 ActiveMask;
    ULONG Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(AssociatedObject);

    Controller = GcxGetController(WdfInterruptGetDevice(Interrupt));
    if (Controller == NULL || Controller->Banks == NULL)
    {
        return;
    }

    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        Bank = &Controller->Banks[Index];

        if (Bank->EnabledMask == 0)
        {
            continue;
        }

        Status = GcxQueryActiveInterrupts(Controller, Bank->BankId,
                                          Bank->EnabledMask, &ActiveMask);
        if (!NT_SUCCESS(Status) || ActiveMask == 0)
        {
            continue;
        }

        /*
         * Only ever acknowledge what was armed. A pin that asserted while
         * masked stays asserted, which is what lets it be seen when it is
         * unmasked again.
         */
        ActiveMask &= Bank->EnabledMask;
        if (ActiveMask == 0)
        {
            continue;
        }

        Bank->ActiveMask |= ActiveMask;

        Status = GcxClearActiveInterrupts(Controller, Bank->BankId, ActiveMask);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("GpioClx: bank %u would not acknowledge 0x%I64x: 0x%08lX\n",
                    Bank->BankId, ActiveMask, Status);
        }

        GcxInvokeTargetIsr(Controller, Bank, ActiveMask);
    }
}

/**
 * @brief
 * Hands each asserted pin to whoever armed it.
 *
 * Pins come out of the mask lowest first, which is the order the reference
 * takes them in at :16640 and the order a consumer sees its own pins in when
 * several assert together.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] Bank
 * The bank the pins are in.
 *
 * @param[in] ActiveMask
 * The pins that asserted, already narrowed to the armed ones.
 */
VOID
GcxInvokeTargetIsr(
    _In_ PGCX_CONTROLLER Controller,
    _In_ PGCX_BANK Bank,
    _In_ ULONG64 ActiveMask)
{
    PGCX_PIN Pin;
    PIN_NUMBER PinNumber;

    while (ActiveMask != 0)
    {
        PinNumber = (PIN_NUMBER)RtlFindLeastSignificantBit(ActiveMask);
        ActiveMask &= ~(1ULL << PinNumber);

        Pin = GcxPin(Controller, Bank->BankId, PinNumber);
        if (Pin == NULL || (Pin->Flags & GCX_PIN_INTERRUPT_ENABLED) == 0)
        {
            continue;
        }

        /*
         * The hand-off goes back through the HAL, which knows what the kernel
         * connected to this pin's GSIV. A pin nobody has connected still comes
         * off the pending mask: it was acknowledged in the hardware, so holding
         * it here would only make it look perpetually asserted.
         */
        Bank->ActiveMask &= ~(1ULL << PinNumber);

        if (!GcxDeliverPinInterrupt(Pin))
        {
            DPRINT("GpioClx: bank %u pin %u asserted with no target (GSIV %lu)\n",
                   Bank->BankId, PinNumber, Pin->Gsiv);
        }
    }
}

/**
 * @brief
 * Builds the per-bank state and connects the controller's interrupt.
 *
 * Called once the client has reported its geometry, because the bank count
 * comes out of that.
 *
 * @param[in] Controller
 * The controller being brought up.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the controller coming up.
 */
NTSTATUS
GcxInitializeBanks(
    _In_ PGCX_CONTROLLER Controller)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDFMEMORY BankMemory;
    PGCX_PIN Pins;
    ULONG PinsPerBank;
    ULONG Length;
    ULONG Index;
    NTSTATUS Status;

    /*
     * The pin registrations sit after the bank array in the same allocation:
     * both live exactly as long as the device, and one block is one thing to
     * fail and one thing to free.
     */
    PinsPerBank = Controller->Information.NumberOfPinsPerBank;
    Length = Controller->BankCount * (sizeof(GCX_BANK) + PinsPerBank * sizeof(GCX_PIN));

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Controller->Device;

    Status = WdfMemoryCreate(&Attributes,
                             NonPagedPool,
                             GCX_POOL_TAG,
                             Length,
                             &BankMemory,
                             (PVOID *)&Controller->Banks);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: could not allocate %lu bank(s): 0x%08lX\n",
                Controller->BankCount, Status);
        return Status;
    }

    RtlZeroMemory(Controller->Banks, Length);

    Pins = (PGCX_PIN)&Controller->Banks[Controller->BankCount];

    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        Controller->Banks[Index].BankId = (BANK_ID)Index;
        Controller->Banks[Index].Pins = &Pins[Index * PinsPerBank];
    }

    /*
     * A controller without a line of its own has nothing to connect: its pins
     * are polled, or routed some other way the client knows about.
     */
    if (!Controller->Information.Flags.DeviceInterruptSupported)
    {
        return STATUS_SUCCESS;
    }

    /*
     * A controller that says it has a line but whose resources do not describe
     * one cannot have an interrupt built for it.  Say so rather than handing
     * KMDF a config with nothing in it.
     */
    if ((Controller->InterruptRaw == NULL) || (Controller->InterruptTranslated == NULL))
    {
        DPRINT1("GpioClx: controller claims an interrupt but none is in its "
                "resources - not creating one\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, GcxEvtInterruptIsr, GcxEvtInterruptDpc);

    /*
     * Created from PrepareHardware, so the descriptors have to be supplied -
     * KMDF only finds them itself for an interrupt created in EvtDeviceAdd.
     */
    InterruptConfig.InterruptRaw = Controller->InterruptRaw;
    InterruptConfig.InterruptTranslated = Controller->InterruptTranslated;

    Status = WdfInterruptCreate(Controller->Device,
                                &InterruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &Controller->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: WdfInterruptCreate failed 0x%08lX\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the controller a name consumers can open it by.
 *
 * The reference numbers these \Device\GPIO_0, \Device\GPIO_1 and so on
 * off a counter shared by every controller in the
 * system, which is why the number is not the bank or the instance.
 *
 * @param[in] Controller
 * The controller to name.
 *
 * @return
 * STATUS_SUCCESS, or the failure from WDF.
 */
NTSTATUS
GcxCreateControllerDevice(
    _In_ PGCX_CONTROLLER Controller)
{
    static volatile LONG GcxNextControllerNumber = 0;

    DECLARE_UNICODE_STRING_SIZE(DeviceName, 32);
    NTSTATUS Status;

    Status = RtlUnicodeStringPrintf(&DeviceName, L"\\Device\\GPIO_%d",
                                    InterlockedIncrement(&GcxNextControllerNumber) - 1);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WdfDeviceCreateSymbolicLink(Controller->Device, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: could not name the controller %wZ: 0x%08lX\n",
                &DeviceName, Status);
        return Status;
    }

    DPRINT("GpioClx: controller is %wZ\n", &DeviceName);

    return STATUS_SUCCESS;
}
