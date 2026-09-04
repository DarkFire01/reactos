/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Resource translators and legacy bus lookup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define TAG_IO_TRANSLATOR 'rTpI'

/* Resource types with a bit in the translator and arbiter masks of a device node */
#define IOP_MASKED_RESOURCE_TYPES (RTL_FIELD_SIZE(DEVICE_NODE, NoTranslatorMask) * 8)

/*
 * Answer of a device node for the translator of one resource type. A missing
 * translator is remembered in the masks of the device node for the masked
 * types, and with an entry that is not Present for the other types.
 */
typedef struct _IOP_TRANSLATOR_ENTRY
{
    LIST_ENTRY ListEntry;
    UCHAR ResourceType;
    BOOLEAN Present;
    TRANSLATOR_INTERFACE Interface;
} IOP_TRANSLATOR_ENTRY, *PIOP_TRANSLATOR_ENTRY;

/* Bus device nodes of each legacy interface type, sorted by bus number */
static LIST_ENTRY IopLegacyBusLists[Vmcs + 1];

#if DBG
/* The first translations that failed, for inspection in the debugger */
typedef struct _IOP_TRANSLATION_FAILURE
{
    PDEVICE_NODE DeviceNode;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Resource;
} IOP_TRANSLATION_FAILURE;

IOP_TRANSLATION_FAILURE IopTranslationFailures[32];
ULONG IopTranslationFailureCount;
#endif

/* ROOT TRANSLATOR **********************************************************/

/* The root translator has no state, so there is nothing to count */
static
VOID
NTAPI
IopRootTranslatorReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/* The root bus uses the same resources as the processor, so they are copied */
static
NTSTATUS
NTAPI
IopRootTranslateResources(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Direction);
    UNREFERENCED_PARAMETER(AlternativesCount);
    UNREFERENCED_PARAMETER(Alternatives);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    *Target = *Source;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
