/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minidriver registration, device creation and IRP dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Every minidriver that registered with us, guarded by HidClassDriverLock */
static LIST_ENTRY HidClassDriverList = { &HidClassDriverList, &HidClassDriverList };
static KSPIN_LOCK HidClassDriverLock;

/* Number handed to the next FDO, also its bus number */
static LONG HidClassNextId;

/* Key of the driver object extension, any unique address does */
static const CHAR HidClassClientId[] = "HIDCLASS";

static DRIVER_ADD_DEVICE HidClassAddDevice;
static DRIVER_UNLOAD HidClassDriverUnload;
static DRIVER_DISPATCH HidClassDispatch;

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&HidClassDriverLock);
    InterlockedExchange(&HidClassNextId, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finds the registration of a minidriver and takes a reference on it.
 *
 * @return
 * The driver extension, or NULL when the driver never registered.
 */
PHIDCLASS_DRIVER_EXTENSION
NTAPI
HidClassReferenceDriver(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PHIDCLASS_DRIVER_EXTENSION Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HidClassDriverLock, &OldIrql);
    for (Entry = HidClassDriverList.Flink; Entry != &HidClassDriverList; Entry = Entry->Flink)
    {
        PHIDCLASS_DRIVER_EXTENSION DriverExtension;

        DriverExtension = CONTAINING_RECORD(Entry, HIDCLASS_DRIVER_EXTENSION, Link);
        if (DriverExtension->DriverObject == DriverObject)
        {
            DriverExtension->ReferenceCount++;
            Found = DriverExtension;
            break;
        }
    }
    KeReleaseSpinLock(&HidClassDriverLock, OldIrql);

    if (!Found)
        DPRINT1("HIDCLASS: driver %p is not registered\n", DriverObject);

    return Found;
}

/**
 * @brief
 * Drops a reference on a minidriver registration. The registration is
 * unlinked once the count goes below zero, which only unload does.
 */
VOID
NTAPI
HidClassDereferenceDriver(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PHIDCLASS_DRIVER_EXTENSION Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HidClassDriverLock, &OldIrql);
    for (Entry = HidClassDriverList.Flink; Entry != &HidClassDriverList; Entry = Entry->Flink)
    {
        PHIDCLASS_DRIVER_EXTENSION DriverExtension;

        DriverExtension = CONTAINING_RECORD(Entry, HIDCLASS_DRIVER_EXTENSION, Link);
        if (DriverExtension->DriverObject != DriverObject)
            continue;

        if (--DriverExtension->ReferenceCount < 0)
        {
            RemoveEntryList(&DriverExtension->Link);
            Found = DriverExtension;
        }
        break;
    }
    KeReleaseSpinLock(&HidClassDriverLock, OldIrql);

    if (Found && Found->RegistryPath.Buffer)
    {
        ExFreePoolWithTag(Found->RegistryPath.Buffer, HIDCLASS_TAG);
        Found->RegistryPath.Buffer = NULL;
    }
}

static
VOID
NTAPI
HidClassDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;
    PDRIVER_UNLOAD Unload;

    DriverExtension = IoGetDriverObjectExtension(DriverObject, (PVOID)HidClassClientId);
    Unload = DriverExtension ? DriverExtension->DriverUnload : NULL;

    HidClassDereferenceDriver(DriverObject);

    if (Unload)
        Unload(DriverObject);
}

