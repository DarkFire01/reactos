/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, configuration and class device creation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdclass.h"

#define NDEBUG
#include <debug.h>

KBDCLASS_GLOBALS KbdClassGlobals;
const UCHAR KbdClassTrustedOpen = 0;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE KbdClassAddDevice;
static IO_COMPLETION_ROUTINE KbdClassSyncComplete;
static RTL_QUERY_REGISTRY_ROUTINE KbdClassCountPortValue;

/* HELPERS ********************************************************************/

/**
 * @brief Writes an event log entry with optional ULONG dump data.
 */
VOID
NTAPI
KbdClassLogError(
    _In_ PVOID IoObject,
    _In_ NTSTATUS ErrorCode,
    _In_ ULONG UniqueErrorValue,
    _In_ NTSTATUS FinalStatus,
    _In_ ULONG DumpCount,
    _In_reads_opt_(DumpCount) PULONG DumpData,
    _In_ UCHAR MajorFunction)
{
    PIO_ERROR_LOG_PACKET Entry;
    ULONG EntrySize;
    ULONG Index;

    EntrySize = sizeof(IO_ERROR_LOG_PACKET) + DumpCount * sizeof(ULONG);
    if (EntrySize > ERROR_LOG_MAXIMUM_SIZE)
        return;

    Entry = IoAllocateErrorLogEntry(IoObject, (UCHAR)EntrySize);
    if (!Entry)
        return;

    Entry->MajorFunctionCode = MajorFunction;
    Entry->RetryCount = 0;
    Entry->DumpDataSize = (USHORT)(DumpCount * sizeof(ULONG));
    Entry->ErrorCode = ErrorCode;
    Entry->UniqueErrorValue = UniqueErrorValue;
    Entry->FinalStatus = FinalStatus;
    Entry->SequenceNumber = 0;
    Entry->IoControlCode = 0;

    for (Index = 0; Index < DumpCount; Index++)
        Entry->DumpData[Index] = DumpData[Index];

    IoWriteErrorLogEntry(Entry);
}

static
NTSTATUS
NTAPI
KbdClassSyncComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Sends an IRP down and waits until the lower driver hands it back.
 */
NTSTATUS
NTAPI
KbdClassSendIrpSynchronously(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ BOOLEAN CopyStack)
{
    KEVENT Event;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    if (CopyStack)
        IoCopyCurrentIrpStackLocationToNext(Irp);

    IoSetCompletionRoutine(Irp, KbdClassSyncComplete, &Event, TRUE, TRUE, TRUE);
    IoCallDriver(DeviceObject, Irp);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    return Irp->IoStatus.Status;
}

static
NTSTATUS
KbdClassOpenParametersKey(
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE KeyHandle)
{
    UNICODE_STRING SubKey = RTL_CONSTANT_STRING(L"Parameters");
    OBJECT_ATTRIBUTES Attributes;
    HANDLE ServiceKey;
    NTSTATUS Status;

    *KeyHandle = NULL;

    InitializeObjectAttributes(&Attributes,
                               &KbdClassGlobals.RegistryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenKey(&ServiceKey, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&Attributes,
                               &SubKey,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               ServiceKey,
                               NULL);
    Status = ZwOpenKey(KeyHandle, DesiredAccess, &Attributes);
    ZwClose(ServiceKey);

    return Status;
}

/**
 * @brief Reads a value of at most four bytes, whatever its registry type.
 */
NTSTATUS
NTAPI
KbdClassQueryRegistryUlong(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    ULONG Buffer[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 2 * sizeof(ULONG)) / sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING Name;
    ULONG ResultLength;
    NTSTATUS Status;

    *Value = 0;
    RtlInitUnicodeString(&Name, ValueName);

    Status = ZwQueryValueKey(KeyHandle,
                             &Name,
                             KeyValuePartialInformation,
                             Info,
                             sizeof(Buffer),
                             &ResultLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Info->DataLength > sizeof(*Value))
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Value, Info->Data, Info->DataLength);
    return STATUS_SUCCESS;
}

