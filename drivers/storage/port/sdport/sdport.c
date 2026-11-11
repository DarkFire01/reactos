/*
 * PROJECT:     ReactOS Storage Stack / SD host controller port driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Port library entry points and exported miniport services
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Poison values returned by the register read accessors when the mapped base
   is not valid, matching the host controller port driver behaviour. */
#define SDPORT_POISON_UCHAR  ((UCHAR)0xFF)
#define SDPORT_POISON_USHORT ((USHORT)0xFBAD)
#define SDPORT_POISON_ULONG  ((ULONG)0xFFBADFED)

static
VOID
SdPortReadRegistryParameters(
    _In_ PSDPORT_DRIVER_EXTENSION DriverExtension)
{
    HANDLE KeyHandle;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Parameters = RTL_CONSTANT_STRING(L"\\Parameters");
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"SynchronousStartDevice");
    UNICODE_STRING FullPath;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    ULONG ResultLength;
    NTSTATUS Status;

    FullPath.Length = 0;
    FullPath.MaximumLength = DriverExtension->RegistryPath.Length + Parameters.Length + sizeof(WCHAR);
    FullPath.Buffer = ExAllocatePoolWithTag(PagedPool, FullPath.MaximumLength, SDPORT_POOL_TAG);
    if (FullPath.Buffer == NULL)
        return;

    RtlCopyUnicodeString(&FullPath, &DriverExtension->RegistryPath);
    RtlAppendUnicodeStringToString(&FullPath, &Parameters);

    InitializeObjectAttributes(&ObjectAttributes,
                               &FullPath,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (NT_SUCCESS(Status))
    {
        Status = ZwQueryValueKey(KeyHandle,
                                 &ValueName,
                                 KeyValuePartialInformation,
                                 Value,
                                 sizeof(Buffer),
                                 &ResultLength);
        if (NT_SUCCESS(Status) && Value->Type == REG_DWORD && Value->DataLength >= sizeof(ULONG))
            DriverExtension->SynchronousStartDevice = (*(PULONG)Value->Data != 0);

        ZwClose(KeyHandle);
    }

    ExFreePoolWithTag(FullPath.Buffer, SDPORT_POOL_TAG);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ ULONG Reason)
{
    UNREFERENCED_PARAMETER(Reason);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

/* The port driver is a library brought in by a host controller miniport, which
   calls SdPortInitialize from its own DriverEntry.  This entry point only runs
   when the image is loaded on its own and performs no initialization. */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdPortInitialize(
    _In_ PVOID DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_ PSDPORT_INITIALIZATION_DATA InitializationData)
{
    PDRIVER_OBJECT Driver = DriverObject;
    PSDPORT_DRIVER_EXTENSION DriverExtension;
    NTSTATUS Status;

    if (DriverObject == NULL || RegistryPath == NULL || InitializationData == NULL)
        return STATUS_INVALID_PARAMETER;

    if (InitializationData->GetSlotCount == NULL ||
        InitializationData->Initialize == NULL ||
        InitializationData->IssueRequest == NULL ||
        InitializationData->Interrupt == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DriverExtension = IoGetDriverObjectExtension(Driver, (PVOID)SdPortAddDevice);
    if (DriverExtension == NULL)
    {
        Status = IoAllocateDriverObjectExtension(Driver,
                                                 (PVOID)SdPortAddDevice,
                                                 sizeof(*DriverExtension),
                                                 (PVOID *)&DriverExtension);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    RtlZeroMemory(DriverExtension, sizeof(*DriverExtension));
    DriverExtension->DriverObject = Driver;
    RtlCopyMemory(&DriverExtension->InitializationData,
                  InitializationData,
                  sizeof(SDPORT_INITIALIZATION_DATA));

    DriverExtension->RegistryPath.Length = 0;
    DriverExtension->RegistryPath.MaximumLength = RegistryPath->Length + sizeof(WCHAR);
    DriverExtension->RegistryPath.Buffer =
        ExAllocatePoolWithTag(NonPagedPool, DriverExtension->RegistryPath.MaximumLength, SDPORT_POOL_TAG);
    if (DriverExtension->RegistryPath.Buffer != NULL)
        RtlCopyUnicodeString(&DriverExtension->RegistryPath, RegistryPath);

    SdPortReadRegistryParameters(DriverExtension);

    Driver->DriverExtension->AddDevice = SdPortAddDevice;
    Driver->DriverUnload = SdPortUnload;
    Driver->MajorFunction[IRP_MJ_CREATE] = SdPortDispatchDeviceControl;
    Driver->MajorFunction[IRP_MJ_CLOSE] = SdPortDispatchDeviceControl;
    Driver->MajorFunction[IRP_MJ_CLEANUP] = SdPortDispatchDeviceControl;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SdPortDispatchDeviceControl;
    Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = SdPortDispatchInternalDeviceControl;
    Driver->MajorFunction[IRP_MJ_POWER] = SdPortDispatchPower;
    Driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = SdPortDispatchSystemControl;
    Driver->MajorFunction[IRP_MJ_PNP] = SdPortDispatchPnp;

    DPRINT1("sdport: SdPortInitialize succeeded\n");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdPortAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PSDPORT_DRIVER_EXTENSION DriverExtension;
    PSDPORT_FDO_EXTENSION FdoExtension;
    PDEVICE_OBJECT Fdo;
    UNICODE_STRING DeviceName;
    WCHAR NameBuffer[64];
    static volatile LONG DeviceNumber = -1;
    LONG Index;
    NTSTATUS Status;

    DriverExtension = IoGetDriverObjectExtension(DriverObject, (PVOID)SdPortAddDevice);
    if (DriverExtension == NULL)
        return STATUS_NO_SUCH_DEVICE;

    Index = InterlockedIncrement(&DeviceNumber);
    Status = RtlStringCchPrintfW(NameBuffer,
                                 RTL_NUMBER_OF(NameBuffer),
                                 L"\\Device\\Sdhost%d",
                                 Index);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&DeviceName, NameBuffer);

    Status = IoCreateDevice(DriverObject,
                            sizeof(SDPORT_FDO_EXTENSION),
                            &DeviceName,
                            FILE_DEVICE_CONTROLLER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    FdoExtension = Fdo->DeviceExtension;
    RtlZeroMemory(FdoExtension, sizeof(*FdoExtension));
    FdoExtension->ExtensionType = SdportExtensionFdo;
    FdoExtension->Self = Fdo;
    FdoExtension->Pdo = PhysicalDeviceObject;
    FdoExtension->DriverExtension = DriverExtension;
    FdoExtension->InitData = &DriverExtension->InitializationData;
    FdoExtension->State = SdportStateStopped;
    IoInitializeRemoveLock(&FdoExtension->RemoveLock, SDPORT_POOL_TAG, 0, 0);
    KeInitializeDpc(&FdoExtension->CompletionDpc, SdPortCompletionDpc, FdoExtension);
    KeInitializeDpc(&FdoExtension->IsrDpc, SdPortIsrDpc, FdoExtension);
    KeInitializeDpc(&FdoExtension->PollDpc, SdPortPollDpc, FdoExtension);
    KeInitializeTimer(&FdoExtension->PollTimer);
    KeInitializeSpinLock(&FdoExtension->PollLock);

    FdoExtension->LowerDevice = IoAttachDeviceToDeviceStack(Fdo, PhysicalDeviceObject);
    if (FdoExtension->LowerDevice == NULL)
    {
        IoDeleteDevice(Fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    Fdo->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

VOID
NTAPI
SdPortUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PSDPORT_DRIVER_EXTENSION DriverExtension;

    DriverExtension = IoGetDriverObjectExtension(DriverObject, (PVOID)SdPortAddDevice);
    if (DriverExtension != NULL && DriverExtension->RegistryPath.Buffer != NULL)
        ExFreePoolWithTag(DriverExtension->RegistryPath.Buffer, SDPORT_POOL_TAG);
}

VOID
NTAPI
SdPortCompleteRequest(
    _In_ PSDPORT_REQUEST Request,
    _In_ NTSTATUS Status)
{
    PSDPORT_WORK_REQUEST Work = SdPortWorkFromPublic(Request);
    PSDPORT_SLOT_EXTENSION Slot = Work->Slot;

    /* Guarantee a single completion whether the miniport completes the request
       itself, the timeout fires, or the submit path reports a synchronous error. */
    if (InterlockedCompareExchange(&Work->CompletionQueued, 1, 0) != 0)
        return;

    Request->Status = Status;

    if (Work->TimerArmed && !Slot->CrashdumpMode)
    {
        KeCancelTimer(&Work->TimeoutTimer);
        Work->TimerArmed = FALSE;
    }

    if (Work->Internal || Slot->CrashdumpMode)
    {
        KeSetEvent(&Work->CompletionEvent, IO_NO_INCREMENT, FALSE);
        return;
    }

    KeInsertQueueDpc(&Slot->FdoExtension->CompletionDpc, Work, NULL);
}

VOID
NTAPI
SdPortWait(
    _In_ ULONG Microseconds)
{
    LARGE_INTEGER Interval;

    if (Microseconds > 50 && KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        Interval.QuadPart = -10LL * (LONGLONG)Microseconds;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }
    else
    {
        KeStallExecutionProcessor(Microseconds);
    }
}

FORCEINLINE
VOID
SdPortMemoryBarrier(VOID)
{
    KeMemoryBarrier();
}

UCHAR
NTAPI
SdPortReadRegisterUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset)
{
    SdPortMemoryBarrier();
    if (Base == NULL)
        return SDPORT_POISON_UCHAR;

    return READ_REGISTER_UCHAR((PUCHAR)((PUCHAR)Base + Offset));
}

USHORT
NTAPI
SdPortReadRegisterUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset)
{
    SdPortMemoryBarrier();
    if (Base == NULL)
        return SDPORT_POISON_USHORT;

    return READ_REGISTER_USHORT((PUSHORT)((PUCHAR)Base + Offset));
}

ULONG
NTAPI
SdPortReadRegisterUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset)
{
    SdPortMemoryBarrier();
    if (Base == NULL)
        return SDPORT_POISON_ULONG;

    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset));
}

