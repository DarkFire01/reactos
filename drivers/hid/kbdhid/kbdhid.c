/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper driver entry, open and close handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdhid.h"

#define NDEBUG
#include <debug.h>

KBDHID_GLOBALS KbdHidGlobals;

/* Dump2Key index to set 1 scan code, bit 7 set means the code carries an E0 prefix */
static const UCHAR KbdHidDumpKeyTable[] =
{
    0x00, 0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x7D, 0x0E,
    0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x00, 0x3A, 0x1E,
    0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B, 0x1C, 0x2A, 0x00, 0x2C, 0x2D,
    0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x73, 0x36, 0x1D, 0x00, 0x38, 0x39, 0xB8, 0x00,
    0x9D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD2, 0xD3, 0x00, 0x00, 0xCB,
    0xC7, 0xCF, 0x00, 0xC8, 0xD0, 0xC9, 0xD1, 0x00, 0x00, 0xCD, 0x45, 0x47, 0x4B, 0x4F, 0x00, 0xB5,
    0x48, 0x4C, 0x50, 0x52, 0x37, 0x49, 0x4D, 0x51, 0x53, 0x4A, 0x4E, 0x00, 0x9C, 0x00, 0x01, 0x00,
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0x00, 0x46, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x7B, 0x79, 0x70
};

/* Dump2Key index of Print Screen, which is matched by both of its scan codes */
#define KBDHID_DUMP_KEY_PRINT_SCREEN    124
#define KBDHID_SCAN_PRINT_SCREEN        0xB7
#define KBDHID_SCAN_SYSRQ               0x54

static
NTSTATUS
KbdHidOpenSubkey(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE RootHandle,
    _In_ PUNICODE_STRING Name)
{
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes,
                               Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               RootHandle,
                               NULL);

    return ZwOpenKey(KeyHandle, KEY_READ, &ObjectAttributes);
}

static
VOID
KbdHidReadCrashDumpSettings(
    _In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR Suffix[] = L"\\Crashdump";
    RTL_QUERY_REGISTRY_TABLE Table[3];
    UNICODE_STRING Path;
    ULONG Modifiers = 0;
    ULONG Key = 0;
    ULONG DefaultValue = 0;
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;

    Path.Length = 0;
    Path.MaximumLength = RegistryPath->Length + sizeof(Suffix);
    Path.Buffer = ExAllocatePoolZero(PagedPool, Path.MaximumLength, KBDHID_TAG);
    if (Path.Buffer != NULL)
    {
        RtlAppendUnicodeStringToString(&Path, RegistryPath);
        RtlAppendUnicodeToString(&Path, Suffix);

        RtlZeroMemory(Table, sizeof(Table));
        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[0].Name = L"Dump1Keys";
        Table[0].EntryContext = &Modifiers;
        Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[0].DefaultData = &DefaultValue;
        Table[0].DefaultLength = sizeof(DefaultValue);
        Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[1].Name = L"Dump2Key";
        Table[1].EntryContext = &Key;
        Table[1].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[1].DefaultData = &DefaultValue;
        Table[1].DefaultLength = sizeof(DefaultValue);

        Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL,
                                        Path.Buffer,
                                        Table,
                                        NULL,
                                        NULL);
        ExFreePoolWithTag(Path.Buffer, KBDHID_TAG);
    }

    if (!NT_SUCCESS(Status))
    {
        Modifiers = 0;
        Key = 0;
    }

    KbdHidGlobals.DumpModifiers = Modifiers;
    if (Modifiers == 0)
        return;

    if (Key == KBDHID_DUMP_KEY_PRINT_SCREEN)
    {
        KbdHidGlobals.DumpKey = KBDHID_SCAN_PRINT_SCREEN;
        KbdHidGlobals.DumpKeyAlt = KBDHID_SCAN_SYSRQ;
    }
    else
    {
        KbdHidGlobals.DumpKey = (Key < ARRAYSIZE(KbdHidDumpKeyTable)) ? KbdHidDumpKeyTable[Key] : 0;
        KbdHidGlobals.DumpKeyAlt = 0;
    }
}

