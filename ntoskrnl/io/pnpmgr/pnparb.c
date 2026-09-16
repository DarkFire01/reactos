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

/*
 * Resources claimed with IoAssignResources or IoReportResourceUsage, for one
 * driver and device pair. The arbiters need a PDO as owner, so a device node is
 * created for the claim. It is not linked into the device tree, and its
 * resources reach the arbiters through the legacy bus they are on. They are
 * written to RESOURCEMAP\<ClassName>\<DriverName>\<ValueName>.Raw and .Translated.
 */
typedef struct _IOP_LEGACY_RESOURCE_OWNER
{
    LIST_ENTRY ListEntry;
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_NODE DeviceNode;
    UNICODE_STRING ClassName;
    UNICODE_STRING DriverName;
    UNICODE_STRING ValueName;
} IOP_LEGACY_RESOURCE_OWNER, *PIOP_LEGACY_RESOURCE_OWNER;

static LIST_ENTRY IopLegacyResourceOwnerList;

/* TRUE once IopRegisterRootArbiters has run */
static BOOLEAN IopResourceAssignmentReady = FALSE;

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
    InitializeListHead(&IopLegacyResourceOwnerList);
    IopResourceAssignmentReady = TRUE;

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

/* RESOURCE ASSIGNMENT *****************************************************/

static
NTSTATUS
IopUpdateLegacyResourceMap(
    _In_ PDEVICE_NODE DeviceNode);


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


/**
 * @brief
 * Builds the RESOURCEMAP value name of a device, without suffix. The buffer
 * has room for the ".Translated" suffix. The caller frees the buffer.
 *
 * @param[in] ValueBaseName
 * The name to use, or NULL to use the PDO name.
 */
