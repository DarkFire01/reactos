/*
 * PROJECT:     ReactOS Mouse Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, configuration and class device creation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouclass.h"

#define NDEBUG
#include <debug.h>

MOUCLASS_GLOBALS MouGlobals;

static
VOID
NTAPI
MouseFindMorePorts(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count);

static RTL_QUERY_REGISTRY_ROUTINE MouseCountPortValue;

/**
 * @brief
 * Writes an entry to the system event log.
 */
VOID
NTAPI
MouseLogError(
    _In_ PVOID IoObject,
    _In_ NTSTATUS ErrorCode,
    _In_ ULONG UniqueId,
    _In_ NTSTATUS FinalStatus,
    _In_ ULONG DumpCount,
    _In_reads_opt_(DumpCount) PULONG DumpData,
    _In_ UCHAR MajorFunction)
{
    PIO_ERROR_LOG_PACKET Entry;
    ULONG Index;

    Entry = IoAllocateErrorLogEntry(IoObject,
                                    (UCHAR)(sizeof(*Entry) + DumpCount * sizeof(ULONG)));
    if (Entry == NULL)
        return;

    Entry->MajorFunctionCode = MajorFunction;
    Entry->RetryCount = 0;
    Entry->DumpDataSize = (USHORT)(DumpCount * sizeof(ULONG));
    Entry->NumberOfStrings = 0;
    Entry->StringOffset = 0;
    Entry->ErrorCode = ErrorCode;
    Entry->UniqueErrorValue = UniqueId;
    Entry->FinalStatus = FinalStatus;
    Entry->SequenceNumber = 0;
    Entry->IoControlCode = 0;

    for (Index = 0; Index < DumpCount; Index++)
        Entry->DumpData[Index] = DumpData[Index];

    IoWriteErrorLogEntry(Entry);
}

/**
 * @brief
 * Reads the Parameters key of the service and applies the defaults.
 */
static
VOID
NTAPI
MouseReadConfiguration(VOID)
{
    RTL_QUERY_REGISTRY_TABLE Table[5];
    UNICODE_STRING ParametersPath;
    ULONG QueueEntries = MOUCLASS_DEFAULT_QUEUE_ENTRIES;
    ULONG PortsServiced = MOUCLASS_DEFAULT_PORTS_SERVICED;
    ULONG ConnectMultiplePorts = 0;

    RtlAppendUnicodeToString(&MouGlobals.BaseName, DD_POINTER_CLASS_BASE_NAME_U);

    ParametersPath.Length = 0;
    ParametersPath.MaximumLength = (USHORT)(MouGlobals.RegistryPath.Length +
                                            sizeof(L"\\Parameters"));
    ParametersPath.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                  ParametersPath.MaximumLength,
                                                  MOUCLASS_TAG);
    if (ParametersPath.Buffer != NULL)
    {
        RtlCopyUnicodeString(&ParametersPath, &MouGlobals.RegistryPath);
        RtlAppendUnicodeToString(&ParametersPath, L"\\Parameters");

        RtlZeroMemory(Table, sizeof(Table));

        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[0].Name = L"MouseDataQueueSize";
        Table[0].EntryContext = &QueueEntries;
        Table[0].DefaultType = REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;

        Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[1].Name = L"MaximumPortsServiced";
        Table[1].EntryContext = &PortsServiced;
        Table[1].DefaultType = REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;

        Table[2].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[2].Name = L"PointerDeviceBaseName";
        Table[2].EntryContext = &MouGlobals.BaseName;
        Table[2].DefaultType = REG_SZ << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;

        Table[3].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[3].Name = L"ConnectMultiplePorts";
        Table[3].EntryContext = &ConnectMultiplePorts;
        Table[3].DefaultType = REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;

        RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL,
                               ParametersPath.Buffer,
                               Table,
                               NULL,
                               NULL);

        ExFreePoolWithTag(ParametersPath.Buffer, MOUCLASS_TAG);
    }

    MouGlobals.BaseName.Buffer[MouGlobals.BaseName.Length / sizeof(WCHAR)] = UNICODE_NULL;

    if (QueueEntries == 0)
        QueueEntries = MOUCLASS_DEFAULT_QUEUE_ENTRIES;
    if (QueueEntries > MAXULONG / MOUCLASS_PACKET_SIZE)
        QueueEntries = MOUCLASS_DEFAULT_QUEUE_ENTRIES;

    MouGlobals.QueueBytes = QueueEntries * MOUCLASS_PACKET_SIZE;
    MouGlobals.PortsServiced = PortsServiced;
    MouGlobals.IsSharedMode = (ConnectMultiplePorts != 0);

    DPRINT("Queue %lu bytes, %lu legacy ports, shared %u, base %wZ\n",
           MouGlobals.QueueBytes, PortsServiced, MouGlobals.IsSharedMode, &MouGlobals.BaseName);
}