static
VOID
KbdHidReadServiceParameters(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING ParametersName = RTL_CONSTANT_STRING(L"Parameters");
    RTL_QUERY_REGISTRY_TABLE Table[4];
    HANDLE ServiceKey;
    HANDLE ParametersKey;
    ULONG BreakOnSysRq = 1;
    ULONG CrashOnCtrlScroll = 0;
    ULONG DisableAutoRepeat = 0;
    ULONG DefaultOn = 1;
    ULONG DefaultOff = 0;
    NTSTATUS Status;

    Status = KbdHidOpenSubkey(&ServiceKey, NULL, RegistryPath);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to open the service key 0x%lx\n", Status);
        return;
    }

    Status = KbdHidOpenSubkey(&ParametersKey, ServiceKey, &ParametersName);
    ZwClose(ServiceKey);

    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Table, sizeof(Table));
        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[0].Name = L"BreakOnSysRq";
        Table[0].EntryContext = &BreakOnSysRq;
        Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[0].DefaultData = &DefaultOn;
        Table[0].DefaultLength = sizeof(DefaultOn);
        Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[1].Name = L"CrashOnCtrlScroll";
        Table[1].EntryContext = &CrashOnCtrlScroll;
        Table[1].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[1].DefaultData = &DefaultOff;
        Table[1].DefaultLength = sizeof(DefaultOff);
        Table[2].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[2].Name = L"DisableAutoRepeat";
        Table[2].EntryContext = &DisableAutoRepeat;
        Table[2].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[2].DefaultData = &DefaultOff;
        Table[2].DefaultLength = sizeof(DefaultOff);

        Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE | RTL_REGISTRY_OPTIONAL,
                                        (PCWSTR)ParametersKey,
                                        Table,
                                        NULL,
                                        NULL);
        ZwClose(ParametersKey);
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        /* A missing Parameters key means every value takes its default */
        Status = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to read the service parameters 0x%lx\n", Status);
        return;
    }

    KbdHidGlobals.BreakOnSysRq = (BreakOnSysRq != 0);

    if (CrashOnCtrlScroll != 0)
    {
        KbdHidGlobals.DumpModifiers = KBDHID_DUMP_RIGHT_CTRL;
        KbdHidGlobals.DumpKey = KBDHID_SCAN_SCROLL_LOCK;
        KbdHidGlobals.DumpKeyAlt = 0;
    }
    else
    {
        KbdHidReadCrashDumpSettings(RegistryPath);
    }

    KbdHidGlobals.DisableAutoRepeat = (DisableAutoRepeat != 0);
}

static
NTSTATUS
NTAPI
KbdHidCreateComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Forwards an open to the HID collection. The first successful open with
 * read access after kbdclass has connected starts the read loop on its
 * file object, which then runs until the last open is closed.
 */
static
NTSTATUS
NTAPI
KbdHidCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    static const UNICODE_STRING KbdName = RTL_CONSTANT_STRING(L"\\KBD");
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IoStack->FileObject;
    PFILE_OBJECT BoundFileObject = NULL;
    ACCESS_MASK DesiredAccess;
    KEVENT Event;
    NTSTATUS Status;

    DesiredAccess = IoStack->Parameters.Create.SecurityContext->DesiredAccess;

    if ((FileObject != NULL) && (FileObject->FileName.Length != 0))
    {
        Status = STATUS_SUCCESS;
        if (!RtlEqualUnicodeString(&FileObject->FileName, &KbdName, TRUE))
            Status = STATUS_ACCESS_DENIED;
        else if (DesiredAccess & FILE_READ_DATA)
            Status = STATUS_SHARING_VIOLATION;

        if (!NT_SUCCESS(Status))
        {
            Irp->IoStatus.Status = Status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }
    }

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    ExAcquireFastMutex(&DeviceExtension->OpenMutex);

    if (DeviceExtension->ConnectData.ClassService == NULL)
    {
        DPRINT1("Open refused, no class driver is connected\n");
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, KbdHidCreateComplete, &Event, TRUE, TRUE, TRUE);

        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        if (NT_SUCCESS(Status))
        {
            Status = Irp->IoStatus.Status;
            InterlockedIncrement(&DeviceExtension->OpenCount);

            if ((DeviceExtension->ReadFileObject == NULL) && (DesiredAccess & FILE_READ_DATA))
            {
                BoundFileObject = FileObject;
                DeviceExtension->ReadFileObject = FileObject;
                KeClearEvent(&DeviceExtension->ReadStoppedEvent);
                InterlockedExchange(&DeviceExtension->ReadState, KbdHidReadIdle);

                /* The read loop holds its own reference until it stops */
                IoAcquireRemoveLock(&DeviceExtension->RemoveLock, DeviceExtension->ReadIrp);
                DeviceExtension->ReadIrp->IoStatus.Status = STATUS_SUCCESS;

                Status = KbdHidStartRead(DeviceExtension);
                if (Status == STATUS_PENDING)
                    Status = STATUS_SUCCESS;
                else if (!NT_SUCCESS(Status))
                    DeviceExtension->ReadFileObject = NULL;
            }
        }
    }

    ExReleaseFastMutex(&DeviceExtension->OpenMutex);

    if (NT_SUCCESS(Status) &&
        (BoundFileObject != NULL) &&
        (InterlockedCompareExchange(&DeviceExtension->ExtendedAttributesState,
                                    KbdHidQueryInProgress,
                                    KbdHidQueryNotStarted) == KbdHidQueryNotStarted))
    {
        /* A device without the extended attributes still opens fine */
        KbdHidQueryExtendedAttributes(DeviceExtension);
        Status = STATUS_SUCCESS;

        InterlockedExchange(&DeviceExtension->ExtendedAttributesState, KbdHidQueryDone);
        KeSetEvent(&DeviceExtension->ExtendedAttributesEvent, IO_NO_INCREMENT, FALSE);
    }

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

