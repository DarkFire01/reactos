/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device setup, ACPI, image paths and the statistics interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"
#include <ntstrsafe.h>

#define NDEBUG
#include <debug.h>

#define SS_METHOD_SHUT              'TUHS'
#define SS_METHOD_SSRE              'ERSS'

#define SS_CHIP_ID_PHYSICAL         0xFD514028
#define SS_PART_8992                0x969
#define SS_PART_8994                0x940

#define SS_IOCTL_RELEASE_WAKE_A     0x9CA30800
#define SS_IOCTL_RELEASE_WAKE_B     0x9CA32800
#define SS_IOCTL_RELOAD_A           0x9CA30C00
#define SS_IOCTL_RELOAD_B           0x9CA32C00
#define SS_IOCTL_FIRMWARE_ID_A      0x9CA30C06
#define SS_IOCTL_FIRMWARE_ID_B      0x9CA32C06

#define SS_RELOAD_INPUT_SIZE        1042
#define SS_RELOAD_SW_OFFSET         0
#define SS_RELOAD_FW_OFFSET         520
#define SS_RELOAD_FLAG_OFFSET       1040

LIST_ENTRY SsDeviceList;
WDFWAITLOCK SsDeviceListLock;

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD SsEvtDeviceAdd;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SsEvtSelfManagedIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SsEvtSelfManagedIoCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SsEvtIoDeviceControl;
static EVT_WDF_INTERRUPT_ISR SsEvtWatchdogIsr;
static EVT_WDF_INTERRUPT_DPC SsEvtWatchdogDpc;
static EVT_WDF_TIMER SsEvtTimer;

NTSTATUS
SsEvaluateMethod(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Method,
    _Out_ PACPI_EVAL_OUTPUT_BUFFER *Output)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Buffer;
    WDF_MEMORY_DESCRIPTOR InDesc, OutDesc;
    ULONG Size = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 64 * sizeof(ACPI_METHOD_ARGUMENT);
    ULONG_PTR Returned;
    NTSTATUS Status;

    *Output = NULL;
    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    Input.MethodNameAsUlong = Method;

    for (;;)
    {
        Buffer = ExAllocatePoolZero(NonPagedPool, Size, SS_TAG);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDesc, &Input, sizeof(Input));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutDesc, Buffer, Size);

        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device->WdfDevice),
                                                   NULL,
                                                   IOCTL_ACPI_EVAL_METHOD,
                                                   &InDesc,
                                                   &OutDesc,
                                                   NULL,
                                                   &Returned);
        if (Status != STATUS_BUFFER_OVERFLOW || Buffer->Length <= Size)
            break;

        Size = Buffer->Length;
        ExFreePoolWithTag(Buffer, SS_TAG);
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, SS_TAG);
        return Status;
    }

    if (Buffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE)
    {
        ExFreePoolWithTag(Buffer, SS_TAG);
        return STATUS_ACPI_INVALID_DATA;
    }

    *Output = Buffer;
    return STATUS_SUCCESS;
}

/* First integer of a method result, or a default when it does not evaluate */
static
ULONG
SsEvaluateInteger(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Method,
    _In_ ULONG Default)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    ULONG Value = Default;

    if (!NT_SUCCESS(SsEvaluateMethod(Device, Method, &Output)))
        return Default;

    if (Output->Count >= 1 && Output->Argument->Type == ACPI_METHOD_ARGUMENT_INTEGER)
        Value = Output->Argument->Argument;

    ExFreePoolWithTag(Output, SS_TAG);
    return Value;
}

static
VOID
SsDetectChip(
    _Inout_ PSS_DEVICE Device)
{
    PHYSICAL_ADDRESS Physical;
    PULONG Register;
    ULONG Part;

    Device->ChipFamily = SS_FAMILY_UNKNOWN;

    Physical.QuadPart = SS_CHIP_ID_PHYSICAL;
    Register = MmMapIoSpace(Physical, sizeof(ULONG), MmNonCached);
    if (!Register)
        return;

    Part = (*Register >> 12) & 0xFFFF;
    DPRINT1("[subsys] chip id reg 0x%08lx, part 0x%lx\n", *Register, Part);
    MmUnmapIoSpace(Register, sizeof(ULONG));

    if (Part == SS_PART_8992)
        Device->ChipFamily = SS_FAMILY_8992;
    else if (Part == SS_PART_8994)
        Device->ChipFamily = SS_FAMILY_8994;

    DPRINT1("[subsys] chip family 0x%lx\n", Device->ChipFamily);
}