VOID
NTAPI
SdPortReadRegisterBufferUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PUCHAR Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        Buffer[Index] = SdPortReadRegisterUchar(Base, Offset);
}

VOID
NTAPI
SdPortReadRegisterBufferUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PUSHORT Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        Buffer[Index] = SdPortReadRegisterUshort(Base, Offset);
}

VOID
NTAPI
SdPortReadRegisterBufferUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PULONG Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        Buffer[Index] = SdPortReadRegisterUlong(Base, Offset);
}

VOID
NTAPI
SdPortWriteRegisterUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ UCHAR Value)
{
    SdPortMemoryBarrier();
    if (Base != NULL)
        WRITE_REGISTER_UCHAR((PUCHAR)((PUCHAR)Base + Offset), Value);
}

VOID
NTAPI
SdPortWriteRegisterUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ USHORT Value)
{
    SdPortMemoryBarrier();
    if (Base != NULL)
        WRITE_REGISTER_USHORT((PUSHORT)((PUCHAR)Base + Offset), Value);
}

VOID
NTAPI
SdPortWriteRegisterUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    SdPortMemoryBarrier();
    if (Base != NULL)
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset), Value);
}

VOID
NTAPI
SdPortWriteRegisterBufferUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PUCHAR Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        SdPortWriteRegisterUchar(Base, Offset, Buffer[Index]);
}