/* CONFIGURATION **************************************************************/

static
VOID
KbdClassReadConfiguration(VOID)
{
    RTL_QUERY_REGISTRY_TABLE Table[6];
    ULONG QueueEntries = KBDCLASS_DEFAULT_QUEUE_ENTRIES;
    ULONG ConnectMultiplePorts = KBDCLASS_DEFAULT_CONNECT_MULTIPLE;
    ULONG DwordType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    ULONG StringType = (REG_SZ << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_SZ;
    HANDLE KeyHandle;
    NTSTATUS Status;

    KbdClassGlobals.PortsServiced = KBDCLASS_DEFAULT_PORTS_SERVICED;
    KbdClassGlobals.SendOutputToAllPorts = 0;
    KbdClassGlobals.BaseClassName.Length = 0;
    RtlAppendUnicodeToString(&KbdClassGlobals.BaseClassName, KBDCLASS_DEFAULT_BASE_NAME);

    Status = KbdClassOpenParametersKey(KEY_READ, &KeyHandle);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Table, sizeof(Table));

        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[0].Name = L"KeyboardDataQueueSize";
        Table[0].EntryContext = &QueueEntries;
        Table[0].DefaultType = DwordType;

        Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[1].Name = L"MaximumPortsServiced";
        Table[1].EntryContext = &KbdClassGlobals.PortsServiced;
        Table[1].DefaultType = DwordType;

        Table[2].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[2].Name = L"KeyboardDeviceBaseName";
        Table[2].EntryContext = &KbdClassGlobals.BaseClassName;
        Table[2].DefaultType = StringType;

        Table[3].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[3].Name = L"ConnectMultiplePorts";
        Table[3].EntryContext = &ConnectMultiplePorts;
        Table[3].DefaultType = DwordType;

        Table[4].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[4].Name = L"SendOutputToAllPorts";
        Table[4].EntryContext = &KbdClassGlobals.SendOutputToAllPorts;
        Table[4].DefaultType = DwordType;

        Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE | RTL_REGISTRY_OPTIONAL,
                                        (PCWSTR)KeyHandle,
                                        Table,
                                        NULL,
                                        NULL);
        if (!NT_SUCCESS(Status))
            DPRINT1("Reading kbdclass parameters failed 0x%08lx\n", Status);

        ZwClose(KeyHandle);
    }

    if (QueueEntries == 0)
        QueueEntries = KBDCLASS_DEFAULT_QUEUE_ENTRIES;

    if (QueueEntries > MAXULONG / sizeof(KEYBOARD_INPUT_DATA))
        QueueEntries = KBDCLASS_DEFAULT_QUEUE_ENTRIES;

    KbdClassGlobals.QueueBytes = QueueEntries * sizeof(KEYBOARD_INPUT_DATA);

    /* The registry value names the opposite of what the driver tracks */
    KbdClassGlobals.ConnectOneClassToOnePort = (ConnectMultiplePorts == 0);
}

/* CLASS DEVICE OBJECTS *******************************************************/

