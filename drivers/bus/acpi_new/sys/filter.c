/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Filter device objects over foreign PDOs matched by _ADR
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <wdmguid.h>
#include <debug.h>

/* Cached QUERY_CAPABILITIES answer for one entry of a relations list */
typedef struct _UACPINT_FILTER_PROBE
{
    ULONG   Address;
    BOOLEAN Queried;
} UACPINT_FILTER_PROBE, *PUACPINT_FILTER_PROBE;

static
NTSTATUS
NTAPI
UacpiNtFilterSignalCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Address and UINumber read back as -1 when the bus leaves them alone */
static
NTSTATUS
NTAPI
UacpiNtQueryForeignCapabilities(
    _In_ PDEVICE_OBJECT ForeignPdo,
    _Out_ PDEVICE_CAPABILITIES Capabilities)
{
    PIO_STACK_LOCATION IoStack;
    PDEVICE_OBJECT TopDevice;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Version = 1;
    Capabilities->Address = MAXULONG;
    Capabilities->UINumber = MAXULONG;

    TopDevice = IoGetAttachedDeviceReference(ForeignPdo);

    Irp = IoAllocateIrp(TopDevice->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(TopDevice);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* PnP IRPs must start out as not supported */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_PNP;
    IoStack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    IoStack->Parameters.DeviceCapabilities.Capabilities = Capabilities;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, UacpiNtFilterSignalCompletion, &Event, TRUE, TRUE, TRUE);

    IoCallDriver(TopDevice, Irp);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;

    IoFreeIrp(Irp);
    ObDereferenceObject(TopDevice);
    return Status;
}

/* Matches on either the node or the foreign PDO, ChildLock guards the list */
static
PUACPINT_FLT
NTAPI
UacpiNtFindFilter(
    _In_ PUACPINT_FDO Fdo,
    _In_opt_ uacpi_namespace_node *Node,
    _In_opt_ PDEVICE_OBJECT ForeignPdo)
{
    PUACPINT_FLT Filter;
    PUACPINT_FLT Match = NULL;
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&Fdo->ChildLock);

    for (Entry = Fdo->FilterList.Flink; Entry != &Fdo->FilterList; Entry = Entry->Flink)
    {
        Filter = CONTAINING_RECORD(Entry, UACPINT_FLT, Link);
        if ((Node && Filter->Node == Node) ||
            (ForeignPdo && Filter->ForeignPdo == ForeignPdo))
        {
            Match = Filter;
            break;
        }
    }

    ExReleaseFastMutex(&Fdo->ChildLock);
    return Match;
}

