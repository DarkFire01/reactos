/*
 * PROJECT:     ReactOS Synopsys USB Role Switch Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Role switch client for DesignWare USB 3 dual role controllers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <wdf.h>
#include <ursdevice.h>
#include <ursstub.h>

#define NDEBUG
#include <debug.h>

#define URSS_POOL_TAG                   'SsrU'

/* The controller window has to reach the global registers */
#define URSS_MINIMUM_WINDOW             0xCC20
#define URSS_GLOBALS_OFFSET             0xC100

#define DWC3_GCTL                       0x10
#define DWC3_GCTL_CORESOFTRESET         0x00000800
#define DWC3_GCTL_PRTCAPDIR_SHIFT       12
#define DWC3_GCTL_PRTCAPDIR_MASK        0x00003000
#define DWC3_GCTL_PRTCAPDIR_HOST        1
#define DWC3_GCTL_PRTCAPDIR_DEVICE      2

typedef struct _URSS_ID_PIN
{
    WDFDEVICE Device;
    PKINTERRUPT Interrupt;

    /* Event this line reports, or none when one line toggles between both */
    URS_HARDWARE_EVENT Event;
    URS_HARDWARE_EVENT ToggleState;
} URSS_ID_PIN, *PURSS_ID_PIN;

typedef enum _URSS_ID_PIN_KIND
{
    UrssIdPinToggle,
    UrssIdPinGround = UrsHardwareEventIdGround,
    UrssIdPinFloat = UrsHardwareEventIdFloat
} URSS_ID_PIN_KIND;

typedef struct _URSS_DEVICE
{
    WDFDEVICE Device;
    URSS_ID_PIN Ground;
    URSS_ID_PIN Float;
    URSS_ID_PIN Toggle;
    KSPIN_LOCK InterruptLock;
    PUCHAR Registers;
    SIZE_T RegistersSize;
    PUCHAR Globals;
} URSS_DEVICE, *PURSS_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(URSS_DEVICE, UrssGetDevice)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD UrssEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD UrssEvtDriverUnload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE UrssEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE UrssEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY UrssEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT UrssEvtD0Exit;
static EVT_WDFDEVICE_WDM_POST_PO_FX_REGISTER_DEVICE UrssEvtPostPoFxRegister;
static EVT_WDFDEVICE_WDM_PRE_PO_FX_UNREGISTER_DEVICE UrssEvtPrePoFxUnregister;
static EVT_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS UrssEvtFilterRemoveResourceRequirements;
static EVT_URS_SET_ROLE UrssEvtSetRole;
static KSERVICE_ROUTINE UrssIdPinIsr;

static
BOOLEAN
NTAPI
UrssIdPinIsr(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID Context)
{
    PURSS_ID_PIN Pin = Context;
    URS_HARDWARE_EVENT Event = Pin->Event;

    UNREFERENCED_PARAMETER(Interrupt);

    /* A single line flips between ground and float on every edge */
    if (Event == UrsHardwareEventNone)
    {
        Pin->ToggleState = (Pin->ToggleState == UrsHardwareEventIdFloat) ? UrsHardwareEventIdGround
                                                                          : UrsHardwareEventIdFloat;
        Event = Pin->ToggleState;
    }

    UrsReportHardwareEvent(Pin->Device, Event);
    return TRUE;
}

