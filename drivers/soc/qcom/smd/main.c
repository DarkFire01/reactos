/*
 * PROJECT:     ReactOS Qualcomm SMD driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device setup, resources, interrupts and exported interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "smd.h"

#define NDEBUG
#include <debug.h>

#define SMD_INTR_METHOD             'RTNI'

PSMD_DEVICE SmdGlobal;

static QCOM_SMDL_INTERFACE SmdlInterface;
static QCOM_SMEM_INTERFACE SmemInterface;

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD SmdEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE SmdEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE SmdEvtReleaseHardware;
static EVT_WDF_INTERRUPT_ISR SmdEvtInterruptIsr;
static EVT_WDF_INTERRUPT_DPC SmdEvtInterruptDpc;

static
BOOLEAN
SmdEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);

    /* Remote processors only ring a doorbell, all protocol work happens in the DPC */
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static
VOID
SmdEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PSMD_IRQ_CONTEXT Context = SmdGetIrqContext(Interrupt);

    UNREFERENCED_PARAMETER(AssociatedObject);

    DPRINT1("[smd] doorbell kind %lu host %lu\n", Context->Kind, Context->Host);

    if (Context->Kind == SMD_IRQ_SMD)
        SmdProcessHost(Context->Device, Context->Host);
}

/* Evaluates INTR, which lists every doorbell as {host, register, bit} grouped by protocol */
static
NTSTATUS
SmdReadInterruptMap(
    _Inout_ PSMD_DEVICE Device)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output = NULL;
    WDF_MEMORY_DESCRIPTOR InDesc, OutDesc;
    PACPI_METHOD_ARGUMENT Arg;
    ULONG OutSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 32 * sizeof(ACPI_METHOD_ARGUMENT);
    ULONG Values[64];
    ULONG Count = 0, Groups[3], Index, Kind, i;
    ULONG_PTR Returned;
    NTSTATUS Status;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    Input.MethodNameAsUlong = SMD_INTR_METHOD;

    for (;;)
    {
        Output = ExAllocatePoolZero(NonPagedPool, OutSize, SMD_TAG);
        if (!Output)
            return STATUS_INSUFFICIENT_RESOURCES;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDesc, &Input, sizeof(Input));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutDesc, Output, OutSize);

        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device->WdfDevice),
                                                   NULL,
                                                   IOCTL_ACPI_EVAL_METHOD,
                                                   &InDesc,
                                                   &OutDesc,
                                                   NULL,
                                                   &Returned);
        if (Status != STATUS_BUFFER_OVERFLOW || Output->Length <= OutSize)
            break;

        OutSize = Output->Length;
        ExFreePoolWithTag(Output, SMD_TAG);
        Output = NULL;
    }

    if (!NT_SUCCESS(Status))
        goto Exit;

    if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Output->Count > RTL_NUMBER_OF(Values))
    {
        Status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    Arg = Output->Argument;
    for (i = 0; i < Output->Count; i++)
    {
        if (Arg->Type != ACPI_METHOD_ARGUMENT_INTEGER)
        {
            Status = STATUS_ACPI_INVALID_DATA;
            goto Exit;
        }
        Values[Count++] = Arg->Argument;
        Arg = ACPI_METHOD_NEXT_ARGUMENT(Arg);
    }

    if (Count < 2)
    {
        Status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    /* v1 lists SMD doorbells only, v3 adds the SMP2P and SMSM groups */
    if (Values[0] == 1)
    {
        Groups[0] = Values[1];
        Groups[1] = Groups[2] = 0;
        Index = 2;
    }
    else if (Values[0] == 3 && Count >= 4)
    {
        Groups[0] = Values[1];
        Groups[1] = Values[2];
        Groups[2] = Values[3];
        Index = 4;
    }
    else
    {
        Status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    if (Index + 4 * (Groups[0] + Groups[1] + Groups[2]) != Count ||
        Groups[0] + Groups[1] + Groups[2] > RTL_NUMBER_OF(Device->Irqs))
    {
        Status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    for (Kind = 0; Kind < 3; Kind++)
    {
        for (i = 0; i < Groups[Kind]; i++, Index += 4)
        {
            if (Values[Index] >= QCOM_HOST_COUNT)
            {
                Status = STATUS_ACPI_INVALID_DATA;
                goto Exit;
            }

            Device->Irqs[Device->IrqCount].Kind = Kind;
            Device->Irqs[Device->IrqCount].Host = Values[Index];
            Device->Irqs[Device->IrqCount].RegisterPhys = Values[Index + 1];
            Device->Irqs[Device->IrqCount].Mask = Values[Index + 2];
            Device->IrqCount++;
        }
    }

    Status = STATUS_SUCCESS;

Exit:
    if (Output)
        ExFreePoolWithTag(Output, SMD_TAG);
    return Status;
}

/* One WDF interrupt per INTR entry, in order, so they pair with the _CRS interrupts */
static
NTSTATUS
SmdCreateInterrupts(
    _Inout_ PSMD_DEVICE Device)
{
    WDF_INTERRUPT_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PSMD_IRQ_CONTEXT Context;
    WDFINTERRUPT Interrupt;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < Device->IrqCount; i++)
    {
        WDF_INTERRUPT_CONFIG_INIT(&Config, SmdEvtInterruptIsr, SmdEvtInterruptDpc);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SMD_IRQ_CONTEXT);

        Status = WdfInterruptCreate(Device->WdfDevice, &Config, &Attributes, &Interrupt);
        if (!NT_SUCCESS(Status))
            return Status;

        Context = SmdGetIrqContext(Interrupt);
        Context->Device = Device;
        Context->Kind = Device->Irqs[i].Kind;
        Context->Host = Device->Irqs[i].Host;
    }

    return STATUS_SUCCESS;
}

static
PULONG
SmdIpcRegisterVa(
    _In_ PSMD_DEVICE Device,
    _In_ ULONGLONG Phys)
{
    ULONG i;

    for (i = 0; i < Device->IpcRegCount; i++)
    {
        if ((ULONGLONG)Device->IpcRegPhys[i].QuadPart == Phys)
            return Device->IpcRegVa[i];
    }

    return NULL;
}

static
VOID
SmdBindDoorbells(
    _Inout_ PSMD_DEVICE Device)
{
    PSMD_IRQ_DESC Irq;
    PSMD_HOST Host;
    PULONG Register;
    ULONG i;

    for (i = 0; i < Device->IrqCount; i++)
    {
        Irq = &Device->Irqs[i];
        Host = &Device->Hosts[Irq->Host];
        Register = SmdIpcRegisterVa(Device, Irq->RegisterPhys);
        if (!Register)
        {
            DPRINT1("Doorbell %I64x for host %lu is not a mapped resource\n", Irq->RegisterPhys, Irq->Host);
            continue;
        }

        switch (Irq->Kind)
        {
            case SMD_IRQ_SMD:
                Host->SmdIrqRegister = Register;
                Host->SmdIrqMask = Irq->Mask;
                break;
            case SMD_IRQ_SMP2P:
                Host->Smp2pIrqRegister = Register;
                Host->Smp2pIrqMask = Irq->Mask;
                break;
            case SMD_IRQ_SMSM:
                Host->SmsmIrqRegister = Register;
                Host->SmsmIrqMask = Irq->Mask;
                break;
        }
    }

    /* The RPM has no interrupt towards us but is reached through the modem's doorbell register */
    if (Device->Hosts[QCOM_HOST_MODEM].SmdIrqRegister && !Device->Hosts[QCOM_HOST_RPM].SmdIrqRegister)
    {
        Device->Hosts[QCOM_HOST_RPM].SmdIrqRegister = Device->Hosts[QCOM_HOST_MODEM].SmdIrqRegister;
        Device->Hosts[QCOM_HOST_RPM].SmdIrqMask = 0x1;
    }
}

static
VOID
SmdUnmapAll(
    _Inout_ PSMD_DEVICE Device)
{
    ULONG i;

    if (Device->SmemBase)
        MmUnmapIoSpace(Device->SmemBase, Device->SmemSize);
    if (Device->AuxBase)
        MmUnmapIoSpace(Device->AuxBase, Device->AuxSize);
    if (Device->MutexBase)
        MmUnmapIoSpace(Device->MutexBase, Device->MutexStride * 32);

    for (i = 0; i < Device->IpcRegCount; i++)
        MmUnmapIoSpace(Device->IpcRegVa[i], sizeof(ULONG));

    Device->SmemBase = NULL;
    Device->AuxBase = NULL;
    Device->MutexBase = NULL;
    Device->IpcRegCount = 0;
    Device->SmemReady = FALSE;
}

/* Reads the {"SIII", size, base} block that the boot loader leaves in IMEM */
static
VOID
SmdApplyTargetInfo(
    _Inout_ PSMD_DEVICE Device,
    _In_ PHYSICAL_ADDRESS Phys)
{
    struct
    {
        ULONG Magic;
        ULONG Size;
        ULONGLONG Base;
    } *Info;
    PHYSICAL_ADDRESS Base;
    ULONG Size;

    Info = MmMapIoSpace(Phys, sizeof(*Info), MmNonCached);
    if (!Info)
        return;

    if (Info->Magic == 0x49494953 && Info->Size)
    {
        Base.QuadPart = Info->Base;
        Size = Info->Size;
        MmUnmapIoSpace(Info, sizeof(*Info));

        if (Device->SmemBase)
            MmUnmapIoSpace(Device->SmemBase, Device->SmemSize);

        Device->SmemBase = MmMapIoSpace(Base, Size, MmWriteCombined);
        Device->SmemSize = Device->SmemBase ? Size : 0;
        Device->SmemPhys = Base;
        return;
    }

    MmUnmapIoSpace(Info, sizeof(*Info));
}

static
NTSTATUS
SmdEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST Resources,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PSMD_DEVICE Device = SmdGetDevice(WdfDevice);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    ULONG Count, i, MemoryIndex = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Resources);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count; i++)
    {
        Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!Desc || Desc->Type != CmResourceTypeMemory)
            continue;

        switch (MemoryIndex++)
        {
            case SMD_RES_SMEM:
                Device->SmemPhys = Desc->u.Memory.Start;
                Device->SmemSize = Desc->u.Memory.Length;
                Device->SmemBase = MmMapIoSpace(Desc->u.Memory.Start, Desc->u.Memory.Length, MmWriteCombined);
                break;

            case SMD_RES_MUTEX:
                Device->MutexStride = Desc->u.Memory.Length >> 5;
                if (Device->MutexStride == 0 || Desc->u.Memory.Length / Device->MutexStride < SMEM_MUTEX_COUNT)
                {
                    Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    goto Fail;
                }
                Device->MutexBase = MmMapIoSpace(Desc->u.Memory.Start, Desc->u.Memory.Length, MmNonCached);
                break;

            case SMD_RES_SMEM_TARGET:
                SmdApplyTargetInfo(Device, Desc->u.Memory.Start);
                break;

            case SMD_RES_AUX:
                Device->AuxSize = Desc->u.Memory.Length;
                Device->AuxBase = MmMapIoSpace(Desc->u.Memory.Start, Desc->u.Memory.Length, MmWriteCombined);
                break;

            default:
                if (Device->IpcRegCount >= SMD_MAX_IPC_REGS || Desc->u.Memory.Length != sizeof(ULONG))
                    break;
                Device->IpcRegPhys[Device->IpcRegCount] = Desc->u.Memory.Start;
                Device->IpcRegVa[Device->IpcRegCount] = MmMapIoSpace(Desc->u.Memory.Start, sizeof(ULONG), MmNonCached);
                if (Device->IpcRegVa[Device->IpcRegCount])
                    Device->IpcRegCount++;
                break;
        }
    }

    DPRINT1("[smd] SMEM phys 0x%I64x size 0x%lx va %p, mutex %p stride 0x%lx, aux %p, %lu IPC regs\n",
            Device->SmemPhys.QuadPart, Device->SmemSize, Device->SmemBase,
            Device->MutexBase, Device->MutexStride, Device->AuxBase, Device->IpcRegCount);

    if (!Device->SmemBase || !Device->MutexBase)
    {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Fail;
    }

    SmdBindDoorbells(Device);

    Status = SmemInitialize(Device);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = SmdInitialize(Device);
    if (!NT_SUCCESS(Status))
        goto Fail;

    DPRINT1("[smd] hardware ready\n");
    return STATUS_SUCCESS;

