/*
 * PROJECT:     ReactOS Intel Cherryview GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller lifetime, bank context and the community operation region
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "iagpioe.h"

#define NDEBUG
#include <debug.h>

/* What a community reports when firmware gives no usable _UID */
#define CHV_UID_UNKNOWN 0xFF

/**
 * @brief
 * Evaluates _UID through the ACPI driver below us, it picks the community geometry.
 *
 * @return
 * STATUS_SUCCESS with CHV_UID_UNKNOWN when ACPI has no answer, STATUS_ACPI_INVALID_DATA
 * when the answer is not a single integer.
 */
static
NTSTATUS
ChvQueryUid(
    _In_ WDFDEVICE Device,
    _Out_ PULONG Uid)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    ULONG Buffer[0x80];
    WDF_MEMORY_DESCRIPTOR InputDescriptor;
    WDF_MEMORY_DESCRIPTOR OutputDescriptor;
    ULONG_PTR Returned = 0;
    NTSTATUS Status;

    PAGED_CODE();

    *Uid = CHV_UID_UNKNOWN;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    Input.MethodNameAsUlong = (ULONG)'DIU_';

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Output = (PACPI_EVAL_OUTPUT_BUFFER)Buffer;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, &Input, sizeof(Input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Buffer, sizeof(Buffer));

    Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device),
                                               NULL,
                                               IOCTL_ACPI_EVAL_METHOD,
                                               &InputDescriptor,
                                               &OutputDescriptor,
                                               NULL,
                                               &Returned);
    if (!NT_SUCCESS(Status) || !Returned)
    {
        DPRINT1("Cherryview GPIO: _UID evaluation failed 0x%08lx\n", Status);
        return STATUS_SUCCESS;
    }

    if ((Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) ||
        (Output->Count != 1) ||
        (Output->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER))
    {
        return STATUS_ACPI_INVALID_DATA;
    }

    *Uid = Output->Argument[0].Argument;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Serves the per community OEM region firmware uses to reach pad registers directly.
 */
static
NTSTATUS
__cdecl
ChvOpRegionHandler(
    _In_ ULONG AccessType,
    _In_ PVOID OperationRegionObject,
    _In_ ULONG Address,
    _In_ ULONG Size,
    _Inout_ PULONG Data,
    _In_ ULONG_PTR Context,
    _In_opt_ PACPI_OP_REGION_CALLBACK CompletionHandler,
    _In_opt_ PVOID CompletionContext)
{
    PCHV_CONTROLLER Controller = (PCHV_CONTROLLER)Context;

    UNREFERENCED_PARAMETER(OperationRegionObject);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(CompletionHandler);
    UNREFERENCED_PARAMETER(CompletionContext);

    /* Every access is a whole register */
    if (!Controller->Base || (Address > Controller->Length - sizeof(ULONG)))
        return STATUS_INVALID_PARAMETER;

    switch (AccessType)
    {
        case ACPI_OPREGION_READ:
            *Data = ChvReadRegister(Controller, Address);
            return STATUS_SUCCESS;

        case ACPI_OPREGION_WRITE:
            ChvWriteRegister(Controller, Address, *Data);
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvPrepareController(
    WDFDEVICE Device,
    PVOID Context,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated)
{
    PCHV_CONTROLLER Controller = Context;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    const CHV_COMMUNITY *Community;
    BANK_ID Bank;
    PIN_NUMBER Pin;
    ULONG Count, Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    if (!Controller)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Controller, sizeof(*Controller));
    Controller->Device = Device;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (!Descriptor)
            return STATUS_DEVICE_CONFIGURATION_ERROR;

        /* One window per community, the interrupt belongs to GpioClx */
        if ((Descriptor->Type != CmResourceTypeMemory) || Controller->Base)
            continue;

        Controller->Length = Descriptor->u.Memory.Length;
        if (Controller->Length < sizeof(ULONG))
            return STATUS_UNSUCCESSFUL;

        Controller->Base = MmMapIoSpace(Descriptor->u.Memory.Start, Controller->Length, MmNonCached);
        if (!Controller->Base)
        {
            Controller->Length = 0;
            return STATUS_UNSUCCESSFUL;
        }
    }

    if (!Controller->Base)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Status = ChvQueryUid(Device, &Controller->Uid);
    if (!NT_SUCCESS(Status))
    {
        ChvReleaseController(Device, Controller);
        return Status;
    }

    /* An unknown community stays up with no pads, firmware may still use its region */
    Community = ChvFindCommunity(Controller->Uid);
    if (Community)
    {
        Controller->Community = Community;
        Controller->BankCount = (USHORT)((Community->PadCount + Community->PinsPerBank - 1) /
                                         Community->PinsPerBank);

        /* Nothing left over from firmware may fire once a line is unmasked */
        for (Bank = 0; Bank < Controller->BankCount; Bank++)
        {
            for (Pin = 0; Pin < Community->PinsPerBank; Pin++)
                ChvAckLine(Controller, Bank, Pin);
        }
    }
    else
    {
        DPRINT1("Cherryview GPIO: no pad layout for community %lu\n", Controller->Uid);
    }

    Status = RegisterOpRegionHandler(WdfDeviceWdmGetPhysicalDevice(Device),
                                     ACPI_OPREGION_ACCESS_AS_COOKED,
                                     CHV_REGION_SPACE_BASE + Controller->Uid,
                                     ChvOpRegionHandler,
                                     Controller,
                                     0,
                                     &Controller->OpRegion);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cherryview GPIO: region space 0x%lx not registered 0x%08lx\n",
                CHV_REGION_SPACE_BASE + Controller->Uid, Status);
        Controller->OpRegion = NULL;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvReleaseController(
    WDFDEVICE Device,
    PVOID Context)
{
    PCHV_CONTROLLER Controller = Context;

    if (!Controller)
        return STATUS_INVALID_PARAMETER;

    if (Controller->OpRegion)
    {
        DeRegisterOpRegionHandler(WdfDeviceWdmGetPhysicalDevice(Device), Controller->OpRegion);
        Controller->OpRegion = NULL;
    }

    if (Controller->Base)
    {
        MmUnmapIoSpace(Controller->Base, Controller->Length);
        Controller->Base = NULL;
        Controller->Length = 0;
    }

    Controller->Community = NULL;
    Controller->BankCount = 0;
    return STATUS_SUCCESS;
}