static
VOID
SsReadString(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _Out_writes_(Count) PWCHAR Buffer,
    _In_ ULONG Count)
{
    UNICODE_STRING ValueName;
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Value;
    WDFSTRING String;

    RtlInitUnicodeString(&ValueName, Name);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    if (!NT_SUCCESS(WdfStringCreate(NULL, &Attributes, &String)))
        return;

    if (NT_SUCCESS(WdfRegistryQueryString(Key, &ValueName, String)))
    {
        WdfStringGetUnicodeString(String, &Value);
        RtlStringCchCopyNW(Buffer, Count, Value.Buffer, Value.Length / sizeof(WCHAR));
    }

    WdfObjectDelete(String);
}

/* ACPI gates, then the service and device registry values */
static
VOID
SsLoadParameters(
    _Inout_ PSS_DEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFKEY Key;
    ULONG Value;

    Device->GracefulShutdown = SsEvaluateInteger(Device, SS_METHOD_SHUT, 0) != 0;
    Device->RestartEnabled = SsEvaluateInteger(Device, SS_METHOD_SSRE, 0) != 0;
    Device->ShutdownInProgress = FALSE;
    Device->MaxRestartCount = 30;
    Device->RestartTimerMinutes = 300;
    Device->LoadTimerSeconds = 30;
    Device->RmbTimeoutMs = 1000;
    Device->CrashDetection = FALSE;

    RtlStringCchCopyW(Device->FwImagePath, QCOM_PIL_PATH_LENGTH, L"\\SystemRoot\\system32\\qcdsp1v28992.mbn");
    RtlStringCchCopyW(Device->SwImagePath, QCOM_PIL_PATH_LENGTH, L"\\SystemRoot\\system32\\qcdsp28992.mbn");

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, &Attributes, &Key)))
    {
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"AmssRestartEnabled"), &Value)))
        {
            if (Value == 0 || Value == 1)
                Device->RestartEnabled = Value != 0;
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"MaxRestartCount"), &Value)))
            Device->MaxRestartCount = Value;
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"RestartTimer"), &Value)))
            Device->RestartTimerMinutes = Value;
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"LoadTimer"), &Value)))
            Device->LoadTimerSeconds = Value;
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"ModemRestartCrashDetection"), &Value)))
            Device->CrashDetection = Value == 1;
        WdfRegistryClose(Key);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device->WdfDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &Attributes, &Key)))
    {
        SsReadString(Key, L"AmssFwImagePath", Device->FwImagePath, QCOM_PIL_PATH_LENGTH);
        SsReadString(Key, L"AmssSwImagePath", Device->SwImagePath, QCOM_PIL_PATH_LENGTH);
        WdfRegistryClose(Key);
    }

    DPRINT1("[subsys] params: SHUT=%u SSRE/restart=%u max=%lu window=%lu min load=%lu s crashdet=%u\n",
            Device->GracefulShutdown, Device->RestartEnabled, Device->MaxRestartCount,
            Device->RestartTimerMinutes, Device->LoadTimerSeconds, Device->CrashDetection);
    DPRINT1("[subsys] fw image '%ls'\n", Device->FwImagePath);
    DPRINT1("[subsys] sw image '%ls'\n", Device->SwImagePath);
}

static
NTSTATUS
SsOpenByName(
    _In_ PSS_DEVICE Device,
    _In_ PCWSTR Name,
    _Out_ WDFIOTARGET *Target)
{
    WDF_IO_TARGET_OPEN_PARAMS Params;
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING TargetName;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device->WdfDevice;
    Status = WdfIoTargetCreate(Device->WdfDevice, &Attributes, Target);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&TargetName, Name);
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Params, &TargetName, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Params);
    DPRINT1("[subsys] open target '%ls' -> 0x%lx\n", Name, Status);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