IopRootTranslateRequirements(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR Copy;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    Copy = ExAllocatePoolWithTag(PagedPool, sizeof(*Copy), TAG_IO_TRANSLATOR);
    if (Copy == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Copy = *Source;
    *Target = Copy;
    *TargetCount = 1;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Handles IRP_MN_QUERY_INTERFACE on the root PDO for the root translator. The
 * same translator is returned for every resource type.
 *
 * @return
 * STATUS_SUCCESS if the interface was returned, ExistingStatus otherwise.
 */
NTSTATUS
NTAPI
IopTranslatorQueryRootInterface(
    _In_ PIO_STACK_LOCATION IoStack,
    _In_ NTSTATUS ExistingStatus)
{
    PTRANSLATOR_INTERFACE Output;

    PAGED_CODE();

    if (!IsEqualGUID(IoStack->Parameters.QueryInterface.InterfaceType,
                     &GUID_TRANSLATOR_INTERFACE_STANDARD) ||
        IoStack->Parameters.QueryInterface.Size < sizeof(TRANSLATOR_INTERFACE) ||
        IoStack->Parameters.QueryInterface.Interface == NULL)
    {
        return ExistingStatus;
    }

    Output = (PTRANSLATOR_INTERFACE)IoStack->Parameters.QueryInterface.Interface;
    Output->Size = sizeof(TRANSLATOR_INTERFACE);
    Output->Version = 0;
    Output->Context = NULL;
    Output->InterfaceReference = IopRootTranslatorReference;
    Output->InterfaceDereference = IopRootTranslatorReference;
    Output->TranslateResources = IopRootTranslateResources;
    Output->TranslateResourceRequirements = IopRootTranslateRequirements;

    return STATUS_SUCCESS;
}

/* DEVICE TRANSLATORS *******************************************************/

/**
 * @brief
 * Checks if the stack of a device node may be asked for an arbiter or a
 * translator. Device nodes of legacy resource claims and devices that were not
 * enumerated by a bus are never asked.
 */
BOOLEAN
NTAPI
IopCanQueryResourceHandlers(
    _In_ PDEVICE_NODE DeviceNode)
{
    return DeviceNode->PhysicalDeviceObject != NULL &&
           !(DeviceNode->Flags & DNF_LEGACY_RESOURCE_DEVICENODE) &&
           (DeviceNode->PhysicalDeviceObject->Flags & DO_BUS_ENUMERATED_DEVICE);
}

static
PIOP_TRANSLATOR_ENTRY
IopFindTranslatorEntry(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType)
{
    PLIST_ENTRY ListEntry;

    for (ListEntry = DeviceNode->DeviceTranslatorList.Flink;
         ListEntry != &DeviceNode->DeviceTranslatorList;
         ListEntry = ListEntry->Flink)
    {
        PIOP_TRANSLATOR_ENTRY Entry = CONTAINING_RECORD(ListEntry, IOP_TRANSLATOR_ENTRY, ListEntry);

        if (Entry->ResourceType == ResourceType)
            return Entry;
    }

    return NULL;
}

/**
 * @brief
 * Returns the cached answer of a device node for a translator.
 *
 * @param[out] Translator
 * Receives the translator, or NULL if the device has none.
 *
 * @return
 * FALSE if the device was not asked yet.
 */
static
BOOLEAN
IopGetCachedTranslator(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PTRANSLATOR_INTERFACE *Translator)
{
    PIOP_TRANSLATOR_ENTRY Entry = IopFindTranslatorEntry(DeviceNode, ResourceType);

    *Translator = NULL;

    if (Entry != NULL)
    {
        if (Entry->Present)
            *Translator = &Entry->Interface;

        return TRUE;
    }

    return ResourceType < IOP_MASKED_RESOURCE_TYPES &&
           (DeviceNode->NoTranslatorMask & (1 << ResourceType));
}

/* Sends IRP_MN_QUERY_INTERFACE for GUID_TRANSLATOR_INTERFACE_STANDARD to a device */
static
NTSTATUS
IopQueryTranslatorInterface(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PTRANSLATOR_INTERFACE Interface)
{
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STACK_LOCATION Stack;
    NTSTATUS Status;

    RtlZeroMemory(Interface, sizeof(*Interface));

    if (!IopCanQueryResourceHandlers(DeviceNode))
        return STATUS_NOT_SUPPORTED;

    Interface->Size = sizeof(*Interface);

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryInterface.InterfaceType = &GUID_TRANSLATOR_INTERFACE_STANDARD;
    Stack.Parameters.QueryInterface.Size = sizeof(*Interface);
    Stack.Parameters.QueryInterface.Version = 0;
    Stack.Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    Stack.Parameters.QueryInterface.InterfaceSpecificData = UlongToPtr(ResourceType);

    Status = IopInitiatePnpIrp(DeviceNode->PhysicalDeviceObject,
                               &IoStatusBlock,
                               IRP_MN_QUERY_INTERFACE,
                               &Stack);
    if (NT_SUCCESS(Status) &&
        (Interface->TranslateResources == NULL || Interface->TranslateResourceRequirements == NULL))
    {
        DPRINT1("%wZ returned a translator without handlers\n", &DeviceNode->InstancePath);

        if (Interface->InterfaceDereference != NULL)
            Interface->InterfaceDereference(Interface->Context);

        Status = STATUS_UNSUCCESSFUL;
    }

    return Status;
}

/**
 * @brief
 * Returns the translator of a device node for a resource type. The stack of
 * the device is asked once, and the answer is cached on the device node.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_FOUND if the device has no translator for the
 * type, or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
NTAPI
IopGetDeviceTranslator(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PTRANSLATOR_INTERFACE *Translator)
{
    USHORT TypeBit = (ResourceType < IOP_MASKED_RESOURCE_TYPES) ? (USHORT)(1 << ResourceType) : 0;
    PIOP_TRANSLATOR_ENTRY Entry;
    TRANSLATOR_INTERFACE Queried;
    NTSTATUS Status;

    PAGED_CODE();

    if (IopGetCachedTranslator(DeviceNode, ResourceType, Translator))
        return (*Translator != NULL) ? STATUS_SUCCESS : STATUS_NOT_FOUND;

    Status = IopQueryTranslatorInterface(DeviceNode, ResourceType, &Queried);
    DeviceNode->QueryTranslatorMask |= TypeBit;

    if (!NT_SUCCESS(Status))
    {
        DeviceNode->NoTranslatorMask |= TypeBit;
        if (TypeBit != 0)
            return STATUS_NOT_FOUND;
    }

    Entry = ExAllocatePoolZero(PagedPool, sizeof(*Entry), TAG_IO_TRANSLATOR);
    if (Entry == NULL)
    {
        if (NT_SUCCESS(Status) && Queried.InterfaceDereference != NULL)
            Queried.InterfaceDereference(Queried.Context);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Entry->ResourceType = ResourceType;
    Entry->Present = NT_SUCCESS(Status);
    Entry->Interface = Queried;
    InsertTailList(&DeviceNode->DeviceTranslatorList, &Entry->ListEntry);

    if (!Entry->Present)
        return STATUS_NOT_FOUND;

    DPRINT("%wZ translates resource type %u\n", &DeviceNode->InstancePath, ResourceType);

    *Translator = &Entry->Interface;
    return STATUS_SUCCESS;
}

/* Releases the translators cached on a device node */
VOID
NTAPI
IopFreeDeviceNodeTranslators(
    _In_ PDEVICE_NODE DeviceNode)
{
    while (!IsListEmpty(&DeviceNode->DeviceTranslatorList))
    {
        PIOP_TRANSLATOR_ENTRY Entry =
            CONTAINING_RECORD(RemoveHeadList(&DeviceNode->DeviceTranslatorList),
                              IOP_TRANSLATOR_ENTRY,
                              ListEntry);

        if (Entry->Present && Entry->Interface.InterfaceDereference != NULL)
            Entry->Interface.InterfaceDereference(Entry->Interface.Context);

        ExFreePoolWithTag(Entry, TAG_IO_TRANSLATOR);
    }

    DeviceNode->NoTranslatorMask = 0;
    DeviceNode->QueryTranslatorMask = 0;
}

/* LEGACY BUSES *************************************************************/

static
PLIST_ENTRY
IopLegacyBusList(
    _In_ INTERFACE_TYPE InterfaceType)
{
    /* EISA buses are found as ISA buses */
    if (InterfaceType == Eisa)
        InterfaceType = Isa;

    if ((ULONG)InterfaceType > (ULONG)Vmcs || InterfaceType == PNPBus)
        return NULL;

    return &IopLegacyBusLists[InterfaceType];
}

CODE_SEG("INIT")
VOID
NTAPI
IopInitializeLegacyBusLists(VOID)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(IopLegacyBusLists); Index++)
        InitializeListHead(&IopLegacyBusLists[Index]);
}