static
NTSTATUS
KbdClassCreateNamedDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ BOOLEAN Legacy,
    _Out_ PDEVICE_OBJECT *DeviceObject,
    _Out_ PUNICODE_STRING DeviceName)
{
    PCWSTR Format = L"\\Device\\%wZ%u";
    UNICODE_STRING Name;
    ULONG DumpData;
    NTSTATUS Status;

    *DeviceObject = NULL;
    RtlZeroMemory(DeviceName, sizeof(*DeviceName));

    /* Room for the prefix, up to six digits and the terminator */
    Name.Length = 0;
    Name.MaximumLength = KbdClassGlobals.BaseClassName.Length +
                         sizeof(L"\\Device\\") + 6 * sizeof(WCHAR);

    if (Legacy && KbdClassGlobals.ConnectOneClassToOnePort)
    {
        Format = L"\\Device\\%wZLegacy%u";
        Name.MaximumLength += sizeof(L"Legacy");
    }

    Name.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, Name.MaximumLength, KBDCLASS_TAG);
    if (!Name.Buffer)
    {
        DumpData = Name.MaximumLength;
        KbdClassLogError(DriverObject,
                         KBDCLASS_LOG_INSUFFICIENT_RESOURCES,
                         KBDCLASS_ERRID_CREATE_DEVICE,
                         STATUS_UNSUCCESSFUL,
                         1,
                         &DumpData,
                         0);
        return STATUS_UNSUCCESSFUL;
    }

    do
    {
        Status = RtlUnicodeStringPrintf(&Name,
                                        Format,
                                        &KbdClassGlobals.BaseClassName,
                                        KbdClassGlobals.NextDeviceNumber++);
        if (!NT_SUCCESS(Status))
            break;

        Status = IoCreateDevice(DriverObject,
                                sizeof(KBDCLASS_DEVICE_EXTENSION),
                                &Name,
                                FILE_DEVICE_KEYBOARD,
                                0,
                                FALSE,
                                DeviceObject);
    } while (Status == STATUS_OBJECT_NAME_COLLISION);

    if (!NT_SUCCESS(Status))
    {
        DumpData = Name.MaximumLength;
        KbdClassLogError(DriverObject,
                         KBDCLASS_LOG_DEVICE_CREATE_FAILED,
                         KBDCLASS_ERRID_CREATE_DEVICE,
                         Status,
                         1,
                         &DumpData,
                         0);
        ExFreePoolWithTag(Name.Buffer, KBDCLASS_TAG);
        return Status;
    }

    if (Name.MaximumLength < Name.Length + sizeof(UNICODE_NULL))
    {
        IoDeleteDevice(*DeviceObject);
        *DeviceObject = NULL;
        ExFreePoolWithTag(Name.Buffer, KBDCLASS_TAG);
        return STATUS_UNSUCCESSFUL;
    }

    Name.Buffer[Name.Length / sizeof(WCHAR)] = UNICODE_NULL;
    *DeviceName = Name;
    return STATUS_SUCCESS;
}

/**
 * @brief Creates a class device object and its input queue.
 *
 * The device is named after KeyboardDeviceBaseName unless the grand master
 * exists, in which case every further device is unnamed.
 */