/* Opens the dependencies and kicks off the boot */
static
NTSTATUS
SsEvtSelfManagedIoInit(
    _In_ WDFDEVICE WdfDevice)
{
    PSS_DEVICE Device = SsGetDevice(WdfDevice);
    NTSTATUS Status;

    DPRINT1("[subsys] SelfManagedIoInit\n");
    Status = SsOpenByName(Device, QCOM_PIL_DEVICE_NAME, &Device->Pil);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PIL target open failed 0x%lx\n", Status);
        return Status;
    }

    Status = SsOpenByName(Device, L"\\Device\\SMD", &Device->Smd);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SMD target open failed 0x%lx\n", Status);
        return Status;
    }

    Device->Smp2p.Header.Size = sizeof(Device->Smp2p);
    Status = WdfIoTargetQueryForInterface(Device->Smd,
                                          &GUID_QCOM_SMP2P_INTERFACE,
                                          &Device->Smp2p.Header,
                                          sizeof(Device->Smp2p),
                                          1,
                                          NULL);
    if (NT_SUCCESS(Status))
    {
        Device->Smem.Header.Size = sizeof(Device->Smem);
        Status = WdfIoTargetQueryForInterface(Device->Smd,
                                              &GUID_QCOM_SMEM_INTERFACE,
                                              &Device->Smem.Header,
                                              sizeof(Device->Smem),
                                              1,
                                              NULL);
    }
    if (NT_SUCCESS(Status))
    {
        Device->Smdl.Header.Size = sizeof(Device->Smdl);
        Status = WdfIoTargetQueryForInterface(Device->Smd,
                                              &GUID_QCOM_SMDL_INTERFACE,
                                              &Device->Smdl.Header,
                                              sizeof(Device->Smdl),
                                              0,
                                              NULL);
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[subsys] SMD interface query FAILED 0x%lx (SMP2P %p SMEM %p SMDL %p)\n",
                Status, Device->Smp2p.Create, Device->Smem.GetAddr, Device->Smdl.Open);
        return Status;
    }
    DPRINT1("[subsys] SMD interfaces: SMP2P, SMEM, SMDL ok\n");

    /* TrustZone and the system monitor are not needed to boot the modem on this chip */
    SsSysmonOpen(Device);

    Status = SsMapRegisters(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[subsys] MSS register map FAILED 0x%lx\n", Status);
        return Status;
    }

    DPRINT1("[subsys] starting modem boot\n");
    return SsPost(Device, SS_EV_START, 0);
}

static
VOID
SsEvtSelfManagedIoCleanup(
    _In_ WDFDEVICE WdfDevice)
{
    PSS_DEVICE Device = SsGetDevice(WdfDevice);

    SsStopMonitor(Device);
    SsSysmonClose(Device);
    SsUnmapRegisters(Device);
    SsFreeChildren(Device);

    if (Device->Failure)
        ExFreePoolWithTag(Device->Failure, SS_TAG);
    if (Device->VersionPage)
        ExFreePoolWithTag(Device->VersionPage, SS_TAG);

    WdfWaitLockAcquire(SsDeviceListLock, NULL);
    RemoveEntryList(&Device->Link);
    WdfWaitLockRelease(SsDeviceListLock);
}

static
BOOLEAN
SsEvtWatchdogIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static
VOID
SsEvtWatchdogDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PSS_DEVICE Device = SsGetDevice((WDFDEVICE)AssociatedObject);

    UNREFERENCED_PARAMETER(Interrupt);

    DPRINT1("[subsys] WATCHDOG BITE from modem (armed %ld)\n", Device->WatchdogArmed);
    SsPost(Device, SS_EV_FATAL, SS_FAULT_WATCHDOG);
}

static
VOID
SsEvtTimer(
    _In_ WDFTIMER Timer)
{
    PSS_DEVICE Device = SsGetDevice(WdfTimerGetParentObject(Timer));

    DPRINT1("[subsys] timer fired in state %ld\n", Device->State);
    SsPost(Device, SS_EV_TIMEOUT, 0);
}

static
VOID
SsHandleReload(
    _In_ PSS_DEVICE Device,
    _In_ PVOID Input,
    _In_ size_t Length,
    _Out_ NTSTATUS *Status)
{
    PUCHAR Buffer = Input;

    if (Length < SS_RELOAD_INPUT_SIZE)
    {
        *Status = STATUS_INVALID_PARAMETER;
        return;
    }

    switch (Device->State)
    {
        case SS_ST_ERROR:
        case SS_ST_OFF:
            *Status = STATUS_DEVICE_POWERED_OFF;
            return;

        case SS_ST_DUMP_STARTED:
        case SS_ST_SSR_STOPPING:
        case SS_ST_RESTART_POWER_CYCLE:
        case SS_ST_SSR_WAIT_ACKS:
        case SS_ST_SSR_WAIT_CHILDREN:
        case SS_ST_REBOOTED_AFTER_SSR:
        case SS_ST_RESTART_REQUEST:
        case SS_ST_RESTART_STOPPING:
            *Status = STATUS_SHUTDOWN_IN_PROGRESS;
            return;

        case SS_ST_READY:
            break;

        default:
            *Status = STATUS_DEVICE_BUSY;
            return;
    }

    if (*(PWCHAR)(Buffer + SS_RELOAD_FW_OFFSET) != UNICODE_NULL)
        RtlStringCchCopyW(Device->FwImagePath, QCOM_PIL_PATH_LENGTH, (PWCHAR)(Buffer + SS_RELOAD_FW_OFFSET));
    if (*(PWCHAR)(Buffer + SS_RELOAD_SW_OFFSET) != UNICODE_NULL)
        RtlStringCchCopyW(Device->SwImagePath, QCOM_PIL_PATH_LENGTH, (PWCHAR)(Buffer + SS_RELOAD_SW_OFFSET));

    if (Buffer[SS_RELOAD_FLAG_OFFSET])
    {
        SsPost(Device, SS_EV_RESTART_REQUEST, 0);
        *Status = STATUS_SUCCESS;
    }
    else
    {
        *Status = STATUS_UNSUCCESSFUL;
    }
}