static
VOID
ChvSaveBank(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    if (BankId >= Controller->BankCount)
        return;

    /* Pads are replayed from what we wrote, only the shared mask is captured */
    Controller->SavedMask = ChvReadRegister(Controller, CHV_INTERRUPT_MASK);
    Controller->MaskSaved = TRUE;
    Controller->BankSaved[BankId] = TRUE;
}

static
VOID
ChvRestoreBank(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    PCHV_PAD_SHADOW Shadow;
    PIN_NUMBER Pin;
    ULONG Offset;

    if (BankId >= Controller->BankCount)
        return;

    if (Controller->BankSaved[BankId])
    {
        for (Pin = 0; Pin < Controller->Community->PinsPerBank; Pin++)
        {
            if (!ChvIsPinValid(Controller, BankId, Pin))
                continue;

            Offset = ChvPadOffset(Controller, BankId, Pin);
            if (ChvReadRegister(Controller, Offset) & CHV_CTRL0_KEEP_ON_RESTORE)
                continue;

            /* A zero shadow means the pad was never touched */
            Shadow = &Controller->Shadow[BankId][Pin];
            if (Shadow->Ctrl0)
                ChvWriteRegister(Controller, Offset, Shadow->Ctrl0);
            if (Shadow->Ctrl1)
                ChvWriteRegister(Controller, Offset + sizeof(ULONG), Shadow->Ctrl1);
        }
    }

    Controller->BankSaved[BankId] = FALSE;

    if (Controller->MaskSaved)
        ChvWriteRegister(Controller, CHV_INTERRUPT_MASK, Controller->SavedMask);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvStartController(
    PVOID Context,
    BOOLEAN RestoreContext,
    WDF_POWER_DEVICE_STATE PreviousPowerState)
{
    PCHV_CONTROLLER Controller = Context;
    BANK_ID Bank;

    UNREFERENCED_PARAMETER(PreviousPowerState);

    if (!Controller)
        return STATUS_INVALID_PARAMETER;

    if (RestoreContext)
    {
        for (Bank = 0; Bank < Controller->BankCount; Bank++)
            ChvRestoreBank(Controller, Bank);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvStopController(
    PVOID Context,
    BOOLEAN SaveContext,
    WDF_POWER_DEVICE_STATE TargetState)
{
    PCHV_CONTROLLER Controller = Context;
    BANK_ID Bank;

    UNREFERENCED_PARAMETER(TargetState);

    if (!Controller)
        return STATUS_INVALID_PARAMETER;

    if (SaveContext)
    {
        for (Bank = 0; Bank < Controller->BankCount; Bank++)
            ChvSaveBank(Controller, Bank);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
ChvQueryControllerBasicInformation(
    PVOID Context,
    PCLIENT_CONTROLLER_BASIC_INFORMATION Information)
{
    PCHV_CONTROLLER Controller = Context;

    if (!Controller || !Information)
        return STATUS_INVALID_PARAMETER;

    Information->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Information->Size = sizeof(*Information);
    Information->TotalPins = Controller->Community ? Controller->Community->PadCount : 0;
    Information->NumberOfPinsPerBank = Controller->Community ? Controller->Community->PinsPerBank : 0;

    /* GpioClx covers debounce and both edge triggers, the pads have neither */
    Information->Flags.MemoryMappedController = 1;
    Information->Flags.ActiveInterruptsAutoClearOnRead = 0;
    Information->Flags.FormatIoRequestsAsMasks = 0;
    Information->Flags.DeviceIdlePowerMgmtSupported = 0;
    Information->Flags.BankIdlePowerMgmtSupported = 0;
    Information->Flags.EmulateDebouncing = 1;
    Information->Flags.EmulateActiveBoth = 1;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
ChvSaveBankHardwareContext(
    PVOID Context,
    PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters)
{
    if (Context && Parameters)
        ChvSaveBank(Context, Parameters->BankId);
}

_Use_decl_annotations_
VOID
NTAPI
ChvRestoreBankHardwareContext(
    PVOID Context,
    PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters)
{
    if (Context && Parameters)
        ChvRestoreBank(Context, Parameters->BankId);
}
