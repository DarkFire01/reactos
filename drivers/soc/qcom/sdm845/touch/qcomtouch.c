/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synaptics RMI4 touch controller as a HID digitizer under mshidkmdf
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Lower filter below mshidkmdf on the ACPI touch device. The I2C target, the
 * attention GpioInt and the reset and rail GpioIo lines all come from the
 * resource hub, so the same code runs on any board ACPI describes this way.
 */

#include "qcomtouch.h"
#include <wmistr.h>
#include <wmiguid.h>
#include <wmidata.h>
#include <dmilib.h>

#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD QcTouchEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE QcTouchEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE QcTouchEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY QcTouchEvtD0Entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED QcTouchEvtD0EntryPostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT QcTouchEvtD0Exit;
static EVT_WDF_INTERRUPT_ISR QcTouchEvtInterruptIsr;
static EVT_WDF_INTERRUPT_WORKITEM QcTouchEvtInterruptWorkItem;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL QcTouchEvtIoInternalDeviceControl;

static const GUID QcTouchSmbiosGuid = SMBIOS_DATA_GUID;

/* The OnePlus 6 and 6T feed the sensor core from an RPMh LDO ACPI leaves out */
static const QCTOUCH_BOARD QcTouchBoards[] =
{
    { "OnePlus", "6",  200, "ldoa28", 3000000, 1664000, 8000, 7 },
    { "OnePlus", "6T", 200, "ldoa28", 3000000, 1664000, 8000, 7 },
};

/* Attention reads per interrupt before giving the line back */
#define QCTOUCH_ATTENTION_LOOPS     4

static
PCQCTOUCH_BOARD
QcTouchDetectBoard(VOID)
{
    PVOID DataBlock;
    PWNODE_ALL_DATA AllData;
    PCHAR Strings[ID_STRINGS_MAX] = { 0 };
    PCQCTOUCH_BOARD Board = NULL;
    ULONG BufferSize = 0;
    ULONG Index;
    NTSTATUS Status;

    Status = IoWMIOpenBlock((LPGUID)&QcTouchSmbiosGuid, WMIGUID_QUERY, &DataBlock);
    if (!NT_SUCCESS(Status))
        return NULL;

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, NULL);
    if (Status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(Status))
    {
        ObDereferenceObject(DataBlock);
        return NULL;
    }

    AllData = ExAllocatePoolWithTag(PagedPool, BufferSize, QCTOUCH_POOL_TAG);
    if (!AllData)
    {
        ObDereferenceObject(DataBlock);
        return NULL;
    }

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, AllData);
    if (NT_SUCCESS(Status))
    {
        ParseSMBiosTables(AllData + 1, AllData->WnodeHeader.BufferSize, Strings);

        for (Index = 0; Index < RTL_NUMBER_OF(QcTouchBoards); Index++)
        {
            if (Strings[SYS_VENDOR] && Strings[SYS_PRODUCT] &&
                !strcmp(Strings[SYS_VENDOR], QcTouchBoards[Index].Manufacturer) &&
                !strcmp(Strings[SYS_PRODUCT], QcTouchBoards[Index].Product))
            {
                Board = &QcTouchBoards[Index];
                break;
            }
        }
    }

    ExFreePoolWithTag(AllData, QCTOUCH_POOL_TAG);
    ObDereferenceObject(DataBlock);
    return Board;
}