/**
 * @brief
 * Returns the device node of the bus that provides a legacy bus, or the root
 * device node if no device provides it.
 */
PDEVICE_NODE
NTAPI
IopFindLegacyBusDeviceNode(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber)
{
    PLIST_ENTRY List = IopLegacyBusList(InterfaceType);
    PLIST_ENTRY ListEntry;

    if (List == NULL)
        return IopRootDeviceNode;

    for (ListEntry = List->Flink; ListEntry != List; ListEntry = ListEntry->Flink)
    {
        PDEVICE_NODE Bus = CONTAINING_RECORD(ListEntry, DEVICE_NODE, LegacyBusListEntry);

        if (Bus->BusNumber > BusNumber)
            break;

        if (Bus->BusNumber == BusNumber)
            return Bus;
    }

    return IopRootDeviceNode;
}

/* Adds a device node to the list of its legacy bus type, unless the bus is already there */
static
VOID
IopInsertLegacyBus(
    _In_ PDEVICE_NODE DeviceNode)
{
    PLIST_ENTRY List = IopLegacyBusList(DeviceNode->InterfaceType);
    PLIST_ENTRY Next;

    if (List == NULL)
        return;

    for (Next = List->Flink; Next != List; Next = Next->Flink)
    {
        PDEVICE_NODE Bus = CONTAINING_RECORD(Next, DEVICE_NODE, LegacyBusListEntry);

        if (Bus->BusNumber == DeviceNode->BusNumber)
            return;

        if (Bus->BusNumber > DeviceNode->BusNumber)
            break;
    }

    /* The list is read without a lock, so it is linked in last */
    DeviceNode->LegacyBusListEntry.Flink = Next;
    DeviceNode->LegacyBusListEntry.Blink = Next->Blink;
    Next->Blink->Flink = &DeviceNode->LegacyBusListEntry;
    Next->Blink = &DeviceNode->LegacyBusListEntry;
}

/**
 * @brief
 * Asks a device which legacy bus it provides, with
 * IRP_MN_QUERY_LEGACY_BUS_INFORMATION, and makes the device the owner of that
 * bus for resources that the device tree does not lead to.
 */