/**
 * @brief
 * Forwards a close. The last close stops the read loop and the timers.
 */
static
NTSTATUS
NTAPI
KbdHidClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    BOOLEAN MustFlushDpcs;

    ExAcquireFastMutex(&DeviceExtension->OpenMutex);

    if (InterlockedDecrement(&DeviceExtension->OpenCount) == 0)
    {
        /* Let a read that is being sent reach the lower driver before canceling it */
        KeWaitForSingleObject(&DeviceExtension->ReadIssueDoneEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);

        if (DeviceExtension->ReadFileObject != NULL)
        {
            IoCancelIrp(DeviceExtension->ReadIrp);
            KeWaitForSingleObject(&DeviceExtension->ReadStoppedEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
        }

        MustFlushDpcs = KbdHidCancelAutoRepeat(DeviceExtension);
        if (KeCancelTimer(&DeviceExtension->RetryTimer))
            MustFlushDpcs = TRUE;

        if (MustFlushDpcs)
            KeFlushQueuedDpcs();

        DeviceExtension->ReadFileObject = NULL;
    }

    ExReleaseFastMutex(&DeviceExtension->OpenMutex);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

/**
 * @brief
 * Turns a flush into a flush of the HID class input queue.
 */
static
NTSTATUS
NTAPI
KbdHidFlush(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION NextStack;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    NextStack->Parameters.DeviceIoControl.OutputBufferLength = 0;
    NextStack->Parameters.DeviceIoControl.InputBufferLength = 0;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_FLUSH_QUEUE;
    NextStack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;

    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

NTSTATUS
NTAPI
KbdHidPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

static
VOID
NTAPI
KbdHidUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (KbdHidGlobals.RegistryPath.Buffer != NULL)
    {
        ExFreePoolWithTag(KbdHidGlobals.RegistryPath.Buffer, KBDHID_TAG);
        KbdHidGlobals.RegistryPath.Buffer = NULL;
    }
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    RtlZeroMemory(&KbdHidGlobals, sizeof(KbdHidGlobals));
    KbdHidGlobals.BreakOnSysRq = TRUE;

    KbdHidGlobals.RegistryPath.Length = RegistryPath->Length;
    KbdHidGlobals.RegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    KbdHidGlobals.RegistryPath.Buffer = ExAllocatePoolZero(PagedPool,
                                                           KbdHidGlobals.RegistryPath.MaximumLength,
                                                           KBDHID_TAG);
    if (KbdHidGlobals.RegistryPath.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(KbdHidGlobals.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);

    KbdHidReadServiceParameters(&KbdHidGlobals.RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = KbdHidCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KbdHidClose;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = KbdHidInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KbdHidPassThrough;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = KbdHidFlush;
    DriverObject->MajorFunction[IRP_MJ_PNP] = KbdHidPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = KbdHidPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = KbdHidSystemControl;
    DriverObject->DriverUnload = KbdHidUnload;
    DriverObject->DriverExtension->AddDevice = KbdHidAddDevice;

    return STATUS_SUCCESS;
}