static
NTSTATUS
KbdClassCreateClassObject(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ BOOLEAN Legacy,
    _Out_ PDEVICE_OBJECT *ClassDeviceObject,
    _Outptr_result_maybenull_ PWSTR *FullDeviceName)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_OBJECT DeviceObject = NULL;
    UNICODE_STRING Name = {0};
    BOOLEAN HaveGrandMaster;
    ULONG DumpData;
    NTSTATUS Status;

    *ClassDeviceObject = NULL;
    *FullDeviceName = NULL;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);
    HaveGrandMaster = (KbdClassGlobals.GrandMaster != NULL);
    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    if (HaveGrandMaster)
    {
        Status = IoCreateDevice(DriverObject,
                                sizeof(KBDCLASS_DEVICE_EXTENSION),
                                NULL,
                                FILE_DEVICE_KEYBOARD,
                                0,
                                FALSE,
                                &DeviceObject);
        if (!NT_SUCCESS(Status))
        {
            DumpData = 0;
            KbdClassLogError(DriverObject,
                             KBDCLASS_LOG_DEVICE_CREATE_FAILED,
                             KBDCLASS_ERRID_CREATE_DEVICE,
                             Status,
                             1,
                             &DumpData,
                             0);
            return Status;
        }
    }
    else
    {
        Status = KbdClassCreateNamedDevice(DriverObject, Legacy, &DeviceObject, &Name);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    DeviceObject->Flags |= DO_BUFFERED_IO;

    DeviceExtension = DeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExtension, sizeof(*DeviceExtension));
    DeviceExtension->Self = DeviceObject;
    DeviceExtension->QueueLength = KbdClassGlobals.QueueBytes;

    IoInitializeRemoveLock(&DeviceExtension->RemoveLock, KBDCLASS_TAG, 0, 0);
    KeInitializeSpinLock(&DeviceExtension->QueueLock);
    KeInitializeSpinLock(&DeviceExtension->WaitWakeLock);
    InitializeListHead(&DeviceExtension->ReadQueue);
    InitializeListHead(&DeviceExtension->LegacyLink);

    DeviceExtension->QueueBase = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 DeviceExtension->QueueLength,
                                                 KBDCLASS_TAG);
    if (!DeviceExtension->QueueBase)
    {
        KbdClassLogError(DeviceObject,
                         KBDCLASS_LOG_NO_QUEUE_BUFFER,
                         KBDCLASS_ERRID_QUEUE_BUFFER,
                         STATUS_INSUFFICIENT_RESOURCES,
                         0,
                         NULL,
                         0);
        IoDeleteDevice(DeviceObject);
        if (Name.Buffer)
            ExFreePoolWithTag(Name.Buffer, KBDCLASS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KbdClassResetQueue(DeviceExtension);

    *ClassDeviceObject = DeviceObject;
    *FullDeviceName = Name.Buffer;
    return STATUS_SUCCESS;
}

static
VOID
KbdClassDeleteClassObject(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    if (DeviceExtension->QueueBase)
    {
        ExFreePoolWithTag(DeviceExtension->QueueBase, KBDCLASS_TAG);
        DeviceExtension->QueueBase = NULL;
    }

    IoDeleteDevice(DeviceExtension->Self);
}

/* PORT CONNECTION ************************************************************/

/**
 * @brief Hands the class device and service callback to the port.
 */
static
NTSTATUS
KbdClassSendConnect(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    CONNECT_DATA ConnectData;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    ConnectData.ClassDeviceObject = DeviceExtension->ClassDevice;
    ConnectData.ClassService = (PVOID)KbdClassServiceCallback;

    Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_KEYBOARD_CONNECT,
                                        DeviceExtension->TopPort,
                                        &ConnectData,
                                        sizeof(ConnectData),
                                        NULL,
                                        0,
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(DeviceExtension->TopPort, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

static
NTSTATUS
KbdClassWriteDeviceMap(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PCWSTR FullDeviceName)
{
    NTSTATUS Status;

    Status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                   KbdClassGlobals.BaseClassName.Buffer,
                                   FullDeviceName,
                                   REG_SZ,
                                   KbdClassGlobals.RegistryPath.Buffer,
                                   KbdClassGlobals.RegistryPath.Length + sizeof(UNICODE_NULL));
    if (!NT_SUCCESS(Status))
    {
        KbdClassLogError(DeviceExtension->Self,
                         KBDCLASS_LOG_NO_DEVICEMAP_ENTRY,
                         KBDCLASS_ERRID_DEVICEMAP,
                         Status,
                         0,
                         NULL,
                         0);
    }

    return Status;
}

/**
 * @brief Adds a port device to the grand master port list.
 */
static
NTSTATUS
KbdClassJoinGrandMaster(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_opt_ PFILE_OBJECT File)
{
    PKBDCLASS_DEVICE_EXTENSION GrandMaster = KbdClassGlobals.GrandMaster;
    PKBDCLASS_PORT_ENTRY NewList;
    PKBDCLASS_PORT_ENTRY Entry;
    ULONG Index;
    NTSTATUS Status;

    Status = KbdClassSendConnect(DeviceExtension);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    for (Index = 0; Index < KbdClassGlobals.PortCount; Index++)
    {
        if (KbdClassGlobals.PortList[Index].Free)
            break;
    }

    if (Index == KbdClassGlobals.PortCount)
    {
        NewList = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                  (KbdClassGlobals.PortCount + 1) * sizeof(*NewList),
                                  KBDCLASS_TAG);
        if (!NewList)
        {
            ExReleaseFastMutex(&KbdClassGlobals.Mutex);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (KbdClassGlobals.PortCount)
        {
            RtlCopyMemory(NewList,
                          KbdClassGlobals.PortList,
                          KbdClassGlobals.PortCount * sizeof(*NewList));
            ExFreePoolWithTag(KbdClassGlobals.PortList, KBDCLASS_TAG);
        }

        KbdClassGlobals.PortList = NewList;
        KbdClassGlobals.PortCount++;
    }

    Entry = &KbdClassGlobals.PortList[Index];
    Entry->File = File;
    Entry->Port = DeviceExtension;
    Entry->Enabled = FALSE;
    Entry->Free = FALSE;

    DeviceExtension->UnitId = Index;
    DeviceExtension->InPortList = TRUE;

    /* IRPs sent to the grand master travel down any of the port stacks */
    if (GrandMaster->Self->StackSize < DeviceExtension->Self->StackSize)
        GrandMaster->Self->StackSize = DeviceExtension->Self->StackSize;

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);
    return Status;
}

static
PFILE_OBJECT
KbdClassLeaveGrandMaster(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    PKBDCLASS_PORT_ENTRY Entry;
    PFILE_OBJECT File = NULL;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    if (DeviceExtension->InPortList)
    {
        Entry = &KbdClassGlobals.PortList[DeviceExtension->UnitId];
        File = Entry->File;
        Entry->File = NULL;
        Entry->Port = NULL;
        Entry->Free = TRUE;
        DeviceExtension->InPortList = FALSE;
    }

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);
    return File;
}

