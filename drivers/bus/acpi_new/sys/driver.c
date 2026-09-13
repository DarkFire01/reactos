/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DriverEntry, AddDevice and IRP dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

int UacpiTraceEnabled = 1;   // `ed acpi!UacpiTraceEnabled 0` to quiet

PUACPI_FDO g_AcpiFdo;        // the single motherboard FDO (set on start)

PDRIVER_OBJECT g_AcpiDriverObject;   // tells our PDOs from foreign ones

// Shared IRP helpers

NTSTATUS
UacpiCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
UacpiForwardAndForget(PDEVICE_OBJECT Lower, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Lower, Irp);
}

static NTSTATUS
NTAPI
UacpiSyncCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
UacpiForwardAndWait(PDEVICE_OBJECT Lower, PIRP Irp)
{
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, UacpiSyncCompletion, &event, TRUE, TRUE, TRUE);

    status = IoCallDriver(Lower, Irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }
    return status;
}

// Central dispatch

static NTSTATUS
NTAPI
UacpiDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PUACPI_COMMON common = (PUACPI_COMMON)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (common->Type == UacpiExtFdo)
    {
        PUACPI_FDO fdo = (PUACPI_FDO)common;
        switch (sp->MajorFunction)
        {
        case IRP_MJ_PNP:            return UacpiFdoPnp(fdo, Irp);
        case IRP_MJ_POWER:          return UacpiFdoPower(fdo, Irp);
        case IRP_MJ_SYSTEM_CONTROL: return UacpiForwardAndForget(fdo->LowerDevice, Irp);
        case IRP_MJ_CREATE:
        case IRP_MJ_CLOSE:
        case IRP_MJ_CLEANUP:        return UacpiCompleteIrp(Irp, STATUS_SUCCESS, 0);
        default:                    return UacpiForwardAndForget(fdo->LowerDevice, Irp);
        }
    }
    else if (common->Type == UacpiExtFilter)
    {
        PUACPI_FILTER filter = (PUACPI_FILTER)common;
        switch (sp->MajorFunction)
        {
        case IRP_MJ_PNP:            return UacpiFilterPnp(filter, Irp);
        case IRP_MJ_POWER:          return UacpiFilterPower(filter, Irp);
        case IRP_MJ_DEVICE_CONTROL:
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
            // Eval IOCTLs on filtered devices; unhandled codes forward down.
            return UacpiFilterDeviceControl(filter, Irp);
        default:
            // Everything else passes through to the foreign PDO.
            return UacpiForwardAndForget(filter->LowerDevice, Irp);
        }
    }
    else if (common->Type == UacpiExtPdo)
    {
        PUACPI_PDO pdo = (PUACPI_PDO)common;
        switch (sp->MajorFunction)
        {
        case IRP_MJ_PNP:            return UacpiPdoPnp(pdo, Irp);
        case IRP_MJ_POWER:          return UacpiPdoPower(pdo, Irp);
        case IRP_MJ_DEVICE_CONTROL:
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
            // Method-evaluation IOCTLs (ioctl.c).
            return UacpiPdoDeviceControl(pdo, Irp);
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
            // Only the EC has an address space to transfer (drvs/ec.c).
            if (pdo->Ec != NULL)
            {
                return UacpiEcReadWrite(pdo, Irp);
            }
            return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
        case IRP_MJ_CREATE:
        case IRP_MJ_CLOSE:
        case IRP_MJ_CLEANUP:        return UacpiCompleteIrp(Irp, STATUS_SUCCESS, 0);
        case IRP_MJ_SYSTEM_CONTROL:
        {
            // Thermal zones answer WMI; other PDOs complete it unchanged.
            BOOLEAN handled = FALSE;
            NTSTATUS s = UacpiThermalSystemControl(pdo, Irp, &handled);
            if (handled)
            {
                return s;
            }
            return UacpiCompleteIrp(Irp, Irp->IoStatus.Status, 0);
        }
        default:
            return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
        }
    }

    return UacpiCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

// AddDevice

static NTSTATUS
NTAPI
UacpiAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    PDEVICE_OBJECT fdoDevice;
    PUACPI_FDO fdo;

    status = IoCreateDevice(DriverObject, sizeof(UACPI_FDO), NULL,
                            FILE_DEVICE_ACPI, FILE_DEVICE_SECURE_OPEN, FALSE,
                            &fdoDevice);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] IoCreateDevice(FDO) failed 0x%X\n", status);
        return status;
    }

    fdo = (PUACPI_FDO)fdoDevice->DeviceExtension;
    RtlZeroMemory(fdo, sizeof(*fdo));
    fdo->Common.Type = UacpiExtFdo;
    fdo->Common.Self = fdoDevice;
    fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    ExInitializeFastMutex(&fdo->ChildLock);
    InitializeListHead(&fdo->Children);
    InitializeListHead(&fdo->Filters);

    fdo->LowerDevice = IoAttachDeviceToDeviceStack(fdoDevice, PhysicalDeviceObject);
    if (fdo->LowerDevice == NULL)
    {
        UacpiTrace("[acpi] IoAttachDeviceToDeviceStack failed\n");
        IoDeleteDevice(fdoDevice);
        return STATUS_NO_SUCH_DEVICE;
    }

    fdoDevice->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    fdoDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    UacpiTrace("[acpi] AddDevice: FDO %p over PDO %p\n", fdoDevice, PhysicalDeviceObject);
    return STATUS_SUCCESS;
}