VOID
NTAPI
SdPortWriteRegisterBufferUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PUSHORT Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        SdPortWriteRegisterUshort(Base, Offset, Buffer[Index]);
}

VOID
NTAPI
SdPortWriteRegisterBufferUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PULONG Buffer,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        SdPortWriteRegisterUlong(Base, Offset, Buffer[Index]);
}

static
PSDPORT_FDO_EXTENSION
SdPortGetFdoExtension(
    _In_ PVOID DeviceExtension)
{
    PSDPORT_FDO_EXTENSION FdoExtension = DeviceExtension;
    PSDPORT_SLOT_EXTENSION Slot;

    if (FdoExtension->ExtensionType == SdportExtensionFdo)
        return FdoExtension;

    Slot = DeviceExtension;
    return Slot->FdoExtension;
}

NTSTATUS
NTAPI
SdPortGetPciConfigSpace(
    _In_ PVOID Fdo,
    _In_ UCHAR Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    PSDPORT_FDO_EXTENSION FdoExtension = SdPortGetFdoExtension(((PDEVICE_OBJECT)Fdo)->DeviceExtension);
    ULONG BytesRead;

    if (!FdoExtension->BusInterfaceValid)
        return STATUS_SUCCESS;

    BytesRead = FdoExtension->BusInterface.GetBusData(FdoExtension->BusInterface.Context,
                                                      PCI_WHICHSPACE_CONFIG,
                                                      Buffer,
                                                      Offset,
                                                      Length);
    if (BytesRead != Length)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdPortSetPciConfigSpace(
    _In_ PVOID Fdo,
    _In_ UCHAR Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    PSDPORT_FDO_EXTENSION FdoExtension = SdPortGetFdoExtension(((PDEVICE_OBJECT)Fdo)->DeviceExtension);
    ULONG BytesWritten;

    if (!FdoExtension->BusInterfaceValid)
        return STATUS_NOT_SUPPORTED;

    BytesWritten = FdoExtension->BusInterface.SetBusData(FdoExtension->BusInterface.Context,
                                                         PCI_WHICHSPACE_CONFIG,
                                                         Buffer,
                                                         Offset,
                                                         Length);
    if (BytesWritten != Length)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdPortPoFxPowerControl(
    _In_ PVOID PrivateExtension,
    _In_ LPCGUID PowerControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_opt_ PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned)
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferSize);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferSize);

    if (BytesReturned != NULL)
        *BytesReturned = 0;

    return STATUS_NOT_SUPPORTED;
}