VOID
NTAPI
IopRegisterLegacyBus(
    _In_ PDEVICE_NODE DeviceNode)
{
    PLEGACY_BUS_INFORMATION BusInformation;
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STACK_LOCATION Stack;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(&Stack, sizeof(Stack));
    Status = IopInitiatePnpIrp(DeviceNode->PhysicalDeviceObject,
                               &IoStatusBlock,
                               IRP_MN_QUERY_LEGACY_BUS_INFORMATION,
                               &Stack);
    if (!NT_SUCCESS(Status))
    {
        DeviceNode->InterfaceType = InterfaceTypeUndefined;
        DeviceNode->BusNumber = 0xFFFFFFF0;
        return;
    }

    BusInformation = (PLEGACY_BUS_INFORMATION)IoStatusBlock.Information;
    if (BusInformation != NULL)
    {
        DeviceNode->InterfaceType = BusInformation->LegacyBusType;
        DeviceNode->BusNumber = BusInformation->BusNumber;
        ExFreePool(BusInformation);
    }
    else
    {
        DPRINT1("%wZ succeeded IRP_MN_QUERY_LEGACY_BUS_INFORMATION without data\n",
                &DeviceNode->InstancePath);
    }

    IopInsertLegacyBus(DeviceNode);
}

/* Removes a device node that leaves the device tree from the legacy bus lists */
VOID
NTAPI
IopUnregisterLegacyBus(
    _In_ PDEVICE_NODE DeviceNode)
{
    /* Most device nodes never provide a legacy bus */
    if (IsListEmpty(&DeviceNode->LegacyBusListEntry))
        return;

    RemoveEntryList(&DeviceNode->LegacyBusListEntry);
    InitializeListHead(&DeviceNode->LegacyBusListEntry);
}

/**
 * @brief
 * Returns the bus a resource belongs to when the device tree does not lead to
 * a translator. An internal resource uses ISA bus 0 when there is no internal
 * bus.
 *
 * @param[in] ListInterfaceType
 * The interface type of the whole resource list, which decides the fallback.
 */
PDEVICE_NODE
NTAPI
IopFindResourceBus(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ INTERFACE_TYPE ListInterfaceType)
{
    PDEVICE_NODE Bus = IopFindLegacyBusDeviceNode(InterfaceType, BusNumber);

    if (Bus == IopRootDeviceNode && ListInterfaceType == Internal)
        Bus = IopFindLegacyBusDeviceNode(Isa, 0);

    return Bus;
}

/**
 * @brief
 * Returns the device node above a device node for resources. Device nodes of
 * legacy resource claims are not linked into the device tree, and belong to
 * the root.
 */
PDEVICE_NODE
NTAPI
IopGetResourceParent(
    _In_ PDEVICE_NODE DeviceNode)
{
    if (DeviceNode->Parent == NULL && DeviceNode != IopRootDeviceNode)
        return IopRootDeviceNode;

    return DeviceNode->Parent;
}

/* TRANSLATION **************************************************************/

/* Changes a requirement so that it cannot be satisfied */
static
VOID
IopMakeRequirementUnsatisfiable(
    _Inout_ PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    switch (Descriptor->Type)
    {
        case CmResourceTypePort:
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            Descriptor->u.Generic.MinimumAddress.QuadPart = 2;
            Descriptor->u.Generic.MaximumAddress.QuadPart = 1;
            break;

        case CmResourceTypeInterrupt:
            Descriptor->u.Interrupt.MinimumVector = 2;
            Descriptor->u.Interrupt.MaximumVector = 1;
            break;

        case CmResourceTypeDma:
            Descriptor->u.Dma.MinimumChannel = 2;
            Descriptor->u.Dma.MaximumChannel = 1;
            break;

        case CmResourceTypeBusNumber:
            Descriptor->u.BusNumber.MinBusNumber = 2;
            Descriptor->u.BusNumber.MaxBusNumber = 1;
            break;

        default:
            DPRINT1("Cannot restrict requirement type %u\n", Descriptor->Type);
            ASSERT(FALSE);
            break;
    }
}

/**
 * @brief
 * Translates the alternatives of a requirement to the resource space of the
 * parent bus. An alternative the translator does not translate is kept, but
 * cannot be satisfied.
 *
 * @param[out] Translated
 * Receives the translated alternatives, from paged pool.
 *
 * @return
 * STATUS_TRANSLATION_COMPLETE if an alternative was translated up to the root,
 * another success status if any alternative was translated, or the status of
 * the last alternative when none was.
 */