static
NTSTATUS
NTAPI
UacpiNtFilterAttach(
    _In_ PUACPINT_FDO Fdo,
    _In_ uacpi_namespace_node *Node,
    _In_ PDEVICE_OBJECT ForeignPdo)
{
    PDEVICE_OBJECT DeviceObject;
    PUACPINT_FLT Filter;
    uacpi_object_name Name;
    NTSTATUS Status;

    Status = IoCreateDevice(Fdo->Shared.Self->DriverObject,
                            sizeof(UACPINT_FLT),
                            NULL,
                            FILE_DEVICE_ACPI,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("uACPI-NT: IoCreateDevice for a filter failed 0x%lx\n", Status);
        return Status;
    }

    Filter = DeviceObject->DeviceExtension;
    RtlZeroMemory(Filter, sizeof(*Filter));
    Filter->Shared.Type = UacpiNtDevObjFilter;
    Filter->Shared.Self = DeviceObject;
    Filter->Fdo = Fdo;
    Filter->Node = Node;
    Filter->ForeignPdo = ForeignPdo;
    UacpiNtWakeInit(&Filter->Wake, Node);

    Filter->LowerDevice = IoAttachDeviceToDeviceStack(DeviceObject, ForeignPdo);
    if (!Filter->LowerDevice)
    {
        DPRINT1("uACPI-NT: could not attach a filter over PDO %p\n", ForeignPdo);
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Take over the I/O model, power flags and alignment of the stack */
    DeviceObject->Flags |= Filter->LowerDevice->Flags &
                           (DO_POWER_PAGABLE | DO_DIRECT_IO | DO_BUFFERED_IO);
    DeviceObject->AlignmentRequirement = Filter->LowerDevice->AlignmentRequirement;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    ExAcquireFastMutex(&Fdo->ChildLock);
    InsertTailList(&Fdo->FilterList, &Filter->Link);
    ExReleaseFastMutex(&Fdo->ChildLock);

    Name = uacpi_namespace_node_name(Node);
    DPRINT("uACPI-NT: filter %.4s attached over PDO %p\n", Name.text, ForeignPdo);
    return STATUS_SUCCESS;
}

/* Attaches a filter to each foreign PDO whose Address matches a child _ADR */
NTSTATUS
NTAPI
UacpiNtDetectFilterDevices(
    _In_ PUACPINT_FDO Fdo,
    _In_ uacpi_namespace_node *Parent,
    _In_ PDEVICE_RELATIONS Relations)
{
    uacpi_namespace_node *Child = NULL;
    PUACPINT_FILTER_PROBE Probes;
    DEVICE_CAPABILITIES Capabilities;
    PDEVICE_OBJECT Candidate;
    uacpi_u64 Adr;
    ULONG Index;

    if (!Fdo || !Parent || !Relations || !Relations->Count)
        return STATUS_SUCCESS;

    Probes = ExAllocatePoolWithTag(PagedPool, Relations->Count * sizeof(*Probes), UACPINT_POOL_TAG);
    if (!Probes)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Probes, Relations->Count * sizeof(*Probes));

    while (uacpi_likely_success(uacpi_namespace_node_next_typed(Parent, &Child,
                                                                UACPI_OBJECT_DEVICE_BIT)) &&
           Child)
    {
        if (!UacpiNtNodeIsPresent(Child))
            continue;

        /* Children without _ADR are enum.c PDOs */
        if (uacpi_unlikely_error(uacpi_eval_simple_integer(Child, "_ADR", &Adr)))
            continue;

        if (UacpiNtFindFilter(Fdo, Child, NULL) || UacpiNtFindPdoByNode(Fdo, Child))
            continue;

        for (Index = 0; Index < Relations->Count; Index++)
        {
            Candidate = Relations->Objects[Index];
            if (Candidate->DriverObject == GlobalAcpiDriverObj)
                continue;

            if (!Probes[Index].Queried)
            {
                Probes[Index].Queried = TRUE;
                if (NT_SUCCESS(UacpiNtQueryForeignCapabilities(Candidate, &Capabilities)))
                    Probes[Index].Address = Capabilities.Address;
                else
                    Probes[Index].Address = MAXULONG;
            }

            if (Probes[Index].Address == MAXULONG || Probes[Index].Address != (ULONG)Adr)
                continue;

            if (!UacpiNtFindFilter(Fdo, NULL, Candidate))
                UacpiNtFilterAttach(Fdo, Child, Candidate);
            break;
        }
    }

    ExFreePoolWithTag(Probes, UACPINT_POOL_TAG);
    return STATUS_SUCCESS;
}

/* ACPI PDOs go in before the IRP goes down, filters for the next level after */
static
NTSTATUS
NTAPI
UacpiNtFilterBusRelations(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    NTSTATUS Status;

    UacpiNtBuildChildPdosForNode(Filter->Fdo, Filter->Node);

    /* Another pass, the settled inventory waits for these to stop */
    UacpiNtEnumDiagArm(Filter->Fdo);

    Status = UacpiNtMergeChildRelations(Filter->Fdo, Filter->Node, NULL, Irp);
    if (!NT_SUCCESS(Status))
        return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    Status = UacpiNtForwardAndWait(Filter->LowerDevice, Irp);
    if (NT_SUCCESS(Status))
    {
        UacpiNtDetectFilterDevices(Filter->Fdo,
                                   Filter->Node,
                                   (PDEVICE_RELATIONS)Irp->IoStatus.Information);
    }

    return UacpiNtCompleteIrp(Irp, Irp->IoStatus.Status, Irp->IoStatus.Information);
}