/**
 * @brief Connects a new class device to its port and publishes it.
 *
 * In grand master mode the device joins the shared port list. Otherwise the
 * device is its own class device and gets a DEVICEMAP\KeyboardClass value.
 */
static
NTSTATUS
KbdClassAddDeviceEx(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_opt_ PCWSTR FullDeviceName,
    _In_opt_ PFILE_OBJECT File)
{
    PKBDCLASS_DEVICE_EXTENSION Owner;

    if (KbdClassGlobals.ConnectOneClassToOnePort)
        Owner = DeviceExtension;
    else
        Owner = KbdClassGlobals.GrandMaster;

    DeviceExtension->ClassDevice = Owner->Self;

    if (Owner != DeviceExtension)
        return KbdClassJoinGrandMaster(DeviceExtension, File);

    /* The connect result does not decide the outcome in this mode */
    if (DeviceExtension != KbdClassGlobals.GrandMaster)
        KbdClassSendConnect(DeviceExtension);

    if (!FullDeviceName)
        return STATUS_SUCCESS;

    return KbdClassWriteDeviceMap(DeviceExtension, FullDeviceName);
}

/* PNP ADD DEVICE *************************************************************/

static
NTSTATUS
NTAPI
KbdClassAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_OBJECT DeviceObject;
    POWER_STATE PowerState;
    HANDLE KeyHandle;
    ULONG AllowDisable;
    PWSTR FullName;
    NTSTATUS Status;

    Status = KbdClassCreateClassObject(DriverObject, FALSE, &DeviceObject, &FullName);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceExtension = DeviceObject->DeviceExtension;
    DeviceExtension->TopPort = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!DeviceExtension->TopPort)
    {
        KbdClassLogError(DriverObject,
                         KBDCLASS_LOG_ATTACH_FAILED,
                         0,
                         STATUS_DEVICE_NOT_CONNECTED,
                         0,
                         NULL,
                         0);
        KbdClassDeleteClassObject(DeviceExtension);
        if (FullName)
            ExFreePoolWithTag(FullName, KBDCLASS_TAG);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    DeviceExtension->Pdo = PhysicalDeviceObject;
    DeviceExtension->PnP = TRUE;
    DeviceExtension->Started = FALSE;
    DeviceExtension->DeviceState = PowerDeviceD0;
    DeviceExtension->SystemState = PowerSystemWorking;
    DeviceExtension->MinDeviceWake = PowerDeviceUnspecified;
    DeviceExtension->MinSystemWake = PowerSystemUnspecified;
    DeviceExtension->WaitWakeEnabled = FALSE;
    DeviceExtension->WaitWakeIrp = NULL;
    DeviceExtension->ExtraWaitWakeIrp = NULL;
    DeviceExtension->WaitWakeCancelled = FALSE;
    DeviceExtension->WaitWakeRemoveState = KBDCLASS_WW_ARMED;
    DeviceExtension->LedIrp = NULL;
    DeviceExtension->LedState = KBDCLASS_LEDS_IDLE;
    DeviceExtension->AllowDisable = FALSE;

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, PowerState);

    if (NT_SUCCESS(KbdClassOpenParametersKey(KEY_READ, &KeyHandle)))
    {
        if (NT_SUCCESS(KbdClassQueryRegistryUlong(KeyHandle, L"AllowDisable", &AllowDisable)))
            DeviceExtension->AllowDisable = (AllowDisable != 0);

        ZwClose(KeyHandle);
    }

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoRegisterDeviceInterface(PhysicalDeviceObject,
                                       &GUID_DEVINTERFACE_KEYBOARD,
                                       NULL,
                                       &DeviceExtension->InterfaceName);
    if (NT_SUCCESS(Status))
    {
        Status = KbdClassAddDeviceEx(DeviceExtension, FullName, NULL);
    }
    else
    {
        IoDetachDevice(DeviceExtension->TopPort);
        DeviceExtension->TopPort = NULL;
        KbdClassDeleteClassObject(DeviceExtension);
    }

    if (FullName)
        ExFreePoolWithTag(FullName, KBDCLASS_TAG);

    return Status;
}

