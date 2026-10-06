/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller lifetime and geometry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * This is the whole of a client's hardware contract: GpioClx owns the WDF
 * device and its PnP and power callbacks, and the controller driver only ever
 * sees the four calls made from here.
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Checks what a client says about its silicon before believing it.
 *
 * A controller with no pins, or with a bank wider than the pin count, would
 * have every bank calculation above this produce nonsense, so it is refused
 * here rather than left to fail later somewhere less obvious.
 *
 * @param[in] Information
 * What the client reported.
 *
 * @return
 * TRUE if the geometry is usable.
 */
static
BOOLEAN
GcxValidateBasicInformation(
    _In_ PCLIENT_CONTROLLER_BASIC_INFORMATION Information)
{
    if (Information->Version != 1 ||
        Information->Size != sizeof(CLIENT_CONTROLLER_BASIC_INFORMATION))
    {
        DPRINT1("GpioClx: basic information is version %u size %u\n",
                Information->Version, Information->Size);
        return FALSE;
    }

    if (Information->TotalPins == 0 || Information->NumberOfPinsPerBank == 0)
    {
        DPRINT1("GpioClx: controller reports %u pins in banks of %u\n",
                Information->TotalPins, Information->NumberOfPinsPerBank);
        return FALSE;
    }

    if (Information->NumberOfPinsPerBank > Information->TotalPins)
    {
        DPRINT1("GpioClx: a bank of %u does not fit in %u pins\n",
                Information->NumberOfPinsPerBank, Information->TotalPins);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Asks the client how its controller is laid out.
 *
 * Called once the client has claimed its resources, because the answer can
 * depend on them: a controller that reads its own pin count out of a register
 * has nothing to report until its window is mapped.
 *
 * @param[in] Controller
 * The controller being brought up.
 *
 * @return
 * STATUS_SUCCESS, or a failure from the client, or
 * STATUS_DEVICE_CONFIGURATION_ERROR for geometry that cannot be used.
 */
NTSTATUS
GcxQueryControllerInformation(
    _In_ PGCX_CONTROLLER Controller)
{
    CLIENT_CONTROLLER_BASIC_INFORMATION Information;
    NTSTATUS Status;

    if (Controller->Registration.CLIENT_QueryControllerBasicInformation == NULL)
    {
        DPRINT1("GpioClx: client answers no QueryControllerBasicInformation\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RtlZeroMemory(&Information, sizeof(Information));

    Status = Controller->Registration.CLIENT_QueryControllerBasicInformation(
                 GcxClientContext(Controller), &Information);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: QueryControllerBasicInformation failed 0x%08lX\n", Status);
        return Status;
    }

    if (!GcxValidateBasicInformation(&Information))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Controller->Information = Information;

    /* A trailing part-bank still needs a bank of its own */
    Controller->BankCount = (Information.TotalPins + Information.NumberOfPinsPerBank - 1) /
                            Information.NumberOfPinsPerBank;

    DPRINT("GpioClx: %u pins in %lu bank(s) of %u\n",
           Information.TotalPins, Controller->BankCount,
           Information.NumberOfPinsPerBank);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Hands the controller its resources and reads back its geometry.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] ResourcesRaw
 * The raw resource list.
 *
 * @param[in] ResourcesTranslated
 * The translated resource list.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the controller coming up.
 */
/**
 * @brief
 * Notes the interrupt the controller itself was given.
 *
 * A device connected to one of this controller's pins runs its ISR from inside
 * this interrupt, so this line is the answer to what such a device's IRQL is.
 * The reference reports the same thing from GpioHubQueryPrimaryInterruptInformation.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] ResourcesTranslated
 * Its translated resource list.
 */
static
VOID
GcxRecordPrimaryInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Index;
    ULONG Count;

    Controller->PrimaryGsiv = GCX_NO_PRIMARY_GSIV;
    Controller->InterruptRaw = NULL;
    Controller->InterruptTranslated = NULL;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if ((Descriptor == NULL) || (Descriptor->Type != CmResourceTypeInterrupt))
        {
            continue;
        }

        /*
         * Keep the pair of descriptors, not just the numbers in them.
         *
         * WdfInterruptCreate is called from PrepareHardware, and KMDF only
         * matches an interrupt to a resource by itself for interrupts created
         * in EvtDeviceAdd.  Created here, the config has to carry both the raw
         * and the translated descriptor or KMDF rejects it - which is exactly
         * what "NULL InterruptRaw or InterruptTranslated in WDF_INTERRUPT_CONFIG"
         * was saying, and why WdfInterruptCreate returned
         * STATUS_INVALID_DEVICE_STATE and took the whole controller down with
         * it.
         *
         * The two lists are index-parallel, so the raw descriptor is the one at
         * the same position.
         */
        Controller->InterruptTranslated = Descriptor;
        Controller->InterruptRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, Index);

        /*
         * The translated form carries the vector; what identifies the line to
         * the HAL is the raw level, which the arbiter left in the translated
         * descriptor's own Level for a line interrupt.
         */
        Controller->PrimaryGsiv = Descriptor->u.Interrupt.Vector;
        Controller->PrimaryIrql = (KIRQL)Descriptor->u.Interrupt.Level;
        Controller->PrimaryAffinity = Descriptor->u.Interrupt.Affinity;
        break;
    }
}