static
NTSTATUS
IopResourceMapValueName(
    _In_ PDEVICE_NODE DeviceNode,
    _In_opt_ PCUNICODE_STRING ValueBaseName,
    _Out_ PUNICODE_STRING Name)
{
    static const UNICODE_STRING TranslatedSuffix = RTL_CONSTANT_STRING(L".Translated");
    NTSTATUS Status;
    ULONG Length = 0;

    RtlZeroMemory(Name, sizeof(*Name));

    if (ValueBaseName != NULL)
    {
        Name->MaximumLength = ValueBaseName->Length + TranslatedSuffix.Length;
        Name->Buffer = ExAllocatePool(PagedPool, Name->MaximumLength);
        if (Name->Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Name->Length = 0;
        RtlAppendUnicodeStringToString(Name, ValueBaseName);
        return STATUS_SUCCESS;
    }

    /* Get the size of the PDO name */
    Status = IoGetDeviceProperty(DeviceNode->PhysicalDeviceObject,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 0,
                                 NULL,
                                 &Length);
    if (Status != STATUS_BUFFER_OVERFLOW && Status != STATUS_BUFFER_TOO_SMALL)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    if (Length <= sizeof(UNICODE_NULL) ||
        Length + TranslatedSuffix.Length > MAXUSHORT)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Name->Buffer = ExAllocatePool(PagedPool, Length + TranslatedSuffix.Length);
    if (Name->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Name->Length = 0;
    Name->MaximumLength = (USHORT)(Length + TranslatedSuffix.Length);

    Status = IoGetDeviceProperty(DeviceNode->PhysicalDeviceObject,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 Length,
                                 Name->Buffer,
                                 &Length);
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(Name->Buffer);
        Name->Buffer = NULL;
        return Status;
    }

    /* Remove the terminating NULL */
    Name->Length = (USHORT)(Length - sizeof(UNICODE_NULL));

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Writes a resource list to the value Name + Suffix, or deletes the value
 * when ResourceList is NULL. The length of Name is restored before returning.
 */
static
NTSTATUS
IopWriteResourceMapValue(
    _In_ HANDLE KeyHandle,
    _Inout_ PUNICODE_STRING Name,
    _In_ PCUNICODE_STRING Suffix,
    _In_opt_ PCM_RESOURCE_LIST ResourceList)
{
    USHORT BaseLength = Name->Length;
    NTSTATUS Status;

    RtlAppendUnicodeStringToString(Name, Suffix);

    if (ResourceList != NULL)
    {
        Status = ZwSetValueKey(KeyHandle,
                               Name,
                               0,
                               REG_RESOURCE_LIST,
                               ResourceList,
                               PnpDetermineResourceListSize(ResourceList));
    }
    else
    {
        Status = ZwDeleteValueKey(KeyHandle, Name);
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            Status = STATUS_SUCCESS;
    }

    Name->Length = BaseLength;

    return Status;
}

static
NTSTATUS
IopUpdateResourceMap(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PCWSTR Level1Key,
    _In_ PCWSTR Level2Key,
    _In_opt_ PCUNICODE_STRING ValueBaseName)
{
    static const UNICODE_STRING RawSuffix = RTL_CONSTANT_STRING(L".Raw");
    static const UNICODE_STRING TranslatedSuffix = RTL_CONSTANT_STRING(L".Translated");
    NTSTATUS Status;
    ULONG Disposition;
    HANDLE PnpMgrLevel1, PnpMgrLevel2, ResourceMapKey;
    UNICODE_STRING KeyName;
    UNICODE_STRING NameU;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName,
                         L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
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

    RtlInitUnicodeString(&KeyName, (PWSTR)Level1Key);
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

    RtlInitUnicodeString(&KeyName, (PWSTR)Level2Key);
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

    Status = IopResourceMapValueName(DeviceNode, ValueBaseName, &NameU);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(PnpMgrLevel2);
        return Status;
    }

    /* The values are deleted when the device has no resources */
    ASSERT(DeviceNode->ResourceList == NULL || DeviceNode->ResourceListTranslated != NULL);

    Status = IopWriteResourceMapValue(PnpMgrLevel2, &NameU, &RawSuffix,
                                      DeviceNode->ResourceList);
    if (NT_SUCCESS(Status))
    {
        Status = IopWriteResourceMapValue(PnpMgrLevel2, &NameU, &TranslatedSuffix,
                                          DeviceNode->ResourceListTranslated);
    }

    ZwClose(PnpMgrLevel2);
    ExFreePool(NameU.Buffer);

    return Status;
}

NTSTATUS
IopUpdateResourceMapForPnPDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    return IopUpdateResourceMap(DeviceNode, L"PnP Manager", L"PnpManager", NULL);
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
        goto RegistryFailure;

    Status = IopUpdateControlKeyWithResources(DeviceNode);
    if (!NT_SUCCESS(Status))
        goto RegistryFailure;

    PiSetDevNodeState(DeviceNode, DeviceNodeResourcesAssigned);

    IopUnlockResourceAssignment();
    return STATUS_SUCCESS;

RegistryFailure:
    /* Without a problem the node is assigned again on every enumeration pass */
    PiSetDevNodeProblem(DeviceNode, CM_PROB_REGISTRY);

Failure:
    /* The arbiters granted the ranges before this failed. Only the list freed
     * below names them, so give them back while it still exists. */
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

    IopUnlockResourceAssignment();
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
        if (DeviceNode->Flags & DNF_LEGACY_RESOURCE_DEVICENODE)
        {
            IopUpdateLegacyResourceMap(DeviceNode);
        }
        else if (DeviceNode->PhysicalDeviceObject != NULL)
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

/* LEGACY RESOURCE CLAIMS ***************************************************/

/* Returns the claim owner of a driver and device pair, or NULL */
static
PIOP_LEGACY_RESOURCE_OWNER
IopFindLegacyResourceOwner(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    PLIST_ENTRY ListEntry;

    for (ListEntry = IopLegacyResourceOwnerList.Flink;
         ListEntry != &IopLegacyResourceOwnerList;
         ListEntry = ListEntry->Flink)
    {
        PIOP_LEGACY_RESOURCE_OWNER Owner =
            CONTAINING_RECORD(ListEntry, IOP_LEGACY_RESOURCE_OWNER, ListEntry);

        if (Owner->DriverObject == DriverObject && Owner->DeviceObject == DeviceObject)
            return Owner;
    }

    return NULL;
}

/* Returns the claim owner of a legacy resource device node, or NULL */
static
PIOP_LEGACY_RESOURCE_OWNER
IopLegacyResourceOwnerOfNode(
    _In_ PDEVICE_NODE DeviceNode)
{
    PLIST_ENTRY ListEntry;

    for (ListEntry = IopLegacyResourceOwnerList.Flink;
         ListEntry != &IopLegacyResourceOwnerList;
         ListEntry = ListEntry->Flink)
    {
        PIOP_LEGACY_RESOURCE_OWNER Owner =
            CONTAINING_RECORD(ListEntry, IOP_LEGACY_RESOURCE_OWNER, ListEntry);

        if (Owner->DeviceNode == DeviceNode)
            return Owner;
    }

    return NULL;
}

/* Writes or deletes the RESOURCEMAP values of a legacy claim */
static
NTSTATUS
IopUpdateLegacyResourceMap(
    _In_ PDEVICE_NODE DeviceNode)
{
    PIOP_LEGACY_RESOURCE_OWNER Owner = IopLegacyResourceOwnerOfNode(DeviceNode);

    if (Owner == NULL)
        return STATUS_SUCCESS;

    return IopUpdateResourceMap(DeviceNode,
                                Owner->ClassName.Buffer,
                                Owner->DriverName.Buffer,
                                &Owner->ValueName);
}

/**
 * @brief
 * Returns the last component of the driver name, pointing into the driver
 * object, or "Unknown".
 */
static
VOID
IopLegacyDriverBaseName(
    _In_ PDRIVER_OBJECT DriverObject,
    _Out_ PUNICODE_STRING Name)
{
    static const UNICODE_STRING Unknown = RTL_CONSTANT_STRING(L"Unknown");
    USHORT Index;

    *Name = DriverObject->DriverName;

    Index = Name->Length / sizeof(WCHAR);
    while (Index > 0 && Name->Buffer[Index - 1] != OBJ_NAME_PATH_SEPARATOR)
        Index--;

    Name->Buffer += Index;
    Name->Length -= Index * sizeof(WCHAR);
    Name->MaximumLength = Name->Length;

    if (Name->Length == 0)
        *Name = Unknown;
}

/**
 * @brief
 * Builds the RESOURCEMAP value name of a claim. This is the name of the
 * device object, or the driver name if the device has no name.
 */
static
NTSTATUS
IopLegacyClaimValueName(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ PCUNICODE_STRING DriverName,
    _Out_ PUNICODE_STRING ValueName)
{
    POBJECT_NAME_INFORMATION NameInfo;
    ULONG Length = 0;
    NTSTATUS Status;

    if (DeviceObject != NULL)
        ObQueryNameString(DeviceObject, NULL, 0, &Length);

    NameInfo = (Length != 0) ? ExAllocatePoolWithTag(PagedPool, Length, TAG_IO_ARBITER) : NULL;
    if (NameInfo != NULL)
    {
        Status = ObQueryNameString(DeviceObject, NameInfo, Length, &Length);
        if (NT_SUCCESS(Status) && NameInfo->Name.Length != 0)
            Status = RtlDuplicateUnicodeString(0, &NameInfo->Name, ValueName);
        else
            Status = STATUS_UNSUCCESSFUL;

        ExFreePoolWithTag(NameInfo, TAG_IO_ARBITER);

        if (NT_SUCCESS(Status))
            return STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(RtlDuplicateUnicodeString(0, DriverName, ValueName)))
        return STATUS_INSUFFICIENT_RESOURCES;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates the device node that owns the resources of a legacy claim, with
 * the instance path ROOT\LEGACY_<driver name>.
 *
 * @remarks
 * The node is marked root enumerated, so the claim can share driver exclusive
 * resources that were reported for the same hardware. It has no parent, and
 * belongs to the root.
 */
static
NTSTATUS
IopCreateLegacyResourceNode(
    _In_ PCUNICODE_STRING DriverName,
    _Out_ PDEVICE_NODE *DeviceNode)
{
    static const UNICODE_STRING Prefix = RTL_CONSTANT_STRING(L"ROOT\\LEGACY_");
    UNICODE_STRING Path;
    PDEVICE_OBJECT Pdo;
    PDEVICE_NODE Node;
    ULONG PathLength;
    NTSTATUS Status;

    *DeviceNode = NULL;

    PathLength = Prefix.Length + DriverName->Length + sizeof(UNICODE_NULL);
    if (PathLength > MAXUSHORT)
        return STATUS_INVALID_PARAMETER;

    Path.Buffer = ExAllocatePoolWithTag(PagedPool, PathLength, TAG_IO_ARBITER);
    if (Path.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Path.Length = 0;
    Path.MaximumLength = (USHORT)PathLength;
    RtlAppendUnicodeStringToString(&Path, &Prefix);
    RtlAppendUnicodeStringToString(&Path, DriverName);
    Path.Buffer[Path.Length / sizeof(WCHAR)] = UNICODE_NULL;

    Status = PnpRootCreateDeviceObject(&Pdo);
    if (!NT_SUCCESS(Status))
        goto Done;

    Node = PipAllocateDeviceNode(Pdo);
    if (Node == NULL || !RtlCreateUnicodeString(&Node->InstancePath, Path.Buffer))
    {
        if (Node != NULL)
            PiSetDevNodeState(Node, DeviceNodeRemoved);

        IoDeleteDevice(Pdo);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Pdo->Flags |= DO_BUS_ENUMERATED_DEVICE;
    Node->Flags |= DNF_MADEUP | DNF_ENUMERATED | DNF_IDS_QUERIED |
                   DNF_LEGACY_RESOURCE_DEVICENODE | DNF_NO_RESOURCE_REQUIRED;
    PiSetDevNodeState(Node, DeviceNodeStarted);

    *DeviceNode = Node;

Done:
    ExFreePoolWithTag(Path.Buffer, TAG_IO_ARBITER);
    return Status;
}

/**
 * @brief
 * Creates the owner of a legacy claim and its device node.
 *
 * @param[in] DriverClassName
 * The RESOURCEMAP class, or NULL for "OtherDrivers".
 *
 * @return
 * The owner, or NULL on failure.
 */
static
PIOP_LEGACY_RESOURCE_OWNER
IopCreateLegacyResourceOwner(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PCUNICODE_STRING DriverClassName)
{
    static const UNICODE_STRING OtherDrivers = RTL_CONSTANT_STRING(L"OtherDrivers");
    PIOP_LEGACY_RESOURCE_OWNER Owner;
    UNICODE_STRING DriverName;
    NTSTATUS Status;

    IopLegacyDriverBaseName(DriverObject, &DriverName);

    Owner = ExAllocatePoolZero(PagedPool, sizeof(*Owner), TAG_IO_ARBITER);
    if (Owner == NULL)
        return NULL;

    Owner->DriverObject = DriverObject;
    Owner->DeviceObject = DeviceObject;

    Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                                       DriverClassName ? DriverClassName : &OtherDrivers,
                                       &Owner->ClassName);
    if (NT_SUCCESS(Status))
    {
        Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                                           &DriverName,
                                           &Owner->DriverName);
    }
    if (NT_SUCCESS(Status))
        Status = IopLegacyClaimValueName(DeviceObject, &DriverName, &Owner->ValueName);
    if (NT_SUCCESS(Status))
        Status = IopCreateLegacyResourceNode(&DriverName, &Owner->DeviceNode);

    if (!NT_SUCCESS(Status))
    {
        RtlFreeUnicodeString(&Owner->ClassName);
        RtlFreeUnicodeString(&Owner->DriverName);
        RtlFreeUnicodeString(&Owner->ValueName);
        ExFreePoolWithTag(Owner, TAG_IO_ARBITER);
        return NULL;
    }

    InsertTailList(&IopLegacyResourceOwnerList, &Owner->ListEntry);

    return Owner;
}

/* Frees the resources, the device node and the owner of a legacy claim */
static
VOID
IopDestroyLegacyResourceOwner(
    _In_ PIOP_LEGACY_RESOURCE_OWNER Owner)
{
    PDEVICE_NODE DeviceNode = Owner->DeviceNode;

    IopFreeDeviceResources(DeviceNode);

    RemoveEntryList(&Owner->ListEntry);

    PiSetDevNodeState(DeviceNode, DeviceNodeRemoved);

    /* Deleting the PDO also frees the device node */
    IoDeleteDevice(DeviceNode->PhysicalDeviceObject);

    RtlFreeUnicodeString(&Owner->ClassName);
    RtlFreeUnicodeString(&Owner->DriverName);
    RtlFreeUnicodeString(&Owner->ValueName);
    ExFreePoolWithTag(Owner, TAG_IO_ARBITER);
}

/* Checks if the assigned resources of a device contain a descriptor */
static
BOOLEAN
IopArbiterDeviceOwnsDescriptor(
    _In_opt_ PDEVICE_NODE DeviceNode,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm)
{
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    IO_RESOURCE_DESCRIPTOR Wanted;
    IO_RESOURCE_DESCRIPTOR Owned;
    ULONG Index;

    if (DeviceNode == NULL ||
        DeviceNode->ResourceList == NULL ||
        DeviceNode->ResourceList->Count == 0 ||
        !IopArbiterCmToFixedRequirement(Cm, TRUE, &Wanted))
    {
        return FALSE;
    }

    PartialList = &DeviceNode->ResourceList->List[0].PartialResourceList;

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        BOOLEAN Contained;

        if (!IopArbiterCmToFixedRequirement(&PartialList->PartialDescriptors[Index],
                                            TRUE,
                                            &Owned) ||
            Owned.Type != Wanted.Type)
        {
            continue;
        }

        switch (Wanted.Type)
        {
            case CmResourceTypePort:
            case CmResourceTypeMemory:
            case CmResourceTypeMemoryLarge:
                Contained = Wanted.u.Generic.MinimumAddress.QuadPart >=
                            Owned.u.Generic.MinimumAddress.QuadPart &&
                            Wanted.u.Generic.MaximumAddress.QuadPart <=
                            Owned.u.Generic.MaximumAddress.QuadPart;
                break;

            case CmResourceTypeBusNumber:
                Contained = Wanted.u.BusNumber.MinBusNumber >= Owned.u.BusNumber.MinBusNumber &&
                            Wanted.u.BusNumber.MaxBusNumber <= Owned.u.BusNumber.MaxBusNumber;
                break;

            case CmResourceTypeInterrupt:
                Contained = (Wanted.u.Interrupt.MinimumVector == Owned.u.Interrupt.MinimumVector);
                break;

            case CmResourceTypeDma:
                Contained = (Wanted.u.Dma.MinimumChannel == Owned.u.Dma.MinimumChannel);
                break;

            default:
                Contained = FALSE;
                break;
        }

        if (Contained)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Converts an assigned resource list to a requirements list with one fixed
 * requirement per arbitrated descriptor.
 *
 * @param[in] OwningNode
 * If not NULL, the descriptors this device already owns are skipped.
 *
 * @return
 * The requirements list, or NULL if there is nothing to arbitrate or the
 * allocation failed.
 */
static
PIO_RESOURCE_REQUIREMENTS_LIST
IopArbiterCmListToRequirements(
    _In_opt_ PCM_RESOURCE_LIST ResourceList,
    _In_opt_ PDEVICE_NODE OwningNode)
{
    const ULONG HeaderSize = FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List) +
                             FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors);
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements;
    PIO_RESOURCE_LIST IoList;
    ULONG Index;

    if (ResourceList == NULL || ResourceList->Count == 0)
        return NULL;

    PartialList = &ResourceList->List[0].PartialResourceList;
    if (PartialList->Count == 0)
        return NULL;

    /* Allocate for all descriptors, the unused tail is not part of the list */
    Requirements = ExAllocatePoolZero(PagedPool,
                                      HeaderSize +
                                          PartialList->Count * sizeof(IO_RESOURCE_DESCRIPTOR),
                                      TAG_IO_ARBITER);
    if (Requirements == NULL)
        return NULL;

    IoList = &Requirements->List[0];

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];

        if (IopArbiterCmToFixedRequirement(Cm, TRUE, &IoList->Descriptors[IoList->Count]) &&
            !IopArbiterDeviceOwnsDescriptor(OwningNode, Cm))
        {
            IoList->Count++;
        }
    }

    if (IoList->Count == 0)
    {
        ExFreePoolWithTag(Requirements, TAG_IO_ARBITER);
        return NULL;
    }

    IoList->Version = 1;
    IoList->Revision = 1;
    Requirements->ListSize = HeaderSize + IoList->Count * sizeof(IO_RESOURCE_DESCRIPTOR);
    Requirements->InterfaceType = ResourceList->List[0].InterfaceType;
    Requirements->BusNumber = ResourceList->List[0].BusNumber;
    Requirements->AlternativeLists = 1;

    return Requirements;
}