Fail:
    DPRINT1("[smd] hardware setup FAILED 0x%lx\n", Status);
    SmdUnmapAll(Device);
    return Status;
}

static
NTSTATUS
SmdEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    SmdUnmapAll(SmdGetDevice(WdfDevice));
    return STATUS_SUCCESS;
}

static
NTSTATUS
SmdAddQueryInterface(
    _In_ WDFDEVICE WdfDevice,
    _In_ PINTERFACE Interface,
    _In_ const GUID *Guid)
{
    WDF_QUERY_INTERFACE_CONFIG Config;

    WDF_QUERY_INTERFACE_CONFIG_INIT(&Config, Interface, Guid, NULL);
    return WdfDeviceAddQueryInterface(WdfDevice, &Config);
}

static
NTSTATUS
SmdEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\SMD");
    PSMD_DEVICE Device;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(Driver);

    /* The function tables carry no context, so only one instance can serve them */
    if (SmdGlobal)
        return STATUS_DEVICE_ALREADY_ATTACHED;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = SmdEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = SmdEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    /* Clients open the device by name and then query its interfaces */
    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SMD_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = SmdGetDevice(WdfDevice);
    Device->WdfDevice = WdfDevice;
    SmdRLockInitialize(&Device->MutexLock);
    for (i = 0; i < QCOM_HOST_COUNT; i++)
    {
        SmdRLockInitialize(&Device->Hosts[i].Lock);
        InitializeListHead(&Device->Hosts[i].Ports);
    }

    Status = SmdReadInterruptMap(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("INTR evaluation failed 0x%lx\n", Status);
        return Status;
    }

    Status = SmdCreateInterrupts(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    SmdlInterface = SmdlInterfaceTemplate;
    SmemInterface.Header.Size = sizeof(SmemInterface);
    SmemInterface.Header.Version = 0;
    SmemInterface.Alloc = SmemIfAlloc;
    SmemInterface.VersionSet = SmemIfVersionSet;
    SmemInterface.GetAddr = SmemIfGetAddr;
    SmemInterface.GetIpcRegister = SmemIfGetIpcRegister;

    Status = SmdAddQueryInterface(WdfDevice, &SmemInterface.Header, &GUID_QCOM_SMEM_INTERFACE);
    if (NT_SUCCESS(Status))
        Status = SmdAddQueryInterface(WdfDevice, &SmdlInterface.Header, &GUID_QCOM_SMDL_INTERFACE);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Clients wait for this interface to arrive before querying the function table */
    Status = WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_QCOM_SMDL_INTERFACE, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    SmdGlobal = Device;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, SmdEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