static
VOID
QcTouchDelay(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/**
 * @brief Records the hub ids of the I2C target and the two GpioIo lines.
 *
 * The _CRS lists the reset line before the rail enable.
 */
static
NTSTATUS
NTAPI
QcTouchEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST Resources,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PQCTOUCH_DEVICE Device = QcTouchGetDevice(WdfDevice);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Resources);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Descriptor == NULL || Descriptor->Type != CmResourceTypeConnection)
            continue;

        if (Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C &&
            !Device->HasI2c)
        {
            Device->I2cId.LowPart = Descriptor->u.Connection.IdLowPart;
            Device->I2cId.HighPart = Descriptor->u.Connection.IdHighPart;
            Device->HasI2c = TRUE;
        }
        else if (Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
                 Descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO)
        {
            if (!Device->HasReset)
            {
                Device->ResetId.LowPart = Descriptor->u.Connection.IdLowPart;
                Device->ResetId.HighPart = Descriptor->u.Connection.IdHighPart;
                Device->HasReset = TRUE;
            }
            else if (!Device->HasEnable)
            {
                Device->EnableId.LowPart = Descriptor->u.Connection.IdLowPart;
                Device->EnableId.HighPart = Descriptor->u.Connection.IdHighPart;
                Device->HasEnable = TRUE;
            }
        }
    }

    if (!Device->HasI2c)
    {
        DPRINT1("no I2C connection in the resources\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = QcTouchOpenConnection(Device, Device->I2cId, &Device->I2c);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Device->HasReset)
    {
        Status = QcTouchOpenConnection(Device, Device->ResetId, &Device->Reset);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (Device->HasEnable)
    {
        Status = QcTouchOpenConnection(Device, Device->EnableId, &Device->Enable);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Device->Board = QcTouchDetectBoard();
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcTouchEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PQCTOUCH_DEVICE Device = QcTouchGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Device->Enable)
    {
        WdfObjectDelete(Device->Enable);
        Device->Enable = NULL;
    }

    if (Device->Reset)
    {
        WdfObjectDelete(Device->Reset);
        Device->Reset = NULL;
    }

    if (Device->I2c)
    {
        WdfObjectDelete(Device->I2c);
        Device->I2c = NULL;
    }

    Device->HasI2c = FALSE;
    Device->HasReset = FALSE;
    Device->HasEnable = FALSE;
    return STATUS_SUCCESS;
}

/**
 * @brief Powers the sensor, takes it out of reset and configures RMI4.
 */
static
NTSTATUS
NTAPI
QcTouchEvtD0Entry(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PQCTOUCH_DEVICE Device = QcTouchGetDevice(WdfDevice);
    PCQCTOUCH_BOARD Board = Device->Board;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PreviousState);

    if (Board && Board->VddResource)
    {
        QCOM_RPMH_VRM_REQUEST Vdd;

        Vdd.Resource = Board->VddResource;
        Vdd.TargetMicrovolts = Board->VddMicrovolts;
        Vdd.MinMicrovolts = Board->VddMinMicrovolts;
        Vdd.StepMicrovolts = Board->VddStepMicrovolts;
        Vdd.Mode = Board->VddMode;

        Status = QcRpmhEnableRegulator(&Vdd);
        if (!NT_SUCCESS(Status))
            DPRINT1("%s did not come up, 0x%08lx\n", Board->VddResource, Status);
    }

    if (Device->Enable)
    {
        Status = QcTouchGpioWrite(Device->Enable, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (Device->Reset)
    {
        Status = QcTouchGpioWrite(Device->Reset, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    QcTouchDelay(10);

    WdfWaitLockAcquire(Device->RmiLock, NULL);

    Device->Page = 0xFF;
    RtlZeroMemory(&Device->F01, sizeof(Device->F01));
    RtlZeroMemory(&Device->F12, sizeof(Device->F12));

    Status = RmiScanFunctions(Device);
    if (NT_SUCCESS(Status))
        Status = RmiConfigure(Device);

    Device->Ready = NT_SUCCESS(Status);
    WdfWaitLockRelease(Device->RmiLock);

    if (!NT_SUCCESS(Status))
        DPRINT1("RMI4 bring-up failed, 0x%08lx\n", Status);

    return Status;
}

/**
 * @brief Reads attention until the sensor has nothing pending.
 */
static
VOID
QcTouchServiceAttention(
    _In_ PQCTOUCH_DEVICE Device)
{
    UCHAR Packet[RMI_READ_MAX];
    BOOLEAN HasObjects;
    ULONG Loop;
    NTSTATUS Status;

    WdfWaitLockAcquire(Device->RmiLock, NULL);

    for (Loop = 0; Device->Ready && Loop < QCTOUCH_ATTENTION_LOOPS; Loop++)
    {
        Status = RmiReadAttention(Device, &HasObjects, Packet);
        if (!NT_SUCCESS(Status) || Status == STATUS_NO_MORE_ENTRIES)
            break;

        if (HasObjects)
            QcTouchReportObjects(Device, Packet);
    }

    WdfWaitLockRelease(Device->RmiLock);
}

/* An edge raised before the interrupt was connected is otherwise lost */
static
NTSTATUS
NTAPI
QcTouchEvtD0EntryPostInterruptsEnabled(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    QcTouchServiceAttention(QcTouchGetDevice(WdfDevice));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcTouchEvtD0Exit(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PQCTOUCH_DEVICE Device = QcTouchGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(TargetState);

    WdfWaitLockAcquire(Device->RmiLock, NULL);

    if (Device->Ready)
        RmiSleep(Device);

    Device->Ready = FALSE;
    QcTouchReleaseAll(Device);

    WdfWaitLockRelease(Device->RmiLock);
    return STATUS_SUCCESS;
}

/* The line is a GPIO behind msgpioclx, the bus work has to wait for passive level */
static
BOOLEAN
NTAPI
QcTouchEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    UNREFERENCED_PARAMETER(MessageID);

    WdfInterruptQueueWorkItemForIsr(Interrupt);
    return TRUE;
}

static
VOID
NTAPI
QcTouchEvtInterruptWorkItem(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

    QcTouchServiceAttention(QcTouchGetDevice(WdfInterruptGetDevice(Interrupt)));
}

static
VOID
NTAPI
QcTouchEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    QcTouchHidInternalIoctl(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode);
}

static
NTSTATUS
NTAPI
QcTouchEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    PQCTOUCH_DEVICE Device;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = QcTouchEvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = QcTouchEvtReleaseHardware;
    PnpPower.EvtDeviceD0Entry = QcTouchEvtD0Entry;
    PnpPower.EvtDeviceD0EntryPostInterruptsEnabled = QcTouchEvtD0EntryPostInterruptsEnabled;
    PnpPower.EvtDeviceD0Exit = QcTouchEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, QCTOUCH_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = QcTouchGetDevice(WdfDevice);
    Device->Device = WdfDevice;
    Device->PrimaryContact = MAXULONG;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfDevice;
    Status = WdfWaitLockCreate(&Attributes, &Device->RmiLock);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfSpinLockCreate(&Attributes, &Device->ReportLock);
    if (!NT_SUCCESS(Status))
        return Status;

    /* hidclass talks to the minidriver only through internal device controls */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = QcTouchEvtIoInternalDeviceControl;
    Status = WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Device->ReadQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, QcTouchEvtInterruptIsr, NULL);
    InterruptConfig.EvtInterruptWorkItem = QcTouchEvtInterruptWorkItem;
    Status = WdfInterruptCreate(WdfDevice, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Device->Interrupt);
    if (!NT_SUCCESS(Status))
        DPRINT1("WdfInterruptCreate failed, 0x%08lx\n", Status);

    return Status;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, QcTouchEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