NTSTATUS
NTAPI
IopTranslateRequirement(
    _In_ PTRANSLATOR_INTERFACE Translator,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_reads_(AlternativeCount) PIO_RESOURCE_DESCRIPTOR Alternatives,
    _In_ ULONG AlternativeCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Translated,
    _Out_ PULONG TranslatedCount)
{
    struct
    {
        PIO_RESOURCE_DESCRIPTOR Descriptors;
        ULONG Count;
    } *Results;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    NTSTATUS SuccessStatus = STATUS_SUCCESS;
    PIO_RESOURCE_DESCRIPTOR Output, Cursor;
    BOOLEAN AnyTranslated = FALSE;
    ULONG Total = 0;
    ULONG Index;

    PAGED_CODE();

    *Translated = NULL;
    *TranslatedCount = 0;

    if (AlternativeCount == 0)
        return STATUS_INVALID_PARAMETER;

    Results = ExAllocatePoolZero(PagedPool, AlternativeCount * sizeof(*Results), TAG_IO_TRANSLATOR);
    if (Results == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < AlternativeCount; Index++)
    {
        PIO_RESOURCE_DESCRIPTOR Descriptors = NULL;
        ULONG Count = 0;

        Status = Translator->TranslateResourceRequirements(Translator->Context,
                                                           &Alternatives[Index],
                                                           PhysicalDeviceObject,
                                                           &Count,
                                                           &Descriptors);
        if (NT_SUCCESS(Status) && Count != 0)
        {
            Results[Index].Descriptors = Descriptors;
            Results[Index].Count = Count;
            Total += Count;
            AnyTranslated = TRUE;
        }
        else
        {
            DPRINT("Requirement %lu of %p was not translated (Status 0x%08lx)\n",
                   Index, PhysicalDeviceObject, Status);
            Total++;
        }

        if (NT_SUCCESS(Status) && SuccessStatus != STATUS_TRANSLATION_COMPLETE)
            SuccessStatus = Status;
    }

    if (AnyTranslated)
        Status = SuccessStatus;

    Output = ExAllocatePoolWithTag(PagedPool, Total * sizeof(*Output), TAG_IO_TRANSLATOR);
    if (Output == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Cursor = Output;
    for (Index = 0; Index < AlternativeCount; Index++)
    {
        if (Results[Index].Count != 0)
        {
            RtlCopyMemory(Cursor, Results[Index].Descriptors, Results[Index].Count * sizeof(*Cursor));
            Cursor += Results[Index].Count;
        }
        else
        {
            *Cursor = Alternatives[Index];
            IopMakeRequirementUnsatisfiable(Cursor);
            Cursor++;
        }
    }

    if (NT_SUCCESS(Status))
    {
        *Translated = Output;
        *TranslatedCount = Total;
    }
    else
    {
        ExFreePoolWithTag(Output, TAG_IO_TRANSLATOR);
    }

Cleanup:
    for (Index = 0; Index < AlternativeCount; Index++)
    {
        if (Results[Index].Descriptors != NULL)
            ExFreePool(Results[Index].Descriptors);
    }

    ExFreePoolWithTag(Results, TAG_IO_TRANSLATOR);
    return Status;
}

#if DBG
static
VOID
IopRecordTranslationFailure(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    if (IopTranslationFailureCount < RTL_NUMBER_OF(IopTranslationFailures))
    {
        IopTranslationFailures[IopTranslationFailureCount].DeviceNode = DeviceNode;
        IopTranslationFailures[IopTranslationFailureCount].Resource = *Resource;
        IopTranslationFailureCount++;
    }
}
#else
#define IopRecordTranslationFailure(DeviceNode, Resource)
#endif

/**
 * @brief
 * Translates an assigned resource to the resource the processor uses, with the
 * translators cached on the device node and above it. When the root is
 * reached, the translation continues from the bus that provides the legacy bus
 * of the resource, unless the resource was reported by the HAL.
 *
 * @param[in] DeviceNode
 * The device the resource is assigned to, or NULL to start at the legacy bus.
 *
 * @param[out] Translated
 * Receives the translated resource.
 *
 * @return
 * The status of the last translator that was called, or its failure status.
 */
NTSTATUS
NTAPI
IopTranslateResourceToRoot(
    _In_opt_ PDEVICE_NODE DeviceNode,
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR Current = *Raw;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Next;
    PDEVICE_OBJECT PhysicalDeviceObject = NULL;
    BOOLEAN LegacyBusVisited = (RequestSource == ArbiterRequestHalReported);
    NTSTATUS Status = STATUS_SUCCESS;
    PDEVICE_NODE Node;

    PAGED_CODE();

    if (DeviceNode != NULL)
    {
        Node = DeviceNode;
        PhysicalDeviceObject = DeviceNode->PhysicalDeviceObject;
    }
    else
    {
        Node = IopFindLegacyBusDeviceNode(InterfaceType, BusNumber);
    }

    while (Node != NULL)
    {
        PTRANSLATOR_INTERFACE Translator;

        if (Node == IopRootDeviceNode && !LegacyBusVisited)
        {
            LegacyBusVisited = TRUE;
            Node = IopFindResourceBus(InterfaceType, BusNumber, InterfaceType);
            continue;
        }

        if (IopGetCachedTranslator(Node, Raw->Type, &Translator) && Translator != NULL)
        {
            Status = Translator->TranslateResources(Translator->Context,
                                                    &Current,
                                                    TranslateChildToParent,
                                                    0,
                                                    NULL,
                                                    PhysicalDeviceObject,
                                                    &Next);
            if (!NT_SUCCESS(Status))
            {
                if (DeviceNode != NULL && Status != STATUS_RETRY)
                {
                    DPRINT1("Translator of %wZ failed for %wZ, type %u data %lx %lx %lx (Status 0x%08lx)\n",
                            &Node->InstancePath, &DeviceNode->InstancePath, Current.Type,
                            Current.u.DevicePrivate.Data[0], Current.u.DevicePrivate.Data[1],
                            Current.u.DevicePrivate.Data[2], Status);
                    IopRecordTranslationFailure(DeviceNode, &Current);
                }

                return Status;
            }

            Current = Next;

            if (Status == STATUS_TRANSLATION_COMPLETE)
                break;
        }

        Node = IopGetResourceParent(Node);
    }

    *Translated = Current;
    return Status;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
IoTranslateBusAddress(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ PHYSICAL_ADDRESS BusAddress,
    _Inout_ PULONG AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR Current, Next;
    PDEVICE_NODE Node;

    /* Translators are only called at PASSIVE_LEVEL, and need the device tree */
    if (KeGetCurrentIrql() > PASSIVE_LEVEL || IopRootDeviceNode == NULL)
    {
        *TranslatedAddress = BusAddress;
        return TRUE;
    }

    RtlZeroMemory(&Current, sizeof(Current));
    Current.ShareDisposition = CmResourceShareShared;
    Current.u.Generic.Start = BusAddress;
    Current.u.Generic.Length = 1;

    switch (*AddressSpace)
    {
        case 0:
            Current.Type = CmResourceTypeMemory;
            Current.Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            break;

        case 1:
            Current.Type = CmResourceTypePort;
            Current.Flags = CM_RESOURCE_PORT_IO;
            break;

        default:
            return FALSE;
    }

    for (Node = IopFindLegacyBusDeviceNode(InterfaceType, BusNumber);
         Node != IopRootDeviceNode && Node != NULL;
         Node = IopGetResourceParent(Node))
    {
        PTRANSLATOR_INTERFACE Translator;
        TRANSLATOR_INTERFACE Queried;
        BOOLEAN Cached;
        NTSTATUS Status;

        Cached = IopGetCachedTranslator(Node, Current.Type, &Translator);
        if (!Cached)
        {
            /* A translator that is not cached yet is only used for this call */
            if (!NT_SUCCESS(IopQueryTranslatorInterface(Node, Current.Type, &Queried)))
                continue;

            Translator = &Queried;
        }
        else if (Translator == NULL)
        {
            continue;
        }

        Status = Translator->TranslateResources(Translator->Context,
                                                &Current,
                                                TranslateChildToParent,
                                                0,
                                                NULL,
                                                NULL,
                                                &Next);

        if (!Cached && Queried.InterfaceDereference != NULL)
            Queried.InterfaceDereference(Queried.Context);

        if (!NT_SUCCESS(Status))
            return FALSE;

        Current = Next;

        if (Status == STATUS_TRANSLATION_COMPLETE)
            break;
    }

    switch (Current.Type)
    {
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            *AddressSpace = 0;
            break;

        case CmResourceTypePort:
            *AddressSpace = 1;
            break;

        default:
            ASSERT(FALSE);
            return FALSE;
    }

    *TranslatedAddress = Current.u.Generic.Start;
    return TRUE;
}

/* EOF */
