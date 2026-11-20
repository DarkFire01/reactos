/*
 * PROJECT:     ReactOS ACPI Time and Alarm Device driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Keeps the system time with the platform clock behind ACPI000E
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <acpiioct.h>
#include <ndk/kefuncs.h>

#define NDEBUG
#include <debug.h>

#define ACPITIME_TAG            'mTcA'
#define ACPITIME_EVAL_BUFFER    256

/* An unspecified zone means the clock keeps local time */
#define ACPITIME_ZONE_LOCAL     2047

#define ACPITIME_YEAR_FIRST     2000
#define ACPITIME_YEAR_LAST      2099

/* The time record _GRT returns and _SRT takes */
#include <pshpack1.h>
typedef struct _ACPITIME_TIME
{
    USHORT Year;
    UCHAR Month;
    UCHAR Day;
    UCHAR Hour;
    UCHAR Minute;
    UCHAR Second;
    UCHAR Valid;
    USHORT Milliseconds;
    SHORT TimeZone;
    UCHAR Daylight;
    UCHAR Reserved[3];
} ACPITIME_TIME, *PACPITIME_TIME;
#include <poppack.h>

C_ASSERT(sizeof(ACPITIME_TIME) == 16);

typedef struct _ACPITIME_DEVICE
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Lower;
    PVOID TimeCallback;
    PIO_WORKITEM WriteItem;
    volatile LONG WriteQueued;
    volatile LONG IgnoreNextSet;
} ACPITIME_DEVICE, *PACPITIME_DEVICE;

static
NTSTATUS
AcpiTimeCall(
    _In_ PACPITIME_DEVICE Device,
    _In_reads_bytes_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength)
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,
                                        Device->Lower,
                                        Input,
                                        InputLength,
                                        Output,
                                        OutputLength,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(Device->Lower, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

static
NTSTATUS
AcpiTimeRead(
    _In_ PACPITIME_DEVICE Device,
    _Out_ PACPITIME_TIME Time)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    NTSTATUS Status;

    Output = ExAllocatePoolWithTag(PagedPool, ACPITIME_EVAL_BUFFER, ACPITIME_TAG);
    if (!Output)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "_GRT", sizeof(Input.MethodName));

    Status = AcpiTimeCall(Device, &Input, sizeof(Input), Output, ACPITIME_EVAL_BUFFER);
    if (NT_SUCCESS(Status))
    {
        Argument = Output->Argument;
        if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || !Output->Count ||
            Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER || Argument->DataLength < sizeof(*Time))
        {
            Status = STATUS_ACPI_INVALID_DATA;
        }
        else
        {
            RtlCopyMemory(Time, Argument->Data, sizeof(*Time));
        }
    }

    ExFreePoolWithTag(Output, ACPITIME_TAG);
    return Status;
}

static
NTSTATUS
AcpiTimeWrite(
    _In_ PACPITIME_DEVICE Device,
    _In_ const ACPITIME_TIME *Time)
{
    UCHAR InputBuffer[FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) +
                      ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ACPITIME_TIME))];
    PACPI_EVAL_INPUT_BUFFER_COMPLEX Input = (PACPI_EVAL_INPUT_BUFFER_COMPLEX)InputBuffer;
    ACPI_EVAL_OUTPUT_BUFFER Output;

    RtlZeroMemory(InputBuffer, sizeof(InputBuffer));
    Input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(Input->MethodName, "_SRT", sizeof(Input->MethodName));
    Input->Size = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ACPITIME_TIME));
    Input->ArgumentCount = 1;
    Input->Argument[0].Type = ACPI_METHOD_ARGUMENT_BUFFER;
    Input->Argument[0].DataLength = sizeof(ACPITIME_TIME);
    RtlCopyMemory(Input->Argument[0].Data, Time, sizeof(*Time));

    return AcpiTimeCall(Device, Input, sizeof(InputBuffer), &Output, sizeof(Output));
}

