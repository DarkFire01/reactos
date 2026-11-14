/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller lifetime and geometry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

/* Attribute bits defined by the WDK as of GPIO client version 4 */
#define GCX_KNOWN_ATTRIBUTE_FLAGS 0xFF

/**
 * @brief
 * Checks the geometry and attributes a client reports.
 */
static
BOOLEAN
NTAPI
GcxIsBasicInformationValid(
    _In_ PCLIENT_CONTROLLER_BASIC_INFORMATION Information)
{
    if (Information->Version == 0 ||
        Information->Size < sizeof(*Information))
    {
        return FALSE;
    }

    if (Information->TotalPins == 0 ||
        Information->NumberOfPinsPerBank == 0 ||
        Information->NumberOfPinsPerBank > Information->TotalPins)
    {
        return FALSE;
    }

    /* A bank is addressed with 64 bit masks */
    if (Information->NumberOfPinsPerBank > 64)
        return FALSE;

    if ((Information->Flags.AsULONG & ~GCX_KNOWN_ATTRIBUTE_FLAGS) != 0)
        return FALSE;

    /* F-state bank power management is only offered for memory mapped controllers */
    if (Information->Flags.BankIdlePowerMgmtSupported &&
        !Information->Flags.MemoryMappedController)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Asks the client how its controller is laid out.
 *
 * @param[in] Controller
 * The controller being brought up.
 *
 * @return
 * STATUS_SUCCESS, the client's failure, or STATUS_GPIO_CLIENT_INFORMATION_INVALID.
 */
static
NTSTATUS
NTAPI
GcxQueryControllerInformation(
    _In_ PGCX_CONTROLLER Controller)
{
    CLIENT_CONTROLLER_BASIC_INFORMATION Information;
    ULONG PinsPerBank;
    NTSTATUS Status;

    RtlZeroMemory(&Information, sizeof(Information));
    Status = Controller->Registration.CLIENT_QueryControllerBasicInformation(
                 GcxClientContext(Controller),
                 &Information);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: QueryControllerBasicInformation failed 0x%08lx\n", Status);
        return Status;
    }

    if (!GcxIsBasicInformationValid(&Information))
    {
        DPRINT1("GpioClx: unusable geometry, %u pins in banks of %u\n",
                Information.TotalPins,
                Information.NumberOfPinsPerBank);
        return STATUS_GPIO_CLIENT_INFORMATION_INVALID;
    }

    Controller->Information = Information;

    /* The last bank may be short */
    PinsPerBank = Information.NumberOfPinsPerBank;
    Controller->BankCount = (USHORT)((Information.TotalPins + PinsPerBank - 1) / PinsPerBank);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Notes the line the controller itself was given, if any.
 */
static
VOID
NTAPI
GcxFindPrimaryInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw;
    ULONG Count;
    ULONG Index;

    Controller->PrimaryGsiv = GCX_NO_PRIMARY_GSIV;
    Controller->InterruptRaw = NULL;
    Controller->InterruptTranslated = NULL;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Translated = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Translated == NULL || Translated->Type != CmResourceTypeInterrupt)
            continue;

        /* Both lists index the same resources */
        Raw = WdfCmResourceListGetDescriptor(ResourcesRaw, Index);
        if (Raw == NULL)
            continue;

        /* An interrupt created outside EvtDeviceAdd needs both descriptors */
        Controller->InterruptRaw = Raw;
        Controller->InterruptTranslated = Translated;

        /* The raw vector of a line based interrupt is its GSIV */
        Controller->PrimaryGsiv = Raw->u.Interrupt.Vector;
        Controller->PrimaryIrql = (KIRQL)Translated->u.Interrupt.Level;
        Controller->PrimaryAffinity = Translated->u.Interrupt.Affinity;

        if (Translated->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
            Controller->PrimaryMode = Latched;
        else
            Controller->PrimaryMode = LevelSensitive;

        return;
    }
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxEvtDevicePrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated)
{
    PGCX_CONTROLLER Controller;
    NTSTATUS Status;

    Controller = GcxGetController(Device);
    InterlockedExchange(&Controller->State, GcxControllerPreparing);

    GcxFindPrimaryInterrupt(Controller, ResourcesRaw, ResourcesTranslated);

    /* Without a firmware name the pins still work, they just cannot be found by GSIV */
    Status = GcxQueryDeviceBiosName(Device, &Controller->BiosName);
    if (NT_SUCCESS(Status))
        GcxAddController(Controller);
    else
        DPRINT1("GpioClx: controller has no firmware name 0x%08lx\n", Status);

    /* On failure KMDF still calls EvtDeviceReleaseHardware, which undoes all of this */
    Status = Controller->Registration.CLIENT_PrepareController(Device,
                                                               GcxClientContext(Controller),
                                                               ResourcesRaw,
                                                               ResourcesTranslated);
    if (NT_SUCCESS(Status))
        Status = GcxQueryControllerInformation(Controller);

    if (NT_SUCCESS(Status))
        Status = GcxInitializeBanks(Controller);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: controller start failed 0x%08lx\n", Status);
        InterlockedExchange(&Controller->State, GcxControllerFailed);
        return Status;
    }

    /* Not fatal: pins still do I/O when they cannot be interrupt sources */
    Status = GcxRegisterSecondaryInterruptController(WdfDriverWdmGetDriverObject(WdfGetDriver()));
    if (!NT_SUCCESS(Status))
        DPRINT1("GpioClx: pins will not deliver interrupts 0x%08lx\n", Status);

    InterlockedExchange(&Controller->State, GcxControllerReady);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxEvtDeviceReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated)
{
    PGCX_CONTROLLER Controller;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    Controller = GcxGetController(Device);
    InterlockedExchange(&Controller->State, GcxControllerStopped);

    /* No GSIV may route to this controller once its registers are gone */
    GcxRemoveVirqMappings(Controller);
    GcxRemoveController(Controller);

    if (Controller->BiosName.Buffer != NULL)
    {
        ExFreePoolWithTag(Controller->BiosName.Buffer, GCX_POOL_TAG);
        RtlZeroMemory(&Controller->BiosName, sizeof(Controller->BiosName));
    }

    Controller->Registration.CLIENT_ReleaseController(Device, GcxClientContext(Controller));

    GcxFreeBanks(Controller);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Whether the bank registers have to be saved or restored around a Dx transition.
 */
static
BOOLEAN
NTAPI
GcxNeedsContextTransfer(
    _In_ PGCX_CONTROLLER Controller,
    _In_ WDF_POWER_DEVICE_STATE OtherState)
{
    if (OtherState == WdfPowerDeviceD0)
        return FALSE;

    return Controller->Information.Flags.BankIdlePowerMgmtSupported ? FALSE : TRUE;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxEvtDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState)
{
    PGCX_CONTROLLER Controller;

    Controller = GcxGetController(Device);
    return Controller->Registration.CLIENT_StartController(GcxClientContext(Controller),
                                                           GcxNeedsContextTransfer(Controller,
                                                                                   PreviousState),
                                                           PreviousState);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxEvtDeviceD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState)
{
    PGCX_CONTROLLER Controller;

    Controller = GcxGetController(Device);
    return Controller->Registration.CLIENT_StopController(GcxClientContext(Controller),
                                                          GcxNeedsContextTransfer(Controller,
                                                                                  TargetState),
                                                          TargetState);
}
