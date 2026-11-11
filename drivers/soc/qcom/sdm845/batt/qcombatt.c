/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     BQ27441 fuel gauge as a battery class miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <batclass.h>
#include <qcom/geniif.h>
#include <qcom/qcompnp.h>
#include <wmistr.h>
#include <wmiguid.h>
#include <wmidata.h>
#include <dmilib.h>

#define QCDBG_COMPONENT "qcombatt"
#include <qcom/qcomdbg.h>

#define QCBATT_POOL_TAG     'aBcQ'

/* The gauge sits at 7-bit address 0x55 on the OnePlus 6 fuel gauge I2C bus */
#define BQ27441_ADDRESS     0x55
#define BQ27441_REG_VOLTAGE 0x04
#define BQ27441_REG_SOC     0x1C

#define QCBATT_POLL_SECONDS 5

static const GUID QcBattSmbiosGuid = SMBIOS_DATA_GUID;

typedef struct _QCBATT_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    PQCOM_GENI_I2C I2c;
    PVOID ClassData;
    PKTHREAD Thread;
    KEVENT StopEvent;
    ULONG Percent;
    ULONG MilliVolts;
} QCBATT_DEVICE, *PQCBATT_DEVICE;

static
BOOLEAN
QcBattIsSupportedBoard(VOID)
{
    PVOID DataBlock;
    PWNODE_ALL_DATA AllData;
    PCHAR Strings[ID_STRINGS_MAX] = { 0 };
    BOOLEAN Supported = FALSE;
    ULONG BufferSize = 0;
    NTSTATUS Status;

    Status = IoWMIOpenBlock((LPGUID)&QcBattSmbiosGuid, WMIGUID_QUERY, &DataBlock);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, NULL);
    if (Status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(Status))
    {
        ObDereferenceObject(DataBlock);
        return FALSE;
    }

    AllData = ExAllocatePoolWithTag(PagedPool, BufferSize, QCBATT_POOL_TAG);
    if (!AllData)
    {
        ObDereferenceObject(DataBlock);
        return FALSE;
    }

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, AllData);
    if (NT_SUCCESS(Status))
    {
        ParseSMBiosTables(AllData + 1, AllData->WnodeHeader.BufferSize, Strings);
        if (Strings[SYS_VENDOR] && !strcmp(Strings[SYS_VENDOR], "OnePlus") &&
            Strings[SYS_PRODUCT] &&
            (!strcmp(Strings[SYS_PRODUCT], "6") || !strcmp(Strings[SYS_PRODUCT], "6T")))
        {
            Supported = TRUE;
        }
    }

    ExFreePoolWithTag(AllData, QCBATT_POOL_TAG);
    ObDereferenceObject(DataBlock);
    return Supported;
}

/* The gauge returns a little endian word for a one byte command */
static
NTSTATUS
QcBattReadWord(
    _In_ PQCBATT_DEVICE Device,
    _In_ UCHAR Register,
    _Out_ PUSHORT Value)
{
    UCHAR Buffer[2];
    NTSTATUS Status;

    Status = QcGeniI2cTransfer(Device->I2c, BQ27441_ADDRESS, &Register, 1, Buffer, sizeof(Buffer));
    if (!NT_SUCCESS(Status))
        return Status;

    *Value = (USHORT)Buffer[0] | ((USHORT)Buffer[1] << 8);
    return STATUS_SUCCESS;
}

static
VOID
QcBattRefresh(
    _In_ PQCBATT_DEVICE Device)
{
    USHORT Soc;
    USHORT Voltage;

    if (NT_SUCCESS(QcBattReadWord(Device, BQ27441_REG_SOC, &Soc)))
        Device->Percent = Soc;
    if (NT_SUCCESS(QcBattReadWord(Device, BQ27441_REG_VOLTAGE, &Voltage)))
        Device->MilliVolts = Voltage;
}