// Unload

static VOID
NTAPI
UacpiUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UacpiTrace("[acpi] unload\n");
}

// DriverEntry

// Persistent copy of RegistryPath for the WmiLib REGINFO handler.
UNICODE_STRING UacpiDriverRegistryPath = { 0, 0, NULL };

// REG_DWORD overrides from Services\ACPI\Parameters; absent keeps the default.
static VOID
UacpiReadParameters(PUNICODE_STRING RegistryPath)
{
    RTL_QUERY_REGISTRY_TABLE table[12];
    static const WCHAR suffix[] = L"\\Parameters";
    PWCH   buffer;
    USHORT bytes;

    if (RegistryPath == NULL || RegistryPath->Length == 0 ||
        RegistryPath->Buffer == NULL)
        {
        return;
    }
    bytes  = (USHORT)(RegistryPath->Length + sizeof(suffix));
    buffer = (PWCH)ExAllocatePoolWithTag(PagedPool, bytes, UACPI_POOL_TAG);
    if (buffer == NULL)
    {
        return;
    }
    RtlCopyMemory(buffer, RegistryPath->Buffer, RegistryPath->Length);
    RtlCopyMemory((PUCHAR)buffer + RegistryPath->Length, suffix, sizeof(suffix));

    RtlZeroMemory(table, sizeof(table));   // last entry stays zeroed = terminator

#define UACPI_PARAM(idx, wname, var)                          \
    table[idx].Flags         = RTL_QUERY_REGISTRY_DIRECT;    \
    table[idx].Name          = (PWSTR)(wname);               \
    table[idx].EntryContext  = &(var);                       \
    table[idx].DefaultType   = REG_DWORD;                    \
    table[idx].DefaultData   = &(var);                       \
    table[idx].DefaultLength = sizeof(ULONG)

    UACPI_PARAM(0, L"TraceEnabled",        UacpiTraceEnabled);
    UACPI_PARAM(1, L"IrqArbEnabled",       UacpiIrqArbEnabled);
    UACPI_PARAM(2, L"ResArbEnabled",       UacpiResArbEnabled);
    UACPI_PARAM(3, L"MsiDiagEnabled",      UacpiMsiDiagEnabled);
    UACPI_PARAM(4, L"MsiDiagDelaySeconds", UacpiMsiDiagDelaySeconds);
    UACPI_PARAM(5, L"FlatEnumEnabled",     UacpiFlatEnumEnabled);
    UACPI_PARAM(6, L"HostVerbose",         UacpiHostVerbose);
    UACPI_PARAM(7, L"IrqArbVerbose",       UacpiIrqArbVerbose);
    UACPI_PARAM(8, L"IrqLibHalOverrides",  UacpiIrqLibHalOverrides);
    UACPI_PARAM(9, L"EnumDiagEnabled",     UacpiEnumDiagEnabled);
    UACPI_PARAM(10, L"EnumDiagDelaySeconds", UacpiEnumDiagDelaySeconds);

#undef UACPI_PARAM

    (void)RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, buffer, table, NULL, NULL);
    ExFreePoolWithTag(buffer, UACPI_POOL_TAG);
}

NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    g_AcpiDriverObject = DriverObject;

    if (RegistryPath != NULL && RegistryPath->Length != 0)
    {
        UacpiDriverRegistryPath.Buffer =
            (PWCH)ExAllocatePoolWithTag(NonPagedPool,
                                        RegistryPath->Length + sizeof(WCHAR),
                                        UACPI_POOL_TAG);
        if (UacpiDriverRegistryPath.Buffer != NULL)
        {
            RtlCopyMemory(UacpiDriverRegistryPath.Buffer, RegistryPath->Buffer,
                          RegistryPath->Length);
            UacpiDriverRegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
            UacpiDriverRegistryPath.Length = RegistryPath->Length;
            UacpiDriverRegistryPath.MaximumLength = RegistryPath->Length + sizeof(WCHAR);
        }
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        DriverObject->MajorFunction[i] = UacpiDispatch;
    }
    DriverObject->DriverExtension->AddDevice = UacpiAddDevice;
    DriverObject->DriverUnload = UacpiUnload;

    UacpiReadParameters(RegistryPath);

    // Processor IDs are built from CentralProcessor\0, as acpi.sys does at init.
    UacpiInitProcessorString();

    UacpiTrace("[acpi] DriverEntry (uACPI-backed reconstruction): trace %d irqarb %d "
              "resarb %d msidiag %d/%ds flatenum %d hostverbose %d irqarbverbose %d\n",
              UacpiTraceEnabled, UacpiIrqArbEnabled, UacpiResArbEnabled,
              UacpiMsiDiagEnabled, UacpiMsiDiagDelaySeconds, UacpiFlatEnumEnabled,
              UacpiHostVerbose, UacpiIrqArbVerbose);
    return STATUS_SUCCESS;
}