/* LEGACY PORTS ***************************************************************/

static
NTSTATUS
NTAPI
KbdClassCountPortValue(
    _In_ PWSTR ValueName,
    _In_ ULONG ValueType,
    _In_ PVOID ValueData,
    _In_ ULONG ValueLength,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID EntryContext)
{
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(ValueType);
    UNREFERENCED_PARAMETER(ValueData);
    UNREFERENCED_PARAMETER(ValueLength);
    UNREFERENCED_PARAMETER(EntryContext);

    (*(PULONG)Context)++;
    return STATUS_SUCCESS;
}

/**
 * @brief Counts the values under HARDWARE\DEVICEMAP\<PortBaseName>.
 */
static
VOID
KbdClassCountPorts(
    _In_ PCUNICODE_STRING PortBaseName,
    _Out_ PULONG Count)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    NTSTATUS Status;

    *Count = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].QueryRoutine = KbdClassCountPortValue;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_DEVICEMAP | RTL_REGISTRY_OPTIONAL,
                                    PortBaseName->Buffer,
                                    Table,
                                    Count,
                                    NULL);
    if (!NT_SUCCESS(Status))
        DPRINT1("Counting legacy keyboard ports failed 0x%08lx\n", Status);
}

/**
 * @brief Opens \Device\KeyboardPortN devices listed in the device map.
 *
 * @param[in,out] NextPort  First port number to try, receives the number
 *                          after the last one tried.
 */