static
VOID
NTAPI
QcBattPollThread(
    _In_ PVOID Context)
{
    PQCBATT_DEVICE Device = Context;
    LARGE_INTEGER Period;

    Period.QuadPart = -10000000LL * QCBATT_POLL_SECONDS;
    while (KeWaitForSingleObject(&Device->StopEvent, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT)
        QcBattRefresh(Device);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
QcBattQueryTag(
    _In_ PVOID Context,
    _Out_ PULONG BatteryTag)
{
    UNREFERENCED_PARAMETER(Context);

    *BatteryTag = 1;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBattQueryInformation(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_QUERY_INFORMATION_LEVEL Level,
    _In_ LONG AtRate,
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength)
{
    BATTERY_INFORMATION Information;
    BATTERY_REPORTING_SCALE Scale;
    PVOID Source;
    ULONG Length;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(AtRate);

    switch (Level)
    {
        case BatteryInformation:
            RtlZeroMemory(&Information, sizeof(Information));
            Information.Capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
            Information.Technology = 1;
            Information.Chemistry[0] = 'L';
            Information.Chemistry[1] = 'I';
            Information.Chemistry[2] = 'O';
            Information.Chemistry[3] = 'N';
            Information.DesignedCapacity = 100;
            Information.FullChargedCapacity = 100;
            Information.DefaultAlert1 = 10;
            Information.DefaultAlert2 = 5;
            Source = &Information;
            Length = sizeof(Information);
            break;

        case BatteryGranularityInformation:
            Scale.Granularity = 1;
            Scale.Capacity = 100;
            Source = &Scale;
            Length = sizeof(Scale);
            break;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    *ReturnedLength = Length;
    if (BufferLength < Length)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Buffer, Source, Length);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBattQueryStatus(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _Out_ PBATTERY_STATUS BatteryStatus)
{
    PQCBATT_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(BatteryTag);

    /* The reader thread keeps these fresh, the callback only hands them out */
    BatteryStatus->PowerState = BATTERY_DISCHARGING;
    BatteryStatus->Capacity = Device->Percent;
    BatteryStatus->Voltage = Device->MilliVolts;
    BatteryStatus->Rate = BATTERY_UNKNOWN_RATE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBattSetStatusNotify(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ PBATTERY_NOTIFY BatteryNotify)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(BatteryNotify);

    /* No hardware threshold interrupt, the class polls instead */
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
NTAPI
QcBattDisableStatusNotify(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBattDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCBATT_DEVICE Device = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    if (!Device->ClassData)
        return QcPnpPassDown(&Device->Pnp, Irp);

    /* The class leaves anything that is not a battery request for us to send on */
    Status = BatteryClassIoctl(Device->ClassData, Irp);
    if (Status == STATUS_NOT_SUPPORTED)
        return QcPnpPassDown(&Device->Pnp, Irp);

    return Status;
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcBattStop(
    _In_ PQCBATT_DEVICE Device)
{
    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
        Device->Thread = NULL;
    }
    if (Device->ClassData)
    {
        BatteryClassUnload(Device->ClassData);
        Device->ClassData = NULL;
    }
    if (Device->I2c)
    {
        QcGeniI2cClose(Device->I2c);
        Device->I2c = NULL;
    }
}

static
NTSTATUS
QcBattStart(
    _In_ PQCBATT_DEVICE Device)
{
    QCOM_GENI_I2C_CONFIG Config;
    BATTERY_MINIPORT_INFO Miniport;
    HANDLE Thread;
    NTSTATUS Status;

    /* The ACPI node is the PMIC gauge, this board routes the cell to a BQ27441 instead */
    if (!QcBattIsSupportedBoard())
        return STATUS_NO_SUCH_DEVICE;

    KeClearEvent(&Device->StopEvent);

    /* The fuel gauge hangs off QUPv3 wrap1 S2 at 100 kHz */
    RtlZeroMemory(&Config, sizeof(Config));
    Config.EngineBase.QuadPart = 0x00A88000;
    Config.ClockControllerBase.QuadPart = 0x00100000;
    Config.ClockVoteOffset = 0x5200C;
    Config.ClockVoteMask = 0x01300000;
    Config.EngineBranchOffset = 0x18274;
    Config.EngineRcgOffset = 0x18278;
    Config.BusSpeedHz = 100000;

    Status = QcGeniI2cOpen(&Config, &Device->I2c);
    if (!NT_SUCCESS(Status))
        return Status;

    QcBattRefresh(Device);
    QC_LOG("fuel gauge ready, %lu%% at %lu mV", Device->Percent, Device->MilliVolts);

    RtlZeroMemory(&Miniport, sizeof(Miniport));
    Miniport.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    Miniport.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    Miniport.Context = Device;
    Miniport.QueryTag = QcBattQueryTag;
    Miniport.QueryInformation = QcBattQueryInformation;
    Miniport.QueryStatus = QcBattQueryStatus;
    Miniport.SetStatusNotify = QcBattSetStatusNotify;
    Miniport.DisableStatusNotify = QcBattDisableStatusNotify;

    /* A real PDO lets the class publish GUID_DEVICE_BATTERY for the power UI */
    Miniport.Pdo = Device->Pnp.Pdo;

    Status = BatteryClassInitializeDevice(&Miniport, &Device->ClassData);
    if (!NT_SUCCESS(Status))
    {
        QcBattStop(Device);
        return Status;
    }

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcBattPollThread, Device);
    if (!NT_SUCCESS(Status))
    {
        QcBattStop(Device);
        return Status;
    }
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBattDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCBATT_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcBattStart(Device);
            return QcPnpComplete(Irp, Status);

        /* The gauge has no resources to move, so a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcBattStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcBattStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = QcPnpPassDown(&Device->Pnp, Irp);
            QcPnpDeleteDevice(&Device->Pnp);
            return Status;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        default:
            return QcPnpPassDown(&Device->Pnp, Irp);
    }
}

static
NTSTATUS
NTAPI
QcBattAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCBATT_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_BATTERY, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Pnp->Self->Flags |= DO_BUFFERED_IO;

    Device = CONTAINING_RECORD(Pnp, QCBATT_DEVICE, Pnp);
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcBattUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    /* Every device was stopped and deleted by its REMOVE */
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = QcBattAddDevice;
    DriverObject->DriverUnload = QcBattUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QcBattDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcBattDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