static
NTSTATUS
NTAPI
UacpiNtFilterRemoveDevice(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PUACPINT_FDO Fdo = Filter->Fdo;
    PDEVICE_OBJECT LowerDevice = Filter->LowerDevice;
    PDEVICE_OBJECT Self = Filter->Shared.Self;
    NTSTATUS Status;

    /* Fail a pending WAIT_WAKE and disarm before the extension goes away */
    UacpiNtWakeTeardown(&Filter->Wake);

    ExAcquireFastMutex(&Fdo->ChildLock);
    RemoveEntryList(&Filter->Link);
    ExReleaseFastMutex(&Fdo->ChildLock);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Status = UacpiNtForwardAndForget(LowerDevice, Irp);

    IoDetachDevice(LowerDevice);
    IoDeleteDevice(Self);
    return Status;
}

/* The bus fills the caps, then _PRW adds wake so the power manager sends WAIT_WAKE */
static
NTSTATUS
NTAPI
UacpiNtFilterQueryCapabilities(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_CAPABILITIES Capabilities = IoStack->Parameters.DeviceCapabilities.Capabilities;
    NTSTATUS Status;

    Status = UacpiNtForwardAndWait(Filter->LowerDevice, Irp);
    if (NT_SUCCESS(Status) &&
        Capabilities &&
        Capabilities->Size >= sizeof(*Capabilities) &&
        Filter->Node)
    {
        UacpiNtPowerMergeWakeCaps(Filter->Node, Capabilities);
    }

    return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);
}

/**
 * @brief
 * Tells whether a _CRS descriptor is one the bus below cannot know about.
 *
 * @remarks
 * From ACPI 5 every fixed resource qualifies except the interrupts the bus routes itself.
 * A single vector interrupt always qualifies.
 */
static
BOOLEAN
NTAPI
UacpiNtIsSidebandResource(
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _In_ BOOLEAN Acpi5)
{
    if (Descriptor->Type == CmResourceTypeConfigData)
        return FALSE;

    if (Acpi5 && Descriptor->Type != CmResourceTypeInterrupt)
        return TRUE;

    return (Descriptor->u.Generic.Length == Descriptor->u.Generic.Alignment);
}

/**
 * @brief
 * Builds a requirements list with the sideband resources of Bios added to every configuration of Original.
 */
static
NTSTATUS
NTAPI
UacpiNtAddSidebandResources(
    _In_opt_ PIO_RESOURCE_REQUIREMENTS_LIST Original,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST Bios,
    _Out_ PIO_RESOURCE_REQUIREMENTS_LIST *Merged)
{
    PIO_RESOURCE_LIST BiosList = &Bios->List[0];
    PIO_RESOURCE_DESCRIPTOR Sideband[32];
    PIO_RESOURCE_REQUIREMENTS_LIST Result;
    PIO_RESOURCE_LIST Source, Target;
    struct acpi_fadt *Fadt = NULL;
    ULONG SidebandCount = 0, Alternatives, Size, i, j;
    BOOLEAN Acpi5;

    *Merged = NULL;

    Acpi5 = !uacpi_unlikely_error(uacpi_table_fadt(&Fadt)) && Fadt && Fadt->hdr.revision >= 5;

    for (i = 0; i < BiosList->Count && SidebandCount < RTL_NUMBER_OF(Sideband); i++)
    {
        if (!UacpiNtIsSidebandResource(&BiosList->Descriptors[i], Acpi5))
            continue;

        /* The same resource is only added once */
        for (j = 0; j < SidebandCount; j++)
        {
            if (RtlEqualMemory(Sideband[j], &BiosList->Descriptors[i], sizeof(*Sideband[j])))
                break;
        }

        if (j == SidebandCount)
            Sideband[SidebandCount++] = &BiosList->Descriptors[i];
    }

    if (SidebandCount == 0)
        return STATUS_NOT_FOUND;

    Alternatives = (Original && Original->AlternativeLists) ? Original->AlternativeLists : 1;

    Size = FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List);
    Source = Original ? &Original->List[0] : NULL;
    for (i = 0; i < Alternatives; i++)
    {
        ULONG Count = Source ? Source->Count : 0;

        Size += FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + (Count + SidebandCount) * sizeof(IO_RESOURCE_DESCRIPTOR);
        if (Source)
            Source = (PIO_RESOURCE_LIST)&Source->Descriptors[Source->Count];
    }

    Result = ExAllocatePoolZero(PagedPool, Size, UACPINT_POOL_TAG);
    if (!Result)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Original)
        RtlCopyMemory(Result, Original, FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List));
    else
        RtlCopyMemory(Result, Bios, FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List));

    Result->ListSize = Size;
    Result->AlternativeLists = Alternatives;

    Source = Original ? &Original->List[0] : NULL;
    Target = &Result->List[0];
    for (i = 0; i < Alternatives; i++)
    {
        ULONG Count = Source ? Source->Count : 0;

        Target->Version = Source ? Source->Version : BiosList->Version;
        Target->Revision = Source ? Source->Revision : BiosList->Revision;
        if (Count)
            RtlCopyMemory(Target->Descriptors, Source->Descriptors, Count * sizeof(IO_RESOURCE_DESCRIPTOR));

        for (j = 0; j < SidebandCount; j++)
            Target->Descriptors[Count + j] = *Sideband[j];

        Target->Count = Count + SidebandCount;
        Target = (PIO_RESOURCE_LIST)&Target->Descriptors[Target->Count];
        if (Source)
            Source = (PIO_RESOURCE_LIST)&Source->Descriptors[Source->Count];
    }

    *Merged = Result;
    return STATUS_SUCCESS;
}