/**
 * @brief
 * Creates a class device object with its input queue.
 *
 * @param[in] IsLegacy
 * The device serves a port found through DEVICEMAP.
 *
 * @param[out] FullName
 * Receives the NT name of a named device, NULL otherwise. Free with ExFreePoolWithTag.
 */
static
NTSTATUS
NTAPI
MouseCreateClassObject(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ BOOLEAN IsLegacy,
    _Out_ PDEVICE_OBJECT *ClassObject,
    _Out_ PWSTR *FullName)
{
    PMOUCLASS_EXTENSION Extension;
    PDEVICE_OBJECT DeviceObject = NULL;
    UNICODE_STRING Name;
    UNICODE_STRING Number;
    WCHAR NumberBuffer[12];
    BOOLEAN IsNamed;
    ULONG DeviceNumber;
    ULONG DumpValue;
    NTSTATUS Status;

    *ClassObject = NULL;
    *FullName = NULL;
    RtlInitEmptyUnicodeString(&Name, NULL, 0);

    ExAcquireFastMutex(&MouGlobals.Lock);
    IsNamed = (MouGlobals.SharedClass == NULL);
    ExReleaseFastMutex(&MouGlobals.Lock);

    if (!IsNamed)
    {
        Status = IoCreateDevice(DriverObject,
                                sizeof(*Extension),
                                NULL,
                                FILE_DEVICE_MOUSE,
                                0,
                                FALSE,
                                &DeviceObject);
    }
    else
    {
        Name.MaximumLength = (USHORT)(sizeof(L"\\Device\\") + MouGlobals.BaseName.Length +
                                      sizeof(L"Legacy") + sizeof(NumberBuffer));
        Name.Buffer = ExAllocatePoolWithTag(PagedPool, Name.MaximumLength, MOUCLASS_TAG);
        if (Name.Buffer == NULL)
        {
            DumpValue = Name.MaximumLength;
            MouseLogError(DriverObject, MOUCLASS_EVT_NO_RESOURCES, 20006,
                          STATUS_UNSUCCESSFUL, 1, &DumpValue, 0);
            return STATUS_UNSUCCESSFUL;
        }

        /* Probe numbers until one is free, legacy and PnP devices share the counter */
        do
        {
            Name.Length = 0;
            RtlAppendUnicodeToString(&Name, L"\\Device\\");
            RtlAppendUnicodeStringToString(&Name, &MouGlobals.BaseName);
            if (!MouGlobals.IsSharedMode && IsLegacy)
                RtlAppendUnicodeToString(&Name, L"Legacy");

            DeviceNumber = (ULONG)InterlockedIncrement((PLONG)&MouGlobals.NextDeviceNumber) - 1;
            RtlInitEmptyUnicodeString(&Number, NumberBuffer, sizeof(NumberBuffer));
            RtlIntegerToUnicodeString(DeviceNumber, 10, &Number);
            RtlAppendUnicodeStringToString(&Name, &Number);
            Name.Buffer[Name.Length / sizeof(WCHAR)] = UNICODE_NULL;

            Status = IoCreateDevice(DriverObject,
                                    sizeof(*Extension),
                                    &Name,
                                    FILE_DEVICE_MOUSE,
                                    0,
                                    FALSE,
                                    &DeviceObject);
        } while (Status == STATUS_OBJECT_NAME_COLLISION);
    }

    if (!NT_SUCCESS(Status))
    {
        DumpValue = Name.MaximumLength;
        MouseLogError(DriverObject, MOUCLASS_EVT_NO_DEVICE_OBJECT, 20006, Status, 1, &DumpValue, 0);
        if (Name.Buffer != NULL)
            ExFreePoolWithTag(Name.Buffer, MOUCLASS_TAG);
        return Status;
    }

    DeviceObject->Flags |= DO_BUFFERED_IO;

    Extension = DeviceObject->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Self = DeviceObject;
    IoInitializeRemoveLock(&Extension->RemoveLock, MOUCLASS_TAG, 0, 0);
    KeInitializeSpinLock(&Extension->QueueLock);
    KeInitializeSpinLock(&Extension->WaitWakeLock);
    InitializeListHead(&Extension->ReadQueue);

    Extension->QueueBytes = MouGlobals.QueueBytes;
    Extension->QueueStart = ExAllocatePoolWithTag(NonPagedPool,
                                                  Extension->QueueBytes,
                                                  MOUCLASS_TAG);
    if (Extension->QueueStart == NULL)
    {
        MouseLogError(DeviceObject, MOUCLASS_EVT_NO_QUEUE_BUFFER, 20020,
                      STATUS_INSUFFICIENT_RESOURCES, 0, NULL, 0);
        if (Name.Buffer != NULL)
            ExFreePoolWithTag(Name.Buffer, MOUCLASS_TAG);
        IoDeleteDevice(DeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MouseResetQueue(Extension);

    *ClassObject = DeviceObject;
    *FullName = Name.Buffer;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees a class device object that never made it into service.
 */
static
VOID
NTAPI
MouseDeleteClassObject(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    if (Extension->QueueStart != NULL)
    {
        ExFreePoolWithTag(Extension->QueueStart, MOUCLASS_TAG);
        Extension->QueueStart = NULL;
    }

    IoDeleteDevice(Extension->Self);
}

/**
 * @brief
 * Hands the class service callback to the port with IOCTL_INTERNAL_MOUSE_CONNECT.
 */
static
NTSTATUS
NTAPI
MouseConnectToPort(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    CONNECT_DATA ConnectData;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    ConnectData.ClassDeviceObject = Extension->ClassDevice;
    ConnectData.ClassService = (PVOID)MouseServiceCallback;

    Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_MOUSE_CONNECT,
                                        Extension->LowerDevice,
                                        &ConnectData,
                                        sizeof(ConnectData),
                                        NULL,
                                        0,
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(Extension->LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

/**
 * @brief
 * Publishes a class device under HKLM\HARDWARE\DEVICEMAP\PointerClass.
 */
static
NTSTATUS
NTAPI
MouseWriteDeviceMap(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_opt_ PCWSTR FullName)
{
    NTSTATUS Status;

    if (FullName == NULL)
        return STATUS_SUCCESS;

    Status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                   MouGlobals.BaseName.Buffer,
                                   FullName,
                                   REG_SZ,
                                   MouGlobals.RegistryPath.Buffer,
                                   (ULONG)(MouGlobals.RegistryPath.Length + sizeof(UNICODE_NULL)));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to publish %S, 0x%08lx\n", FullName, Status);
        MouseLogError(Extension->Self, MOUCLASS_EVT_DEVICEMAP_FAILED, 20014, Status, 0, NULL, 0);
    }

    return Status;
}

/**
 * @brief
 * Records a port device in the table served by the shared class device.
 */
static
NTSTATUS
NTAPI
MouseAddSharedPort(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_opt_ PFILE_OBJECT FileObject)
{
    PMOUCLASS_EXTENSION Shared = MouGlobals.SharedClass;
    PMOUCLASS_PORT NewPorts;
    ULONG Index;

    ExAcquireFastMutex(&MouGlobals.Lock);

    for (Index = 0; Index < MouGlobals.PortCount; Index++)
    {
        if (MouGlobals.Ports[Index].Free)
        {
            MouGlobals.Ports[Index].Free = FALSE;
            break;
        }
    }

    if (Index == MouGlobals.PortCount)
    {
        NewPorts = ExAllocatePoolZero(NonPagedPool,
                                      (MouGlobals.PortCount + 1) * sizeof(*NewPorts),
                                      MOUCLASS_TAG);
        if (NewPorts == NULL)
        {
            ExReleaseFastMutex(&MouGlobals.Lock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (MouGlobals.PortCount != 0)
        {
            RtlCopyMemory(NewPorts, MouGlobals.Ports, MouGlobals.PortCount * sizeof(*NewPorts));
            ExFreePoolWithTag(MouGlobals.Ports, MOUCLASS_TAG);
        }

        MouGlobals.Ports = NewPorts;
        MouGlobals.PortCount++;
    }

    Extension->UnitId = Index;
    MouGlobals.Ports[Index].Port = Extension;
    MouGlobals.Ports[Index].FileObject = FileObject;

    /* Requests through the shared device must reach the deepest port */
    if (Shared->Self->StackSize < Extension->Self->StackSize)
        Shared->Self->StackSize = Extension->Self->StackSize;

    ExReleaseFastMutex(&MouGlobals.Lock);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Binds a new class extension to its class device and connects it to its port.
 */
static
NTSTATUS
NTAPI
MouseAttachToClass(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_opt_ PCWSTR FullName,
    _In_opt_ PFILE_OBJECT FileObject)
{
    PMOUCLASS_EXTENSION ClassExtension;
    NTSTATUS ConnectStatus;
    NTSTATUS Status;

    ClassExtension = MouGlobals.IsSharedMode ? MouGlobals.SharedClass : Extension;
    Extension->ClassDevice = ClassExtension->Self;

    if (Extension == MouGlobals.SharedClass)
        return MouseWriteDeviceMap(Extension, FullName);

    if (ClassExtension != Extension)
    {
        ConnectStatus = MouseConnectToPort(Extension);
        Status = MouseAddSharedPort(Extension, FileObject);
        if (!NT_SUCCESS(Status))
            return Status;

        return ConnectStatus;
    }

    /* A per port class device publishes itself even if the port refused the connect */
    MouseConnectToPort(Extension);
    return MouseWriteDeviceMap(Extension, FullName);
}

/**
 * @brief
 * Creates the class device for a newly enumerated mouse stack.
 */
NTSTATUS
NTAPI
MouseAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PMOUCLASS_EXTENSION Extension;
    PDEVICE_OBJECT ClassObject;
    POWER_STATE PowerState;
    PWSTR FullName;
    NTSTATUS Status;

    Status = MouseCreateClassObject(DriverObject, FALSE, &ClassObject, &FullName);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = ClassObject->DeviceExtension;
    Extension->LowerDevice = IoAttachDeviceToDeviceStack(ClassObject, PhysicalDeviceObject);
    if (Extension->LowerDevice == NULL)
    {
        MouseLogError(DriverObject, MOUCLASS_EVT_ATTACH_FAILED, 0,
                      STATUS_DEVICE_NOT_CONNECTED, 0, NULL, 0);
        if (FullName != NULL)
            ExFreePoolWithTag(FullName, MOUCLASS_TAG);
        MouseDeleteClassObject(Extension);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* Wake capabilities stay unknown until start, the extension is zeroed */
    Extension->Pdo = PhysicalDeviceObject;
    Extension->IsPnp = TRUE;
    Extension->DeviceState = PowerDeviceD0;
    Extension->SystemState = PowerSystemWorking;
    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(ClassObject, DevicePowerState, PowerState);
    Extension->WaitWakeRemoval = MOUCLASS_WW_IDLE;

    ClassObject->Flags |= DO_POWER_PAGABLE;
    ClassObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoRegisterDeviceInterface(PhysicalDeviceObject,
                                       &GUID_DEVINTERFACE_MOUSE,
                                       NULL,
                                       &Extension->InterfaceName);
    if (NT_SUCCESS(Status))
    {
        /* On failure the device stays attached, the remove IRP cleans it up */
        Status = MouseAttachToClass(Extension, FullName, NULL);
    }
    else
    {
        IoDetachDevice(Extension->LowerDevice);
        Extension->LowerDevice = NULL;
        MouseDeleteClassObject(Extension);
    }

    if (FullName != NULL)
        ExFreePoolWithTag(FullName, MOUCLASS_TAG);

    return Status;
}

static
NTSTATUS
NTAPI
MouseCountPortValue(
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
 * @brief
 * Undoes a legacy class device whose port could not be attached.
 */
static
VOID
NTAPI
MouseDropLegacyPort(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_ PFILE_OBJECT PortFile)
{
    PMOUCLASS_PORT Entry;
    PFILE_OBJECT FileObject = PortFile;

    if (MouGlobals.SharedClass != NULL)
    {
        ExAcquireFastMutex(&MouGlobals.Lock);
        if (Extension->UnitId < MouGlobals.PortCount &&
            MouGlobals.Ports[Extension->UnitId].Port == Extension)
        {
            Entry = &MouGlobals.Ports[Extension->UnitId];
            FileObject = Entry->FileObject;
            Entry->FileObject = NULL;
            Entry->Port = NULL;
            Entry->Free = TRUE;
        }
        ExReleaseFastMutex(&MouGlobals.Lock);
    }
    else
    {
        Extension->LegacyFile = NULL;
    }

    if (FileObject != NULL)
        ObDereferenceObject(FileObject);

    MouseDeleteClassObject(Extension);
}

/**
 * @brief
 * Creates class devices for legacy ports listed under DEVICEMAP\PointerPort.
 */
static
NTSTATUS
NTAPI
MouseScanLegacyPorts(
    _In_ PDRIVER_OBJECT DriverObject)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    UNICODE_STRING PortBase;
    UNICODE_STRING PortName;
    UNICODE_STRING Number;
    WCHAR PortBaseBuffer[MOUCLASS_BASE_NAME_CHARS + 8];
    WCHAR NumberBuffer[12];
    PMOUCLASS_EXTENSION Extension;
    PDEVICE_OBJECT ClassObject;
    PFILE_OBJECT PortFile;
    PWSTR FullName;
    ULONG PortValues = 0;
    ULONG DumpValue;
    ULONG Index;
    NTSTATUS Status;

    /* The port base name is the class base name with Class replaced by Port */
    RtlInitEmptyUnicodeString(&PortBase, PortBaseBuffer, sizeof(PortBaseBuffer) - sizeof(WCHAR));
    RtlCopyUnicodeString(&PortBase, &MouGlobals.BaseName);
    if (PortBase.Length >= sizeof(L"Class") - sizeof(WCHAR))
        PortBase.Length -= (USHORT)(sizeof(L"Class") - sizeof(WCHAR));
    RtlAppendUnicodeToString(&PortBase, L"Port");
    PortBase.Buffer[PortBase.Length / sizeof(WCHAR)] = UNICODE_NULL;

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].QueryRoutine = MouseCountPortValue;
    Status = RtlQueryRegistryValues(RTL_REGISTRY_DEVICEMAP | RTL_REGISTRY_OPTIONAL,
                                    PortBase.Buffer,
                                    Table,
                                    &PortValues,
                                    NULL);
    if (!NT_SUCCESS(Status))
        DPRINT("No %wZ ports, 0x%08lx\n", &PortBase, Status);

    PortName.Length = 0;
    PortName.MaximumLength = (USHORT)(sizeof(L"\\Device\\") + PortBase.Length +
                                      sizeof(NumberBuffer));
    PortName.Buffer = ExAllocatePoolWithTag(PagedPool, PortName.MaximumLength, MOUCLASS_TAG);
    if (PortName.Buffer == NULL)
    {
        DumpValue = PortName.MaximumLength;
        MouseLogError(DriverObject, MOUCLASS_EVT_NO_RESOURCES, 20008,
                      STATUS_UNSUCCESSFUL, 1, &DumpValue, 0);
        return STATUS_UNSUCCESSFUL;
    }

    for (Index = MouGlobals.LegacyPortCount;
         Index < MouGlobals.PortsServiced && Index < PortValues;
         Index++)
    {
        PortName.Length = 0;
        RtlAppendUnicodeToString(&PortName, L"\\Device\\");
        RtlAppendUnicodeStringToString(&PortName, &PortBase);
        RtlInitEmptyUnicodeString(&Number, NumberBuffer, sizeof(NumberBuffer));
        RtlIntegerToUnicodeString(Index, 10, &Number);
        RtlAppendUnicodeStringToString(&PortName, &Number);

        Status = MouseCreateClassObject(DriverObject, TRUE, &ClassObject, &FullName);
        if (!NT_SUCCESS(Status))
        {
            MouseLogError(DriverObject, MOUCLASS_EVT_NO_RESOURCES, 20008, Status, 0, NULL, 0);
            continue;
        }

        Extension = ClassObject->DeviceExtension;
        Extension->IsPnp = FALSE;
        ClassObject->Flags &= ~DO_DEVICE_INITIALIZING;

        Status = IoGetDeviceObjectPointer(&PortName,
                                          FILE_READ_ATTRIBUTES,
                                          &PortFile,
                                          &Extension->LowerDevice);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("%wZ is not there, 0x%08lx\n", &PortName, Status);
            if (FullName != NULL)
                ExFreePoolWithTag(FullName, MOUCLASS_TAG);
            MouseDeleteClassObject(Extension);
            continue;
        }

        ClassObject->StackSize = Extension->LowerDevice->StackSize + 1;

        Status = MouseAttachToClass(Extension, FullName, PortFile);
        if (!MouGlobals.IsSharedMode)
            Extension->LegacyFile = PortFile;

        if (FullName != NULL)
            ExFreePoolWithTag(FullName, MOUCLASS_TAG);

        if (!NT_SUCCESS(Status))
        {
            MouseDropLegacyPort(Extension, PortFile);
            continue;
        }

        InsertTailList(&MouGlobals.LegacyList, &Extension->LegacyLink);
        MouGlobals.LegacyPortCount++;
    }

    ExFreePoolWithTag(PortName.Buffer, MOUCLASS_TAG);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Picks up legacy ports that showed up after DriverEntry.
 */
static
VOID
NTAPI
MouseFindMorePorts(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);

    MouseScanLegacyPorts(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    PMOUCLASS_EXTENSION Shared;
    PDEVICE_OBJECT SharedObject;
    PWSTR FullName;
    ULONG DumpValue;
    NTSTATUS Status;

    ExInitializeFastMutex(&MouGlobals.Lock);
    InitializeListHead(&MouGlobals.LegacyList);
    RtlInitEmptyUnicodeString(&MouGlobals.BaseName,
                              MouGlobals.BaseNameBuffer,
                              sizeof(MouGlobals.BaseNameBuffer) - sizeof(WCHAR));

    MouGlobals.RegistryPath.Length = RegistryPath->Length;
    MouGlobals.RegistryPath.MaximumLength = (USHORT)(RegistryPath->Length + sizeof(UNICODE_NULL));
    MouGlobals.RegistryPath.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                           MouGlobals.RegistryPath.MaximumLength,
                                                           MOUCLASS_TAG);
    if (MouGlobals.RegistryPath.Buffer == NULL)
    {
        DumpValue = MouGlobals.RegistryPath.MaximumLength;
        MouseLogError(DriverObject, MOUCLASS_EVT_NO_RESOURCES, 20002,
                      STATUS_UNSUCCESSFUL, 1, &DumpValue, 0);
        return STATUS_UNSUCCESSFUL;
    }

    RtlCopyMemory(MouGlobals.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
    MouGlobals.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = UNICODE_NULL;

    MouseReadConfiguration();

    if (MouGlobals.IsSharedMode)
    {
        Status = MouseCreateClassObject(DriverObject, TRUE, &SharedObject, &FullName);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        Shared = SharedObject->DeviceExtension;
        MouGlobals.SharedClass = Shared;
        Shared->IsPnp = FALSE;
        MouseAttachToClass(Shared, FullName, NULL);

        if (FullName != NULL)
            ExFreePoolWithTag(FullName, MOUCLASS_TAG);
        SharedObject->Flags &= ~DO_DEVICE_INITIALIZING;
    }

    Status = MouseScanLegacyPorts(DriverObject);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    IoRegisterDriverReinitialization(DriverObject, MouseFindMorePorts, NULL);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = MouseCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MouseClose;
    DriverObject->MajorFunction[IRP_MJ_READ] = MouseRead;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = MouseFlush;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MouseDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = MousePassThrough;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = MouseCleanup;
    DriverObject->MajorFunction[IRP_MJ_PNP] = MousePnp;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = MouseSystemControl;
    DriverObject->MajorFunction[IRP_MJ_POWER] = MousePower;
    DriverObject->DriverExtension->AddDevice = MouseAddDevice;

    return STATUS_SUCCESS;

Cleanup:
    ExFreePoolWithTag(MouGlobals.RegistryPath.Buffer, MOUCLASS_TAG);
    MouGlobals.RegistryPath.Buffer = NULL;

    if (MouGlobals.Ports != NULL)
    {
        ExFreePoolWithTag(MouGlobals.Ports, MOUCLASS_TAG);
        MouGlobals.Ports = NULL;
        MouGlobals.PortCount = 0;
    }

    if (MouGlobals.SharedClass != NULL)
    {
        MouseDeleteClassObject(MouGlobals.SharedClass);
        MouGlobals.SharedClass = NULL;
    }

    return Status;
}
