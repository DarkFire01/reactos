/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Filter DOs over foreign PDOs matched by _ADR
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <wdmguid.h>        // extern GUID_ARBITER/TRANSLATOR_INTERFACE_STANDARD (defined in guid.c)

// Synchronous PnP IRP helper.

static NTSTATUS
NTAPI
UacpiFilterSyncCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// Send IRP_MN_QUERY_CAPABILITIES to a foreign PDO's stack; Address starts at -1.
static NTSTATUS
UacpiQueryPdoCapabilities(PDEVICE_OBJECT Pdo, PDEVICE_CAPABILITIES Caps)
{
    PDEVICE_OBJECT target;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    KEVENT event;
    NTSTATUS status;

    RtlZeroMemory(Caps, sizeof(*Caps));
    Caps->Size = sizeof(*Caps);
    Caps->Version = 1;
    Caps->Address = (ULONG)-1;
    Caps->UINumber = (ULONG)-1;

    target = IoGetAttachedDeviceReference(Pdo);

    irp = IoAllocateIrp(target->StackSize, FALSE);
    if (irp == NULL)
    {
        ObDereferenceObject(target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    sp->Parameters.DeviceCapabilities.Capabilities = Caps;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(irp, UacpiFilterSyncCompletion, &event, TRUE, TRUE, TRUE);

    (void)IoCallDriver(target, irp);
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    status = irp->IoStatus.Status;

    IoFreeIrp(irp);
    ObDereferenceObject(target);
    return status;
}

// Filter lookup (Fdo->Filters, guarded by ChildLock).

static PUACPI_FILTER
UacpiFindFilter(PUACPI_FDO Fdo, uacpi_namespace_node *node, PDEVICE_OBJECT foreignPdo)
{
    PLIST_ENTRY e;
    PUACPI_FILTER found = NULL;

    ExAcquireFastMutex(&Fdo->ChildLock);
    for (e = Fdo->Filters.Flink; e != &Fdo->Filters; e = e->Flink)
    {
        PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);
        if ((node != NULL && f->Node == node) ||
            (foreignPdo != NULL && f->ForeignPdo == foreignPdo))
            {
            found = f;
            break;
        }
    }
    ExReleaseFastMutex(&Fdo->ChildLock);
    return found;
}

// Attach a filter DO over a foreign PDO and bind the namespace node to it.
static NTSTATUS
UacpiFilterAttach(PUACPI_FDO Fdo, uacpi_namespace_node *node, PDEVICE_OBJECT ForeignPdo)
{
    PDEVICE_OBJECT filterDevice;
    PUACPI_FILTER filter;
    uacpi_object_name name;
    NTSTATUS status;

    status = IoCreateDevice(Fdo->Common.Self->DriverObject, sizeof(UACPI_FILTER),
                            NULL, FILE_DEVICE_ACPI, 0, FALSE, &filterDevice);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    filter = (PUACPI_FILTER)filterDevice->DeviceExtension;
    RtlZeroMemory(filter, sizeof(*filter));
    filter->Common.Type = UacpiExtFilter;
    filter->Common.Self = filterDevice;
    filter->Fdo = Fdo;
    filter->Node = node;
    filter->ForeignPdo = ForeignPdo;
    UacpiWakeInit(&filter->Wake, node);

    filter->LowerDevice = IoAttachDeviceToDeviceStack(filterDevice, ForeignPdo);
    if (filter->LowerDevice == NULL)
    {
        IoDeleteDevice(filterDevice);
        return STATUS_NO_SUCH_DEVICE;
    }

    // Inherit I/O model, power flags, and alignment from the lower device.
    filterDevice->Flags |= filter->LowerDevice->Flags &
                           (DO_POWER_PAGABLE | DO_DIRECT_IO | DO_BUFFERED_IO);
    filterDevice->AlignmentRequirement = filter->LowerDevice->AlignmentRequirement;
    filterDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    ExAcquireFastMutex(&Fdo->ChildLock);
    InsertTailList(&Fdo->Filters, &filter->Link);
    ExReleaseFastMutex(&Fdo->ChildLock);

    name = uacpi_namespace_node_name(node);
    UacpiTrace("[acpi] filter %c%c%c%c attached over foreign PDO %p\n",
              name.text[0], name.text[1], name.text[2], name.text[3], ForeignPdo);
    return STATUS_SUCCESS;
}

// Attach filters to foreign PDOs whose caps Address matches a child's _ADR.
NTSTATUS
UacpiDetectFilterDevices(PUACPI_FDO Fdo, uacpi_namespace_node *parent,
                        PDEVICE_RELATIONS Relations)
{
    uacpi_namespace_node *child = NULL;
    PULONG addr;         // per-relation cached Capabilities.Address
    PBOOLEAN queried;
    ULONG i;

    if (Fdo == NULL || parent == NULL || Relations == NULL || Relations->Count == 0)
    {
        return STATUS_SUCCESS;
    }

    addr = (PULONG)ExAllocatePoolWithTag(
        PagedPool, Relations->Count * (sizeof(ULONG) + sizeof(BOOLEAN)),
        UACPI_POOL_TAG);
    if (addr == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    queried = (PBOOLEAN)(addr + Relations->Count);
    RtlZeroMemory(queried, Relations->Count * sizeof(BOOLEAN));

    while (uacpi_likely_success(uacpi_namespace_node_next_typed(
               parent, &child, UACPI_OBJECT_DEVICE_BIT)) &&
           child != NULL)
           {

        uacpi_u64 adr = 0;

        if (!UacpiNodeIsPresentEx(child))
        {
            continue;
        }
        if (uacpi_unlikely_error(uacpi_eval_simple_integer(child, "_ADR", &adr)))
        {
            continue;                       // PDO-type child; enum.c owns it
        }
        if (UacpiFindFilter(Fdo, child, NULL) != NULL)
        {
            continue;                       // already filtered
        }
        if (UacpiFindPdoByNode(Fdo, child) != NULL)
        {
            continue;                       // already exposed as an ACPI PDO
        }

        for (i = 0; i < Relations->Count; i++)
        {
            PDEVICE_OBJECT candidate = Relations->Objects[i];

            if (candidate->DriverObject == g_AcpiDriverObject)
            {
                continue;                   // one of our own PDOs
            }
            if (!queried[i])
            {
                DEVICE_CAPABILITIES caps;
                queried[i] = TRUE;
                if (NT_SUCCESS(UacpiQueryPdoCapabilities(candidate, &caps)))
                {
                    addr[i] = caps.Address;
                }
                else
                {
                    addr[i] = (ULONG)-1;
                }
            }
            if (addr[i] != (ULONG)-1 && addr[i] == (ULONG)adr)
            {
                if (UacpiFindFilter(Fdo, NULL, candidate) == NULL)
                {
                    (void)UacpiFilterAttach(Fdo, child, candidate);
                }
                break;
            }
        }
    }

    ExFreePoolWithTag(addr, UACPI_POOL_TAG);
    return STATUS_SUCCESS;
}

// PnP dispatch for a filter DO.
NTSTATUS
UacpiFilterPnp(PUACPI_FILTER Filter, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    switch (sp->MinorFunction)
    {

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == BusRelations)
        {
            // Inject ACPI PDOs, forward, then filter the next _ADR level.
            UacpiBuildChildPdosForNode(Filter->Fdo, Filter->Node);

            /* Another pass: the settled inventory waits for these to stop */
            UacpiEnumDiagArm(Filter->Fdo);

            status = UacpiMergeChildRelations(Filter->Fdo, Filter->Node, NULL, Irp);
            if (!NT_SUCCESS(status))
            {
                return UacpiCompleteIrp(Irp, status, Irp->IoStatus.Information);
            }
            Irp->IoStatus.Status = STATUS_SUCCESS;

            status = UacpiForwardAndWait(Filter->LowerDevice, Irp);
            if (NT_SUCCESS(status))
            {
                (void)UacpiDetectFilterDevices(
                    Filter->Fdo, Filter->Node,
                    (PDEVICE_RELATIONS)Irp->IoStatus.Information);
            }
            return UacpiCompleteIrp(Irp, Irp->IoStatus.Status, Irp->IoStatus.Information);
        }
        return UacpiForwardAndForget(Filter->LowerDevice, Irp);

    case IRP_MN_REMOVE_DEVICE:
    {
        PUACPI_FDO fdo = Filter->Fdo;
        PDEVICE_OBJECT lower = Filter->LowerDevice;
        PDEVICE_OBJECT self = Filter->Common.Self;

        // Complete any pended WAIT_WAKE and disarm before we vanish.
        UacpiWakeTeardown(&Filter->Wake);

        ExAcquireFastMutex(&fdo->ChildLock);
        RemoveEntryList(&Filter->Link);
        ExReleaseFastMutex(&fdo->ChildLock);

        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(lower, Irp);

        IoDetachDevice(lower);
        IoDeleteDevice(self);
        return status;
    }

    case IRP_MN_QUERY_CAPABILITIES:
    {
        // Let the bus fill caps, then advertise the node's _PRW wake capability
        // so the power manager will send a WAIT_WAKE we can arm (UacpiFilterPower).
        PDEVICE_CAPABILITIES caps = sp->Parameters.DeviceCapabilities.Capabilities;

        status = UacpiForwardAndWait(Filter->LowerDevice, Irp);
        if (NT_SUCCESS(status) && caps != NULL &&
            caps->Size >= sizeof(DEVICE_CAPABILITIES) && Filter->Node != NULL)
        {
            UacpiPowerMergeWakeCaps(Filter->Node, caps);
        }
        return UacpiCompleteIrp(Irp, status, Irp->IoStatus.Information);
    }

    case IRP_MN_QUERY_INTERFACE:
    {
        // Serve our IRQ arbiter and translator here, then always forward down.
        const GUID *guid = sp->Parameters.QueryInterface.InterfaceType;
        ULONG_PTR data = (ULONG_PTR)sp->Parameters.QueryInterface.InterfaceSpecificData;
        NTSTATUS st = STATUS_NOT_SUPPORTED;
        char name[8] = "FILT";

        if (Filter->Node != NULL)
        {
            uacpi_object_name n = uacpi_namespace_node_name(Filter->Node);
            name[0] = n.text[0]; name[1] = n.text[1];
            name[2] = n.text[2]; name[3] = n.text[3]; name[4] = '\0';
        }
        if (IsEqualGUID(guid, &GUID_ARBITER_INTERFACE_STANDARD) &&
            data == CmResourceTypeInterrupt)
            {
            st = UacpiQueryIrqArbiter(sp);
        }
        else if (IsEqualGUID(guid, &GUID_TRANSLATOR_INTERFACE_STANDARD) &&
                   data == CmResourceTypeInterrupt)
                   {
            st = UacpiBuildIrqTranslator(name, sp);
        }
        else
        {
            // Filtered devnodes also get the ACPI evaluation interface.
            st = UacpiBuildAcpiInterface(Filter->Common.Self, name, sp);
            if (st == STATUS_NOT_SUPPORTED)
            {
                // Then the firmware-driven device reset interface.
                st = UacpiBuildDeviceResetInterface(Filter->Node, name, sp);
            }
        }
        if (st != STATUS_NOT_SUPPORTED)
        {
            Irp->IoStatus.Status = st;
        }
        return UacpiForwardAndForget(Filter->LowerDevice, Irp);
    }

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
    {
        // Paging/hibernation/dump path DOs must be non-pageable; track per type.
        BOOLEAN inPath = sp->Parameters.UsageNotification.InPath;
        BOOLEAN setPageable = FALSE;
        PLONG   counter;

        switch (sp->Parameters.UsageNotification.Type)
        {
        case DeviceUsageTypeHibernation: counter = &Filter->HibernationCount; break;
        case DeviceUsageTypeDumpFile:    counter = &Filter->DumpCount;        break;
        case DeviceUsageTypePaging:
        default:                         counter = &Filter->PagingCount;      break;
        }

        // Clear DO_POWER_PAGABLE on first use, restore when all counts are 0.
        if (inPath &&
            Filter->PagingCount == 0 && Filter->HibernationCount == 0 &&
            Filter->DumpCount == 0)
            {
            Filter->Common.Self->Flags &= ~DO_POWER_PAGABLE;
            setPageable = TRUE;
        }

        status = UacpiForwardAndWait(Filter->LowerDevice, Irp);
        if (NT_SUCCESS(status))
        {
            IoAdjustPagingPathCount(counter, inPath);
            if (!inPath &&
                Filter->PagingCount == 0 && Filter->HibernationCount == 0 &&
                Filter->DumpCount == 0)
                {
                Filter->Common.Self->Flags |= DO_POWER_PAGABLE;
            }
        }
        else if (setPageable)
        {
            Filter->Common.Self->Flags |= DO_POWER_PAGABLE;
        }
        return UacpiCompleteIrp(Irp, status, Irp->IoStatus.Information);
    }

    case IRP_MN_SURPRISE_REMOVAL:
        // Device is gone: fail the pended wait and drop the GPE arm now.
        UacpiWakeTeardown(&Filter->Wake);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return UacpiForwardAndForget(Filter->LowerDevice, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return UacpiForwardAndForget(Filter->LowerDevice, Irp);

    default:
        // Everything else passes through; the bus identity is unchanged.
        return UacpiForwardAndForget(Filter->LowerDevice, Irp);
    }
}

// Power: mostly transparent, but a WAIT_WAKE for a device whose namespace node
// carries a usable _PRW is armed here. The bus driver below knows nothing about
// that GPE, so ACPI owns the wait and completes it on Notify(2). Everything else
// (including a WAIT_WAKE with no _PRW) passes straight down.
NTSTATUS
UacpiFilterPower(PUACPI_FILTER Filter, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (sp->MinorFunction == IRP_MN_WAIT_WAKE &&
        Filter->Node != NULL && UacpiWakeHasPrw(&Filter->Wake))
    {
        PoStartNextPowerIrp(Irp);
        return UacpiWakeArm(&Filter->Wake, Irp);
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Filter->LowerDevice, Irp);
}