static
PCM_RESOURCE_LIST
IopDuplicateResourceList(
    _In_ PCM_RESOURCE_LIST ResourceList)
{
    ULONG Size = PnpDetermineResourceListSize(ResourceList);
    PCM_RESOURCE_LIST Copy;

    Copy = ExAllocatePoolWithTag(PagedPool, Size, TAG_IO_ARBITER);
    if (Copy != NULL)
        RtlCopyMemory(Copy, ResourceList, Size);

    return Copy;
}

/**
 * @brief
 * Assigns the resources of a legacy claim, translates them and writes them to
 * the registry. The previous resources of the claim are freed first.
 *
 * @param[in] ForceList
 * If not NULL, these resources are recorded when the requirements conflict.
 *
 * @param[out] ConflictDetected
 * Set to TRUE if the requirements conflict with assigned resources.
 *
 * @return
 * STATUS_SUCCESS, STATUS_CONFLICTING_ADDRESSES, or the failure status of the
 * translation or allocation.
 */
static
NTSTATUS
IopLegacyClaim(
    _In_ PIOP_LEGACY_RESOURCE_OWNER Owner,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST Requirements,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_opt_ PCM_RESOURCE_LIST ForceList,
    _Out_ PBOOLEAN ConflictDetected)
{
    PDEVICE_NODE DeviceNode = Owner->DeviceNode;
    NTSTATUS Status;

    *ConflictDetected = FALSE;

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

    Status = IopArbiterAllocateResourcesEx(DeviceNode,
                                           Requirements,
                                           RequestSource,
                                           &DeviceNode->ResourceList,
                                           &DeviceNode->ResourceListTranslated);
    if (!NT_SUCCESS(Status))
    {
        *ConflictDetected = TRUE;

        if (ForceList == NULL)
        {
            /* Delete the registry values of the previous claim */
            IopUpdateLegacyResourceMap(DeviceNode);
            return STATUS_CONFLICTING_ADDRESSES;
        }

        DeviceNode->ResourceList = IopDuplicateResourceList(ForceList);
        if (DeviceNode->ResourceList == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        IopArbiterReserveResourceList(DeviceNode->ResourceList,
                                      DeviceNode->PhysicalDeviceObject);

        Status = IopTranslateResourceList(DeviceNode,
                                          RequestSource,
                                          DeviceNode->ResourceList,
                                          &DeviceNode->ResourceListTranslated);
        if (!NT_SUCCESS(Status))
        {
            IopArbiterReleaseResources(DeviceNode);
            ExFreePool(DeviceNode->ResourceList);
            DeviceNode->ResourceList = NULL;
            return Status;
        }
    }

    IopUpdateLegacyResourceMap(DeviceNode);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Updates the legacy claim of a driver and device pair. A NULL requirements
 * list frees the claim. Must be called with the resource assignment lock held.
 *
 * @param[out] ClaimOwner
 * Receives the owner of the claim, or NULL if the claim was freed.
 */
static
NTSTATUS
IopUpdateLegacyClaim(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PCUNICODE_STRING DriverClassName,
    _In_opt_ PIO_RESOURCE_REQUIREMENTS_LIST Requirements,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_opt_ PCM_RESOURCE_LIST ForceList,
    _Out_ PBOOLEAN ConflictDetected,
    _Out_ PIOP_LEGACY_RESOURCE_OWNER *ClaimOwner)
{
    PIOP_LEGACY_RESOURCE_OWNER Owner = IopFindLegacyResourceOwner(DriverObject, DeviceObject);

    *ClaimOwner = NULL;

    if (Requirements == NULL)
    {
        if (Owner != NULL)
            IopDestroyLegacyResourceOwner(Owner);

        return STATUS_SUCCESS;
    }

    if (Owner == NULL)
    {
        Owner = IopCreateLegacyResourceOwner(DriverObject, DeviceObject, DriverClassName);
        if (Owner == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    *ClaimOwner = Owner;

    return IopLegacyClaim(Owner, Requirements, RequestSource, ForceList, ConflictDetected);
}

/**
 * @brief
 * Assigns the resources a legacy driver requests with IoAssignResources.
 *
 * @param[in] Requirements
 * The requested resources, or NULL to free the previous claim.
 *
 * @param[out] AllocatedResources
 * If not NULL, receives a copy of the assigned resources. The caller frees it.
 */
NTSTATUS
NTAPI
IopLegacyAssignResources(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PIO_RESOURCE_REQUIREMENTS_LIST Requirements,
    _Out_opt_ PCM_RESOURCE_LIST *AllocatedResources)
{
    PIOP_LEGACY_RESOURCE_OWNER Owner;
    BOOLEAN Conflict;
    NTSTATUS Status;

    PAGED_CODE();

    if (AllocatedResources != NULL)
        *AllocatedResources = NULL;

    if (!IopResourceAssignmentReady)
        return STATUS_UNSUCCESSFUL;

    IopLockResourceAssignment();

    Status = IopUpdateLegacyClaim(DriverObject,
                                  DeviceObject,
                                  NULL,
                                  Requirements,
                                  ArbiterRequestLegacyAssigned,
                                  NULL,
                                  &Conflict,
                                  &Owner);

    if (NT_SUCCESS(Status) &&
        AllocatedResources != NULL &&
        Owner != NULL &&
        Owner->DeviceNode->ResourceList != NULL)
    {
        *AllocatedResources = IopDuplicateResourceList(Owner->DeviceNode->ResourceList);
        if (*AllocatedResources == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    IopUnlockResourceAssignment();

    return Status;
}

/**
 * @brief
 * Claims the resources a legacy driver reports with IoReportResourceUsage or
 * IoReportResourceForDetection.
 *
 * @param[in] ResourceList
 * The resources in use, or NULL to free the previous claim.
 *
 * @param[in] OverrideConflict
 * TRUE to claim the resources even if they conflict.
 *
 * @param[out] ConflictDetected
 * Set to TRUE if the resources conflict with assigned resources.
 */
NTSTATUS
NTAPI
IopLegacyReportResources(
    _In_opt_ PCUNICODE_STRING DriverClassName,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PCM_RESOURCE_LIST ResourceList,
    _In_ BOOLEAN OverrideConflict,
    _Out_ PBOOLEAN ConflictDetected)
{
    PIOP_LEGACY_RESOURCE_OWNER Owner;
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements;
    PDEVICE_NODE PnpNode = NULL;
    NTSTATUS Status;

    PAGED_CODE();

    *ConflictDetected = FALSE;

    if (!IopResourceAssignmentReady)
        return STATUS_UNSUCCESSFUL;

    /* The resources a PnP device already has are not claimed again */
    if (DeviceObject != NULL)
    {
        PnpNode = IopGetDeviceNode(DeviceObject);
        if (PnpNode != NULL && (PnpNode->Flags & DNF_LEGACY_RESOURCE_DEVICENODE))
            PnpNode = NULL;
    }

    IopLockResourceAssignment();

    Requirements = IopArbiterCmListToRequirements(ResourceList, PnpNode);

    Status = IopUpdateLegacyClaim(DriverObject,
                                  DeviceObject,
                                  DriverClassName,
                                  Requirements,
                                  ArbiterRequestLegacyReported,
                                  OverrideConflict ? ResourceList : NULL,
                                  ConflictDetected,
                                  &Owner);

    if (Requirements != NULL)
        ExFreePoolWithTag(Requirements, TAG_IO_ARBITER);

    IopUnlockResourceAssignment();

    return Status;
}

/* Frees all legacy claims of a driver that is unloaded */
VOID
NTAPI
IopReleaseLegacyResources(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PLIST_ENTRY ListEntry;

    PAGED_CODE();

    if (!IopResourceAssignmentReady)
        return;

    IopLockResourceAssignment();

    ListEntry = IopLegacyResourceOwnerList.Flink;
    while (ListEntry != &IopLegacyResourceOwnerList)
    {
        PIOP_LEGACY_RESOURCE_OWNER Owner =
            CONTAINING_RECORD(ListEntry, IOP_LEGACY_RESOURCE_OWNER, ListEntry);

        ListEntry = ListEntry->Flink;

        if (Owner->DriverObject == DriverObject)
            IopDestroyLegacyResourceOwner(Owner);
    }

    IopUnlockResourceAssignment();
}

/* CONFLICT QUERIES *********************************************************/

/**
 * @brief
 * Adds assigned resources to the root arbiters as allocated ranges of Owner.
 * Used for resources that were not assigned by the arbiters.
 */
VOID
NTAPI
IopArbiterReserveResourceList(
    _In_ PCM_RESOURCE_LIST ResourceList,
    _In_opt_ PVOID Owner)
{
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    ULONG Index;

    PAGED_CODE();

    if (ResourceList == NULL || ResourceList->Count == 0)
        return;

    PartialList = &ResourceList->List[0].PartialResourceList;

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(Cm->Type);
        PARBITER_INSTANCE Arbiter;
        ULONGLONG Start = 0;
        ULONGLONG Length = 0;

        if (Interface == NULL || Interface->Context == NULL)
            continue;

        Arbiter = (PARBITER_INSTANCE)Interface->Context;
        if (Arbiter->UnpackResource == NULL)
            continue;

        Arbiter->UnpackResource(Cm, &Start, &Length);
        if (Length == 0)
            continue;

        ArbiterLibReserveRange(Arbiter,
                               Start,
                               Start + Length - 1,
                               Owner,
                               Cm->ShareDisposition == CmResourceShareShared);
    }
}

/**
 * @brief
 * Returns a copy of Request that describes the committed range which
 * conflicts with it.
 */
static
VOID
IopDescribeConflictingRange(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Request,
    _In_ PRTL_RANGE Range,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conflicting)
{
    ULONG Extent = (ULONG)min(Range->End - Range->Start + 1, MAXULONG);

    *Conflicting = *Request;

    switch (Request->Type)
    {
        case CmResourceTypePort:
            Conflicting->u.Port.Start.QuadPart = (LONGLONG)Range->Start;
            Conflicting->u.Port.Length = Extent;
            break;

        case CmResourceTypeMemory:
            Conflicting->u.Memory.Start.QuadPart = (LONGLONG)Range->Start;
            Conflicting->u.Memory.Length = Extent;
            break;

        case CmResourceTypeInterrupt:
            Conflicting->u.Interrupt.Vector = (ULONG)Range->Start;
            break;

        case CmResourceTypeDma:
            Conflicting->u.Dma.Channel = (ULONG)Range->Start;
            break;

        case CmResourceTypeBusNumber:
            Conflicting->u.BusNumber.Start = (ULONG)Range->Start;
            Conflicting->u.BusNumber.Length = Extent;
            break;

        default:
            break;
    }
}

/* Checks if a committed range may overlap a requested descriptor */
static
BOOLEAN
IopArbiterOverlapAllowed(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm,
    _In_ PRTL_RANGE Range,
    _In_opt_ PVOID RootPdo)
{
    /* Both are shared */
    if (Cm->ShareDisposition == CmResourceShareShared && (Range->Flags & RTL_RANGE_SHARED))
        return TRUE;

    /* System resources, shared with the devices that use the same hardware */
    if (RootPdo != NULL && Range->Owner == RootPdo)
        return TRUE;

    return !!(Range->Attributes & (ARBITER_RANGE_BOOT_ALLOCATED | ARBITER_RANGE_SHARED_DRIVER));
}

/**
 * @brief
 * Checks if a resource list conflicts with ranges committed in the root
 * arbiters.
 *
 * @param[out] ConflictingDescriptor
 * If not NULL, receives the first conflicting range.
 *
 * @return
 * TRUE if a descriptor conflicts.
 */
BOOLEAN
NTAPI
IopArbiterResourceConflict(
    _In_ PCM_RESOURCE_LIST ResourceList,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    PVOID RootPdo;
    ULONG Index;

    PAGED_CODE();

    if (ResourceList == NULL || ResourceList->Count == 0)
        return FALSE;

    RootPdo = (IopRootDeviceNode != NULL) ? IopRootDeviceNode->PhysicalDeviceObject : NULL;
    PartialList = &ResourceList->List[0].PartialResourceList;

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(Cm->Type);
        PARBITER_INSTANCE Arbiter;
        RTL_RANGE_LIST_ITERATOR Iterator;
        PRTL_RANGE Range;
        ULONGLONG Start = 0;
        ULONGLONG Length = 0;
        ULONGLONG End;

        if (Interface == NULL || Interface->Context == NULL)
            continue;

        Arbiter = (PARBITER_INSTANCE)Interface->Context;
        if (Arbiter->UnpackResource == NULL)
            continue;

        Arbiter->UnpackResource(Cm, &Start, &Length);
        if (Length == 0)
            continue;

        End = Start + Length - 1;

        if (!NT_SUCCESS(RtlGetFirstRange(Arbiter->Allocation, &Iterator, &Range)))
            continue;

        while (Range != NULL)
        {
            if (Range->Start <= End && Range->End >= Start &&
                !IopArbiterOverlapAllowed(Cm, Range, RootPdo))
            {
                if (ConflictingDescriptor != NULL)
                    IopDescribeConflictingRange(Cm, Range, ConflictingDescriptor);

                return TRUE;
            }

            if (!NT_SUCCESS(RtlGetNextRange(&Iterator, &Range, TRUE)))
                break;
        }
    }

    return FALSE;
}

/**
 * @brief
 * Checks if the resources of a device conflict with resources of other
 * devices, using the QueryConflict action of the arbiters. The ranges of the
 * device itself are not counted.
 *
 * @param[out] ConflictingDescriptor
 * If not NULL, receives the first conflicting descriptor.
 *
 * @return
 * TRUE if a descriptor conflicts.
 */
BOOLEAN
NTAPI
IopArbiterQueryConflict(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ PCM_RESOURCE_LIST ResourceList,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR ConflictingDescriptor)
{
    PDEVICE_NODE DeviceNode;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    INTERFACE_TYPE InterfaceType;
    ULONG Index;

    PAGED_CODE();

    if (ResourceList == NULL || ResourceList->Count == 0 || PhysicalDeviceObject == NULL)
        return FALSE;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    PartialList = &ResourceList->List[0].PartialResourceList;
    InterfaceType = IopResourceInterface(ResourceList->List[0].InterfaceType);

    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_CONFLICT_INFO Conflicts = NULL;
        PARBITER_INTERFACE Interface = NULL;
        ARBITER_PARAMETERS Parameters;
        IO_RESOURCE_DESCRIPTOR IoDescriptor;
        IOP_REQUIREMENT Requirement;
        ULONG ConflictCount = 0;
        NTSTATUS Status;

        /* Forwarding windows are not checked */
        if (!IopArbiterCmToFixedRequirement(Cm, FALSE, &IoDescriptor))
            continue;

        /* The arbiter is asked about the resource as its requirements reach it */
        IopInitializeRequirement(&Requirement,
                                 PhysicalDeviceObject,
                                 ArbiterRequestPnpEnumerated,
                                 InterfaceType,
                                 ResourceList->List[0].BusNumber,
                                 &IoDescriptor,
                                 1);

        if ((DeviceNode != NULL) &&
            NT_SUCCESS(IopFindRequirementHandlers(&Requirement, InterfaceType)))
        {
            Interface = Requirement.Arbiter->ArbiterInterface;
        }
        else
        {
            IopFreeRequirementLevels(&Requirement);
            Interface = IopGetRootArbiterInterface(Cm->Type);
        }

        if (Interface == NULL || Interface->Context == NULL)
        {
            IopFreeRequirementLevels(&Requirement);
            continue;
        }

        RtlZeroMemory(&Parameters, sizeof(Parameters));
        Parameters.Parameters.QueryConflict.PhysicalDeviceObject = PhysicalDeviceObject;
        Parameters.Parameters.QueryConflict.ConflictingResource =
            &Requirement.Top->Entry.Alternatives[0];
        Parameters.Parameters.QueryConflict.ConflictCount = &ConflictCount;
        Parameters.Parameters.QueryConflict.Conflicts = &Conflicts;

        Status = Interface->ArbiterHandler(Interface->Context,
                                           ArbiterActionQueryConflict,
                                           &Parameters);

        IopFreeRequirementLevels(&Requirement);

        if (Conflicts != NULL)
            ExFreePool(Conflicts);

        if (NT_SUCCESS(Status) && ConflictCount != 0)
        {
            if (ConflictingDescriptor != NULL)
                *ConflictingDescriptor = *Cm;

            return TRUE;
        }
    }

    return FALSE;
}