static
NTSTATUS
KbdClassAttachLegacyPorts(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG NoMemoryErrorValue,
    _Inout_ PULONG NextPort)
{
    WCHAR PortBaseBuffer[KBDCLASS_BASE_NAME_CHARS];
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension;
    UNICODE_STRING PortBase;
    UNICODE_STRING PortName;
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT File;
    PWSTR FullName;
    ULONG PortsListed;
    ULONG DumpData;
    ULONG Index;
    NTSTATUS Status;

    RtlZeroMemory(PortBaseBuffer, sizeof(PortBaseBuffer));
    PortBase.Buffer = PortBaseBuffer;
    PortBase.Length = 0;
    PortBase.MaximumLength = sizeof(PortBaseBuffer);

    /* KeyboardClass becomes KeyboardPort */
    RtlCopyUnicodeString(&PortBase, &KbdClassGlobals.BaseClassName);
    if (PortBase.Length >= sizeof(L"Class") - sizeof(UNICODE_NULL))
        PortBase.Length -= sizeof(L"Class") - sizeof(UNICODE_NULL);
    else
        PortBase.Length = 0;

    RtlAppendUnicodeToString(&PortBase, L"Port");
    if (PortBase.MaximumLength < PortBase.Length + sizeof(UNICODE_NULL))
        return STATUS_UNSUCCESSFUL;

    PortBase.Buffer[PortBase.Length / sizeof(WCHAR)] = UNICODE_NULL;

    PortName.Length = 0;
    PortName.MaximumLength = PortBase.Length + sizeof(L"\\Device\\") + sizeof(WCHAR);
    PortName.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, PortName.MaximumLength, KBDCLASS_TAG);
    if (!PortName.Buffer)
    {
        DumpData = PortName.MaximumLength;
        KbdClassLogError(DriverObject,
                         KBDCLASS_LOG_INSUFFICIENT_RESOURCES,
                         NoMemoryErrorValue,
                         STATUS_UNSUCCESSFUL,
                         1,
                         &DumpData,
                         0);
        return STATUS_UNSUCCESSFUL;
    }

    RtlAppendUnicodeToString(&PortName, L"\\Device\\");
    RtlAppendUnicodeStringToString(&PortName, &PortBase);
    RtlAppendUnicodeToString(&PortName, L"0");

    KbdClassCountPorts(&PortBase, &PortsListed);

    for (Index = *NextPort;
         Index < KbdClassGlobals.PortsServiced && Index < PortsListed;
         Index++)
    {
        PortName.Buffer[PortName.Length / sizeof(WCHAR) - 1] = (WCHAR)(L'0' + Index);

        Status = KbdClassCreateClassObject(DriverObject, TRUE, &DeviceObject, &FullName);
        if (!NT_SUCCESS(Status))
        {
            KbdClassLogError(DriverObject,
                             KBDCLASS_LOG_INSUFFICIENT_RESOURCES,
                             KBDCLASS_ERRID_LEGACY_PORT,
                             Status,
                             0,
                             NULL,
                             0);
            continue;
        }

        DeviceExtension = DeviceObject->DeviceExtension;
        DeviceExtension->PnP = FALSE;

        Status = IoGetDeviceObjectPointer(&PortName,
                                          FILE_READ_ATTRIBUTES,
                                          &File,
                                          &DeviceExtension->TopPort);
        if (!NT_SUCCESS(Status))
        {
            KbdClassDeleteClassObject(DeviceExtension);
            if (FullName)
                ExFreePoolWithTag(FullName, KBDCLASS_TAG);
            continue;
        }

        DeviceObject->StackSize = DeviceExtension->TopPort->StackSize + 1;
        DeviceExtension->LegacyFile = File;

        Status = KbdClassAddDeviceEx(DeviceExtension, FullName, File);
        DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

        if (FullName)
            ExFreePoolWithTag(FullName, KBDCLASS_TAG);

        if (NT_SUCCESS(Status))
        {
            InsertTailList(&KbdClassGlobals.LegacyDevices, &DeviceExtension->LegacyLink);
            continue;
        }

        KbdClassLeaveGrandMaster(DeviceExtension);
        ObDereferenceObject(File);
        KbdClassDeleteClassObject(DeviceExtension);
    }

    *NextPort = Index;
    ExFreePoolWithTag(PortName.Buffer, KBDCLASS_TAG);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
KbdClassFindMorePorts(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count)
{
    ULONG NextPort = KbdClassGlobals.LegacyPortsFound;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);

    if (NT_SUCCESS(KbdClassAttachLegacyPorts(DriverObject, KBDCLASS_ERRID_LEGACY_PORT, &NextPort)))
        KbdClassGlobals.LegacyPortsFound = NextPort;
}