/* Some firmware leaves Valid clear on a good read, so the fields themselves decide */
static
BOOLEAN
AcpiTimeToSystem(
    _In_ const ACPITIME_TIME *Time,
    _Out_ PLARGE_INTEGER SystemTime)
{
    TIME_FIELDS Fields;
    LARGE_INTEGER ClockTime;

    if (Time->Year < ACPITIME_YEAR_FIRST || Time->Year > ACPITIME_YEAR_LAST)
        return FALSE;

    RtlZeroMemory(&Fields, sizeof(Fields));
    Fields.Year = Time->Year;
    Fields.Month = Time->Month;
    Fields.Day = Time->Day;
    Fields.Hour = Time->Hour;
    Fields.Minute = Time->Minute;
    Fields.Second = Time->Second;
    Fields.Milliseconds = (CSHORT)min(Time->Milliseconds, 999);
    if (!RtlTimeFieldsToTime(&Fields, &ClockTime))
        return FALSE;

    /* A zone in minutes is the clock's own offset from UTC, otherwise it is local like the CMOS clock */
    if (Time->TimeZone != ACPITIME_ZONE_LOCAL &&
        Time->TimeZone >= -1440 && Time->TimeZone <= 1440)
    {
        SystemTime->QuadPart = ClockTime.QuadPart + (LONGLONG)Time->TimeZone * 60 * 10000000;
    }
    else
    {
        ExLocalTimeToSystemTime(&ClockTime, SystemTime);
    }

    return TRUE;
}

static
VOID
AcpiTimeSyncFromClock(
    _In_ PACPITIME_DEVICE Device)
{
    ACPITIME_TIME Time;
    LARGE_INTEGER SystemTime;
    NTSTATUS Status;

    Status = AcpiTimeRead(Device, &Time);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("acpitime: _GRT failed 0x%lx, the system time is left as it is\n", Status);
        return;
    }

    if (!AcpiTimeToSystem(&Time, &SystemTime))
    {
        DPRINT1("acpitime: the clock reads %04u-%02u-%02u %02u:%02u:%02u, not a usable time\n",
                Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
        return;
    }

    /* Setting the time notifies the callback, which would write the same time back */
    InterlockedExchange(&Device->IgnoreNextSet, 1);
    Status = ZwSetSystemTime(&SystemTime, NULL);
    if (!NT_SUCCESS(Status))
    {
        InterlockedExchange(&Device->IgnoreNextSet, 0);
        DPRINT1("acpitime: setting the system time failed 0x%lx\n", Status);
        return;
    }

    DPRINT1("acpitime: system time set from the platform clock, %04u-%02u-%02u %02u:%02u:%02u\n",
            Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
}

/* Writes the current local time to the platform clock */
static
VOID
NTAPI
AcpiTimeWriteWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PACPITIME_DEVICE Device = DeviceObject->DeviceExtension;
    LARGE_INTEGER SystemTime, LocalTime;
    TIME_FIELDS Fields;
    ACPITIME_TIME Time;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    InterlockedExchange(&Device->WriteQueued, 0);

    KeQuerySystemTime(&SystemTime);
    ExSystemTimeToLocalTime(&SystemTime, &LocalTime);
    RtlTimeToTimeFields(&LocalTime, &Fields);

    RtlZeroMemory(&Time, sizeof(Time));
    Time.Year = Fields.Year;
    Time.Month = (UCHAR)Fields.Month;
    Time.Day = (UCHAR)Fields.Day;
    Time.Hour = (UCHAR)Fields.Hour;
    Time.Minute = (UCHAR)Fields.Minute;
    Time.Second = (UCHAR)Fields.Second;
    Time.Milliseconds = Fields.Milliseconds;
    Time.TimeZone = ACPITIME_ZONE_LOCAL;

    Status = AcpiTimeWrite(Device, &Time);
    if (!NT_SUCCESS(Status))
        DPRINT1("acpitime: _SRT failed 0x%lx\n", Status);
}