NTSTATUS
NTAPI
GcxEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PGCX_CONTROLLER Controller = GcxGetController(Device);
    NTSTATUS Status;

    InterlockedExchange(&Controller->State, GcxControllerPreparing);

    GcxRecordPrimaryInterrupt(Controller, ResourcesRaw, ResourcesTranslated);

    /*
     * Learn what the firmware calls this controller before anything can ask
     * for one of its pins. A controller with no ACPI name behind it can still
     * do everything except answer a GpioInt described on another device, so
     * this is not fatal.
     */
    InitializeListHead(&Controller->ControllerLink);
    Status = GcxQueryDeviceBiosName(Device, &Controller->BiosName);
    if (NT_SUCCESS(Status))
    {
        DPRINT("GpioClx: controller is %wZ\n", &Controller->BiosName);
        GcxAddController(Controller);
    }
    else
    {
        DPRINT1("GpioClx: no firmware name for this controller: 0x%08lX\n", Status);
    }

    if (Controller->Registration.CLIENT_PrepareController == NULL)
    {
        DPRINT1("GpioClx: client answers no PrepareController\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = Controller->Registration.CLIENT_PrepareController(
                 Device, GcxClientContext(Controller),
                 ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: PrepareController failed 0x%08lX\n", Status);
        return Status;
    }

    Status = GcxQueryControllerInformation(Controller);
    if (!NT_SUCCESS(Status))
    {
        /*
         * The client has already taken its resources, so unwind it rather than
         * leave a half-prepared controller behind: PnP will not call
         * ReleaseHardware for a PrepareHardware that failed.
         */
        if (Controller->Registration.CLIENT_ReleaseController != NULL)
        {
            Controller->Registration.CLIENT_ReleaseController(
                Device, GcxClientContext(Controller));
        }
        return Status;
    }

    Status = GcxInitializeBanks(Controller);
    if (!NT_SUCCESS(Status))
    {
        if (Controller->Registration.CLIENT_ReleaseController != NULL)
        {
            Controller->Registration.CLIENT_ReleaseController(
                Device, GcxClientContext(Controller));
        }
        return Status;
    }

    /*
     * Claim the secondary GSIV space now that there is a controller to route
     * to. It is once for the whole driver, not once per controller, so a
     * second controller coming up finds it already done. Failing is not fatal
     * to the controller: its pins still read and write, they just cannot be
     * connected as interrupts.
     */
    Status = GcxRegisterSecondaryInterruptController(
                 WdfDriverWdmGetDriverObject(WdfGetDriver()));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: pins will not deliver interrupts: 0x%08lX\n", Status);
    }

    InterlockedExchange(&Controller->State, GcxControllerReady);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the controller's resources back.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] ResourcesTranslated
 * The translated resource list. Unused; the client kept what it needed.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GcxEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PGCX_CONTROLLER Controller = GcxGetController(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    InterlockedExchange(&Controller->State, GcxControllerStopped);

    /* No GSIV may name this controller's pins once its registers are gone */
    GcxRemoveVirqMappings(Controller);
    GcxRemoveController(Controller);

    if (Controller->BiosName.Buffer != NULL)
    {
        ExFreePoolWithTag(Controller->BiosName.Buffer, GCX_POOL_TAG);
        RtlZeroMemory(&Controller->BiosName, sizeof(Controller->BiosName));
    }

    if (Controller->Registration.CLIENT_ReleaseController != NULL)
    {
        Controller->Registration.CLIENT_ReleaseController(
            Device, GcxClientContext(Controller));
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Starts the controller as the device enters D0.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] PreviousState
 * The power state being left. A client that saved its registers on the way out
 * uses this to decide whether they have to be put back.
 *
 * @return
 * STATUS_SUCCESS, or the failure reported by the client.
 */
NTSTATUS
NTAPI
GcxEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PGCX_CONTROLLER Controller = GcxGetController(Device);
    BOOLEAN RestoreContext;

    if (Controller->Registration.CLIENT_StartController == NULL)
    {
        return STATUS_SUCCESS;
    }

    /*
     * Coming back from anything below D0 means the registers may be gone. A
     * controller whose banks survive idle says so through
     * BankIdlePowerMgmtSupported and is not asked to restore them.
     */
    RestoreContext = (BOOLEAN)(PreviousState != WdfPowerDeviceD0 &&
                               !Controller->Information.Flags.BankIdlePowerMgmtSupported);

    return Controller->Registration.CLIENT_StartController(
               GcxClientContext(Controller), RestoreContext, PreviousState);
}

/**
 * @brief
 * Stops the controller as the device leaves D0.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] TargetState
 * The power state being entered.
 *
 * @return
 * STATUS_SUCCESS, or the failure reported by the client.
 */
NTSTATUS
NTAPI
GcxEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PGCX_CONTROLLER Controller = GcxGetController(Device);
    BOOLEAN SaveContext;

    if (Controller->Registration.CLIENT_StopController == NULL)
    {
        return STATUS_SUCCESS;
    }

    SaveContext = (BOOLEAN)(TargetState != WdfPowerDeviceD0 &&
                            !Controller->Information.Flags.BankIdlePowerMgmtSupported);

    return Controller->Registration.CLIENT_StopController(
               GcxClientContext(Controller), SaveContext, TargetState);
}