/* The bus answers first, then the resources only the _CRS of the node describes are added */
static
NTSTATUS
NTAPI
UacpiNtFilterResourceRequirements(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PIO_RESOURCE_REQUIREMENTS_LIST Original, Bios = NULL, Merged;
    NTSTATUS Status;

    Status = UacpiNtForwardAndWait(Filter->LowerDevice, Irp);
    if ((!NT_SUCCESS(Status) && Status != STATUS_NOT_SUPPORTED) || !Filter->Node)
        return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);

    Original = (PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;
    if (!Original)
        Original = IoStack->Parameters.FilterResourceRequirements.IoResourceRequirementList;

    if (!NT_SUCCESS(UacpiNtCrsToRequirements(Filter->Node, NULL, &Bios)) || !Bios)
        return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);

    if (NT_SUCCESS(UacpiNtAddSidebandResources(Original, Bios, &Merged)))
    {
        /* Whoever changes the list frees the one it was handed */
        if (Original)
            ExFreePool(Original);

        Irp->IoStatus.Information = (ULONG_PTR)Merged;
        Status = STATUS_SUCCESS;
    }

    ExFreePoolWithTag(Bios, UACPINT_POOL_TAG);
    return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);
}

/* Answers the interfaces served here and always sends the IRP down */
static
NTSTATUS
NTAPI
UacpiNtFilterQueryInterface(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    const GUID *InterfaceType = IoStack->Parameters.QueryInterface.InterfaceType;
    BOOLEAN Interrupt;
    uacpi_object_name NodeName;
    CHAR Name[8] = "FILT";
    NTSTATUS Status;

    Interrupt = (ULONG_PTR)IoStack->Parameters.QueryInterface.InterfaceSpecificData ==
                CmResourceTypeInterrupt;

    if (Filter->Node)
    {
        NodeName = uacpi_namespace_node_name(Filter->Node);
        RtlCopyMemory(Name, NodeName.text, sizeof(NodeName.text));
        Name[sizeof(NodeName.text)] = ANSI_NULL;
    }

    if (Interrupt && IsEqualGUID(InterfaceType, &GUID_ARBITER_INTERFACE_STANDARD))
    {
        Status = UacpiNtQueryIrqArbiter(IoStack);
    }
    else if (Interrupt && IsEqualGUID(InterfaceType, &GUID_TRANSLATOR_INTERFACE_STANDARD))
    {
        Status = UacpiNtBuildIrqTranslator(Name, IoStack);
    }
    else
    {
        /* Filtered devnodes get the evaluation interface, then device reset */
        Status = UacpiNtBuildAcpiInterface(Filter->Shared.Self, Name, IoStack);
        if (Status == STATUS_NOT_SUPPORTED)
            Status = UacpiNtBuildDeviceResetInterface(Filter->Node, Name, IoStack);
    }

    if (Status != STATUS_NOT_SUPPORTED)
        Irp->IoStatus.Status = Status;

    return UacpiNtForwardAndForget(Filter->LowerDevice, Irp);
}

