/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     PnP manager resource arbitration and assignment
 * COPYRIGHT:   Copyright 2005 Cameron Gutman <cameron.gutman@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define TAG_IO_ARBITER 'AbrI'

/* Initialized by IopInitializeArbiters */
extern ARBITER_INSTANCE IopRootBusNumberArbiter;
extern ARBITER_INSTANCE IopRootIrqArbiter;
extern ARBITER_INSTANCE IopRootDmaArbiter;
extern ARBITER_INSTANCE IopRootMemArbiter;
extern ARBITER_INSTANCE IopRootPortArbiter;

static const struct
{
    UCHAR ResourceType;
    PARBITER_INSTANCE Instance;
} IopRootArbiterTable[] =
{
    { CmResourceTypePort,      &IopRootPortArbiter },
    { CmResourceTypeInterrupt, &IopRootIrqArbiter },
    { CmResourceTypeMemory,    &IopRootMemArbiter },
    { CmResourceTypeDma,       &IopRootDmaArbiter },
    { CmResourceTypeBusNumber, &IopRootBusNumberArbiter },
};

/* Interfaces of the root arbiters, in the same order as IopRootArbiterTable */
static ARBITER_INTERFACE IopRootArbiterInterface[RTL_NUMBER_OF(IopRootArbiterTable)];

/*
 * Arbiter entry for an arbiter found on a device. The interface returned by
 * the device is kept in the entry, since the IRP buffer it came in does not
 * last. For a resource type without a bit in the arbiter masks, an entry
 * without interface remembers that the device has no arbiter.
 */
typedef struct _IOP_ARBITER_ENTRY
{
    PI_RESOURCE_ARBITER_ENTRY Entry;
    ARBITER_INTERFACE Interface;
} IOP_ARBITER_ENTRY, *PIOP_ARBITER_ENTRY;

/* Resource types with a bit in the arbiter masks of a device node */
#define IOP_MASKED_RESOURCE_TYPES (RTL_FIELD_SIZE(DEVICE_NODE, NoArbiterMask) * 8)

/* Clear to skip reserving the RESOURCEMAP resources in the root arbiters */
BOOLEAN IopArbiterSeedResourceMap = TRUE;

CODE_SEG("INIT") static VOID IopArbiterSeedFromResourceMap(VOID);

/*
 * Root enumerated devices describe resources of buses whose drivers are not
 * loaded yet. Their boot configurations are held on this list until the boot
 * drivers are started, then reserved by IopReserveDeferredBootConfigs.
 */
typedef struct _IOP_PENDING_BOOT_CONFIG
{
    LIST_ENTRY ListEntry;
    PDEVICE_OBJECT DeviceObject;
} IOP_PENDING_BOOT_CONFIG, *PIOP_PENDING_BOOT_CONFIG;

static LIST_ENTRY IopPendingBootConfigList;
static BOOLEAN IopHoldRootBootConfigs = TRUE;

/* Serializes all arbiter operations */
static ERESOURCE IopResourceAssignmentLock;

/* sdk/lib/rtl/memres.c */
ULONGLONG
NTAPI
RtlCmDecodeMemIoResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Out_opt_ PULONGLONG Start);

NTSTATUS
NTAPI
RtlIoEncodeMemIoResource(
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _In_ UCHAR Type,
    _In_ ULONGLONG Length,
    _In_ ULONGLONG Alignment,
    _In_ ULONGLONG MinimumAddress,
    _In_ ULONGLONG MaximumAddress);

/* FUNCTIONS ****************************************************************/

/* LOCKING *******************************************************************/

static
VOID
IopLockResourceAssignment(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&IopResourceAssignmentLock, TRUE);
}

static
VOID
IopUnlockResourceAssignment(VOID)
{
    ExReleaseResourceLite(&IopResourceAssignmentLock);
    KeLeaveCriticalRegion();
}

/* ARBITER DISCOVERY ********************************************************/

/* The root arbiters are never freed, so there is nothing to count */
static
VOID
NTAPI
IopRootArbiterReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
VOID
NTAPI
IopRootArbiterDereference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/* Large memory ranges are arbitrated with the memory ranges */
static
UCHAR
IopArbitratedType(
    _In_ UCHAR ResourceType)
{
    return (ResourceType == CmResourceTypeMemoryLarge) ? CmResourceTypeMemory : ResourceType;
}

/**
 * @brief
 * Returns the root arbiter interface for a resource type, or NULL.
 * CmResourceTypeMemoryLarge uses the memory arbiter.
 */
static
CODE_SEG("PAGE")
PARBITER_INTERFACE
IopGetRootArbiterInterface(
    _In_ UCHAR ResourceType)
{
    ULONG Index;

    PAGED_CODE();

    ResourceType = IopArbitratedType(ResourceType);

    for (Index = 0; Index < RTL_NUMBER_OF(IopRootArbiterTable); Index++)
    {
        if (IopRootArbiterTable[Index].ResourceType == ResourceType)
            return &IopRootArbiterInterface[Index];
    }

    return NULL;
}

static
VOID
IopInitializeArbiterEntry(
    _Out_ PPI_RESOURCE_ARBITER_ENTRY Entry,
    _In_ UCHAR ResourceType,
    _In_opt_ PARBITER_INTERFACE Interface,
    _In_ ULONG Level)
{
    Entry->ResourceType = ResourceType;
    Entry->ArbiterInterface = Interface;
    Entry->Level = Level;
    InitializeListHead(&Entry->ResourceList);
    InitializeListHead(&Entry->BestResourceList);
    InitializeListHead(&Entry->BestConfig);
    InitializeListHead(&Entry->ActiveArbiterList);
}

/* Returns the arbiter entry of a resource type cached on a device node, or NULL */
static
PPI_RESOURCE_ARBITER_ENTRY
IopFindArbiterEntry(
    _In_ PDEVICE_NODE Node,
    _In_ UCHAR ResourceType)
{
    PLIST_ENTRY ListEntry;

    for (ListEntry = Node->DeviceArbiterList.Flink;
         ListEntry != &Node->DeviceArbiterList;
         ListEntry = ListEntry->Flink)
    {
        PPI_RESOURCE_ARBITER_ENTRY Entry =
            CONTAINING_RECORD(ListEntry, PI_RESOURCE_ARBITER_ENTRY, DeviceArbiterList);

        if (Entry->ResourceType == ResourceType)
            return Entry;
    }

    return NULL;
}

/**
 * @brief
 * Sends IRP_MN_QUERY_INTERFACE for GUID_ARBITER_INTERFACE_STANDARD to a device.
 *
 * @remarks
 * The interface version is 0. The PCI driver refuses any other version, and
 * PCI devices would then be assigned from the root arbiters instead.
 */
static
NTSTATUS
IopQueryArbiterInterface(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PARBITER_INTERFACE Interface)
{
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STACK_LOCATION Stack;
    NTSTATUS Status;

    RtlZeroMemory(Interface, sizeof(*Interface));

    if (!IopCanQueryResourceHandlers(DeviceNode))
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryInterface.InterfaceType = &GUID_ARBITER_INTERFACE_STANDARD;
    Stack.Parameters.QueryInterface.Size = sizeof(ARBITER_INTERFACE);
    Stack.Parameters.QueryInterface.Version = 0;
    Stack.Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    Stack.Parameters.QueryInterface.InterfaceSpecificData = UlongToPtr(ResourceType);

    Status = IopInitiatePnpIrp(DeviceNode->PhysicalDeviceObject,
                               &IoStatusBlock,
                               IRP_MN_QUERY_INTERFACE,
                               &Stack);
    if (NT_SUCCESS(Status) && Interface->ArbiterHandler == NULL)
    {
        DPRINT1("%wZ returned an arbiter without handler\n", &DeviceNode->InstancePath);

        if (Interface->InterfaceDereference != NULL)
            Interface->InterfaceDereference(Interface->Context);

        Status = STATUS_UNSUCCESSFUL;
    }

    return Status;
}

/**
 * @brief
 * Adds the root arbiters to the arbiter list of the root device node.
 * Called once, after IopInitializeArbiters.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
IopRegisterRootArbiters(
    _In_ PDEVICE_NODE RootNode)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(IopRootArbiterTable); Index++)
    {
        PARBITER_INTERFACE Interface = &IopRootArbiterInterface[Index];
        PPI_RESOURCE_ARBITER_ENTRY Entry;

        Interface->Size = sizeof(ARBITER_INTERFACE);
        Interface->Version = 0;
        Interface->Context = IopRootArbiterTable[Index].Instance;
        Interface->InterfaceReference = IopRootArbiterReference;
        Interface->InterfaceDereference = IopRootArbiterDereference;
        Interface->ArbiterHandler = ArbiterLibHandler;
        Interface->Flags = 0;

        Entry = ExAllocatePoolZero(NonPagedPool, sizeof(*Entry), TAG_IO_ARBITER);
        if (Entry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        IopInitializeArbiterEntry(Entry,
                                  IopRootArbiterTable[Index].ResourceType,
                                  Interface,
                                  RootNode->Level);
        InsertTailList(&RootNode->DeviceArbiterList, &Entry->DeviceArbiterList);
    }

    ExInitializeResourceLite(&IopResourceAssignmentLock);
    InitializeListHead(&IopPendingBootConfigList);

    if (IopArbiterSeedResourceMap)
        IopArbiterSeedFromResourceMap();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Handles IRP_MN_QUERY_INTERFACE on the root PDO for the root arbiters.
 *
 * @param[in] IoStack
 * The stack location of the IRP. InterfaceSpecificData is the resource type.
 *
 * @param[in] ExistingStatus
 * The current status of the IRP, returned when the query is not handled.
 *
 * @return
 * STATUS_SUCCESS if the interface was returned, ExistingStatus otherwise.
 */