/* \Callback\SetSystemTime, the clock follows every change of the system time */
static
VOID
NTAPI
AcpiTimeChanged(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    PACPITIME_DEVICE Device = CallbackContext;

    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    if (!Device || InterlockedExchange(&Device->IgnoreNextSet, 0))
        return;

    if (!InterlockedExchange(&Device->WriteQueued, 1))
        IoQueueWorkItem(Device->WriteItem, AcpiTimeWriteWorker, DelayedWorkQueue, NULL);
}

static
VOID
AcpiTimeStop(
    _Inout_ PACPITIME_DEVICE Device)
{
    if (Device->TimeCallback)
    {
        ExUnregisterCallback(Device->TimeCallback);
        Device->TimeCallback = NULL;
    }
}

static
NTSTATUS
AcpiTimeStart(
    _Inout_ PACPITIME_DEVICE Device)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Callback\\SetSystemTime");
    OBJECT_ATTRIBUTES Attributes;
    PCALLBACK_OBJECT Callback;
    NTSTATUS Status;

    AcpiTimeSyncFromClock(Device);

    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ExCreateCallback(&Callback, &Attributes, FALSE, TRUE);
    if (NT_SUCCESS(Status))
    {
        Device->TimeCallback = ExRegisterCallback(Callback, AcpiTimeChanged, Device);
        ObDereferenceObject(Callback);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
AcpiTimeCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
AcpiTimeForwardAndWait(
    _In_ PACPITIME_DEVICE Device,
    _Inout_ PIRP Irp)
{
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, AcpiTimeCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Device->Lower, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    return Irp->IoStatus.Status;
}

static
NTSTATUS
AcpiTimeComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
AcpiTimePassDown(
    _In_ PACPITIME_DEVICE Device,
    _Inout_ PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Device->Lower, Irp);
}

static
NTSTATUS
NTAPI
AcpiTimeDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PACPITIME_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = AcpiTimeForwardAndWait(Device, Irp);
            if (NT_SUCCESS(Status))
                Status = AcpiTimeStart(Device);
            return AcpiTimeComplete(Irp, Status);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            AcpiTimeStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return AcpiTimePassDown(Device, Irp);

        case IRP_MN_REMOVE_DEVICE:
            AcpiTimeStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = AcpiTimePassDown(Device, Irp);
            IoFreeWorkItem(Device->WriteItem);
            IoDetachDevice(Device->Lower);
            IoDeleteDevice(DeviceObject);
            return Status;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return AcpiTimePassDown(Device, Irp);

        default:
            return AcpiTimePassDown(Device, Irp);
    }
}

static
NTSTATUS
NTAPI
AcpiTimeDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PACPITIME_DEVICE Device = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Device->Lower, Irp);
}

static
NTSTATUS
NTAPI
AcpiTimeDispatchPassDown(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    return AcpiTimePassDown(DeviceObject->DeviceExtension, Irp);
}

static
NTSTATUS
NTAPI
AcpiTimeAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT DeviceObject;
    PACPITIME_DEVICE Device;
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject, sizeof(*Device), NULL, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = DeviceObject->DeviceExtension;
    RtlZeroMemory(Device, sizeof(*Device));
    Device->Self = DeviceObject;

    Device->WriteItem = IoAllocateWorkItem(DeviceObject);
    if (!Device->WriteItem)
    {
        IoDeleteDevice(DeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Device->Lower = IoAttachDeviceToDeviceStack(DeviceObject, Pdo);
    if (!Device->Lower)
    {
        IoFreeWorkItem(Device->WriteItem);
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
AcpiTimeUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = AcpiTimeDispatchPassDown;

    DriverObject->MajorFunction[IRP_MJ_PNP] = AcpiTimeDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = AcpiTimeDispatchPower;
    DriverObject->DriverExtension->AddDevice = AcpiTimeAddDevice;
    DriverObject->DriverUnload = AcpiTimeUnload;
    return STATUS_SUCCESS;
}