/* Paging, hibernate and dump path objects must not be power pageable */
static
NTSTATUS
NTAPI
UacpiNtFilterUsageNotification(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN InPath = IoStack->Parameters.UsageNotification.InPath;
    PDEVICE_OBJECT Self = Filter->Shared.Self;
    BOOLEAN ClearedPageable = FALSE;
    BOOLEAN Unused;
    PLONG Counter;
    NTSTATUS Status;

    if (IoStack->Parameters.UsageNotification.Type == DeviceUsageTypeHibernation)
        Counter = &Filter->HibernationCount;
    else if (IoStack->Parameters.UsageNotification.Type == DeviceUsageTypeDumpFile)
        Counter = &Filter->DumpCount;
    else
        Counter = &Filter->PagingCount;

    Unused = !Filter->PagingCount && !Filter->HibernationCount && !Filter->DumpCount;
    if (InPath && Unused)
    {
        Self->Flags &= ~DO_POWER_PAGABLE;
        ClearedPageable = TRUE;
    }

    Status = UacpiNtForwardAndWait(Filter->LowerDevice, Irp);
    if (NT_SUCCESS(Status))
    {
        IoAdjustPagingPathCount(Counter, InPath);

        if (!InPath && !Filter->PagingCount && !Filter->HibernationCount && !Filter->DumpCount)
            Self->Flags |= DO_POWER_PAGABLE;
    }
    else if (ClearedPageable)
    {
        Self->Flags |= DO_POWER_PAGABLE;
    }

    return UacpiNtCompleteIrp(Irp, Status, Irp->IoStatus.Information);
}

NTSTATUS
NTAPI
UacpiNtFilterPnp(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IoStack->Parameters.QueryDeviceRelations.Type == BusRelations)
                return UacpiNtFilterBusRelations(Filter, Irp);
            return UacpiNtForwardAndForget(Filter->LowerDevice, Irp);

        case IRP_MN_REMOVE_DEVICE:
            return UacpiNtFilterRemoveDevice(Filter, Irp);

        case IRP_MN_QUERY_CAPABILITIES:
            return UacpiNtFilterQueryCapabilities(Filter, Irp);

        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
            return UacpiNtFilterResourceRequirements(Filter, Irp);

        case IRP_MN_QUERY_INTERFACE:
            return UacpiNtFilterQueryInterface(Filter, Irp);

        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            return UacpiNtFilterUsageNotification(Filter, Irp);

        /* The device is gone, fail the pending wait and drop the GPE now */
        case IRP_MN_SURPRISE_REMOVAL:
            UacpiNtWakeTeardown(&Filter->Wake);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return UacpiNtForwardAndForget(Filter->LowerDevice, Irp);

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return UacpiNtForwardAndForget(Filter->LowerDevice, Irp);

        default:
            return UacpiNtForwardAndForget(Filter->LowerDevice, Irp);
    }
}

/*
 * The bus below knows nothing about the _PRW GPE, so a WAIT_WAKE for a node
 * with a usable _PRW is owned here and completed on Notify(2).
 */
NTSTATUS
NTAPI
UacpiNtFilterPower(
    _In_ PUACPINT_FLT Filter,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);

    PoStartNextPowerIrp(Irp);

    if (IoStack->MinorFunction == IRP_MN_WAIT_WAKE &&
        Filter->Node &&
        UacpiNtWakeHasPrw(&Filter->Wake))
    {
        return UacpiNtWakeArm(&Filter->Wake, Irp);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Filter->LowerDevice, Irp);
}