static
VOID
SsEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PSS_DEVICE Device = SsGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;
    PVOID Input;

    DPRINT1("[subsys] AMSSStats IOCTL 0x%08lx in %Iu out %Iu (state %ld)\n",
            IoControlCode, InputBufferLength, OutputBufferLength, Device->State);

    switch (IoControlCode)
    {
        case SS_IOCTL_RELEASE_WAKE_A:
        case SS_IOCTL_RELEASE_WAKE_B:
            if (InputBufferLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &Input, NULL);
            if (NT_SUCCESS(Status))
                SsReleaseWakeIrps(Device, *(PULONG)Input);
            break;

        case SS_IOCTL_RELOAD_A:
        case SS_IOCTL_RELOAD_B:
            Status = WdfRequestRetrieveInputBuffer(Request, SS_RELOAD_INPUT_SIZE, &Input, NULL);
            if (NT_SUCCESS(Status))
                SsHandleReload(Device, Input, InputBufferLength, &Status);
            break;

        case SS_IOCTL_FIRMWARE_ID_A:
        case SS_IOCTL_FIRMWARE_ID_B:
            Status = STATUS_NOT_SUPPORTED;
            break;

        default:
            break;
    }

    DPRINT1("[subsys] AMSSStats IOCTL 0x%08lx -> 0x%lx\n", IoControlCode, Status);
    WdfRequestCompleteWithInformation(Request, Status, Information);
}

static
NTSTATUS
SsEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\AMSSStats");
    DECLARE_CONST_UNICODE_STRING(SymbolicLink, L"\\DosDevices\\AMSSStats");
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE WdfDevice;
    PSS_DEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    DPRINT1("[subsys] DeviceAdd\n");

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDeviceSelfManagedIoInit = SsEvtSelfManagedIoInit;
    PnpCallbacks.EvtDeviceSelfManagedIoCleanup = SsEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WdfDeviceInitSetDeviceType(DeviceInit, 0x9CA3);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    Status = SsSetupChildren(NULL, DeviceInit);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SS_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = SsGetDevice(WdfDevice);
    RtlZeroMemory(Device, sizeof(*Device));
    Device->WdfDevice = WdfDevice;
    Device->State = SS_ST_INIT;
    Device->AcksRequired = 0x3;
    KeInitializeEvent(&Device->MonitorStop, NotificationEvent, FALSE);
    KeInitializeEvent(&Device->SysmonOpened, NotificationEvent, FALSE);
    KeInitializeEvent(&Device->SysmonReadable, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Device->TzArrived, NotificationEvent, FALSE);
    KeInitializeEvent(&Device->OffEvent, NotificationEvent, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfDevice;
    Status = WdfWaitLockCreate(&Attributes, &Device->FsmLock);
    if (NT_SUCCESS(Status))
        Status = WdfWaitLockCreate(&Attributes, &Device->SysmonLock);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfDeviceCreateSymbolicLink(WdfDevice, &SymbolicLink);

    Status = WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_DEVINTERFACE_QCOM_AMSS, NULL);
    if (NT_SUCCESS(Status))
        Status = WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_DEVINTERFACE_QCOM_AMSS_GNSS, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = SsEvtIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_TIMER_CONFIG_INIT(&TimerConfig, SsEvtTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfDevice;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Device->Timer);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, SsEvtWatchdogIsr, SsEvtWatchdogDpc);
    Status = WdfInterruptCreate(WdfDevice, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Device->Watchdog);
    if (!NT_SUCCESS(Status))
        return Status;

    Device->VersionPage = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, SS_TAG);

    SsDetectChip(Device);
    SsLoadParameters(Device);
    SsReadChildren(Device);
    DPRINT1("[subsys] CHLD: %lu children\n", Device->ChildCount);

    WdfWaitLockAcquire(SsDeviceListLock, NULL);
    InsertTailList(&SsDeviceList, &Device->Link);
    WdfWaitLockRelease(SsDeviceListLock);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDRIVER WdfDriver;
    NTSTATUS Status;

    InitializeListHead(&SsDeviceList);

    WDF_DRIVER_CONFIG_INIT(&Config, SsEvtDeviceAdd);
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &WdfDriver);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfDriver;
    return WdfWaitLockCreate(&Attributes, &SsDeviceListLock);
}