NTSTATUS
NTAPI
IopArbiterQueryRootInterface(
    _In_ PIO_STACK_LOCATION IoStack,
    _In_ NTSTATUS ExistingStatus)
{
    PARBITER_INTERFACE Interface;
    PARBITER_INTERFACE Output;
    UCHAR ResourceType;
    BOOLEAN Supported;

    PAGED_CODE();

    Supported = IsEqualGUID(IoStack->Parameters.QueryInterface.InterfaceType,
                            &GUID_ARBITER_INTERFACE_STANDARD) &&
                IoStack->Parameters.QueryInterface.Version == 0 &&
                IoStack->Parameters.QueryInterface.Size >= sizeof(ARBITER_INTERFACE) &&
                IoStack->Parameters.QueryInterface.Interface != NULL;
    if (!Supported)
        return ExistingStatus;

    ResourceType = (UCHAR)(ULONG_PTR)IoStack->Parameters.QueryInterface.InterfaceSpecificData;

    /* The handler is NULL until IopRegisterRootArbiters has run */
    Interface = IopGetRootArbiterInterface(ResourceType);
    if (Interface == NULL || Interface->ArbiterHandler == NULL)
        return ExistingStatus;

    Output = (PARBITER_INTERFACE)IoStack->Parameters.QueryInterface.Interface;
    *Output = *Interface;
    Output->InterfaceReference(Output->Context);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns the arbiter of a device node for a resource type. The stack of the
 * device is asked once, and the answer is cached on the device node.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_FOUND if the device has no arbiter for the type,
 * or STATUS_INSUFFICIENT_RESOURCES.
 */
static
NTSTATUS
IopGetDeviceArbiter(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PPI_RESOURCE_ARBITER_ENTRY *ArbiterEntry)
{
    USHORT TypeBit = (ResourceType < IOP_MASKED_RESOURCE_TYPES) ? (USHORT)(1 << ResourceType) : 0;
    PPI_RESOURCE_ARBITER_ENTRY Cached;
    PIOP_ARBITER_ENTRY NewEntry;
    ARBITER_INTERFACE Queried;
    NTSTATUS Status;

    *ArbiterEntry = NULL;

    Cached = IopFindArbiterEntry(DeviceNode, ResourceType);
    if (Cached != NULL)
    {
        if (Cached->ArbiterInterface == NULL)
            return STATUS_NOT_FOUND;

        *ArbiterEntry = Cached;
        return STATUS_SUCCESS;
    }

    if (DeviceNode->NoArbiterMask & TypeBit)
        return STATUS_NOT_FOUND;

    Status = IopQueryArbiterInterface(DeviceNode, ResourceType, &Queried);
    DeviceNode->QueryArbiterMask |= TypeBit;

    if (!NT_SUCCESS(Status))
    {
        DeviceNode->NoArbiterMask |= TypeBit;
        if (TypeBit != 0)
            return STATUS_NOT_FOUND;
    }

    NewEntry = ExAllocatePoolZero(NonPagedPool, sizeof(*NewEntry), TAG_IO_ARBITER);
    if (NewEntry == NULL)
    {
        if (NT_SUCCESS(Status) && Queried.InterfaceDereference != NULL)
            Queried.InterfaceDereference(Queried.Context);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NewEntry->Interface = Queried;
    IopInitializeArbiterEntry(&NewEntry->Entry,
                              ResourceType,
                              NT_SUCCESS(Status) ? &NewEntry->Interface : NULL,
                              DeviceNode->Level);
    InsertTailList(&DeviceNode->DeviceArbiterList, &NewEntry->Entry.DeviceArbiterList);

    if (!NT_SUCCESS(Status))
        return STATUS_NOT_FOUND;

    *ArbiterEntry = &NewEntry->Entry;
    return STATUS_SUCCESS;
}

/* Releases the arbiters cached on a device node */
static
VOID
IopFreeDeviceNodeArbiters(
    _In_ PDEVICE_NODE DeviceNode)
{
    while (!IsListEmpty(&DeviceNode->DeviceArbiterList))
    {
        PPI_RESOURCE_ARBITER_ENTRY Entry =
            CONTAINING_RECORD(RemoveHeadList(&DeviceNode->DeviceArbiterList),
                              PI_RESOURCE_ARBITER_ENTRY,
                              DeviceArbiterList);

        if ((Entry->ArbiterInterface != NULL) &&
            (Entry->ArbiterInterface->InterfaceDereference != NULL))
        {
            Entry->ArbiterInterface->InterfaceDereference(Entry->ArbiterInterface->Context);
        }

        ExFreePoolWithTag(Entry, TAG_IO_ARBITER);
    }

    DeviceNode->NoArbiterMask = 0;
    DeviceNode->QueryArbiterMask = 0;
}

/**
 * @brief
 * Releases the arbiters and translators cached on a device node. The device
 * is asked again the next time they are needed.
 */
VOID
NTAPI
IopUncacheResourceHandlers(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    IopLockResourceAssignment();

    IopFreeDeviceNodeArbiters(DeviceNode);
    IopFreeDeviceNodeTranslators(DeviceNode);

    IopUnlockResourceAssignment();
}

/**
 * @brief
 * Asks an arbiter that only arbitrates some of the ranges of its type whether
 * it arbitrates a requirement.
 */
static
BOOLEAN
IopArbiterTakesRequirement(
    _In_ PARBITER_INTERFACE Interface,
    _Inout_ PARBITER_LIST_ENTRY Entry)
{
    ARBITER_PARAMETERS Parameters;
    LIST_ENTRY ArbitrationList;
    NTSTATUS Status;

    InitializeListHead(&ArbitrationList);
    InsertTailList(&ArbitrationList, &Entry->ListEntry);

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Parameters.QueryArbitrate.ArbitrationList = &ArbitrationList;

    Status = Interface->ArbiterHandler(Interface->Context,
                                      ArbiterActionQueryArbitrate,
                                      &Parameters);

    RemoveEntryList(&Entry->ListEntry);
    InitializeListHead(&Entry->ListEntry);

    return NT_SUCCESS(Status);
}

/**
 * @brief
 * Translates the top level of a requirement with the translator of a bus,
 * which adds a level for the parent of the bus.
 *
 * @return
 * The status of IopTranslateRequirement, or STATUS_INSUFFICIENT_RESOURCES.
 */
static
NTSTATUS
IopAddRequirementLevel(
    _Inout_ PIOP_REQUIREMENT Requirement,
    _In_ PTRANSLATOR_INTERFACE Translator)
{
    PIOP_REQUIREMENT_LEVEL Lower = Requirement->Top;
    PIOP_REQUIREMENT_LEVEL Level;
    PIO_RESOURCE_DESCRIPTOR Alternatives;
    ULONG AlternativeCount;
    NTSTATUS Status;

    Status = IopTranslateRequirement(Translator,
                                     Lower->Entry.PhysicalDeviceObject,
                                     Lower->Entry.Alternatives,
                                     Lower->Entry.AlternativeCount,
                                     &Alternatives,
                                     &AlternativeCount);
    if (!NT_SUCCESS(Status))
        return Status;

    Level = ExAllocatePoolWithTag(PagedPool, sizeof(*Level), TAG_IO_ARBITER);
    if (Level == NULL)
    {
        ExFreePool(Alternatives);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Level = *Lower;
    Level->Lower = Lower;
    Level->Translator = Translator;
    InitializeListHead(&Level->Entry.ListEntry);
    Level->Entry.Alternatives = Alternatives;
    Level->Entry.AlternativeCount = AlternativeCount;
    Level->Entry.Assignment = &Level->Assignment;

    Requirement->Top = Level;

    return Status;
}

/**
 * @brief
 * Initializes a requirement of a device before its arbiter and translators
 * are found.
 *
 * @param[in] InterfaceType
 * The legacy bus of the requirement, used when the device tree does not lead
 * to a translator.
 */
VOID
NTAPI
IopInitializeRequirement(
    _Out_ PIOP_REQUIREMENT Requirement,
    _In_opt_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_reads_(AlternativeCount) PIO_RESOURCE_DESCRIPTOR Alternatives,
    _In_ ULONG AlternativeCount)
{
    PARBITER_LIST_ENTRY Entry = &Requirement->Device.Entry;

    RtlZeroMemory(Requirement, sizeof(*Requirement));
    Requirement->Top = &Requirement->Device;
    Requirement->InterfaceType = InterfaceType;
    Requirement->BusNumber = BusNumber;

    InitializeListHead(&Entry->ListEntry);
    Entry->Alternatives = Alternatives;
    Entry->AlternativeCount = AlternativeCount;
    Entry->PhysicalDeviceObject = PhysicalDeviceObject;
    Entry->RequestSource = RequestSource;
    Entry->InterfaceType = InterfaceType;
    Entry->BusNumber = BusNumber;
    Entry->Assignment = &Requirement->Device.Assignment;
    Entry->Result = ArbiterResultUndefined;

    Requirement->Device.Assignment.Type = CmResourceTypeMaximum;
}

/**
 * @brief
 * Finds the arbiter of a requirement, and the translators between the device
 * and the arbiter. The search starts at the device node, whose own arbiters
 * are skipped, and goes up the device tree. Each translator found before the
 * arbiter translates the requirement for its parent, until one of them
 * translates up to the root.
 *
 * @param[in] ListInterfaceType
 * The interface type of the whole requirements list.
 *
 * @remarks
 * When the root is reached before any translator was found, the search goes
 * on from the bus that provides the legacy bus of the requirement. A resource
 * reported by the HAL is found from the root, and an internal one does not go
 * to the legacy bus.
 *
 * @return
 * STATUS_SUCCESS, STATUS_RESOURCE_TYPE_NOT_FOUND if no arbiter was found, or
 * the failure status of a translator.
 */
NTSTATUS
NTAPI
IopFindRequirementHandlers(
    _Inout_ PIOP_REQUIREMENT Requirement,
    _In_ INTERFACE_TYPE ListInterfaceType)
{
    PDEVICE_OBJECT PhysicalDeviceObject = Requirement->Device.Entry.PhysicalDeviceObject;
    BOOLEAN HalReported = (Requirement->Device.Entry.RequestSource == ArbiterRequestHalReported);
    BOOLEAN LegacyBusVisited = HalReported && Requirement->InterfaceType == Internal;
    BOOLEAN TranslatorFound = FALSE;
    BOOLEAN Translating = TRUE;
    PDEVICE_NODE Node;
    NTSTATUS Status;
    UCHAR Type;

    PAGED_CODE();

    ASSERT(Requirement->Top == &Requirement->Device && Requirement->Arbiter == NULL);

    Type = IopArbitratedType(Requirement->Top->Entry.Alternatives[0].Type);

    if (PhysicalDeviceObject != NULL && !HalReported)
        Node = IopGetDeviceNode(PhysicalDeviceObject);
    else
        Node = IopRootDeviceNode;

    while (Node != NULL)
    {
        PTRANSLATOR_INTERFACE Translator;

        if (Node == IopRootDeviceNode && !TranslatorFound && !LegacyBusVisited)
        {
            LegacyBusVisited = TRUE;
            Node = IopFindResourceBus(Requirement->InterfaceType,
                                      Requirement->BusNumber,
                                      ListInterfaceType);
            continue;
        }

        if (Requirement->Arbiter == NULL && Node->PhysicalDeviceObject != PhysicalDeviceObject)
        {
            Status = IopGetDeviceArbiter(Node, Type, &Requirement->Arbiter);
            if (Status == STATUS_INSUFFICIENT_RESOURCES)
                return Status;

            if (Requirement->Arbiter != NULL &&
                (Requirement->Arbiter->ArbiterInterface->Flags & ARBITER_PARTIAL) &&
                !IopArbiterTakesRequirement(Requirement->Arbiter->ArbiterInterface,
                                            &Requirement->Top->Entry))
            {
                Requirement->Arbiter = NULL;
            }
        }

        if (Translating)
        {
            Status = IopGetDeviceTranslator(Node, Type, &Translator);
            if (Status == STATUS_INSUFFICIENT_RESOURCES)
                return Status;

            if (NT_SUCCESS(Status))
            {
                TranslatorFound = TRUE;

                if (Requirement->Arbiter == NULL)
                {
                    Status = IopAddRequirementLevel(Requirement, Translator);
                    if (!NT_SUCCESS(Status))
                    {
                        DPRINT1("Requirement translation by %wZ failed (Status 0x%08lx)\n",
                                &Node->InstancePath, Status);
                        return Status;
                    }

                    Type = IopArbitratedType(Requirement->Top->Entry.Alternatives[0].Type);

                    if (Status == STATUS_TRANSLATION_COMPLETE)
                        Translating = FALSE;
                }
            }
        }

        Node = IopGetResourceParent(Node);
    }

    if (Requirement->Arbiter == NULL)
    {
        DPRINT1("No arbiter for resource type %u\n", Type);
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }

    return STATUS_SUCCESS;
}

/* Frees the translated levels of a requirement */
VOID
NTAPI
IopFreeRequirementLevels(
    _Inout_ PIOP_REQUIREMENT Requirement)
{
    while (Requirement->Top != &Requirement->Device)
    {
        PIOP_REQUIREMENT_LEVEL Level = Requirement->Top;

        Requirement->Top = Level->Lower;
        ExFreePool(Level->Entry.Alternatives);
        ExFreePoolWithTag(Level, TAG_IO_ARBITER);
    }

    Requirement->Arbiter = NULL;
}

/**
 * @brief
 * Translates the assignment the arbiter made in the top level of a
 * requirement back down to the device, with the translator of each level.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER if a level has no assignment, or
 * the failure status of a translator.
 */
NTSTATUS
NTAPI
IopTranslateAssignmentToDevice(
    _Inout_ PIOP_REQUIREMENT Requirement)
{
    PIOP_REQUIREMENT_LEVEL Level;

    PAGED_CODE();

    for (Level = Requirement->Top; Level != NULL; Level = Level->Lower)
    {
        PIOP_REQUIREMENT_LEVEL Lower = Level->Lower;
        NTSTATUS Status;

        if (Level->Entry.AlternativeCount == 0 || Level->Assignment.Type == CmResourceTypeMaximum)
        {
            DPRINT1("Requirement level without assignment\n");
            return STATUS_INVALID_PARAMETER;
        }

        if (Lower == NULL)
            break;

        Status = Level->Translator->TranslateResources(Level->Translator->Context,
                                                       &Level->Assignment,
                                                       TranslateParentToChild,
                                                       Lower->Entry.AlternativeCount,
                                                       Lower->Entry.Alternatives,
                                                       Lower->Entry.PhysicalDeviceObject,
                                                       &Lower->Assignment);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

/* BOOT CONFIGURATION RESERVATION *******************************************/

/* Resource types that have an arbiter, in the order they are arbitrated */
static const UCHAR IopArbiterResourceTypes[] =
{
    CmResourceTypePort,
    CmResourceTypeInterrupt,
    CmResourceTypeMemory,
    CmResourceTypeDma,
    CmResourceTypeBusNumber,
};

/* Checks if a descriptor type is handled by the arbiter of ArbType */
static
BOOLEAN
IopArbiterTypeMatches(
    _In_ UCHAR EntryType,
    _In_ UCHAR ArbType)
{
    if (EntryType == ArbType)
        return TRUE;

    /* Large memory ranges are handled by the memory arbiter */
    return (EntryType == CmResourceTypeMemoryLarge && ArbType == CmResourceTypeMemory);
}

/**
 * @brief
 * Calls an arbiter action. ArbitrationList is NULL for the commit and
 * rollback actions, which take no parameters.
 */
static
NTSTATUS
IopArbiterInvoke(
    _In_ PARBITER_INTERFACE Interface,
    _In_ ARBITER_ACTION Action,
    _In_opt_ PLIST_ENTRY ArbitrationList)
{
    ARBITER_PARAMETERS Parameters;

    /* The list is the first field of every parameter block that has one */
    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Parameters.TestAllocation.ArbitrationList = ArbitrationList;

    return Interface->ArbiterHandler(Interface->Context, Action, &Parameters);
}

static
BOOLEAN
IopIsForwardingWindow(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm)
{
    switch (Cm->Type)
    {
        case CmResourceTypePort:
            return !!(Cm->Flags & CM_RESOURCE_PORT_WINDOW_DECODE);

        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            return !!(Cm->Flags & CM_RESOURCE_MEMORY_WINDOW_DECODE);

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Converts an assigned descriptor to a requirement that only fits at the
 * same place.
 *
 * @param[in] Cm
 * The assigned descriptor.
 *
 * @param[in] AllowForwardingWindow
 * FALSE to fail for a bus forwarding window.
 *
 * @param[out] Io
 * Receives the requirement.
 *
 * @return
 * FALSE for a forwarding window that is not allowed, or for a resource type
 * without an arbiter.
 */
static
BOOLEAN
IopArbiterCmToFixedRequirement(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm,
    _In_ BOOLEAN AllowForwardingWindow,
    _Out_ PIO_RESOURCE_DESCRIPTOR Io)
{
    ULONGLONG Start, Length;

    RtlZeroMemory(Io, sizeof(*Io));
    Io->Type = Cm->Type;
    Io->Flags = Cm->Flags;
    Io->ShareDisposition = Cm->ShareDisposition;

    if (!AllowForwardingWindow && IopIsForwardingWindow(Cm))
        return FALSE;

    switch (Cm->Type)
    {
        case CmResourceTypeInterrupt:
            Io->u.Interrupt.MinimumVector = Cm->u.Interrupt.Vector;
            Io->u.Interrupt.MaximumVector = Cm->u.Interrupt.Vector;
            return TRUE;

        case CmResourceTypeDma:
            Io->u.Dma.MinimumChannel = Cm->u.Dma.Channel;
            Io->u.Dma.MaximumChannel = Cm->u.Dma.Channel;
            return TRUE;

        case CmResourceTypeBusNumber:
            Io->u.BusNumber.MinBusNumber = Cm->u.BusNumber.Start;
            Io->u.BusNumber.MaxBusNumber = Cm->u.BusNumber.Start + Cm->u.BusNumber.Length - 1;
            Io->u.BusNumber.Length = Cm->u.BusNumber.Length;
            return TRUE;

        case CmResourceTypePort:
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            /* The encoder keeps the full length of large memory ranges */
            Length = RtlCmDecodeMemIoResource(Cm, &Start);
            if (Length == 0)
                return FALSE;

            return NT_SUCCESS(RtlIoEncodeMemIoResource(Io,
                                                       Cm->Type,
                                                       Length,
                                                       1,
                                                       Start,
                                                       Start + Length - 1));

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Reserves the descriptors of one resource type from an assigned resource
 * list, with the BootAllocation action of the arbiter.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES. Failures of the arbiter
 * are ignored.
 */
static
NTSTATUS
IopArbiterReserveType(
    _In_ PARBITER_INTERFACE Interface,
    _In_ UCHAR ResourceType,
    _In_ PCM_PARTIAL_RESOURCE_LIST PartialList,
    _In_opt_ PVOID Owner,
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ BOOLEAN AllowForwardingWindow)
{
    LIST_ENTRY ArbitrationList;
    PARBITER_LIST_ENTRY Entries;
    PIO_RESOURCE_DESCRIPTOR Descriptors;
    ULONG Count = 0;
    ULONG Index;

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        if (IopArbiterTypeMatches(PartialList->PartialDescriptors[Index].Type, ResourceType))
            Count++;
    }

    if (Count == 0)
        return STATUS_SUCCESS;

    Entries = ExAllocatePoolZero(PagedPool, Count * sizeof(ARBITER_LIST_ENTRY), TAG_IO_ARBITER);
    Descriptors = ExAllocatePoolWithTag(PagedPool,
                                        Count * sizeof(IO_RESOURCE_DESCRIPTOR),
                                        TAG_IO_ARBITER);
    if (Entries == NULL || Descriptors == NULL)
    {
        if (Entries != NULL)
            ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
        if (Descriptors != NULL)
            ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InitializeListHead(&ArbitrationList);

    Count = 0;
    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_LIST_ENTRY Entry = &Entries[Count];

        if (!IopArbiterTypeMatches(Cm->Type, ResourceType) ||
            !IopArbiterCmToFixedRequirement(Cm, AllowForwardingWindow, &Descriptors[Count]))
        {
            continue;
        }

        Entry->Alternatives = &Descriptors[Count];
        Entry->AlternativeCount = 1;
        Entry->PhysicalDeviceObject = Owner;
        Entry->InterfaceType = InterfaceType;
        Entry->BusNumber = BusNumber;
        Entry->RequestSource = ArbiterRequestPnpEnumerated;
        Entry->Flags = ARBITER_FLAG_BOOT_CONFIG;
        Entry->Result = ArbiterResultUndefined;
        InsertTailList(&ArbitrationList, &Entry->ListEntry);
        Count++;
    }

    if (Count != 0)
        IopArbiterInvoke(Interface, ArbiterActionBootAllocation, &ArbitrationList);

    ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
    ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);

    return STATUS_SUCCESS;
}

/* Checks if a resource type is arbitrated. Other descriptors are copied as they are. */
static
BOOLEAN
IopIsArbitratedType(
    _In_ UCHAR Type)
{
    return !(Type & CmResourceTypeNonArbitrated) &&
           Type != CmResourceTypeNull &&
           Type != CmResourceTypeDeviceSpecific;
}

/* A failed translation is retried later only when the translator asks for it */
static
NTSTATUS
IopTranslationFailureStatus(
    _In_ NTSTATUS Status)
{
    return (Status == STATUS_RETRY) ? STATUS_RETRY : STATUS_INSUFFICIENT_RESOURCES;
}

/**
 * @brief
 * Adds the top entry of a requirement to the arbitration list of its arbiter.
 * The first entry of an arbiter also adds it to the active arbiters, where
 * the arbiters closer to the root come first.
 */
static
VOID
IopQueueRequirement(
    _Inout_ PLIST_ENTRY ActiveArbiters,
    _In_ PIOP_REQUIREMENT Requirement)
{
    PPI_RESOURCE_ARBITER_ENTRY Arbiter = Requirement->Arbiter;
    PLIST_ENTRY Next;

    InsertTailList(&Arbiter->ResourceList, &Requirement->Top->Entry.ListEntry);

    if (!IsListEmpty(&Arbiter->ActiveArbiterList))
        return;

    for (Next = ActiveArbiters->Flink; Next != ActiveArbiters; Next = Next->Flink)
    {
        PPI_RESOURCE_ARBITER_ENTRY Active =
            CONTAINING_RECORD(Next, PI_RESOURCE_ARBITER_ENTRY, ActiveArbiterList);

        if (Active->Level >= Arbiter->Level)
            break;
    }

    /* Goes before the arbiters of the same depth that are already there */
    InsertTailList(Next, &Arbiter->ActiveArbiterList);
}

/* Empties the arbitration lists of the active arbiters */
static
VOID
IopDequeueRequirements(
    _Inout_ PLIST_ENTRY ActiveArbiters)
{
    while (!IsListEmpty(ActiveArbiters))
    {
        PPI_RESOURCE_ARBITER_ENTRY Arbiter =
            CONTAINING_RECORD(RemoveHeadList(ActiveArbiters),
                              PI_RESOURCE_ARBITER_ENTRY,
                              ActiveArbiterList);

        InitializeListHead(&Arbiter->ActiveArbiterList);

        while (!IsListEmpty(&Arbiter->ResourceList))
        {
            PLIST_ENTRY Entry = RemoveHeadList(&Arbiter->ResourceList);
            InitializeListHead(Entry);
        }
    }
}

/**
 * @brief
 * Builds the translated list of an assigned resource list. The arbitrated
 * resources are translated for the processor, and the other descriptors are
 * copied.
 *
 * @return
 * STATUS_SUCCESS, STATUS_RETRY if a translator asked to be called again later,
 * or STATUS_INSUFFICIENT_RESOURCES.
 */
static
NTSTATUS
IopTranslateResourceList(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_ PCM_RESOURCE_LIST ResourceList,
    _Out_ PCM_RESOURCE_LIST *TranslatedList)
{
    ULONG Size = PnpDetermineResourceListSize(ResourceList);
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    PCM_RESOURCE_LIST Translated;
    ULONG ListIndex;

    PAGED_CODE();

    *TranslatedList = NULL;

    Translated = ExAllocatePoolWithTag(PagedPool, Size, TAG_IO_ARBITER);
    if (Translated == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Translated, ResourceList, Size);

    Full = &ResourceList->List[0];
    for (ListIndex = 0; ListIndex < ResourceList->Count; ListIndex++)
    {
        INTERFACE_TYPE InterfaceType = Full->InterfaceType;
        ULONG Index;

        if (InterfaceType == InterfaceTypeUndefined)
            InterfaceType = Isa;

        for (Index = 0; Index < Full->PartialResourceList.Count; Index++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw =
                &Full->PartialResourceList.PartialDescriptors[Index];
            NTSTATUS Status;

            if (!IopIsArbitratedType(Raw->Type))
                continue;

            /* Both lists have the same layout */
            Status = IopTranslateResourceToRoot(DeviceNode,
                                                InterfaceType,
                                                Full->BusNumber,
                                                RequestSource,
                                                Raw,
                                                (PCM_PARTIAL_RESOURCE_DESCRIPTOR)
                                                    ((PUCHAR)Translated +
                                                     ((PUCHAR)Raw - (PUCHAR)ResourceList)));
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(Translated, TAG_IO_ARBITER);
                return IopTranslationFailureStatus(Status);
            }
        }

        Full = CmiGetNextResourceDescriptor(Full);
    }

    *TranslatedList = Translated;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reserves the boot configuration of a device in its arbiters, so other
 * devices are not given these resources. Each descriptor reaches its arbiter
 * through the translators like a requirement, and the translated boot
 * configuration is kept on the device node.
 *
 * @return
 * STATUS_SUCCESS, or the failure status. On failure the boot configuration of
 * the device is freed.
 */
NTSTATUS
NTAPI
IopArbiterReserveBootConfig(
    _In_ PDEVICE_NODE DeviceNode)
{
    PCM_RESOURCE_LIST BootResources = DeviceNode->BootResources;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    PIO_RESOURCE_DESCRIPTOR Descriptors = NULL;
    PIOP_REQUIREMENT Requirements = NULL;
    PCM_RESOURCE_LIST Translated;
    INTERFACE_TYPE InterfaceType;
    LIST_ENTRY ActiveArbiters;
    PLIST_ENTRY ListEntry;
    ULONG Count = 0;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    if (BootResources == NULL || BootResources->Count == 0)
        return STATUS_SUCCESS;

    PartialList = &BootResources->List[0].PartialResourceList;

    InterfaceType = BootResources->List[0].InterfaceType;
    if (InterfaceType == InterfaceTypeUndefined)
        InterfaceType = Isa;

    if (PartialList->Count != 0)
    {
        Requirements = ExAllocatePoolZero(PagedPool,
                                          PartialList->Count * sizeof(*Requirements),
                                          TAG_IO_ARBITER);
        Descriptors = ExAllocatePoolZero(PagedPool,
                                         PartialList->Count * sizeof(*Descriptors),
                                         TAG_IO_ARBITER);
        if (Requirements == NULL || Descriptors == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
    }

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PIOP_REQUIREMENT Requirement = &Requirements[Count];

        if (!IopIsArbitratedType(Cm->Type) ||
            !IopArbiterCmToFixedRequirement(Cm, TRUE, &Descriptors[Count]))
        {
            continue;
        }

        IopInitializeRequirement(Requirement,
                                 DeviceNode->PhysicalDeviceObject,
                                 ArbiterRequestPnpEnumerated,
                                 InterfaceType,
                                 BootResources->List[0].BusNumber,
                                 &Descriptors[Count],
                                 1);
        Requirement->Device.Entry.Flags = ARBITER_FLAG_BOOT_CONFIG;

        /* Counted first, so the levels it gets are freed on failure */
        Count++;

        Status = IopFindRequirementHandlers(Requirement, InterfaceType);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        /*
         * A forwarding window is only reserved in the arbiter of a parent bus,
         * like a PCI bridge window in the bus above. For the root arbiters the
         * window is the range they allocate from.
         */
        if (IopIsForwardingWindow(Cm) &&
            Requirement->Arbiter->ArbiterInterface == IopGetRootArbiterInterface(Cm->Type))
        {
            IopFreeRequirementLevels(Requirement);
            Count--;
        }
    }

    InitializeListHead(&ActiveArbiters);

    for (Index = 0; Index < Count; Index++)
        IopQueueRequirement(&ActiveArbiters, &Requirements[Index]);

    for (ListEntry = ActiveArbiters.Flink;
         ListEntry != &ActiveArbiters;
         ListEntry = ListEntry->Flink)
    {
        PPI_RESOURCE_ARBITER_ENTRY Arbiter =
            CONTAINING_RECORD(ListEntry, PI_RESOURCE_ARBITER_ENTRY, ActiveArbiterList);
        NTSTATUS ArbiterStatus;

        ArbiterStatus = IopArbiterInvoke(Arbiter->ArbiterInterface,
                                         ArbiterActionBootAllocation,
                                         &Arbiter->ResourceList);
        if (!NT_SUCCESS(ArbiterStatus))
        {
            DPRINT1("Boot allocation of type %u failed for %wZ (Status 0x%08lx)\n",
                    Arbiter->ResourceType, &DeviceNode->InstancePath, ArbiterStatus);
            Status = ArbiterStatus;
        }
    }

    IopDequeueRequirements(&ActiveArbiters);

    if (!NT_SUCCESS(Status))
        goto Cleanup;

    IopDeviceNodeSetFlag(DeviceNode, DNF_BOOT_CONFIG_RESERVED);

    for (Index = 0; Index < Count; Index++)
    {
        /* A range the arbiter did not take has no assignment */
        if (Requirements[Index].Top->Assignment.Type == CmResourceTypeMaximum)
            continue;

        Status = IopTranslateAssignmentToDevice(&Requirements[Index]);
        if (!NT_SUCCESS(Status))
        {
            Status = IopTranslationFailureStatus(Status);
            goto Cleanup;
        }
    }

    Status = IopTranslateResourceList(DeviceNode,
                                      ArbiterRequestPnpEnumerated,
                                      BootResources,
                                      &Translated);
    if (NT_SUCCESS(Status))
    {
        if (DeviceNode->BootResourcesTranslated != NULL)
            ExFreePool(DeviceNode->BootResourcesTranslated);

        DeviceNode->BootResourcesTranslated = Translated;
    }

Cleanup:
    for (Index = 0; Index < Count; Index++)
        IopFreeRequirementLevels(&Requirements[Index]);

    if (Requirements != NULL)
        ExFreePoolWithTag(Requirements, TAG_IO_ARBITER);
    if (Descriptors != NULL)
        ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to reserve the boot config of %wZ (Status 0x%08lx)\n",
                &DeviceNode->InstancePath, Status);

        ExFreePool(DeviceNode->BootResources);
        DeviceNode->BootResources = NULL;

        if (DeviceNode->BootResourcesTranslated != NULL)
        {
            ExFreePool(DeviceNode->BootResourcesTranslated);
            DeviceNode->BootResourcesTranslated = NULL;
        }
    }

    return Status;
}

/**
 * @brief
 * Reserves the boot configuration of a newly enumerated device. For a root
 * enumerated device the reservation waits until the boot drivers are started.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INSUFFICIENT_RESOURCES if the device could not be
 * added to the pending list, or the failure status of the reservation.
 */
NTSTATUS
NTAPI
IopReserveBootConfig(
    _In_ PDEVICE_NODE DeviceNode)
{
    PIOP_PENDING_BOOT_CONFIG Pending;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    if (DeviceNode->BootResources == NULL)
        return STATUS_SUCCESS;

    IopLockResourceAssignment();

    if (IopHoldRootBootConfigs && (DeviceNode->Flags & DNF_MADEUP))
    {
        Pending = ExAllocatePoolWithTag(PagedPool, sizeof(*Pending), TAG_IO_ARBITER);
        if (Pending != NULL)
        {
            ObReferenceObject(DeviceNode->PhysicalDeviceObject);
            Pending->DeviceObject = DeviceNode->PhysicalDeviceObject;
            InsertTailList(&IopPendingBootConfigList, &Pending->ListEntry);
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    else if (!(DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED))
    {
        Status = IopArbiterReserveBootConfig(DeviceNode);
    }

    IopUnlockResourceAssignment();

    return Status;
}

/**
 * @brief
 * Reserves the boot configurations held back by IopReserveBootConfig.
 * Called once the boot drivers are started, before devices are assigned
 * resources, so the resources of the boot device are not given away.
 */
VOID
NTAPI
IopReserveDeferredBootConfigs(VOID)
{
    PAGED_CODE();

    IopLockResourceAssignment();

    IopHoldRootBootConfigs = FALSE;

    while (!IsListEmpty(&IopPendingBootConfigList))
    {
        PIOP_PENDING_BOOT_CONFIG Pending =
            CONTAINING_RECORD(RemoveHeadList(&IopPendingBootConfigList),
                              IOP_PENDING_BOOT_CONFIG,
                              ListEntry);
        PDEVICE_NODE DeviceNode = IopGetDeviceNode(Pending->DeviceObject);

        if (DeviceNode != NULL &&
            DeviceNode->BootResources != NULL &&
            !(DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED))
        {
            DPRINT("Reserving the boot config of %wZ\n", &DeviceNode->InstancePath);
            IopArbiterReserveBootConfig(DeviceNode);
        }

        ObDereferenceObject(Pending->DeviceObject);
        ExFreePoolWithTag(Pending, TAG_IO_ARBITER);
    }

    IopUnlockResourceAssignment();
}

/* FIRMWARE RESOURCE MAP SEEDING ********************************************/

/**
 * @brief
 * Reserves the resources of one RESOURCEMAP value in the root arbiters.
 * They are owned by the root PDO, so root enumerated devices can still share
 * them.
 */
static
CODE_SEG("INIT")
VOID
IopArbiterSeedResourceList(
    _In_ PCM_RESOURCE_LIST ResourceList)
{
    PVOID Owner;
    ULONG TypeIndex;

    if (ResourceList->Count == 0)
        return;

    Owner = (IopRootDeviceNode != NULL) ? IopRootDeviceNode->PhysicalDeviceObject : NULL;

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(ResourceType);

        if (Interface == NULL)
            continue;

        /* Forwarding windows of the root buses are not reserved */
        IopArbiterReserveType(Interface,
                              ResourceType,
                              &ResourceList->List[0].PartialResourceList,
                              Owner,
                              ResourceList->List[0].InterfaceType,
                              ResourceList->List[0].BusNumber,
                              FALSE);
    }
}

/**
 * @brief
 * Reserves the raw resource lists of a RESOURCEMAP key and its subkeys.
 * Values that do not fit the buffers are skipped.
 */
static
CODE_SEG("INIT")
VOID
IopArbiterSeedFromKey(
    _In_ HANDLE Key,
    _In_ ULONG Depth)
{
    static const UNICODE_STRING Translated = RTL_CONSTANT_STRING(L".Translated");
    const ULONG ValueInfoLength = 2 * PAGE_SIZE;
    const ULONG KeyInfoLength = 512;
    PKEY_VALUE_FULL_INFORMATION ValueInfo;
    PKEY_BASIC_INFORMATION KeyInfo;
    ULONG Length;
    ULONG Index;
    NTSTATUS Status;

    /* The resource map is only a few levels deep */
    if (Depth > 8)
        return;

    ValueInfo = ExAllocatePoolWithTag(PagedPool, ValueInfoLength, TAG_IO_ARBITER);
    KeyInfo = ExAllocatePoolWithTag(PagedPool, KeyInfoLength, TAG_IO_ARBITER);
    if (ValueInfo == NULL || KeyInfo == NULL)
    {
        if (ValueInfo != NULL)
            ExFreePoolWithTag(ValueInfo, TAG_IO_ARBITER);
        if (KeyInfo != NULL)
            ExFreePoolWithTag(KeyInfo, TAG_IO_ARBITER);

        return;
    }

    for (Index = 0; ; Index++)
    {
        UNICODE_STRING Name;

        Status = ZwEnumerateValueKey(Key,
                                     Index,
                                     KeyValueFullInformation,
                                     ValueInfo,
                                     ValueInfoLength,
                                     &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            break;

        if (!NT_SUCCESS(Status) ||
            ValueInfo->Type != REG_RESOURCE_LIST ||
            ValueInfo->DataLength < sizeof(CM_RESOURCE_LIST) ||
            ValueInfo->NameLength > MAXUSHORT)
        {
            continue;
        }

        Name.Buffer = ValueInfo->Name;
        Name.Length = (USHORT)ValueInfo->NameLength;
        Name.MaximumLength = Name.Length;

        /* The raw value has the same ranges */
        if (RtlEqualUnicodeString(&Translated, &Name, TRUE))
            continue;

        IopArbiterSeedResourceList(
            (PCM_RESOURCE_LIST)((PUCHAR)ValueInfo + ValueInfo->DataOffset));
    }

    for (Index = 0; ; Index++)
    {
        UNICODE_STRING SubName;
        OBJECT_ATTRIBUTES ObjectAttributes;
        HANDLE SubKey;

        Status = ZwEnumerateKey(Key, Index, KeyBasicInformation, KeyInfo, KeyInfoLength, &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            break;

        if (!NT_SUCCESS(Status) || KeyInfo->NameLength > MAXUSHORT)
            continue;

        SubName.Buffer = KeyInfo->Name;
        SubName.Length = (USHORT)KeyInfo->NameLength;
        SubName.MaximumLength = SubName.Length;

        InitializeObjectAttributes(&ObjectAttributes,
                                   &SubName,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   Key,
                                   NULL);
        if (NT_SUCCESS(ZwOpenKey(&SubKey, KEY_READ, &ObjectAttributes)))
        {
            IopArbiterSeedFromKey(SubKey, Depth + 1);
            ZwClose(SubKey);
        }
    }

    ExFreePoolWithTag(ValueInfo, TAG_IO_ARBITER);
    ExFreePoolWithTag(KeyInfo, TAG_IO_ARBITER);
}

/**
 * @brief
 * Reserves the resources listed under HARDWARE\RESOURCEMAP in the root
 * arbiters. These belong to the firmware, the HAL and the boot loader, and
 * not to a PnP device. Runs before any device is enumerated.
 */
static
CODE_SEG("INIT")
VOID
IopArbiterSeedFromResourceMap(VOID)
{
    UNICODE_STRING KeyName =
        RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Key;

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &ObjectAttributes)))
    {
        DPRINT1("Arbiter seeding: RESOURCEMAP is not present\n");
        return;
    }

    IopArbiterSeedFromKey(Key, 0);
    ZwClose(Key);
}

/* RESOURCE ARBITRATION *****************************************************/

/* Requirement descriptor that sets the legacy bus of the descriptors after it */
#define IOP_RESOURCE_TYPE_LEGACY_BUS 0xF0

/*
 * The requirements of one alternative configuration of a device. Arbitrated
 * requirements have an arbiter, and the others already hold the descriptor
 * that is copied to the assigned resources.
 */
typedef struct _IOP_CONFIGURATION
{
    PIOP_REQUIREMENT Requirements;
    ULONG Count;
    NTSTATUS Status;
} IOP_CONFIGURATION, *PIOP_CONFIGURATION;

/* Returns the alternative list that follows List */
static
PIO_RESOURCE_LIST
IopArbiterNextList(
    _In_ PIO_RESOURCE_LIST List)
{
    return (PIO_RESOURCE_LIST)&List->Descriptors[List->Count];
}

/* Resources of an undefined interface are on the ISA bus */
static
INTERFACE_TYPE
IopResourceInterface(
    _In_ INTERFACE_TYPE InterfaceType)
{
    return (InterfaceType == InterfaceTypeUndefined) ? Isa : InterfaceType;
}

/* Checks if two fixed requirements ask for the same resource */
static
BOOLEAN
IopArbiterSamePlacement(
    _In_ PIO_RESOURCE_DESCRIPTOR A,
    _In_ PIO_RESOURCE_DESCRIPTOR B)
{
    if (A->Type != B->Type)
        return FALSE;

    switch (A->Type)
    {
        case CmResourceTypePort:
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            return A->u.Generic.Length == B->u.Generic.Length &&
                   A->u.Generic.MinimumAddress.QuadPart == B->u.Generic.MinimumAddress.QuadPart &&
                   A->u.Generic.MaximumAddress.QuadPart == B->u.Generic.MaximumAddress.QuadPart;

        case CmResourceTypeInterrupt:
            return A->u.Interrupt.MinimumVector == B->u.Interrupt.MinimumVector &&
                   A->u.Interrupt.MaximumVector == B->u.Interrupt.MaximumVector;

        case CmResourceTypeDma:
            return A->u.Dma.MinimumChannel == B->u.Dma.MinimumChannel &&
                   A->u.Dma.MaximumChannel == B->u.Dma.MaximumChannel;

        case CmResourceTypeBusNumber:
            return A->u.BusNumber.Length == B->u.BusNumber.Length &&
                   A->u.BusNumber.MinBusNumber == B->u.BusNumber.MinBusNumber &&
                   A->u.BusNumber.MaxBusNumber == B->u.BusNumber.MaxBusNumber;

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Checks if one of the alternatives of a requirement is exactly a resource
 * from the boot configuration of the device.
 */
static
BOOLEAN
IopArbiterIsBootRequirement(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PARBITER_LIST_ENTRY Entry)
{
    PCM_RESOURCE_LIST BootResources = DeviceNode->BootResources;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    IO_RESOURCE_DESCRIPTOR Fixed;
    ULONG Index;
    ULONG Alt;

    if (!(DeviceNode->Flags & DNF_HAS_BOOT_CONFIG) ||
        BootResources == NULL ||
        BootResources->Count == 0)
    {
        return FALSE;
    }

    PartialList = &BootResources->List[0].PartialResourceList;

    for (Alt = 0; Alt < Entry->AlternativeCount; Alt++)
    {
        for (Index = 0; Index < PartialList->Count; Index++)
        {
            if (IopArbiterCmToFixedRequirement(&PartialList->PartialDescriptors[Index],
                                               TRUE,
                                               &Fixed) &&
                IopArbiterSamePlacement(&Entry->Alternatives[Alt], &Fixed))
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * @brief
 * Prints the committed ranges of a root arbiter that overlap a range.
 *
 * @remarks
 * Only valid for the root arbiters. The context of an arbiter from a bus
 * driver is private to that driver.
 */
static
CODE_SEG("PAGE")
VOID
IopArbiterReportOccupants(
    _In_ PARBITER_INTERFACE Interface,
    _In_ ULONGLONG Start,
    _In_ ULONGLONG End)
{
    PARBITER_INSTANCE Arbiter;
    RTL_RANGE_LIST_ITERATOR Iterator;
    PRTL_RANGE Range;
    BOOLEAN Found = FALSE;

    PAGED_CODE();

    if (Interface == NULL || Interface->Context == NULL)
        return;

    Arbiter = (PARBITER_INSTANCE)Interface->Context;
    if (Arbiter->Allocation == NULL)
        return;

    for (RtlGetFirstRange(Arbiter->Allocation, &Iterator, &Range);
         Range != NULL;
         RtlGetNextRange(&Iterator, &Range, TRUE))
    {
        if (Range->Start > End || Range->End < Start)
            continue;

        Found = TRUE;
        DPRINT1("      used by %I64x..%I64x owner %p attr 0x%x flags 0x%x%s\n",
                Range->Start, Range->End, Range->Owner,
                Range->Attributes, Range->Flags,
                (Range->Attributes & ARBITER_RANGE_BOOT_ALLOCATED) ? " BOOT_ALLOCATED" : "");
    }

    if (!Found)
        DPRINT1("      no committed range overlaps\n");
}

/* Prints the requirements an arbiter could not satisfy */
static
CODE_SEG("PAGE")
VOID
IopArbiterReportFailure(
    _In_ PPI_RESOURCE_ARBITER_ENTRY ArbiterEntry,
    _In_ BOOLEAN UseBootRanges,
    _In_ PDEVICE_NODE DeviceNode)
{
    PARBITER_INTERFACE Interface = ArbiterEntry->ArbiterInterface;
    PLIST_ENTRY Link;
    BOOLEAN RootArbiter;

    PAGED_CODE();

    RootArbiter = (Interface == IopGetRootArbiterInterface(ArbiterEntry->ResourceType));

    DPRINT1("Arbitration failed: type %u, boot ranges %s, device %wZ%s\n",
            ArbiterEntry->ResourceType, UseBootRanges ? "allowed" : "excluded",
            &DeviceNode->InstancePath, RootArbiter ? "" : " (bus arbiter)");

    for (Link = ArbiterEntry->ResourceList.Flink;
         Link != &ArbiterEntry->ResourceList;
         Link = Link->Flink)
    {
        PARBITER_LIST_ENTRY Entry = CONTAINING_RECORD(Link, ARBITER_LIST_ENTRY, ListEntry);
        ULONG Alt;

        DPRINT1("  entry: %lu alternative(s), flags 0x%lx%s, source %u, result %u\n",
                Entry->AlternativeCount, Entry->Flags,
                (Entry->Flags & ARBITER_FLAG_BOOT_CONFIG) ? " (BOOT_CONFIG)" : "",
                Entry->RequestSource, Entry->Result);

        for (Alt = 0; Alt < Entry->AlternativeCount; Alt++)
        {
            PIO_RESOURCE_DESCRIPTOR Desc = &Entry->Alternatives[Alt];

            switch (Desc->Type)
            {
                case CmResourceTypePort:
                case CmResourceTypeMemory:
                case CmResourceTypeMemoryLarge:
                {
                    ULONGLONG Min = Desc->u.Generic.MinimumAddress.QuadPart;
                    ULONGLONG Max = Desc->u.Generic.MaximumAddress.QuadPart;

                    DPRINT1("    [%lu] type %u opt 0x%x flags 0x%x len 0x%lx align 0x%lx "
                            "%I64x..%I64x%s\n",
                            Alt, Desc->Type, Desc->Option, Desc->Flags,
                            Desc->u.Generic.Length, Desc->u.Generic.Alignment,
                            Min, Max,
                            (Max - Min + 1 == Desc->u.Generic.Length) ? " FIXED" : "");

                    if (RootArbiter)
                        IopArbiterReportOccupants(Interface, Min, Max);
                    break;
                }

                case CmResourceTypeInterrupt:
                    DPRINT1("    [%lu] irq opt 0x%x flags 0x%x %lx..%lx\n",
                            Alt, Desc->Option, Desc->Flags,
                            Desc->u.Interrupt.MinimumVector,
                            Desc->u.Interrupt.MaximumVector);
                    break;

                case CmResourceTypeBusNumber:
                    DPRINT1("    [%lu] bus opt 0x%x len 0x%lx %lx..%lx\n",
                            Alt, Desc->Option, Desc->u.BusNumber.Length,
                            Desc->u.BusNumber.MinBusNumber, Desc->u.BusNumber.MaxBusNumber);
                    break;

                default:
                    DPRINT1("    [%lu] type %u opt 0x%x\n", Alt, Desc->Type, Desc->Option);
                    break;
            }
        }
    }
}

/* Frees the requirements of a configuration */
static
VOID
IopFreeConfiguration(
    _Inout_ PIOP_CONFIGURATION Configuration)
{
    ULONG Index;

    if (Configuration->Requirements == NULL)
        return;

    for (Index = 0; Index < Configuration->Count; Index++)
        IopFreeRequirementLevels(&Configuration->Requirements[Index]);

    ExFreePoolWithTag(Configuration->Requirements, TAG_IO_ARBITER);
    Configuration->Requirements = NULL;
}

/**
 * @brief
 * Builds the requirements of one alternative configuration, and finds the
 * arbiter and translators of each arbitrated requirement. A requirement is a
 * descriptor followed by its IO_RESOURCE_ALTERNATIVE descriptors.
 *
 * @param[in,out] ResourcesRequired
 * Set to TRUE if the configuration has a resource that must be assigned.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES. The configuration cannot
 * be used when Configuration->Status is a failure.
 *
 * @remarks
 * A leading CmResourceTypeConfigData descriptor is not a requirement, and a
 * descriptor of type IOP_RESOURCE_TYPE_LEGACY_BUS sets the legacy bus of the
 * requirements that follow it.
 */
static
NTSTATUS
IopBuildConfiguration(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ PIO_RESOURCE_LIST List,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _Out_ PIOP_CONFIGURATION Configuration,
    _Inout_ PBOOLEAN ResourcesRequired)
{
    INTERFACE_TYPE ListInterfaceType = IopResourceInterface(RequirementsList->InterfaceType);
    INTERFACE_TYPE InterfaceType = ListInterfaceType;
    ULONG BusNumber = RequirementsList->BusNumber;
    ULONG Index = 0;

    RtlZeroMemory(Configuration, sizeof(*Configuration));

    Configuration->Requirements = ExAllocatePoolZero(PagedPool,
                                                     List->Count * sizeof(IOP_REQUIREMENT),
                                                     TAG_IO_ARBITER);
    if (Configuration->Requirements == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (List->Descriptors[0].Type == CmResourceTypeConfigData)
        Index++;

    while (Index < List->Count)
    {
        PIO_RESOURCE_DESCRIPTOR Lead = &List->Descriptors[Index];
        PIOP_REQUIREMENT Requirement;
        ULONG AlternativeCount = 1;
        NTSTATUS Status;

        if (Lead->Type == IOP_RESOURCE_TYPE_LEGACY_BUS)
        {
            InterfaceType = IopResourceInterface((INTERFACE_TYPE)Lead->u.DevicePrivate.Data[0]);
            BusNumber = Lead->u.DevicePrivate.Data[1];
            Index++;
            continue;
        }

        if (IopIsArbitratedType(Lead->Type))
        {
            while (Index + AlternativeCount < List->Count &&
                   IopIsArbitratedType(List->Descriptors[Index + AlternativeCount].Type) &&
                   (List->Descriptors[Index + AlternativeCount].Option & IO_RESOURCE_ALTERNATIVE))
            {
                AlternativeCount++;
            }
        }

        Requirement = &Configuration->Requirements[Configuration->Count++];
        IopInitializeRequirement(Requirement,
                                 DeviceNode->PhysicalDeviceObject,
                                 RequestSource,
                                 InterfaceType,
                                 BusNumber,
                                 Lead,
                                 AlternativeCount);
        Requirement->Device.Entry.BusNumber = RequirementsList->BusNumber;
        Requirement->Device.Entry.SlotNumber = RequirementsList->SlotNumber;

        Index += AlternativeCount;

        if (!IopIsArbitratedType(Lead->Type))
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Copy = &Requirement->Device.Assignment;

            Copy->Type = Lead->Type;
            Copy->ShareDisposition = (Lead->Type == CmResourceTypeDevicePrivate) ?
                                         CmResourceShareDeviceExclusive : Lead->ShareDisposition;
            Copy->Flags = Lead->Flags;

            /* The PCI driver needs its private data back to program bridge windows */
            RtlCopyMemory(Copy->u.DevicePrivate.Data,
                          Lead->u.DevicePrivate.Data,
                          sizeof(Copy->u.DevicePrivate.Data));

            if (Lead->Type == CmResourceTypeConnection)
                *ResourcesRequired = TRUE;

            continue;
        }

        *ResourcesRequired = TRUE;

        Status = IopFindRequirementHandlers(Requirement, ListInterfaceType);
        if (Status == STATUS_INSUFFICIENT_RESOURCES)
            return Status;

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Configuration of %wZ cannot be arbitrated (Status 0x%08lx)\n",
                    &DeviceNode->InstancePath, Status);
            Configuration->Status = Status;
            break;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Tries to assign the resources of one alternative configuration. The
 * arbiters closer to the root test their requirements first, and all of them
 * commit when every test succeeded.
 *
 * @param[in] UseBootRanges
 * TRUE to let requirements that match the boot configuration of the device
 * use ranges reserved for boot configurations.
 *
 * @return
 * STATUS_SUCCESS if the configuration was committed, otherwise the failure
 * status of the arbiter or STATUS_CONFLICTING_ADDRESSES.
 */
static
NTSTATUS
IopArbitrateConfiguration(
    _In_ PDEVICE_NODE DeviceNode,
    _Inout_ PIOP_CONFIGURATION Configuration,
    _In_ BOOLEAN UseBootRanges)
{
    LIST_ENTRY ActiveArbiters;
    PLIST_ENTRY ListEntry;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;

    InitializeListHead(&ActiveArbiters);

    for (Index = 0; Index < Configuration->Count; Index++)
    {
        PIOP_REQUIREMENT Requirement = &Configuration->Requirements[Index];
        PIOP_REQUIREMENT_LEVEL Top = Requirement->Top;

        if (Requirement->Arbiter == NULL)
            continue;

        Top->Entry.Result = ArbiterResultUndefined;
        Top->Entry.Flags = 0;
        Top->Assignment.Type = CmResourceTypeMaximum;

        /* Let the device take back the resources of its own boot configuration */
        if (UseBootRanges && IopArbiterIsBootRequirement(DeviceNode, &Requirement->Device.Entry))
            Top->Entry.Flags |= ARBITER_FLAG_BOOT_CONFIG;

        IopQueueRequirement(&ActiveArbiters, Requirement);
    }

    for (ListEntry = ActiveArbiters.Flink;
         ListEntry != &ActiveArbiters;
         ListEntry = ListEntry->Flink)
    {
        PPI_RESOURCE_ARBITER_ENTRY Arbiter =
            CONTAINING_RECORD(ListEntry, PI_RESOURCE_ARBITER_ENTRY, ActiveArbiterList);
        PLIST_ENTRY Tested;

        Status = IopArbiterInvoke(Arbiter->ArbiterInterface,
                                  ArbiterActionTestAllocation,
                                  &Arbiter->ResourceList);
        if (NT_SUCCESS(Status))
            continue;

        IopArbiterReportFailure(Arbiter, UseBootRanges, DeviceNode);

        for (Tested = ActiveArbiters.Flink; Tested != ListEntry; Tested = Tested->Flink)
        {
            PPI_RESOURCE_ARBITER_ENTRY Previous =
                CONTAINING_RECORD(Tested, PI_RESOURCE_ARBITER_ENTRY, ActiveArbiterList);

            IopArbiterInvoke(Previous->ArbiterInterface, ArbiterActionRollbackAllocation, NULL);
        }

        break;
    }

    if (NT_SUCCESS(Status))
    {
        for (ListEntry = ActiveArbiters.Flink;
             ListEntry != &ActiveArbiters;
             ListEntry = ListEntry->Flink)
        {
            PPI_RESOURCE_ARBITER_ENTRY Arbiter =
                CONTAINING_RECORD(ListEntry, PI_RESOURCE_ARBITER_ENTRY, ActiveArbiterList);

            if (!NT_SUCCESS(IopArbiterInvoke(Arbiter->ArbiterInterface,
                                             ArbiterActionCommitAllocation,
                                             NULL)))
            {
                DPRINT1("Arbiter of type %u failed to commit for %wZ\n",
                        Arbiter->ResourceType, &DeviceNode->InstancePath);
                Status = STATUS_CONFLICTING_ADDRESSES;
            }
        }
    }

    IopDequeueRequirements(&ActiveArbiters);

    return Status;
}

/**
 * @brief
 * Builds the raw and translated resource lists of a committed configuration.
 * The assignment of each arbitrated requirement is translated back to the
 * device for the raw list, then up to the processor for the translated list.
 *
 * @return
 * STATUS_SUCCESS, STATUS_RETRY if a translator asked to be called again later,
 * or STATUS_INSUFFICIENT_RESOURCES.
 */
static
NTSTATUS
IopBuildAssignedResources(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ PIOP_CONFIGURATION Configuration,
    _Out_ PCM_RESOURCE_LIST *ResourceList,
    _Out_ PCM_RESOURCE_LIST *TranslatedList)
{
    ULONG Size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors) +
                 Configuration->Count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    PCM_RESOURCE_LIST Lists[2];
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;

    PAGED_CODE();

    *ResourceList = NULL;
    *TranslatedList = NULL;

    if (Configuration->Count == 0)
        return STATUS_INTERNAL_ERROR;

    Lists[0] = ExAllocatePoolZero(PagedPool, Size, TAG_IO_ARBITER);
    Lists[1] = ExAllocatePoolZero(PagedPool, Size, TAG_IO_ARBITER);
    if (Lists[0] == NULL || Lists[1] == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failure;
    }

    for (Index = 0; Index < RTL_NUMBER_OF(Lists); Index++)
    {
        Lists[Index]->Count = 1;
        Lists[Index]->List[0].InterfaceType = IopResourceInterface(RequirementsList->InterfaceType);
        Lists[Index]->List[0].BusNumber = RequirementsList->BusNumber;
        Lists[Index]->List[0].PartialResourceList.Version = 1;
        Lists[Index]->List[0].PartialResourceList.Revision = 1;
        Lists[Index]->List[0].PartialResourceList.Count = Configuration->Count;
    }

    for (Index = 0; Index < Configuration->Count; Index++)
    {
        PIOP_REQUIREMENT Requirement = &Configuration->Requirements[Index];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw =
            &Lists[0]->List[0].PartialResourceList.PartialDescriptors[Index];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated =
            &Lists[1]->List[0].PartialResourceList.PartialDescriptors[Index];

        if (Requirement->Arbiter == NULL)
        {
            *Raw = Requirement->Device.Assignment;
            *Translated = Requirement->Device.Assignment;
            continue;
        }

        /* The arbiter did not assign anything, so there is nothing to translate */
        if (Requirement->Top->Entry.Result == ArbiterResultNullRequest)
        {
            *Raw = Requirement->Top->Assignment;
            *Translated = Requirement->Top->Assignment;
            continue;
        }

        Status = IopTranslateAssignmentToDevice(Requirement);
        if (NT_SUCCESS(Status))
        {
            *Raw = Requirement->Device.Assignment;

            Status = IopTranslateResourceToRoot(DeviceNode,
                                                Requirement->InterfaceType,
                                                Requirement->BusNumber,
                                                Requirement->Device.Entry.RequestSource,
                                                Raw,
                                                Translated);
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Assigned resource %lu of %wZ was not translated (Status 0x%08lx)\n",
                    Index, &DeviceNode->InstancePath, Status);
            Status = IopTranslationFailureStatus(Status);
            goto Failure;
        }
    }

    *ResourceList = Lists[0];
    *TranslatedList = Lists[1];
    return STATUS_SUCCESS;

Failure:
    for (Index = 0; Index < RTL_NUMBER_OF(Lists); Index++)
    {
        if (Lists[Index] != NULL)
            ExFreePoolWithTag(Lists[Index], TAG_IO_ARBITER);
    }

    return Status;
}

/**
 * @brief
 * Assigns the resources of a device through the arbiters. The arbiters and
 * translators of all alternative configurations are found first, then the
 * configurations are tried in order. The resources reserved for boot
 * configurations are only used when no configuration can be assigned without
 * them, and only for a device that has a boot configuration itself.
 *
 * @param[out] ResourceList
 * Receives the assigned resources, or NULL if the device needs none.
 *
 * @param[out] TranslatedList
 * Receives the translated resources, or NULL if the device needs none.
 *
 * @return
 * STATUS_SUCCESS, the status of the configuration that failed last, or the
 * failure status of the translation.
 */
static
NTSTATUS
IopArbiterAllocateResourcesEx(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _Out_ PCM_RESOURCE_LIST *ResourceList,
    _Out_ PCM_RESOURCE_LIST *TranslatedList)
{
    PIOP_CONFIGURATION Configurations;
    PIO_RESOURCE_LIST List;
    BOOLEAN ResourcesRequired = FALSE;
    NTSTATUS FailureStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Selected = MAXULONG;
    ULONG Usable = 0;
    ULONG Index;
    ULONG Pass;

    PAGED_CODE();

    *ResourceList = NULL;
    *TranslatedList = NULL;

    if (RequirementsList->AlternativeLists == 0)
        return STATUS_SUCCESS;

    Configurations = ExAllocatePoolZero(PagedPool,
                                        RequirementsList->AlternativeLists *
                                            sizeof(IOP_CONFIGURATION),
                                        TAG_IO_ARBITER);
    if (Configurations == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    List = &RequirementsList->List[0];
    for (Index = 0; Index < RequirementsList->AlternativeLists; Index++)
    {
        /* An empty configuration means the device needs no resources */
        if (List->Count == 0)
        {
            ResourcesRequired = FALSE;
            goto Cleanup;
        }

        Status = IopBuildConfiguration(DeviceNode,
                                       RequirementsList,
                                       List,
                                       RequestSource,
                                       &Configurations[Index],
                                       &ResourcesRequired);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        if (NT_SUCCESS(Configurations[Index].Status))
            Usable++;
        else
            FailureStatus = Configurations[Index].Status;

        List = IopArbiterNextList(List);
    }

    if (!ResourcesRequired)
        goto Cleanup;

    if (Usable == 0)
    {
        Status = FailureStatus;
        goto Cleanup;
    }

    Status = STATUS_CONFLICTING_ADDRESSES;

    for (Pass = 0; Pass < 2 && Selected == MAXULONG; Pass++)
    {
        BOOLEAN UseBootRanges = (Pass != 0);

        if (UseBootRanges &&
            (!(DeviceNode->Flags & DNF_HAS_BOOT_CONFIG) || DeviceNode->BootResources == NULL))
        {
            break;
        }

        for (Index = 0; Index < RequirementsList->AlternativeLists; Index++)
        {
            if (!NT_SUCCESS(Configurations[Index].Status))
                continue;

            Status = IopArbitrateConfiguration(DeviceNode, &Configurations[Index], UseBootRanges);
            if (NT_SUCCESS(Status))
            {
                Selected = Index;
                break;
            }
        }
    }

    if (Selected == MAXULONG)
    {
        DPRINT1("All %lu configurations failed for %wZ\n",
                RequirementsList->AlternativeLists, &DeviceNode->InstancePath);
        goto Cleanup;
    }

    Status = IopBuildAssignedResources(DeviceNode,
                                       RequirementsList,
                                       &Configurations[Selected],
                                       ResourceList,
                                       TranslatedList);

Cleanup:
    for (Index = 0; Index < RequirementsList->AlternativeLists; Index++)
        IopFreeConfiguration(&Configurations[Index]);

    ExFreePoolWithTag(Configurations, TAG_IO_ARBITER);

    if (NT_SUCCESS(Status) && !ResourcesRequired)
        DPRINT("%wZ requires no resources\n", &DeviceNode->InstancePath);

    return Status;
}

/* Assigns the resources of a PnP enumerated device through the arbiters */
NTSTATUS
NTAPI
IopArbiterAllocateResources(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _Out_ PCM_RESOURCE_LIST *ResourceList,
    _Out_ PCM_RESOURCE_LIST *TranslatedList)
{
    NTSTATUS Status;

    DPRINT1("Arbitrating resources for %wZ (%lu configuration(s))\n",
            &DeviceNode->InstancePath, RequirementsList->AlternativeLists);

    Status = IopArbiterAllocateResourcesEx(DeviceNode,
                                           RequirementsList,
                                           ArbiterRequestPnpEnumerated,
                                           ResourceList,
                                           TranslatedList);

    DPRINT1("Arbitration for %wZ returned 0x%08lx\n", &DeviceNode->InstancePath, Status);

    return Status;
}

/**
 * @brief
 * Frees all ranges of a device in one arbiter. A test allocation with no
 * alternatives removes the ranges of the device, and the commit applies it.
 */
static
VOID
IopArbiterReleaseOwner(
    _In_ PARBITER_INTERFACE Interface,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    ARBITER_LIST_ENTRY Entry;
    LIST_ENTRY ArbitrationList;
    NTSTATUS Status;

    RtlZeroMemory(&Entry, sizeof(Entry));
    Entry.PhysicalDeviceObject = PhysicalDeviceObject;
    Entry.RequestSource = ArbiterRequestPnpEnumerated;
    Entry.Result = ArbiterResultUndefined;

    InitializeListHead(&ArbitrationList);
    InsertTailList(&ArbitrationList, &Entry.ListEntry);

    Status = IopArbiterInvoke(Interface, ArbiterActionTestAllocation, &ArbitrationList);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Arbiter rejected the release request (Status 0x%08lx)\n", Status);
        IopArbiterInvoke(Interface, ArbiterActionRollbackAllocation, NULL);
        return;
    }

    IopArbiterInvoke(Interface, ArbiterActionCommitAllocation, NULL);
}

/**
 * @brief
 * Frees all arbiter ranges of a device, both assigned resources and the
 * reserved boot configuration. Every arbiter cached above the device frees
 * the ranges of the device. When no arbiter is cached below the root, the
 * arbiters above the legacy bus of the resources do it too.
 */
VOID
NTAPI
IopArbiterReleaseResources(
    _In_ PDEVICE_NODE DeviceNode)
{
    PCM_RESOURCE_LIST ResourceList;
    PCM_FULL_RESOURCE_DESCRIPTOR Full = NULL;
    ULONG ListCount = 1;
    ULONG ListIndex;

    PAGED_CODE();

    if (DeviceNode->PhysicalDeviceObject == NULL)
        return;

    ResourceList = (DeviceNode->ResourceList != NULL) ? DeviceNode->ResourceList
                                                      : DeviceNode->BootResources;
    if (ResourceList != NULL && ResourceList->Count != 0)
    {
        ListCount = ResourceList->Count;
        Full = &ResourceList->List[0];
    }

    for (ListIndex = 0; ListIndex < ListCount; ListIndex++)
    {
        INTERFACE_TYPE InterfaceType = Isa;
        ULONG BusNumber = 0;
        BOOLEAN UseLegacyBus = TRUE;
        PDEVICE_NODE Node;

        if (Full != NULL)
        {
            InterfaceType = IopResourceInterface(Full->InterfaceType);
            BusNumber = Full->BusNumber;
        }

        Node = (DeviceNode == IopRootDeviceNode) ? DeviceNode : IopGetResourceParent(DeviceNode);

        while (Node != NULL)
        {
            PLIST_ENTRY ListEntry;

            if (Node == IopRootDeviceNode && UseLegacyBus)
            {
                Node = IopFindResourceBus(InterfaceType, BusNumber, InterfaceType);
                UseLegacyBus = FALSE;
            }

            for (ListEntry = Node->DeviceArbiterList.Flink;
                 ListEntry != &Node->DeviceArbiterList;
                 ListEntry = ListEntry->Flink)
            {
                PPI_RESOURCE_ARBITER_ENTRY Arbiter =
                    CONTAINING_RECORD(ListEntry, PI_RESOURCE_ARBITER_ENTRY, DeviceArbiterList);

                if (Arbiter->ArbiterInterface == NULL)
                    continue;

                UseLegacyBus = FALSE;
                IopArbiterReleaseOwner(Arbiter->ArbiterInterface, DeviceNode->PhysicalDeviceObject);
            }

            Node = IopGetResourceParent(Node);
        }

        if (Full != NULL)
            Full = CmiGetNextResourceDescriptor(Full);
    }

    IopDeviceNodeClearFlag(DeviceNode, DNF_BOOT_CONFIG_RESERVED);
}

/* LEGACY RESOURCE HANDLING *************************************************/


FORCEINLINE
PIO_RESOURCE_LIST
IopGetNextResourceList(
    _In_ const IO_RESOURCE_LIST *ResourceList)
{
    ASSERT((ResourceList->Count > 0) && (ResourceList->Count < 1000));
    return (PIO_RESOURCE_LIST)(
        &ResourceList->Descriptors[ResourceList->Count]);
}

static
BOOLEAN
IopCheckDescriptorForConflict(
    PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    OPTIONAL PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
    CM_RESOURCE_LIST CmList;
    NTSTATUS Status;

    CmList.Count = 1;
    CmList.List[0].InterfaceType = InterfaceTypeUndefined;
    CmList.List[0].BusNumber = 0;
    CmList.List[0].PartialResourceList.Version = 1;
    CmList.List[0].PartialResourceList.Revision = 1;
    CmList.List[0].PartialResourceList.Count = 1;
    CmList.List[0].PartialResourceList.PartialDescriptors[0] = *CmDesc;

    Status = IopDetectResourceConflict(&CmList, TRUE, ConflictingDescriptor);
    if (Status == STATUS_CONFLICTING_ADDRESSES)
        return TRUE;

    return FALSE;
}

static
BOOLEAN
IopFindBusNumberResource(
    IN PIO_RESOURCE_DESCRIPTOR IoDesc,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    ULONG Start;
    CM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDesc;

    ASSERT(IoDesc->Type == CmDesc->Type);
    ASSERT(IoDesc->Type == CmResourceTypeBusNumber);

    for (Start = IoDesc->u.BusNumber.MinBusNumber;
         Start <= IoDesc->u.BusNumber.MaxBusNumber - IoDesc->u.BusNumber.Length + 1;
         Start++)
    {
        CmDesc->u.BusNumber.Length = IoDesc->u.BusNumber.Length;
        CmDesc->u.BusNumber.Start = Start;

        if (IopCheckDescriptorForConflict(CmDesc, &ConflictingDesc))
        {
            Start += ConflictingDesc.u.BusNumber.Start + ConflictingDesc.u.BusNumber.Length;
        }
        else
        {
            DPRINT1("Satisfying bus number requirement with 0x%x (length: 0x%x)\n", Start, CmDesc->u.BusNumber.Length);
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
IopFindMemoryResource(
    IN PIO_RESOURCE_DESCRIPTOR IoDesc,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    ULONGLONG Start;
    CM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDesc;

    ASSERT(IoDesc->Type == CmDesc->Type);
    ASSERT(IoDesc->Type == CmResourceTypeMemory);

    /* HACK */
    if (IoDesc->u.Memory.Alignment == 0)
        IoDesc->u.Memory.Alignment = 1;

    for (Start = (ULONGLONG)IoDesc->u.Memory.MinimumAddress.QuadPart;
         Start <= (ULONGLONG)IoDesc->u.Memory.MaximumAddress.QuadPart - IoDesc->u.Memory.Length + 1;
         Start += IoDesc->u.Memory.Alignment)
    {
        CmDesc->u.Memory.Length = IoDesc->u.Memory.Length;
        CmDesc->u.Memory.Start.QuadPart = (LONGLONG)Start;

        if (IopCheckDescriptorForConflict(CmDesc, &ConflictingDesc))
        {
            Start += (ULONGLONG)ConflictingDesc.u.Memory.Start.QuadPart +
                     ConflictingDesc.u.Memory.Length;
        }
        else
        {
            DPRINT1("Satisfying memory requirement with 0x%I64x (length: 0x%x)\n", Start, CmDesc->u.Memory.Length);
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
IopFindPortResource(
    IN PIO_RESOURCE_DESCRIPTOR IoDesc,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    ULONGLONG Start;
    CM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDesc;

    ASSERT(IoDesc->Type == CmDesc->Type);
    ASSERT(IoDesc->Type == CmResourceTypePort);

    /* HACK */
    if (IoDesc->u.Port.Alignment == 0)
        IoDesc->u.Port.Alignment = 1;

    for (Start = (ULONGLONG)IoDesc->u.Port.MinimumAddress.QuadPart;
         Start <= (ULONGLONG)IoDesc->u.Port.MaximumAddress.QuadPart - IoDesc->u.Port.Length + 1;
         Start += IoDesc->u.Port.Alignment)
    {
        CmDesc->u.Port.Length = IoDesc->u.Port.Length;
        CmDesc->u.Port.Start.QuadPart = (LONGLONG)Start;

        if (IopCheckDescriptorForConflict(CmDesc, &ConflictingDesc))
        {
            Start += (ULONGLONG)ConflictingDesc.u.Port.Start.QuadPart + ConflictingDesc.u.Port.Length;
        }
        else
        {
            DPRINT("Satisfying port requirement with 0x%I64x (length: 0x%x)\n", Start, CmDesc->u.Port.Length);
            return TRUE;
        }
    }

    DPRINT1("IopFindPortResource failed!\n");
    return FALSE;
}

static
BOOLEAN
IopFindDmaResource(
    IN PIO_RESOURCE_DESCRIPTOR IoDesc,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    ULONG Channel;

    ASSERT(IoDesc->Type == CmDesc->Type);
    ASSERT(IoDesc->Type == CmResourceTypeDma);

    for (Channel = IoDesc->u.Dma.MinimumChannel;
         Channel <= IoDesc->u.Dma.MaximumChannel;
         Channel++)
    {
        CmDesc->u.Dma.Channel = Channel;
        CmDesc->u.Dma.Port = 0;

        if (!IopCheckDescriptorForConflict(CmDesc, NULL))
        {
            DPRINT1("Satisfying DMA requirement with channel 0x%x\n", Channel);
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
IopFindInterruptResource(
    IN PIO_RESOURCE_DESCRIPTOR IoDesc,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    ULONG Vector;

    ASSERT(IoDesc->Type == CmDesc->Type);
    ASSERT(IoDesc->Type == CmResourceTypeInterrupt);

    for (Vector = IoDesc->u.Interrupt.MinimumVector;
         Vector <= IoDesc->u.Interrupt.MaximumVector;
         Vector++)
    {
        CmDesc->u.Interrupt.Vector = Vector;
        CmDesc->u.Interrupt.Level = Vector;
        CmDesc->u.Interrupt.Affinity = (KAFFINITY)-1;

        if (!IopCheckDescriptorForConflict(CmDesc, NULL))
        {
            DPRINT1("Satisfying interrupt requirement with IRQ 0x%x\n", Vector);
            return TRUE;
        }
    }

    DPRINT1("Failed to satisfy interrupt requirement with IRQ 0x%x-0x%x\n",
            IoDesc->u.Interrupt.MinimumVector,
            IoDesc->u.Interrupt.MaximumVector);
    return FALSE;
}

NTSTATUS NTAPI
IopFixupResourceListWithRequirements(
    IN PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    OUT PCM_RESOURCE_LIST *ResourceList)
{
    ULONG i, OldCount;
    BOOLEAN AlternateRequired = FALSE;
    PIO_RESOURCE_LIST ResList;

    /* Save the initial resource count when we got here so we can restore if an alternate fails */
    if (*ResourceList != NULL)
        OldCount = (*ResourceList)->List[0].PartialResourceList.Count;
    else
        OldCount = 0;

    ResList = &RequirementsList->List[0];
    for (i = 0; i < RequirementsList->AlternativeLists; i++, ResList = IopGetNextResourceList(ResList))
    {
        ULONG ii;

        /* We need to get back to where we were before processing the last alternative list */
        if (OldCount == 0 && *ResourceList != NULL)
        {
            /* Just free it and kill the pointer */
            ExFreePool(*ResourceList);
            *ResourceList = NULL;
        }
        else if (OldCount != 0)
        {
            PCM_RESOURCE_LIST NewList;

            /* Let's resize it */
            (*ResourceList)->List[0].PartialResourceList.Count = OldCount;

            /* Allocate the new smaller list */
            NewList = ExAllocatePool(PagedPool, PnpDetermineResourceListSize(*ResourceList));
            if (!NewList)
                return STATUS_NO_MEMORY;

            /* Copy the old stuff back */
            RtlCopyMemory(NewList, *ResourceList, PnpDetermineResourceListSize(*ResourceList));

            /* Free the old one */
            ExFreePool(*ResourceList);

            /* Store the pointer to the new one */
            *ResourceList = NewList;
        }

        for (ii = 0; ii < ResList->Count; ii++)
        {
            ULONG iii;
            PCM_PARTIAL_RESOURCE_LIST PartialList = (*ResourceList) ? &(*ResourceList)->List[0].PartialResourceList : NULL;
            PIO_RESOURCE_DESCRIPTOR IoDesc = &ResList->Descriptors[ii];
            BOOLEAN Matched = FALSE;

            /* Skip alternates if we don't need one */
            if (!AlternateRequired && (IoDesc->Option & IO_RESOURCE_ALTERNATIVE))
            {
                DPRINT("Skipping unneeded alternate\n");
                continue;
            }

            /* Check if we couldn't satsify a requirement or its alternates */
            if (AlternateRequired && !(IoDesc->Option & IO_RESOURCE_ALTERNATIVE))
            {
                DPRINT1("Unable to satisfy preferred resource or alternates in list %lu\n", i);

                /* Break out of this loop and try the next list */
                break;
            }

            for (iii = 0; PartialList && iii < PartialList->Count && !Matched; iii++)
            {
                /* Partial resource descriptors can be of variable size (CmResourceTypeDeviceSpecific),
                   but only one is allowed and it must be the last one in the list! */
                PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc = &PartialList->PartialDescriptors[iii];

                /* First check types */
                if (IoDesc->Type != CmDesc->Type)
                    continue;

                switch (IoDesc->Type)
                {
                    case CmResourceTypeInterrupt:
                        /* Make sure it satisfies our vector range */
                        if (CmDesc->u.Interrupt.Vector >= IoDesc->u.Interrupt.MinimumVector &&
                            CmDesc->u.Interrupt.Vector <= IoDesc->u.Interrupt.MaximumVector)
                        {
                            /* Found it */
                            Matched = TRUE;
                        }
                        else
                        {
                            DPRINT("Interrupt - Not a match! 0x%x not inside 0x%x to 0x%x\n",
                                   CmDesc->u.Interrupt.Vector,
                                   IoDesc->u.Interrupt.MinimumVector,
                                   IoDesc->u.Interrupt.MaximumVector);
                        }
                        break;

                    case CmResourceTypeMemory:
                    case CmResourceTypePort:
                        /* Make sure the length matches and it satisfies our address range */
                        if (CmDesc->u.Memory.Length == IoDesc->u.Memory.Length &&
                            (ULONGLONG)CmDesc->u.Memory.Start.QuadPart >= (ULONGLONG)IoDesc->u.Memory.MinimumAddress.QuadPart &&
                            (ULONGLONG)CmDesc->u.Memory.Start.QuadPart + CmDesc->u.Memory.Length - 1 <= (ULONGLONG)IoDesc->u.Memory.MaximumAddress.QuadPart)
                        {
                            /* Found it */
                            Matched = TRUE;
                        }
                        else
                        {
                            DPRINT("Memory/Port - Not a match! 0x%I64x with length 0x%x not inside 0x%I64x to 0x%I64x with length 0x%x\n",
                                   CmDesc->u.Memory.Start.QuadPart,
                                   CmDesc->u.Memory.Length,
                                   IoDesc->u.Memory.MinimumAddress.QuadPart,
                                   IoDesc->u.Memory.MaximumAddress.QuadPart,
                                   IoDesc->u.Memory.Length);
                        }
                        break;

                    case CmResourceTypeBusNumber:
                        /* Make sure the length matches and it satisfies our bus number range */
                        if (CmDesc->u.BusNumber.Length == IoDesc->u.BusNumber.Length &&
                            CmDesc->u.BusNumber.Start >= IoDesc->u.BusNumber.MinBusNumber &&
                            CmDesc->u.BusNumber.Start + CmDesc->u.BusNumber.Length - 1 <= IoDesc->u.BusNumber.MaxBusNumber)
                        {
                            /* Found it */
                            Matched = TRUE;
                        }
                        else
                        {
                            DPRINT("Bus Number - Not a match! 0x%x with length 0x%x not inside 0x%x to 0x%x with length 0x%x\n",
                                   CmDesc->u.BusNumber.Start,
                                   CmDesc->u.BusNumber.Length,
                                   IoDesc->u.BusNumber.MinBusNumber,
                                   IoDesc->u.BusNumber.MaxBusNumber,
                                   IoDesc->u.BusNumber.Length);
                        }
                        break;

                    case CmResourceTypeDma:
                        /* Make sure it fits in our channel range */
                        if (CmDesc->u.Dma.Channel >= IoDesc->u.Dma.MinimumChannel &&
                            CmDesc->u.Dma.Channel <= IoDesc->u.Dma.MaximumChannel)
                        {
                            /* Found it */
                            Matched = TRUE;
                        }
                        else
                        {
                            DPRINT("DMA - Not a match! 0x%x not inside 0x%x to 0x%x\n",
                                   CmDesc->u.Dma.Channel,
                                   IoDesc->u.Dma.MinimumChannel,
                                   IoDesc->u.Dma.MaximumChannel);
                        }
                        break;

                    default:
                        /* Other stuff is fine */
                        Matched = TRUE;
                        break;
                }
            }

            /* Check if we found a matching descriptor */
            if (!Matched)
            {
                PCM_RESOURCE_LIST NewList;
                CM_PARTIAL_RESOURCE_DESCRIPTOR NewDesc;
                PCM_PARTIAL_RESOURCE_DESCRIPTOR DescPtr;
                BOOLEAN FoundResource = TRUE;

                /* Setup the new CM descriptor */
                NewDesc.Type = IoDesc->Type;
                NewDesc.Flags = IoDesc->Flags;
                NewDesc.ShareDisposition = IoDesc->ShareDisposition;

                /* Let'se see if we can find a resource to satisfy this */
                switch (IoDesc->Type)
                {
                    case CmResourceTypeInterrupt:
                        /* Find an available interrupt */
                        if (!IopFindInterruptResource(IoDesc, &NewDesc))
                        {
                            DPRINT1("Failed to find an available interrupt resource (0x%x to 0x%x)\n",
                                    IoDesc->u.Interrupt.MinimumVector, IoDesc->u.Interrupt.MaximumVector);

                            FoundResource = FALSE;
                        }
                        break;

                    case CmResourceTypePort:
                        /* Find an available port range */
                        if (!IopFindPortResource(IoDesc, &NewDesc))
                        {
                            DPRINT1("Failed to find an available port resource (0x%I64x to 0x%I64x length: 0x%x)\n",
                                    IoDesc->u.Port.MinimumAddress.QuadPart, IoDesc->u.Port.MaximumAddress.QuadPart,
                                    IoDesc->u.Port.Length);

                            FoundResource = FALSE;
                        }
                        break;

                    case CmResourceTypeMemory:
                        /* Find an available memory range */
                        if (!IopFindMemoryResource(IoDesc, &NewDesc))
                        {
                            DPRINT1("Failed to find an available memory resource (0x%I64x to 0x%I64x length: 0x%x)\n",
                                    IoDesc->u.Memory.MinimumAddress.QuadPart, IoDesc->u.Memory.MaximumAddress.QuadPart,
                                    IoDesc->u.Memory.Length);

                            FoundResource = FALSE;
                        }
                        break;

                    case CmResourceTypeBusNumber:
                        /* Find an available bus address range */
                        if (!IopFindBusNumberResource(IoDesc, &NewDesc))
                        {
                            DPRINT1("Failed to find an available bus number resource (0x%x to 0x%x length: 0x%x)\n",
                                    IoDesc->u.BusNumber.MinBusNumber, IoDesc->u.BusNumber.MaxBusNumber,
                                    IoDesc->u.BusNumber.Length);

                            FoundResource = FALSE;
                        }
                        break;

                    case CmResourceTypeDma:
                        /* Find an available DMA channel */
                        if (!IopFindDmaResource(IoDesc, &NewDesc))
                        {
                            DPRINT1("Failed to find an available dma resource (0x%x to 0x%x)\n",
                                    IoDesc->u.Dma.MinimumChannel, IoDesc->u.Dma.MaximumChannel);

                            FoundResource = FALSE;
                        }
                        break;

                    default:
                        DPRINT1("Unsupported resource type: %x\n", IoDesc->Type);
                        FoundResource = FALSE;
                        break;
                }

                /* Check if it's missing and required */
                if (!FoundResource && IoDesc->Option == 0)
                {
                    /* Break out of this loop and try the next list */
                    DPRINT1("Unable to satisfy required resource in list %lu\n", i);
                    break;
                }
                else if (!FoundResource)
                {
                    /* Try an alternate for this preferred descriptor */
                    AlternateRequired = TRUE;
                    continue;
                }
                else
                {
                    /* Move on to the next preferred or required descriptor after this one */
                    AlternateRequired = FALSE;
                }

                /* Figure out what we need */
                if (PartialList == NULL)
                {
                    /* We need a new list */
                    NewList = ExAllocatePool(PagedPool, sizeof(CM_RESOURCE_LIST));
                    if (!NewList)
                        return STATUS_NO_MEMORY;

                    /* Set it up */
                    NewList->Count = 1;
                    NewList->List[0].InterfaceType = RequirementsList->InterfaceType;
                    NewList->List[0].BusNumber = RequirementsList->BusNumber;
                    NewList->List[0].PartialResourceList.Version = 1;
                    NewList->List[0].PartialResourceList.Revision = 1;
                    NewList->List[0].PartialResourceList.Count = 1;

                    /* Set our pointer */
                    DescPtr = &NewList->List[0].PartialResourceList.PartialDescriptors[0];
                }
                else
                {
                    /* Allocate the new larger list */
                    NewList = ExAllocatePool(PagedPool, PnpDetermineResourceListSize(*ResourceList) + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
                    if (!NewList)
                        return STATUS_NO_MEMORY;

                    /* Copy the old stuff back */
                    RtlCopyMemory(NewList, *ResourceList, PnpDetermineResourceListSize(*ResourceList));

                    /* Set our pointer */
                    DescPtr = &NewList->List[0].PartialResourceList.PartialDescriptors[NewList->List[0].PartialResourceList.Count];

                    /* Increment the descriptor count */
                    NewList->List[0].PartialResourceList.Count++;

                    /* Free the old list */
                    ExFreePool(*ResourceList);
                }

                /* Copy the descriptor in */
                *DescPtr = NewDesc;

                /* Store the new list */
                *ResourceList = NewList;
            }
        }

        /* Check if we need an alternate with no resources left */
        if (AlternateRequired)
        {
            DPRINT1("Unable to satisfy preferred resource or alternates in list %lu\n", i);

            /* Try the next alternate list */
            continue;
        }

        /* We're done because we satisfied one of the alternate lists */
        return STATUS_SUCCESS;
    }

    /* We ran out of alternates */
    DPRINT1("Out of alternate lists!\n");

    /* Free the list */
    if (*ResourceList)
    {
        ExFreePool(*ResourceList);
        *ResourceList = NULL;
    }

    /* Fail */
    return STATUS_CONFLICTING_ADDRESSES;
}

static
BOOLEAN
IopCheckResourceDescriptor(
    IN PCM_PARTIAL_RESOURCE_DESCRIPTOR ResDesc,
    IN PCM_RESOURCE_LIST ResourceList,
    IN BOOLEAN Silent,
    OUT OPTIONAL PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
    ULONG i, ii;
    BOOLEAN Result = FALSE;
    PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor;

    FullDescriptor = &ResourceList->List[0];
    for (i = 0; i < ResourceList->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST ResList = &FullDescriptor->PartialResourceList;
        FullDescriptor = CmiGetNextResourceDescriptor(FullDescriptor);

        for (ii = 0; ii < ResList->Count; ii++)
        {
            /* Partial resource descriptors can be of variable size (CmResourceTypeDeviceSpecific),
               but only one is allowed and it must be the last one in the list! */
            PCM_PARTIAL_RESOURCE_DESCRIPTOR ResDesc2 = &ResList->PartialDescriptors[ii];

            /* We don't care about shared resources */
            if (ResDesc->ShareDisposition == CmResourceShareShared &&
                ResDesc2->ShareDisposition == CmResourceShareShared)
                continue;

            /* Make sure we're comparing the same types */
            if (ResDesc->Type != ResDesc2->Type)
                continue;

            switch (ResDesc->Type)
            {
                case CmResourceTypeMemory:
                {
                    /* NOTE: ranges are in a form [x1;x2) */
                    UINT64 rStart = (UINT64)ResDesc->u.Memory.Start.QuadPart;
                    UINT64 rEnd = (UINT64)ResDesc->u.Memory.Start.QuadPart
                                  + ResDesc->u.Memory.Length;
                    UINT64 r2Start = (UINT64)ResDesc2->u.Memory.Start.QuadPart;
                    UINT64 r2End = (UINT64)ResDesc2->u.Memory.Start.QuadPart
                                   + ResDesc2->u.Memory.Length;

                    if (rStart < r2End && r2Start < rEnd)
                    {
                        if (!Silent)
                        {
                            DPRINT1("Resource conflict: Memory (0x%I64x to 0x%I64x vs. 0x%I64x to 0x%I64x)\n",
                                    rStart, rEnd, r2Start, r2End);
                        }

                        Result = TRUE;

                        goto ByeBye;
                    }
                    break;
                }
                case CmResourceTypePort:
                {
                    /* NOTE: ranges are in a form [x1;x2) */
                    UINT64 rStart = (UINT64)ResDesc->u.Port.Start.QuadPart;
                    UINT64 rEnd = (UINT64)ResDesc->u.Port.Start.QuadPart
                                  + ResDesc->u.Port.Length;
                    UINT64 r2Start = (UINT64)ResDesc2->u.Port.Start.QuadPart;
                    UINT64 r2End = (UINT64)ResDesc2->u.Port.Start.QuadPart
                                   + ResDesc2->u.Port.Length;

                    if (rStart < r2End && r2Start < rEnd)
                    {
                        if (!Silent)
                        {
                            DPRINT1("Resource conflict: Port (0x%I64x to 0x%I64x vs. 0x%I64x to 0x%I64x)\n",
                                    rStart, rEnd, r2Start, r2End);
                        }

                        Result = TRUE;

                        goto ByeBye;
                    }
                    break;
                }
                case CmResourceTypeInterrupt:
                {
                    if (ResDesc->u.Interrupt.Vector == ResDesc2->u.Interrupt.Vector)
                    {
                        if (!Silent)
                        {
                            DPRINT1("Resource conflict: IRQ (0x%x 0x%x vs. 0x%x 0x%x)\n",
                                    ResDesc->u.Interrupt.Vector, ResDesc->u.Interrupt.Level,
                                    ResDesc2->u.Interrupt.Vector, ResDesc2->u.Interrupt.Level);
                        }

                        Result = TRUE;

                        goto ByeBye;
                    }
                    break;
                }
                case CmResourceTypeBusNumber:
                {
                    /* NOTE: ranges are in a form [x1;x2) */
                    UINT32 rStart = ResDesc->u.BusNumber.Start;
                    UINT32 rEnd = ResDesc->u.BusNumber.Start + ResDesc->u.BusNumber.Length;
                    UINT32 r2Start = ResDesc2->u.BusNumber.Start;
                    UINT32 r2End = ResDesc2->u.BusNumber.Start + ResDesc2->u.BusNumber.Length;

                    if (rStart < r2End && r2Start < rEnd)
                    {
                        if (!Silent)
                        {
                            DPRINT1("Resource conflict: Bus number (0x%x to 0x%x vs. 0x%x to 0x%x)\n",
                                    rStart, rEnd, r2Start, r2End);
                        }

                        Result = TRUE;

                        goto ByeBye;
                    }
                    break;
                }
                case CmResourceTypeDma:
                {
                    if (ResDesc->u.Dma.Channel == ResDesc2->u.Dma.Channel)
                    {
                        if (!Silent)
                        {
                            DPRINT1("Resource conflict: Dma (0x%x 0x%x vs. 0x%x 0x%x)\n",
                                    ResDesc->u.Dma.Channel, ResDesc->u.Dma.Port,
                                    ResDesc2->u.Dma.Channel, ResDesc2->u.Dma.Port);
                        }

                        Result = TRUE;

                        goto ByeBye;
                    }
                    break;
                }
            }
        }
    }

ByeBye:

    if (Result && ConflictingDescriptor)
    {
        RtlCopyMemory(ConflictingDescriptor,
                      ResDesc,
                      sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
    }

    // Hacked, because after fixing resource list parsing
    // we actually detect resource conflicts
    return Silent ? Result : FALSE; // Result;
}

static
NTSTATUS
IopUpdateControlKeyWithResources(
    IN PDEVICE_NODE DeviceNode)
{
    UNICODE_STRING EnumRoot = RTL_CONSTANT_STRING(ENUM_ROOT);
    UNICODE_STRING Control = RTL_CONSTANT_STRING(L"Control");
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"AllocConfig");
    HANDLE EnumKey, InstanceKey, ControlKey;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;

    /* Open the Enum key */
    Status = IopOpenRegistryKeyEx(&EnumKey, NULL, &EnumRoot, KEY_ENUMERATE_SUB_KEYS);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Open the instance key (eg. Root\PNP0A03) */
    Status = IopOpenRegistryKeyEx(&InstanceKey, EnumKey, &DeviceNode->InstancePath, KEY_ENUMERATE_SUB_KEYS);
    ZwClose(EnumKey);

    if (!NT_SUCCESS(Status))
        return Status;

    /* Create/Open the Control key */
    InitializeObjectAttributes(&ObjectAttributes,
                               &Control,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               InstanceKey,
                               NULL);
    Status = ZwCreateKey(&ControlKey,
                         KEY_SET_VALUE,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         NULL);
    ZwClose(InstanceKey);

    if (!NT_SUCCESS(Status))
        return Status;

    /* Write the resource list */
    Status = ZwSetValueKey(ControlKey,
                           &ValueName,
                           0,
                           REG_RESOURCE_LIST,
                           DeviceNode->ResourceList,
                           PnpDetermineResourceListSize(DeviceNode->ResourceList));
    ZwClose(ControlKey);

    if (!NT_SUCCESS(Status))
        return Status;

    return STATUS_SUCCESS;
}

static
NTSTATUS
IopFilterResourceRequirements(
    IN PDEVICE_NODE DeviceNode)
{
    IO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    DPRINT("Sending IRP_MN_FILTER_RESOURCE_REQUIREMENTS to device stack\n");

    Stack.Parameters.FilterResourceRequirements.IoResourceRequirementList = DeviceNode->ResourceRequirements;
    Status = IopInitiatePnpIrp(DeviceNode->PhysicalDeviceObject,
                               &IoStatusBlock,
                               IRP_MN_FILTER_RESOURCE_REQUIREMENTS,
                               &Stack);
    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_SUPPORTED)
    {
        DPRINT1("IopInitiatePnpIrp(IRP_MN_FILTER_RESOURCE_REQUIREMENTS) failed\n");
        return Status;
    }
    else if (NT_SUCCESS(Status) && IoStatusBlock.Information)
    {
        DeviceNode->ResourceRequirements = (PIO_RESOURCE_REQUIREMENTS_LIST)IoStatusBlock.Information;
    }

    return STATUS_SUCCESS;
}


NTSTATUS
IopUpdateResourceMap(
    IN PDEVICE_NODE DeviceNode,
    PWCHAR Level1Key,
    PWCHAR Level2Key)
{
    NTSTATUS Status;
    ULONG Disposition;
    HANDLE PnpMgrLevel1, PnpMgrLevel2, ResourceMapKey;
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName,
                         L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_OPENIF | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwCreateKey(&ResourceMapKey,
                         KEY_ALL_ACCESS,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         &Disposition);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&KeyName, Level1Key);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_OPENIF | OBJ_KERNEL_HANDLE,
                               ResourceMapKey,
                               NULL);
    Status = ZwCreateKey(&PnpMgrLevel1,
                         KEY_ALL_ACCESS,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         &Disposition);
    ZwClose(ResourceMapKey);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&KeyName, Level2Key);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_OPENIF | OBJ_KERNEL_HANDLE,
                               PnpMgrLevel1,
                               NULL);
    Status = ZwCreateKey(&PnpMgrLevel2,
                         KEY_ALL_ACCESS,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         &Disposition);
    ZwClose(PnpMgrLevel1);
    if (!NT_SUCCESS(Status))
        return Status;

    if (DeviceNode->ResourceList)
    {
        UNICODE_STRING NameU;
        UNICODE_STRING RawSuffix, TranslatedSuffix;
        ULONG OldLength = 0;

        ASSERT(DeviceNode->ResourceListTranslated);

        RtlInitUnicodeString(&TranslatedSuffix, L".Translated");
        RtlInitUnicodeString(&RawSuffix, L".Raw");

        Status = IoGetDeviceProperty(DeviceNode->PhysicalDeviceObject,
                                     DevicePropertyPhysicalDeviceObjectName,
                                     0,
                                     NULL,
                                     &OldLength);
        if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
        {
            ASSERT(OldLength);

            NameU.Buffer = ExAllocatePool(PagedPool, OldLength + TranslatedSuffix.Length);
            if (!NameU.Buffer)
            {
                ZwClose(PnpMgrLevel2);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            NameU.Length = 0;
            NameU.MaximumLength = (USHORT)OldLength + TranslatedSuffix.Length;

            Status = IoGetDeviceProperty(DeviceNode->PhysicalDeviceObject,
                                         DevicePropertyPhysicalDeviceObjectName,
                                         NameU.MaximumLength,
                                         NameU.Buffer,
                                         &OldLength);
            if (!NT_SUCCESS(Status))
            {
                ZwClose(PnpMgrLevel2);
                ExFreePool(NameU.Buffer);
                return Status;
            }
        }
        else if (!NT_SUCCESS(Status))
        {
            /* Some failure */
            ZwClose(PnpMgrLevel2);
            return Status;
        }
        else
        {
            /* This should never happen */
            ASSERT(FALSE);
        }

        NameU.Length = (USHORT)OldLength - sizeof(UNICODE_NULL); /* Remove final NULL */

        RtlAppendUnicodeStringToString(&NameU, &RawSuffix);

        Status = ZwSetValueKey(PnpMgrLevel2,
                               &NameU,
                               0,
                               REG_RESOURCE_LIST,
                               DeviceNode->ResourceList,
                               PnpDetermineResourceListSize(DeviceNode->ResourceList));
        if (!NT_SUCCESS(Status))
        {
            ZwClose(PnpMgrLevel2);
            ExFreePool(NameU.Buffer);
            return Status;
        }

        /* "Remove" the suffix by setting the length back to what it used to be */
        NameU.Length = (USHORT)OldLength - sizeof(UNICODE_NULL); /* Remove final NULL */

        RtlAppendUnicodeStringToString(&NameU, &TranslatedSuffix);

        Status = ZwSetValueKey(PnpMgrLevel2,
                               &NameU,
                               0,
                               REG_RESOURCE_LIST,
                               DeviceNode->ResourceListTranslated,
                               PnpDetermineResourceListSize(DeviceNode->ResourceListTranslated));
        ZwClose(PnpMgrLevel2);
        ExFreePool(NameU.Buffer);

        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        ZwClose(PnpMgrLevel2);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
IopUpdateResourceMapForPnPDevice(
    IN PDEVICE_NODE DeviceNode)
{
    return IopUpdateResourceMap(DeviceNode, L"PnP Manager", L"PnpManager");
}

/* Sets the problem of a device whose resources were not assigned, unless it is retried later */
static
VOID
IopSetResourceProblem(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ NTSTATUS Status)
{
    ULONG Problem;

    switch (Status)
    {
        case STATUS_RETRY:
            return;

        case STATUS_DEVICE_CONFIGURATION_ERROR:
            Problem = CM_PROB_NO_SOFTCONFIG;
            break;

        case STATUS_PNP_BAD_MPS_TABLE:
        case STATUS_BAD_MCFG_TABLE:
            Problem = CM_PROB_BIOS_TABLE;
            break;

        case STATUS_PNP_TRANSLATION_FAILED:
            Problem = CM_PROB_TRANSLATION_FAILED;
            break;

        case STATUS_PNP_IRQ_TRANSLATION_FAILED:
            Problem = CM_PROB_IRQ_TRANSLATION_FAILED;
            break;

        case STATUS_RESOURCE_TYPE_NOT_FOUND:
            Problem = CM_PROB_UNKNOWN_RESOURCE;
            break;

        default:
            Problem = CM_PROB_NORMAL_CONFLICT;
            break;
    }

    PiSetDevNodeProblem(DeviceNode, Problem);
}

/**
 * @brief
 * Assigns the resources of a device, translates them and writes them to the
 * registry. On failure the device gets a problem code, unless the assignment
 * is to be retried later.
 */
NTSTATUS
NTAPI
IopAssignDeviceResources(
    _In_ PDEVICE_NODE DeviceNode)
{
    NTSTATUS Status;
    ULONG ListSize;

    PAGED_CODE();

    IopLockResourceAssignment();

    /* Free a previous assignment, so the device does not conflict with itself. A reserved
     * boot configuration stays, the arbiters replace it with the new assignment. Freeing
     * it first would commit a state where the device holds nothing. */
    if (DeviceNode->ResourceList != NULL)
        IopArbiterReleaseResources(DeviceNode);

    if (DeviceNode->ResourceListTranslated != NULL)
    {
        ExFreePool(DeviceNode->ResourceListTranslated);
        DeviceNode->ResourceListTranslated = NULL;
    }

    if (DeviceNode->ResourceList != NULL)
    {
        ExFreePool(DeviceNode->ResourceList);
        DeviceNode->ResourceList = NULL;
    }

    Status = IopFilterResourceRequirements(DeviceNode);
    if (!NT_SUCCESS(Status))
        goto Failure;

    if (DeviceNode->BootResources != NULL && !(DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED))
        IopArbiterReserveBootConfig(DeviceNode);

    if (DeviceNode->ResourceRequirements != NULL)
    {
        /* Call HAL to fixup our resource requirements list */
        HalAdjustResourceList(&DeviceNode->ResourceRequirements);

        Status = IopArbiterAllocateResources(DeviceNode,
                                             DeviceNode->ResourceRequirements,
                                             &DeviceNode->ResourceList,
                                             &DeviceNode->ResourceListTranslated);
    }
    else if (DeviceNode->BootResources != NULL)
    {
        /* Without requirements the boot configuration is used as is */
        ListSize = PnpDetermineResourceListSize(DeviceNode->BootResources);

        DeviceNode->ResourceList = ExAllocatePool(PagedPool, ListSize);
        if (DeviceNode->ResourceList == NULL)
        {
            Status = STATUS_NO_MEMORY;
            goto Failure;
        }

        RtlCopyMemory(DeviceNode->ResourceList, DeviceNode->BootResources, ListSize);

        Status = IopTranslateResourceList(DeviceNode,
                                          ArbiterRequestPnpEnumerated,
                                          DeviceNode->ResourceList,
                                          &DeviceNode->ResourceListTranslated);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to assign resources for %wZ (Status 0x%08lx)\n",
                &DeviceNode->InstancePath, Status);
        IopSetResourceProblem(DeviceNode, Status);
        goto Failure;
    }

    if (DeviceNode->ResourceList == NULL)
    {
        /* No resource needed for this device */
        DeviceNode->Flags |= DNF_NO_RESOURCE_REQUIRED;
        PiSetDevNodeState(DeviceNode, DeviceNodeResourcesAssigned);

        IopUnlockResourceAssignment();
        return STATUS_SUCCESS;
    }

    /* Write the resources to RESOURCEMAP and the Control key */
    Status = IopUpdateResourceMapForPnPDevice(DeviceNode);
    if (!NT_SUCCESS(Status))
        goto Failure;

    Status = IopUpdateControlKeyWithResources(DeviceNode);
    if (!NT_SUCCESS(Status))
        goto Failure;

    PiSetDevNodeState(DeviceNode, DeviceNodeResourcesAssigned);

    IopUnlockResourceAssignment();
    return STATUS_SUCCESS;

Failure:
    if (DeviceNode->ResourceListTranslated != NULL)
    {
        ExFreePool(DeviceNode->ResourceListTranslated);
        DeviceNode->ResourceListTranslated = NULL;
    }

    if (DeviceNode->ResourceList != NULL)
    {
        ExFreePool(DeviceNode->ResourceList);
        DeviceNode->ResourceList = NULL;
    }

    IopUnlockResourceAssignment();
    return Status;
}

static
BOOLEAN
IopCheckForResourceConflict(
   IN PCM_RESOURCE_LIST ResourceList1,
   IN PCM_RESOURCE_LIST ResourceList2,
   IN BOOLEAN Silent,
   OUT OPTIONAL PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
   ULONG i, ii;
   BOOLEAN Result = FALSE;
   PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor;

   FullDescriptor = &ResourceList1->List[0];
   for (i = 0; i < ResourceList1->Count; i++)
   {
      PCM_PARTIAL_RESOURCE_LIST ResList = &FullDescriptor->PartialResourceList;
      FullDescriptor = CmiGetNextResourceDescriptor(FullDescriptor);

      for (ii = 0; ii < ResList->Count; ii++)
      {
        /* Partial resource descriptors can be of variable size (CmResourceTypeDeviceSpecific),
           but only one is allowed and it must be the last one in the list! */
         PCM_PARTIAL_RESOURCE_DESCRIPTOR ResDesc = &ResList->PartialDescriptors[ii];

         Result = IopCheckResourceDescriptor(ResDesc,
                                             ResourceList2,
                                             Silent,
                                             ConflictingDescriptor);
         if (Result) goto ByeBye;
      }
   }

ByeBye:

   return Result;
}

NTSTATUS NTAPI
IopDetectResourceConflict(
   IN PCM_RESOURCE_LIST ResourceList,
   IN BOOLEAN Silent,
   OUT OPTIONAL PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
   OBJECT_ATTRIBUTES ObjectAttributes;
   UNICODE_STRING KeyName;
   HANDLE ResourceMapKey = NULL, ChildKey2 = NULL, ChildKey3 = NULL;
   ULONG KeyInformationLength, RequiredLength, KeyValueInformationLength, KeyNameInformationLength;
   PKEY_BASIC_INFORMATION KeyInformation;
   PKEY_VALUE_PARTIAL_INFORMATION KeyValueInformation;
   PKEY_VALUE_BASIC_INFORMATION KeyNameInformation;
   ULONG ChildKeyIndex1 = 0, ChildKeyIndex2, ChildKeyIndex3;
   NTSTATUS Status;

   RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP");
   InitializeObjectAttributes(&ObjectAttributes,
                              &KeyName,
                              OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                              NULL,
                              NULL);
   Status = ZwOpenKey(&ResourceMapKey, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &ObjectAttributes);
   if (!NT_SUCCESS(Status))
   {
      /* The key is missing which means we are the first device */
      return STATUS_SUCCESS;
   }

   while (TRUE)
   {
      Status = ZwEnumerateKey(ResourceMapKey,
                              ChildKeyIndex1,
                              KeyBasicInformation,
                              NULL,
                              0,
                              &RequiredLength);
      if (Status == STATUS_NO_MORE_ENTRIES)
          break;
      else if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
      {
          KeyInformationLength = RequiredLength;
          KeyInformation = ExAllocatePoolWithTag(PagedPool,
                                                 KeyInformationLength,
                                                 TAG_IO);
          if (!KeyInformation)
          {
              Status = STATUS_INSUFFICIENT_RESOURCES;
              goto cleanup;
          }

          Status = ZwEnumerateKey(ResourceMapKey,
                                  ChildKeyIndex1,
                                  KeyBasicInformation,
                                  KeyInformation,
                                  KeyInformationLength,
                                  &RequiredLength);
      }
      else
         goto cleanup;
      ChildKeyIndex1++;
      if (!NT_SUCCESS(Status))
      {
          ExFreePoolWithTag(KeyInformation, TAG_IO);
          goto cleanup;
      }

      KeyName.Buffer = KeyInformation->Name;
      KeyName.MaximumLength = KeyName.Length = (USHORT)KeyInformation->NameLength;
      InitializeObjectAttributes(&ObjectAttributes,
                                 &KeyName,
                                 OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                 ResourceMapKey,
                                 NULL);
      Status = ZwOpenKey(&ChildKey2,
                         KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
                         &ObjectAttributes);
      ExFreePoolWithTag(KeyInformation, TAG_IO);
      if (!NT_SUCCESS(Status))
          goto cleanup;

      ChildKeyIndex2 = 0;
      while (TRUE)
      {
          Status = ZwEnumerateKey(ChildKey2,
                                  ChildKeyIndex2,
                                  KeyBasicInformation,
                                  NULL,
                                  0,
                                  &RequiredLength);
          if (Status == STATUS_NO_MORE_ENTRIES)
              break;
          else if (Status == STATUS_BUFFER_TOO_SMALL)
          {
              KeyInformationLength = RequiredLength;
              KeyInformation = ExAllocatePoolWithTag(PagedPool,
                                                     KeyInformationLength,
                                                     TAG_IO);
              if (!KeyInformation)
              {
                  Status = STATUS_INSUFFICIENT_RESOURCES;
                  goto cleanup;
              }

              Status = ZwEnumerateKey(ChildKey2,
                                      ChildKeyIndex2,
                                      KeyBasicInformation,
                                      KeyInformation,
                                      KeyInformationLength,
                                      &RequiredLength);
          }
          else
              goto cleanup;
          ChildKeyIndex2++;
          if (!NT_SUCCESS(Status))
          {
              ExFreePoolWithTag(KeyInformation, TAG_IO);
              goto cleanup;
          }

          KeyName.Buffer = KeyInformation->Name;
          KeyName.MaximumLength = KeyName.Length = (USHORT)KeyInformation->NameLength;
          InitializeObjectAttributes(&ObjectAttributes,
                                     &KeyName,
                                     OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                     ChildKey2,
                                     NULL);
          Status = ZwOpenKey(&ChildKey3, KEY_QUERY_VALUE, &ObjectAttributes);
          ExFreePoolWithTag(KeyInformation, TAG_IO);
          if (!NT_SUCCESS(Status))
              goto cleanup;

          ChildKeyIndex3 = 0;
          while (TRUE)
          {
              Status = ZwEnumerateValueKey(ChildKey3,
                                           ChildKeyIndex3,
                                           KeyValuePartialInformation,
                                           NULL,
                                           0,
                                           &RequiredLength);
              if (Status == STATUS_NO_MORE_ENTRIES)
                  break;
              else if (Status == STATUS_BUFFER_TOO_SMALL)
              {
                  KeyValueInformationLength = RequiredLength;
                  KeyValueInformation = ExAllocatePoolWithTag(PagedPool,
                                                              KeyValueInformationLength,
                                                              TAG_IO);
                  if (!KeyValueInformation)
                  {
                      Status = STATUS_INSUFFICIENT_RESOURCES;
                      goto cleanup;
                  }

                  Status = ZwEnumerateValueKey(ChildKey3,
                                               ChildKeyIndex3,
                                               KeyValuePartialInformation,
                                               KeyValueInformation,
                                               KeyValueInformationLength,
                                               &RequiredLength);
              }
              else
                  goto cleanup;
              if (!NT_SUCCESS(Status))
              {
                  ExFreePoolWithTag(KeyValueInformation, TAG_IO);
                  goto cleanup;
              }

              Status = ZwEnumerateValueKey(ChildKey3,
                                           ChildKeyIndex3,
                                           KeyValueBasicInformation,
                                           NULL,
                                           0,
                                           &RequiredLength);
              if (Status == STATUS_BUFFER_TOO_SMALL)
              {
                  KeyNameInformationLength = RequiredLength;
                  KeyNameInformation = ExAllocatePoolWithTag(PagedPool,
                                                             KeyNameInformationLength + sizeof(WCHAR),
                                                             TAG_IO);
                  if (!KeyNameInformation)
                  {
                      Status = STATUS_INSUFFICIENT_RESOURCES;
                      goto cleanup;
                  }

                  Status = ZwEnumerateValueKey(ChildKey3,
                                               ChildKeyIndex3,
                                               KeyValueBasicInformation,
                                               KeyNameInformation,
                                               KeyNameInformationLength,
                                               &RequiredLength);
              }
              else
                  goto cleanup;
              ChildKeyIndex3++;
              if (!NT_SUCCESS(Status))
              {
                  ExFreePoolWithTag(KeyNameInformation, TAG_IO);
                  goto cleanup;
              }

              KeyNameInformation->Name[KeyNameInformation->NameLength / sizeof(WCHAR)] = UNICODE_NULL;

              /* Skip translated entries */
              if (wcsstr(KeyNameInformation->Name, L".Translated"))
              {
                  ExFreePoolWithTag(KeyNameInformation, TAG_IO);
                  ExFreePoolWithTag(KeyValueInformation, TAG_IO);
                  continue;
              }

              ExFreePoolWithTag(KeyNameInformation, TAG_IO);

              if (IopCheckForResourceConflict(ResourceList,
                                              (PCM_RESOURCE_LIST)KeyValueInformation->Data,
                                              Silent,
                                              ConflictingDescriptor))
              {
                  ExFreePoolWithTag(KeyValueInformation, TAG_IO);
                  Status = STATUS_CONFLICTING_ADDRESSES;
                  goto cleanup;
              }

              ExFreePoolWithTag(KeyValueInformation, TAG_IO);
          }
      }
   }

cleanup:
   if (ResourceMapKey != NULL)
       ObCloseHandle(ResourceMapKey, KernelMode);
   if (ChildKey2 != NULL)
       ObCloseHandle(ChildKey2, KernelMode);
   if (ChildKey3 != NULL)
       ObCloseHandle(ChildKey3, KernelMode);

   if (Status == STATUS_NO_MORE_ENTRIES)
       Status = STATUS_SUCCESS;

   return Status;
}


/* Tree walk callback that clears the resource problem of a device */
static
NTSTATUS
IopClearResourceProblem(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PVOID Context)
{
    PULONG Cleared = Context;

    if (DeviceNode->State != DeviceNodeDriversAdded ||
        !(DeviceNode->Flags & DNF_HAS_PROBLEM))
    {
        return STATUS_SUCCESS;
    }

    switch (DeviceNode->Problem)
    {
        case CM_PROB_NORMAL_CONFLICT:
        case CM_PROB_TRANSLATION_FAILED:
        case CM_PROB_IRQ_TRANSLATION_FAILED:
            DPRINT("Retrying resource assignment for %wZ\n", &DeviceNode->InstancePath);
            PiClearDevNodeProblem(DeviceNode);
            (*Cleared)++;
            break;

        default:
            break;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Lets the devices that could not be assigned resources try again, after
 * resources were freed.
 */
static
VOID
IopRequestResourceRetry(VOID)
{
    DEVICETREE_TRAVERSE_CONTEXT Context;
    ULONG Cleared = 0;

    if (IopRootDeviceNode == NULL)
        return;

    IopInitDeviceTreeTraverseContext(&Context,
                                     IopRootDeviceNode,
                                     IopClearResourceProblem,
                                     &Cleared);
    IopTraverseDeviceTree(&Context);

    if (Cleared == 0)
        return;

    PiQueueDeviceAction(IopRootDeviceNode->PhysicalDeviceObject,
                        PiActionEnumDeviceTree,
                        NULL,
                        NULL);
}

/**
 * @brief
 * Frees the resources of a device that is removed or restarted.
 *
 * @remarks
 * A root enumerated device that is still present keeps its boot
 * configuration, which is reserved again. For other devices the boot
 * configuration is freed, and reported again when the device is enumerated.
 */
NTSTATUS
NTAPI
IopFreeDeviceResources(
    _In_ PDEVICE_NODE DeviceNode)
{
    BOOLEAN HadResources;

    PAGED_CODE();

    IopLockResourceAssignment();

    HadResources = (DeviceNode->ResourceList != NULL ||
                    (DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED));

    if (HadResources)
    {
        IopArbiterReleaseResources(DeviceNode);

        if (DeviceNode->ResourceListTranslated != NULL)
        {
            ExFreePool(DeviceNode->ResourceListTranslated);
            DeviceNode->ResourceListTranslated = NULL;
        }

        if (DeviceNode->ResourceList != NULL)
        {
            ExFreePool(DeviceNode->ResourceList);
            DeviceNode->ResourceList = NULL;
        }

        /* Remove the resources from the registry */
        if (DeviceNode->PhysicalDeviceObject != NULL)
        {
            IopUpdateResourceMapForPnPDevice(DeviceNode);
            IopUpdateControlKeyWithResources(DeviceNode);
        }
    }

    if (!(DeviceNode->Flags & DNF_MADEUP) || (DeviceNode->Flags & DNF_DEVICE_GONE))
    {
        if (DeviceNode->BootResources != NULL)
        {
            ExFreePool(DeviceNode->BootResources);
            DeviceNode->BootResources = NULL;
        }

        if (DeviceNode->BootResourcesTranslated != NULL)
        {
            ExFreePool(DeviceNode->BootResourcesTranslated);
            DeviceNode->BootResourcesTranslated = NULL;
        }

        IopDeviceNodeClearFlag(DeviceNode, DNF_HAS_BOOT_CONFIG | DNF_BOOT_CONFIG_RESERVED);
    }
    else if (DeviceNode->BootResources != NULL && (DeviceNode->Flags & DNF_HAS_BOOT_CONFIG))
    {
        IopArbiterReserveBootConfig(DeviceNode);
    }

    if (HadResources)
        IopRequestResourceRetry();

    IopUnlockResourceAssignment();

    return STATUS_SUCCESS;
}