NTSTATUS
NTAPI
HidRegisterMinidriver(
    _In_ PHID_MINIDRIVER_REGISTRATION MinidriverRegistration)
{
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;
    PDRIVER_OBJECT DriverObject;
    PLIST_ENTRY Entry;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (MinidriverRegistration->Revision > HID_REVISION)
    {
        DPRINT1("HIDCLASS: revision %lu is not supported\n", MinidriverRegistration->Revision);
        return STATUS_REVISION_MISMATCH;
    }

    DriverObject = MinidriverRegistration->DriverObject;
    Status = IoAllocateDriverObjectExtension(DriverObject,
                                             (PVOID)HidClassClientId,
                                             sizeof(*DriverExtension),
                                             (PVOID *)&DriverExtension);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(DriverExtension, sizeof(*DriverExtension));
    DriverExtension->DriverObject = DriverObject;
    DriverExtension->DeviceExtensionSize = MinidriverRegistration->DeviceExtensionSize;
    DriverExtension->DevicesArePolled = MinidriverRegistration->DevicesArePolled;

    DriverExtension->RegistryPath.MaximumLength = MinidriverRegistration->RegistryPath->Length + sizeof(WCHAR);
    DriverExtension->RegistryPath.Buffer = ExAllocatePoolZero(NonPagedPool,
                                                              DriverExtension->RegistryPath.MaximumLength,
                                                              HIDCLASS_TAG);
    if (!DriverExtension->RegistryPath.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyUnicodeString(&DriverExtension->RegistryPath, MinidriverRegistration->RegistryPath);

    /* Keep the minidriver handlers, every request we own reaches it through these */
    RtlCopyMemory(DriverExtension->MajorFunction,
                  DriverObject->MajorFunction,
                  sizeof(DriverExtension->MajorFunction));

    DriverObject->MajorFunction[IRP_MJ_CREATE] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_READ] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_POWER] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HidClassDispatch;
    DriverObject->MajorFunction[IRP_MJ_PNP] = HidClassDispatch;

    DriverExtension->AddDevice = DriverObject->DriverExtension->AddDevice;
    DriverObject->DriverExtension->AddDevice = HidClassAddDevice;

    DriverExtension->DriverUnload = DriverObject->DriverUnload;
    DriverObject->DriverUnload = HidClassDriverUnload;

    DriverExtension->ReferenceCount = 0;

    KeAcquireSpinLock(&HidClassDriverLock, &OldIrql);
    for (Entry = HidClassDriverList.Flink; Entry != &HidClassDriverList; Entry = Entry->Flink)
    {
        if (Entry == &DriverExtension->Link)
        {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
            break;
        }
    }
    if (NT_SUCCESS(Status))
        InsertHeadList(&HidClassDriverList, &DriverExtension->Link);
    KeReleaseSpinLock(&HidClassDriverLock, OldIrql);

    return Status;
}

/**
 * @brief
 * Builds the FDO of a HID device and lets the minidriver finish it.
 */