static
NTSTATUS
UrssConnectIdPin(
    _In_ PURSS_DEVICE Device,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _In_ KIRQL SynchronizeIrql,
    _In_ URSS_ID_PIN_KIND Kind)
{
    IO_CONNECT_INTERRUPT_PARAMETERS Parameters;
    PURSS_ID_PIN Pin;

    switch (Kind)
    {
        case UrssIdPinGround:
            Pin = &Device->Ground;
            Pin->Event = UrsHardwareEventIdGround;
            break;

        case UrssIdPinFloat:
            Pin = &Device->Float;
            Pin->Event = UrsHardwareEventIdFloat;
            break;

        case UrssIdPinToggle:
            Pin = &Device->Toggle;
            Pin->Event = UrsHardwareEventNone;
            Pin->ToggleState = UrsHardwareEventIdFloat;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    Pin->Device = Device->Device;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = CONNECT_FULLY_SPECIFIED;
    Parameters.FullySpecified.PhysicalDeviceObject = WdfDeviceWdmGetPhysicalDevice(Device->Device);
    Parameters.FullySpecified.InterruptObject = &Pin->Interrupt;
    Parameters.FullySpecified.ServiceRoutine = UrssIdPinIsr;
    Parameters.FullySpecified.ServiceContext = Pin;
    Parameters.FullySpecified.SpinLock = &Device->InterruptLock;
    Parameters.FullySpecified.SynchronizeIrql = SynchronizeIrql;
    Parameters.FullySpecified.FloatingSave = FALSE;
    Parameters.FullySpecified.ShareVector = (Descriptor->ShareDisposition == CmResourceShareShared);
    Parameters.FullySpecified.Vector = Descriptor->u.Interrupt.Vector;
    Parameters.FullySpecified.Irql = (KIRQL)Descriptor->u.Interrupt.Level;
    Parameters.FullySpecified.InterruptMode = (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched
                                                                                                 : LevelSensitive;
    Parameters.FullySpecified.ProcessorEnableMask = Descriptor->u.Interrupt.Affinity;
    return IoConnectInterruptEx(&Parameters);
}

static
VOID
UrssDisconnectIdPin(
    _Inout_ PURSS_ID_PIN Pin)
{
    IO_DISCONNECT_INTERRUPT_PARAMETERS Parameters;

    if (!Pin->Interrupt)
        return;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = CONNECT_FULLY_SPECIFIED;
    Parameters.ConnectionContext.InterruptObject = Pin->Interrupt;
    IoDisconnectInterruptEx(&Parameters);
    RtlZeroMemory(Pin, sizeof(*Pin));
}

/**
 * @brief
 * Maps the controller and hooks up the ID pin lines when the firmware describes any.
 *
 * @remarks
 * Two lines report ground and float separately, a single line toggles between them.
 */
static
NTSTATUS
UrssEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PURSS_DEVICE Device = UrssGetDevice(WdfDevice);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor, Memory = NULL, First = NULL, Second = NULL;
    KIRQL SynchronizeIrql = 0;
    ULONG Count, i;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count && !(First && Second && Memory); i++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Descriptor->Type == CmResourceTypeInterrupt && !Second)
        {
            if (!First)
                First = Descriptor;
            else
                Second = Descriptor;

            SynchronizeIrql = max(SynchronizeIrql, (KIRQL)Descriptor->u.Interrupt.Level);
        }
        else if (Descriptor->Type == CmResourceTypeMemory && !Memory)
        {
            Memory = Descriptor;
        }
    }

    if (!Memory || Memory->u.Memory.Length < URSS_MINIMUM_WINDOW)
        return STATUS_INSUFFICIENT_RESOURCES;

    Device->Registers = MmMapIoSpace(Memory->u.Memory.Start, Memory->u.Memory.Length, MmNonCached);
    if (!Device->Registers)
        return STATUS_INSUFFICIENT_RESOURCES;

    Device->RegistersSize = Memory->u.Memory.Length;
    Device->Globals = Device->Registers + URSS_GLOBALS_OFFSET;

    UrsSetHardwareEventSupport(WdfDevice, First != NULL);
    if (First)
        UrsReportHardwareEvent(WdfDevice, UrsHardwareEventIdFloat);

    KeInitializeSpinLock(&Device->InterruptLock);

    if (First && !Second)
        return UrssConnectIdPin(Device, First, SynchronizeIrql, UrssIdPinToggle);

    if (First)
    {
        Status = UrssConnectIdPin(Device, First, SynchronizeIrql, UrssIdPinGround);
        if (NT_SUCCESS(Status))
            Status = UrssConnectIdPin(Device, Second, SynchronizeIrql, UrssIdPinFloat);
        return Status;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
UrssEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PURSS_DEVICE Device = UrssGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Device->Registers)
    {
        MmUnmapIoSpace(Device->Registers, Device->RegistersSize);
        Device->Registers = NULL;
        Device->RegistersSize = 0;
        Device->Globals = NULL;
    }

    UrssDisconnectIdPin(&Device->Ground);
    UrssDisconnectIdPin(&Device->Float);
    UrssDisconnectIdPin(&Device->Toggle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
UrssEvtD0Entry(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(WdfDevice);
    UNREFERENCED_PARAMETER(PreviousState);

    return STATUS_SUCCESS;
}

static
NTSTATUS
UrssEvtD0Exit(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(WdfDevice);
    UNREFERENCED_PARAMETER(TargetState);

    return STATUS_SUCCESS;
}

/* Both role children map the controller window the parent was given */
static
NTSTATUS
NTAPI
UrssEvtFilterRemoveResourceRequirements(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFIORESREQLIST IoResourceRequirementsList,
    _In_ URSIORESLIST HostRoleResources,
    _In_ URSIORESLIST FunctionRoleResources)
{
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    WDFIORESLIST List;
    ULONG Count, i;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(WdfDevice);

    if (!WdfIoResourceRequirementsListGetCount(IoResourceRequirementsList))
        return STATUS_SUCCESS;

    List = WdfIoResourceRequirementsListGetIoResList(IoResourceRequirementsList, 0);
    Count = WdfIoResourceListGetCount(List);
    for (i = 0; i < Count; i++)
    {
        Descriptor = WdfIoResourceListGetDescriptor(List, i);
        if (Descriptor->Type != CmResourceTypeMemory && Descriptor->Type != CmResourceTypeMemoryLarge)
            continue;

        Status = UrsIoResourceListAppendDescriptor(HostRoleResources, Descriptor);
        if (!NT_SUCCESS(Status))
            break;

        Status = UrsIoResourceListAppendDescriptor(FunctionRoleResources, Descriptor);
        if (!NT_SUCCESS(Status))
            break;
    }

    return Status;
}

/* The core is reset and its port capability set to the new role before the child starts */
static
NTSTATUS
NTAPI
UrssEvtSetRole(
    _In_ WDFDEVICE WdfDevice,
    _In_ URS_ROLE Role)
{
    PULONG Gctl = (PULONG)(UrssGetDevice(WdfDevice)->Globals + DWC3_GCTL);
    LARGE_INTEGER Interval;
    ULONG Value;

    Value = READ_REGISTER_ULONG(Gctl) | DWC3_GCTL_CORESOFTRESET;
    WRITE_REGISTER_ULONG(Gctl, Value);

    Interval.QuadPart = -10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    WRITE_REGISTER_ULONG(Gctl, Value & ~DWC3_GCTL_CORESOFTRESET);
    Value = READ_REGISTER_ULONG(Gctl);

    if (Role == UrsRoleHost)
        Value = (Value & ~DWC3_GCTL_PRTCAPDIR_MASK) | (DWC3_GCTL_PRTCAPDIR_HOST << DWC3_GCTL_PRTCAPDIR_SHIFT);
    else if (Role == UrsRoleFunction)
        Value = (Value & ~DWC3_GCTL_PRTCAPDIR_MASK) | (DWC3_GCTL_PRTCAPDIR_DEVICE << DWC3_GCTL_PRTCAPDIR_SHIFT);

    WRITE_REGISTER_ULONG(Gctl, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
UrssEvtPostPoFxRegister(
    _In_ WDFDEVICE WdfDevice,
    _In_ POHANDLE PoHandle)
{
    UrsSetPoHandle(WdfDevice, PoHandle);
    return STATUS_SUCCESS;
}

static
VOID
UrssEvtPrePoFxUnregister(
    _In_ WDFDEVICE WdfDevice,
    _In_ POHANDLE PoHandle)
{
    UNREFERENCED_PARAMETER(PoHandle);

    UrsSetPoHandle(WdfDevice, NULL);
}

static
NTSTATUS
UrssRegisterPowerFramework(
    _In_ WDFDEVICE WdfDevice)
{
    WDF_POWER_FRAMEWORK_SETTINGS PowerFramework;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS Idle;
    NTSTATUS Status;

    WDF_POWER_FRAMEWORK_SETTINGS_INIT(&PowerFramework);
    PowerFramework.EvtDeviceWdmPostPoFxRegisterDevice = UrssEvtPostPoFxRegister;
    PowerFramework.EvtDeviceWdmPrePoFxUnregisterDevice = UrssEvtPrePoFxUnregister;
    Status = WdfDeviceWdmAssignPowerFrameworkSettings(WdfDevice, &PowerFramework);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&Idle, IdleCannotWakeFromS0);
    Idle.DxState = PowerDeviceD3;
    Idle.IdleTimeout = 0;
    Idle.IdleTimeoutType = SystemManagedIdleTimeout;
    return WdfDeviceAssignS0IdleSettings(WdfDevice, &Idle);
}

static
NTSTATUS
UrssEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    URS_CONFIG Config;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = UrssEvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = UrssEvtReleaseHardware;
    PnpPower.EvtDeviceD0Entry = UrssEvtD0Entry;
    PnpPower.EvtDeviceD0Exit = UrssEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    Status = UrsDeviceInitInitialize(DeviceInit);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, URSS_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    UrssGetDevice(WdfDevice)->Device = WdfDevice;

    URS_CONFIG_INIT(&Config, UrsHostInterfaceTypeXhci, UrssEvtFilterRemoveResourceRequirements);
    Config.EvtUrsSetRole = UrssEvtSetRole;
    Status = UrsDeviceInitialize(WdfDevice, &Config);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("UrsDeviceInitialize failed 0x%lx\n", Status);
        return Status;
    }

    return UrssRegisterPowerFramework(WdfDevice);
}

static
VOID
UrssEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    UrsStubUnbindClass();
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, UrssEvtDeviceAdd);
    Config.EvtDriverUnload = UrssEvtDriverUnload;
    Config.DriverPoolTag = URSS_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsStubBindClass();
    if (!NT_SUCCESS(Status))
        DPRINT1("Cannot bind to the role switch class extension 0x%lx\n", Status);

    return Status;
}