/* DRIVER ENTRY ***************************************************************/

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_OBJECT DeviceObject;
    PLIST_ENTRY Entry;
    PWSTR FullName;
    ULONG NextPort = 0;
    ULONG DumpData;
    NTSTATUS Status;

    ExInitializeFastMutex(&KbdClassGlobals.Mutex);
    InitializeListHead(&KbdClassGlobals.LegacyDevices);

    KbdClassGlobals.BaseClassName.Buffer = KbdClassGlobals.BaseClassNameBuffer;
    KbdClassGlobals.BaseClassName.Length = 0;
    KbdClassGlobals.BaseClassName.MaximumLength = sizeof(KbdClassGlobals.BaseClassNameBuffer) -
                                                  sizeof(UNICODE_NULL);

    KbdClassGlobals.RegistryPath.Length = 0;
    KbdClassGlobals.RegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    KbdClassGlobals.RegistryPath.Buffer = ExAllocatePool2(POOL_FLAG_PAGED,
                                                          KbdClassGlobals.RegistryPath.MaximumLength,
                                                          KBDCLASS_TAG);
    if (!KbdClassGlobals.RegistryPath.Buffer)
    {
        DumpData = KbdClassGlobals.RegistryPath.MaximumLength;
        KbdClassLogError(DriverObject,
                         KBDCLASS_LOG_INSUFFICIENT_RESOURCES,
                         KBDCLASS_ERRID_REGISTRY_PATH,
                         STATUS_UNSUCCESSFUL,
                         1,
                         &DumpData,
                         0);
        return STATUS_UNSUCCESSFUL;
    }

    RtlCopyUnicodeString(&KbdClassGlobals.RegistryPath, RegistryPath);
    KbdClassGlobals.RegistryPath.Buffer[KbdClassGlobals.RegistryPath.Length / sizeof(WCHAR)] =
        UNICODE_NULL;

    KbdClassReadConfiguration();

    if (!KbdClassGlobals.ConnectOneClassToOnePort)
    {
        Status = KbdClassCreateClassObject(DriverObject, TRUE, &DeviceObject, &FullName);
        if (!NT_SUCCESS(Status))
            goto Failure;

        DeviceExtension = DeviceObject->DeviceExtension;
        DeviceExtension->PnP = FALSE;
        KbdClassGlobals.GrandMaster = DeviceExtension;

        KbdClassAddDeviceEx(DeviceExtension, FullName, NULL);

        if (FullName)
            ExFreePoolWithTag(FullName, KBDCLASS_TAG);

        DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    }

    Status = KbdClassAttachLegacyPorts(DriverObject, KBDCLASS_ERRID_CREATE_DEVICE, &NextPort);
    if (!NT_SUCCESS(Status))
        goto Failure;

    for (Entry = KbdClassGlobals.LegacyDevices.Flink;
         Entry != &KbdClassGlobals.LegacyDevices;
         Entry = Entry->Flink)
    {
        KbdClassGlobals.LegacyPortsFound++;
    }

    IoRegisterDriverReinitialization(DriverObject, KbdClassFindMorePorts, NULL);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = KbdClassCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KbdClassClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = KbdClassCleanup;
    DriverObject->MajorFunction[IRP_MJ_READ] = KbdClassRead;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = KbdClassFlush;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KbdClassDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = KbdClassPassThrough;
    DriverObject->MajorFunction[IRP_MJ_PNP] = KbdClassPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = KbdClassPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = KbdClassSystemControl;
    DriverObject->DriverExtension->AddDevice = KbdClassAddDevice;

    return STATUS_SUCCESS;

Failure:
    if (KbdClassGlobals.PortList)
    {
        ExFreePoolWithTag(KbdClassGlobals.PortList, KBDCLASS_TAG);
        KbdClassGlobals.PortList = NULL;
        KbdClassGlobals.PortCount = 0;
    }

    if (KbdClassGlobals.GrandMaster)
    {
        KbdClassDeleteClassObject(KbdClassGlobals.GrandMaster);
        KbdClassGlobals.GrandMaster = NULL;
    }

    ExFreePoolWithTag(KbdClassGlobals.RegistryPath.Buffer, KBDCLASS_TAG);
    KbdClassGlobals.RegistryPath.Buffer = NULL;
    return Status;
}