static
NTSTATUS
NTAPI
HidClassAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = NULL;
    PHIDCLASS_FDO_EXTENSION Fdo;
    PDEVICE_OBJECT DeviceObject = NULL;
    UNICODE_STRING DeviceName;
    ULONG ExtensionSize;
    PWSTR NameBuffer;
    ULONG HidId;
    ULONG Value;
    NTSTATUS Status;

    DriverExtension = HidClassReferenceDriver(DriverObject);
    if (!DriverExtension)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    NameBuffer = ExAllocatePoolZero(NonPagedPool, 22 * sizeof(WCHAR), HIDCLASS_TAG);
    if (!NameBuffer)
    {
        HidClassDereferenceDriver(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    HidId = (ULONG)InterlockedIncrement(&HidClassNextId) - 1;
    Status = RtlStringCbPrintfW(NameBuffer, 22 * sizeof(WCHAR), L"\\Device\\_HID%08x", HidId);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlInitUnicodeString(&DeviceName, NameBuffer);

    ExtensionSize = HIDCLASS_EXTENSION_SIZE + DriverExtension->DeviceExtensionSize;
    Status = IoCreateDevice(DriverObject,
                            ExtensionSize,
                            &DeviceName,
                            FILE_DEVICE_UNKNOWN,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: IoCreateDevice failed %lx\n", Status);
        DeviceObject = NULL;
        goto Cleanup;
    }

    ObReferenceObject(DeviceObject);

    DeviceExtension = DeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExtension, ExtensionSize);
    DeviceExtension->IsClientPdo = FALSE;
    DeviceExtension->Signature = HIDCLASS_EXTENSION_SIGNATURE;
    DeviceExtension->Hid.MiniDeviceExtension = (PUCHAR)DeviceExtension + HIDCLASS_EXTENSION_SIZE;
    DeviceExtension->Hid.PhysicalDeviceObject = PhysicalDeviceObject;
    DeviceExtension->Hid.NextDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!DeviceExtension->Hid.NextDeviceObject)
    {
        Status = STATUS_NO_SUCH_DEVICE;
        goto Cleanup;
    }

    /* One more location for the minidriver, it runs on this same device object */
    DeviceObject->StackSize++;

    Fdo = &DeviceExtension->Fdo;
    Fdo->DeviceObject = DeviceObject;
    Fdo->DriverExtension = DriverExtension;
    Fdo->HidId = HidId;
    Fdo->DeviceName = DeviceName;
    Fdo->OpenCount = 1;
    Fdo->IsPresent = 1;
    Fdo->ReadsState = HidClassReadsNone;
    Fdo->IoState = HidClassIoFail;
    Fdo->SystemPowerState = PowerSystemWorking;
    Fdo->DevicePowerState = PowerDeviceD0;
    Fdo->IdleState = HidClassIdleActive;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(PhysicalDeviceObject, L"ComboHardwareIdV2Enabled", &Value)))
        Fdo->ComboHardwareIdV2 = (Value != 0);

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(PhysicalDeviceObject, L"DeviceResetNotificationEnabled", &Value)))
        Fdo->DeviceResetSupported = (Value != 0);

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(PhysicalDeviceObject, L"WriteReportExSupported", &Value)))
        Fdo->WriteReportExSupported = (Value != 0);

    KeInitializeSpinLock(&Fdo->ResetLock);
    InitializeListHead(&Fdo->ResetIrps);

    KeInitializeEvent(&Fdo->IdleIrpDone, NotificationEvent, TRUE);
    KeInitializeEvent(&Fdo->PowerDone, NotificationEvent, TRUE);
    KeInitializeEvent(&Fdo->IoDrained, NotificationEvent, TRUE);
    Fdo->IdleIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!Fdo->IdleIrp)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    KeInitializeSpinLock(&Fdo->ClientPdoLock);
    KeInitializeSpinLock(&Fdo->OpenCountLock);
    KeInitializeSpinLock(&Fdo->IoLock);
    KeInitializeSpinLock(&Fdo->WaitWakeLock);
    InitializeListHead(&Fdo->DelayedIrps);
    InitializeListHead(&Fdo->CollectionWaitWakeIrps);
    IoInitializeRemoveLock(&Fdo->RemoveLock, HIDCLASS_TAG, 0, 0);

    DeviceObject->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    if (DriverExtension->DevicesArePolled)
        HidClassSetDeviceRegistryValue(PhysicalDeviceObject, L"PolledDevice", 1);

    Status = DriverExtension->AddDevice(DriverObject, DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: minidriver AddDevice failed %lx\n", Status);
        goto Cleanup;
    }

    return STATUS_SUCCESS;

Cleanup:
    if (DeviceObject)
    {
        if (DeviceExtension->Fdo.IdleIrp)
            IoFreeIrp(DeviceExtension->Fdo.IdleIrp);

        if (DeviceExtension->Hid.NextDeviceObject)
            IoDetachDevice(DeviceExtension->Hid.NextDeviceObject);

        ObDereferenceObject(DeviceObject);
        IoDeleteDevice(DeviceObject);
    }

    HidClassDereferenceDriver(DriverObject);
    ExFreePoolWithTag(NameBuffer, HIDCLASS_TAG);
    return Status;
}

/**
 * @brief
 * Hands an IRP whose next stack location is ready to the minidriver
 * dispatch routine on the FDO.
 */
NTSTATUS
NTAPI
HidClassCallMinidriver(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;

    if (Irp->CurrentLocation <= 1)
        KeBugCheckEx(NO_MORE_IRP_STACK_LOCATIONS, (ULONG_PTR)Irp, 0, 0, 0);

    IoSetNextIrpStackLocation(Irp);
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    IoStack->DeviceObject = Fdo->DeviceObject;

    return Fdo->DriverExtension->MajorFunction[IoStack->MajorFunction](Fdo->DeviceObject, Irp);
}

static
NTSTATUS
NTAPI
HidClassSynchronousCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Calls the minidriver and waits up to five seconds for the IRP. A request
 * that takes longer is canceled and fails with STATUS_IO_TIMEOUT.
 */
NTSTATUS
NTAPI
HidClassCallMinidriverSynchronous(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, HidClassSynchronousCompletion, &Event, TRUE, TRUE, TRUE);

    Status = HidClassCallMinidriver(Fdo, Irp);
    if (Status != STATUS_PENDING)
        return Status;

    Timeout.QuadPart = HIDCLASS_SYNC_TIMEOUT;
    if (KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        DPRINT1("HIDCLASS: minidriver request %p timed out\n", Irp);
        IoCancelIrp(Irp);
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        if (Irp->IoStatus.Status == STATUS_CANCELLED)
            Irp->IoStatus.Status = STATUS_IO_TIMEOUT;
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
NTAPI
HidClassForwardToMinidriver(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return HidClassCallMinidriver(Fdo, Irp);
}

/**
 * @brief
 * Handles requests nobody claims. The FDO sends them on, a collection PDO
 * completes them with whatever status they carry.
 */
NTSTATUS
NTAPI
HidClassPassOrComplete(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    NTSTATUS Status;

    if (!DeviceExtension->IsClientPdo)
        return HidClassForwardToMinidriver(&DeviceExtension->Fdo, Irp);

    Status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
NTAPI
HidClassDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IoStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            return HidClassCreate(DeviceExtension, Irp);

        case IRP_MJ_CLOSE:
            return HidClassClose(DeviceExtension, Irp);

        case IRP_MJ_READ:
            if (!DeviceExtension->IsClientPdo)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER_1;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER_1;
            }
            return HidClassRead(&DeviceExtension->Pdo, Irp);

        case IRP_MJ_WRITE:
            return HidClassWrite(DeviceExtension, Irp);

        case IRP_MJ_DEVICE_CONTROL:
            return HidClassDeviceControl(DeviceExtension, Irp);

        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
            return HidClassInternalDeviceControl(DeviceExtension, Irp);

        case IRP_MJ_POWER:
            if (DeviceExtension->IsClientPdo)
                return HidClassPdoPower(&DeviceExtension->Pdo, Irp);
            return HidClassFdoPower(&DeviceExtension->Fdo, Irp);

        case IRP_MJ_SYSTEM_CONTROL:
            return HidClassSystemControl(DeviceExtension, Irp);

        case IRP_MJ_PNP:
            if (DeviceExtension->IsClientPdo)
                return HidClassPdoPnp(&DeviceExtension->Pdo, Irp);
            return HidClassFdoPnp(&DeviceExtension->Fdo, Irp);

        default:
            return HidClassPassOrComplete(DeviceExtension, Irp);
    }
}

/**
 * @brief
 * Lets a minidriver report that its device left or came back. Collection
 * PDOs are reported again on the next bus relations query.
 */
NTSTATUS
NTAPI
HidNotifyPresence(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN IsPresent)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    LONG NewValue = IsPresent ? 1 : 0;

    if (InterlockedCompareExchange(&DeviceExtension->Fdo.IsPresent, NewValue, !NewValue) != NewValue)
        IoInvalidateDeviceRelations(DeviceExtension->Hid.PhysicalDeviceObject, BusRelations);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidClassSetDeviceRegistryValue(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value)
{
    UNICODE_STRING Name;
    HANDLE KeyHandle;
    NTSTATUS Status;

    Status = IoOpenDeviceRegistryKey(DeviceObject, PLUGPLAY_REGKEY_DEVICE, KEY_ALL_ACCESS, &KeyHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwSetValueKey(KeyHandle, &Name, 0, REG_DWORD, &Value, sizeof(Value));
    ZwClose(KeyHandle);
    return Status;
}

/**
 * @brief
 * Reads a value from the hardware key of a device. REG_DWORD values are
 * returned as is, REG_BINARY values as their first byte.
 */
NTSTATUS
NTAPI
HidClassQueryDeviceRegistryValue(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING Name;
    HANDLE KeyHandle;
    ULONG Length;
    NTSTATUS Status;

    *Value = 0;

    Status = IoOpenDeviceRegistryKey(DeviceObject, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &KeyHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwQueryValueKey(KeyHandle, &Name, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    ZwClose(KeyHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Info->Type == REG_DWORD && Info->DataLength >= sizeof(ULONG))
        *Value = *(PULONG)Info->Data;
    else if (Info->Type == REG_BINARY && Info->DataLength >= 1)
        *Value = Info->Data[0];
    else
        return STATUS_OBJECT_TYPE_MISMATCH;

    return STATUS_SUCCESS;
}
