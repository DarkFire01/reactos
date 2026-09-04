/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     PnP manager resource arbitration and assignment
 * COPYRIGHT:   Copyright 2005 Cameron Gutman <cameron.gutman@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Every device gets its resources through a resource arbiter. An arbiter is
 * owned by a bus, specifically the ancestor that decodes the resource, and is
 * published through IRP_MN_QUERY_INTERFACE for GUID_ARBITER_INTERFACE_STANDARD
 * with the resource type passed in InterfaceSpecificData. When no bus in the
 * parent chain arbitrates a given type, the five root arbiters (Port, Memory,
 * Dma, Interrupt and BusNumber) pre-registered on the root device node act as
 * the terminal fallback.
 *
 * This file holds the discovery half of that machinery: publishing the root
 * arbiters, and walking a device's ancestry to find the arbiter for a resource
 * type while caching the answer on the providing node through
 * DeviceArbiterList together with QueryArbiterMask and NoArbiterMask.
 *
 * It also holds the registry mirror of the assignment results, under
 * RESOURCEMAP and the device's AllocConfig value.
 */

#include <ntoskrnl.h>
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define TAG_IO_ARBITER 'AbrI'

/* The five root arbiter instances, initialized by IopInitializeArbiters. */
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

/* One published ARBITER_INTERFACE per root arbiter, in the table's order. */
static ARBITER_INTERFACE IopRootArbiterInterface[RTL_NUMBER_OF(IopRootArbiterTable)];

/*
 * A cached arbiter entry. An arbiter discovered by querying a bus has its
 * ARBITER_INTERFACE copied out of the transient IRP buffer into the Interface
 * member, so that the cached PI_RESOURCE_ARBITER_ENTRY has stable storage to
 * point at. Root entries point at IopRootArbiterInterface instead and leave
 * Interface unused. PI_RESOURCE_ARBITER_ENTRY is the first member, so a
 * DeviceArbiterList link resolves to the whole IOP_ARBITER_ENTRY.
 */
typedef struct _IOP_ARBITER_ENTRY
{
    PI_RESOURCE_ARBITER_ENTRY Entry;
    ARBITER_INTERFACE Interface;
} IOP_ARBITER_ENTRY, *PIOP_ARBITER_ENTRY;
/*
 * Seeding the root arbiters from \HARDWARE\RESOURCEMAP reserves the fixed
 * firmware, HAL and loader resources. It is on by default; clearing it, for
 * instance from the boot debugger before the PnP phase runs, falls back to
 * unseeded arbiters should the seeding prove to over-reserve on a machine.
 */
BOOLEAN IopArbiterSeedResourceMap = TRUE;

/* Defined below; seeds the root arbiters from the firmware resource map */
CODE_SEG("INIT") static VOID IopArbiterSeedFromResourceMap(VOID);

/*
 * Boot configuration reservation. Enumeration reserves a device's firmware
 * configuration through IopReserveBootConfigRoutine. Until the boot drivers are
 * up that is IopQueueBootConfig, which puts root-enumerated devices on
 * IopDeferredBootConfigList; IopReserveDeferredBootConfigs then reserves the
 * queued entries, switches the routine to IopReserveBootConfig and sets
 * IopDeferredBootConfigsReserved.
 */
typedef struct _IOP_DEFERRED_BOOT_CONFIG
{
    LIST_ENTRY ListEntry;
    /* Referenced, and NULL for a system reservation */
    PDEVICE_OBJECT DeviceObject;
    /* A private copy, held only when DeviceObject is NULL */
    PCM_RESOURCE_LIST ResourceList;
} IOP_DEFERRED_BOOT_CONFIG, *PIOP_DEFERRED_BOOT_CONFIG;

static LIST_ENTRY IopDeferredBootConfigList;
BOOLEAN IopDeferredBootConfigsReserved = FALSE;
PIOP_RESERVE_BOOT_CONFIG_ROUTINE IopReserveBootConfigRoutine = IopQueueBootConfig;

/*
 * Serializes every arbiter transaction. The PnP state machine and the legacy
 * resource APIs run on different threads, and one caller's test and commit pair
 * on an arbiter must not interleave with another's.
 */
static ERESOURCE IopResourceAssignmentLock;

/*
 * Decoders for the port and memory descriptor forms, from sdk/lib/rtl/memres.c.
 * They are not in the NDK headers, so they are declared where they are used.
 */
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

/*
 * The root arbiters live for the life of the system, so their interfaces are
 * static and their reference counting callbacks have nothing to do.
 */
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

/**
 * @brief
 * Returns the published interface of the root arbiter for a resource type.
 *
 * @param[in] ResourceType
 * The CmResourceType to look up. CmResourceTypeMemoryLarge resolves to the
 * Memory arbiter.
 *
 * @return
 * The static root interface, or NULL for a type that has no root arbiter.
 */
CODE_SEG("PAGE")
static
PARBITER_INTERFACE
IopGetRootArbiterInterface(
    _In_ UCHAR ResourceType)
{
    ULONG Index;

    PAGED_CODE();

    for (Index = 0; Index < RTL_NUMBER_OF(IopRootArbiterTable); Index++)
    {
        if (IopRootArbiterTable[Index].ResourceType == ResourceType ||
            (IopRootArbiterTable[Index].ResourceType == CmResourceTypeMemory &&
             ResourceType == CmResourceTypeMemoryLarge))
        {
            return &IopRootArbiterInterface[Index];
        }
    }

    return NULL;
}

/**
 * @brief
 * Returns the cached arbiter entry of a given resource type on a device node.
 *
 * @param[in] Node
 * The device node whose DeviceArbiterList is searched.
 *
 * @param[in] ResourceType
 * The CmResourceType the arbiter must own.
 *
 * @return
 * The cached PI_RESOURCE_ARBITER_ENTRY, or NULL if this node has none for the
 * type.
 */
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
 * Asks a bus PDO for the arbiter of a resource type, through
 * IRP_MN_QUERY_INTERFACE for GUID_ARBITER_INTERFACE_STANDARD.
 *
 * @param[in] Pdo
 * The bus's physical device object to query.
 *
 * @param[in] ResourceType
 * The CmResourceType, passed as InterfaceSpecificData.
 *
 * @param[out] Interface
 * Receives the bus's ARBITER_INTERFACE on success.
 *
 * @return
 * The IRP completion status. A bus that does not arbitrate the type fails the
 * IRP, leaving it at the STATUS_NOT_SUPPORTED default.
 *
 * @remarks
 * GUID_ARBITER_INTERFACE_STANDARD is version 0, as the interface has never
 * been revised. pci.sys advertises its per-bus arbiters with MinVersion and
 * MaxVersion both zero and rejects anything else, so the query has to ask for
 * exactly 0. Asking for 1 makes pci.sys decline, every PCI device then falls
 * through to the root arbiter, and those grants never reach pci.sys's own
 * allocation. PciTranslateBusAddress goes on to reject the now unowned range,
 * so HalTranslateBusAddress fails for every PCI BAR.
 */
static
NTSTATUS
IopQueryArbiterInterface(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ UCHAR ResourceType,
    _Out_ PARBITER_INTERFACE Interface)
{
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STACK_LOCATION Stack;

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryInterface.Size = sizeof(ARBITER_INTERFACE);
    Stack.Parameters.QueryInterface.Version = 0;
    Stack.Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    Stack.Parameters.QueryInterface.InterfaceType = &GUID_ARBITER_INTERFACE_STANDARD;
    Stack.Parameters.QueryInterface.InterfaceSpecificData = (PVOID)(ULONG_PTR)ResourceType;

    return IopInitiatePnpIrp(Pdo, &IoStatusBlock, IRP_MN_QUERY_INTERFACE, &Stack);
}

/**
 * @brief
 * Publishes the five root arbiters on the root device node, so that the
 * ancestry walk always terminates with a fallback arbiter for every resource
 * type.
 *
 * @param[in] RootNode
 * The root device node, IopRootDeviceNode.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES if an entry allocation
 * fails.
 *
 * @remarks
 * Called once from IopInitializePlugPlayServices, after the root node exists
 * and after IopInitializeArbiters has initialized the instances themselves.
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

        Entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), TAG_IO_ARBITER);
        if (Entry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(Entry, sizeof(*Entry));
        Entry->ResourceType = IopRootArbiterTable[Index].ResourceType;
        Entry->ArbiterInterface = Interface;
        InitializeListHead(&Entry->ResourceList);
        InitializeListHead(&Entry->BestResourceList);
        InitializeListHead(&Entry->BestConfig);
        InitializeListHead(&Entry->ActiveArbiterList);

        InsertTailList(&RootNode->DeviceArbiterList, &Entry->DeviceArbiterList);
    }

    ExInitializeResourceLite(&IopResourceAssignmentLock);
    InitializeListHead(&IopDeferredBootConfigList);

    if (IopArbiterSeedResourceMap)
        IopArbiterSeedFromResourceMap();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Answers the root PDO's IRP_MN_QUERY_INTERFACE for
 * GUID_ARBITER_INTERFACE_STANDARD with the root arbiter of the requested
 * resource type.
 *
 * @param[in] IoStack
 * The QUERY_INTERFACE stack location. InterfaceSpecificData carries the
 * CmResourceType.
 *
 * @param[in] ExistingStatus
 * The IRP's current status, returned unchanged when the query is declined, so
 * that a declined IRP is left exactly as it was found.
 *
 * @return
 * STATUS_SUCCESS with the interface copied into the caller's buffer, or
 * ExistingStatus when the query is not for this interface, names an unknown
 * resource type, asks for a version other than 0, or supplies too small a
 * buffer.
 *
 * @remarks
 * This makes the root just another bus in the discovery walk, with no special
 * case for the root: the same query that discovers a bus driver's arbiters
 * discovers the root arbiters once it reaches the root PDO. The cache on the
 * root devnode normally answers first, so this handler is the backstop for
 * queriers that arrive through the IRP path. A version other than 0 is
 * declined, matching how pci.sys treats the never revised ARBITER interface.
 */
NTSTATUS
NTAPI
IopArbiterQueryRootInterface(
    _In_ PIO_STACK_LOCATION IoStack,
    _In_ NTSTATUS ExistingStatus)
{
    PARBITER_INTERFACE Published;
    PARBITER_INTERFACE Out;
    UCHAR ResourceType;

    PAGED_CODE();

    /* Not our interface, so leave the IRP untouched */
    if (!IsEqualGUID(IoStack->Parameters.QueryInterface.InterfaceType,
                     &GUID_ARBITER_INTERFACE_STANDARD))
    {
        return ExistingStatus;
    }

    /* Decline a wrong version or a short buffer */
    if (IoStack->Parameters.QueryInterface.Version != 0 ||
        IoStack->Parameters.QueryInterface.Size < sizeof(ARBITER_INTERFACE) ||
        IoStack->Parameters.QueryInterface.Interface == NULL)
    {
        return ExistingStatus;
    }

    ResourceType = (UCHAR)(ULONG_PTR)IoStack->Parameters.QueryInterface.InterfaceSpecificData;

    /* No root arbiter for the type, or registration has not run yet */
    Published = IopGetRootArbiterInterface(ResourceType);
    if (Published == NULL || Published->ArbiterHandler == NULL)
        return ExistingStatus;

    Out = (PARBITER_INTERFACE)IoStack->Parameters.QueryInterface.Interface;
    RtlCopyMemory(Out, Published, sizeof(ARBITER_INTERFACE));

    /*
     * The provider references the interface before returning it. That is a
     * no-op for the immortal root arbiters, but the protocol requires the call.
     */
    Out->InterfaceReference(Out->Context);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finds the arbiter that owns a resource type for a device, by walking the
 * device's ancestry.
 *
 * @param[in] DeviceNode
 * The device whose resource is being arbitrated.
 *
 * @param[in] ResourceType
 * The CmResourceType to find an arbiter for.
 *
 * @param[out] ArbiterInterface
 * Receives the owning arbiter's interface.
 *
 * @return
 * STATUS_SUCCESS with the interface, STATUS_INSUFFICIENT_RESOURCES if caching
 * an answer fails, or STATUS_NOT_FOUND if no arbiter exists for the type,
 * which is only reachable before the root arbiters are registered.
 *
 * @remarks
 * Arbitration is always provided by an ancestor bus and never by the device
 * itself, so the walk starts at the parent. A cached entry wins outright;
 * otherwise each bus's PDO is queried exactly once, tracked by
 * QueryArbiterMask and NoArbiterMask, and the answer is cached on the node
 * that provided it. The walk terminates at the root node, whose arbiters cover
 * every resource type.
 */
NTSTATUS
NTAPI
IopFindArbiterForResourceType(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PARBITER_INTERFACE *ArbiterInterface)
{
    PDEVICE_NODE Node;
    USHORT Mask;

    PAGED_CODE();

    *ArbiterInterface = NULL;

    ASSERT(ResourceType < RTL_FIELD_SIZE(DEVICE_NODE, NoArbiterMask) * 8);
    Mask = (USHORT)(1 << ResourceType);

    for (Node = DeviceNode->Parent; Node != NULL; Node = Node->Parent)
    {
        PPI_RESOURCE_ARBITER_ENTRY Cached = IopFindArbiterEntry(Node, ResourceType);
        PIOP_ARBITER_ENTRY NewEntry;
        ARBITER_INTERFACE Queried;

        if (Cached != NULL)
        {
            *ArbiterInterface = Cached->ArbiterInterface;
            return STATUS_SUCCESS;
        }

        /* This bus is already known not to arbitrate the type, so keep walking */
        if (Node->NoArbiterMask & Mask)
            continue;

        /* Query this bus for the arbiter exactly once */
        Node->QueryArbiterMask |= Mask;

        /*
         * A bus that does not arbitrate this type leaves the IRP at its
         * STATUS_NOT_SUPPORTED default, so failure is the ordinary answer.
         * Zero the buffer and insist on a handler even on success, so that a
         * driver reporting success without filling the interface in cannot be
         * cached and later called.
         */
        RtlZeroMemory(&Queried, sizeof(Queried));

        if (Node->PhysicalDeviceObject == NULL ||
            !NT_SUCCESS(IopQueryArbiterInterface(Node->PhysicalDeviceObject,
                                                 ResourceType,
                                                 &Queried)) ||
            Queried.ArbiterHandler == NULL)
        {
            /* Nothing here, so do not ask this bus again */
            Node->NoArbiterMask |= Mask;
            continue;
        }

        NewEntry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewEntry), TAG_IO_ARBITER);
        if (NewEntry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(NewEntry, sizeof(*NewEntry));
        NewEntry->Interface = Queried;
        NewEntry->Entry.ResourceType = ResourceType;
        NewEntry->Entry.ArbiterInterface = &NewEntry->Interface;
        InitializeListHead(&NewEntry->Entry.ResourceList);
        InitializeListHead(&NewEntry->Entry.BestResourceList);
        InitializeListHead(&NewEntry->Entry.BestConfig);
        InitializeListHead(&NewEntry->Entry.ActiveArbiterList);

        InsertTailList(&Node->DeviceArbiterList, &NewEntry->Entry.DeviceArbiterList);

        /*
         * Which ancestor bus arbitrates a resource matters for a device behind
         * a PCI to PCI bridge: a memory BAR has to resolve to the bridge's
         * arbiter, so that the child is carved out of the bridge's forwarding
         * window. Falling through to the root arbiter instead lets the child
         * occupy root space, and the bridge's own window can then no longer be
         * placed.
         */
        DPRINT("Device %wZ resource type %u arbitrated by ancestor %wZ\n",
               &DeviceNode->InstancePath, ResourceType, &Node->InstancePath);

        *ArbiterInterface = &NewEntry->Interface;
        return STATUS_SUCCESS;
    }

    /*
     * No bus in the ancestry arbitrates this type, so fall back to the root
     * arbiter, which owns every type. This also covers a device whose parent
     * chain does not reach the root node, for instance one that is not yet
     * fully linked: the published interface is static, so it is available as
     * soon as the root arbiters have been registered.
     */
    *ArbiterInterface = IopGetRootArbiterInterface(ResourceType);
    if (*ArbiterInterface != NULL && (*ArbiterInterface)->Context != NULL)
    {
        DPRINT("Device %wZ resource type %u arbitrated by the root arbiter\n",
               &DeviceNode->InstancePath, ResourceType);
        return STATUS_SUCCESS;
    }

    DPRINT1("No arbiter for resource type %u\n", ResourceType);
    return STATUS_NOT_FOUND;
}

/* BOOT CONFIGURATION RESERVATION *******************************************/

/*
 * The resource types that have an arbiter, in the order they are arbitrated.
 * A CmResourceTypeMemoryLarge requirement is arbitrated by the Memory arbiter.
 */
static const UCHAR IopArbiterResourceTypes[] =
{
    CmResourceTypePort,
    CmResourceTypeInterrupt,
    CmResourceTypeMemory,
    CmResourceTypeDma,
    CmResourceTypeBusNumber,
};

/**
 * @brief
 * Reports whether a requirement of one resource type is handled by the arbiter
 * of another.
 *
 * @param[in] EntryType
 * The requirement descriptor's resource type.
 *
 * @param[in] ArbType
 * The arbiter's resource type.
 *
 * @return
 * TRUE if the arbiter owns the requirement. The Memory arbiter also owns the
 * 64-bit large memory requirements.
 */
static
BOOLEAN
IopArbiterTypeMatches(
    _In_ UCHAR EntryType,
    _In_ UCHAR ArbType)
{
    if (EntryType == ArbType)
        return TRUE;

    return (ArbType == CmResourceTypeMemory && EntryType == CmResourceTypeMemoryLarge);
}

/**
 * @brief
 * Invokes an arbiter action, wrapping the arbitration list into
 * ARBITER_PARAMETERS.
 *
 * @param[in] Interface
 * The arbiter to invoke.
 *
 * @param[in] Action
 * The ARBITER_ACTION to dispatch.
 *
 * @param[in] ArbitrationList
 * The list of ARBITER_LIST_ENTRY for the list bearing actions, or NULL for
 * Commit and Rollback.
 *
 * @return
 * The arbiter's completion status.
 *
 * @remarks
 * TestAllocation and BootAllocation read ArbitrationList from the first union
 * member, while Commit and Rollback ignore Parameters entirely. Every list
 * bearing parameter block shares the same leading ArbitrationList field, so one
 * assignment serves whichever action is dispatched.
 */
static
NTSTATUS
IopArbiterInvoke(
    _In_ PARBITER_INTERFACE Interface,
    _In_ ARBITER_ACTION Action,
    _In_opt_ PLIST_ENTRY ArbitrationList)
{
    ARBITER_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Parameters.TestAllocation.ArbitrationList = ArbitrationList;

    return Interface->ArbiterHandler(Interface->Context, Action, &Parameters);
}

/**
 * @brief
 * Turns an assigned CM descriptor into a requirement that can only be placed
 * where the CM descriptor already sits.
 *
 * @param[in] Cm
 * The assigned descriptor to convert.
 *
 * @param[in] AllowForwardingWindow
 * When FALSE, a bus forwarding window is rejected rather than converted.
 *
 * @param[out] Io
 * Receives the single placement requirement.
 *
 * @return
 * TRUE on success, FALSE for a resource type that has no arbiter or for a
 * rejected forwarding window.
 *
 * @remarks
 * Port and memory go through RtlIoEncodeMemIoResource, so that a
 * CmResourceTypeMemoryLarge descriptor keeps its full 64-bit length instead of
 * being dropped for want of a large form in the requirement.
 */
static
BOOLEAN
IopArbiterCmToFixedRequirement(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm,
    _In_ BOOLEAN AllowForwardingWindow,
    _Out_ PIO_RESOURCE_DESCRIPTOR Io)
{
    ULONGLONG Length;
    ULONGLONG Start;

    RtlZeroMemory(Io, sizeof(*Io));

    /* IO_RESOURCE_ALTERNATIVE is not set, so this is a required descriptor */
    Io->Option = 0;
    Io->Type = Cm->Type;
    Io->ShareDisposition = Cm->ShareDisposition;
    Io->Flags = Cm->Flags;

    if (!AllowForwardingWindow &&
        ((Cm->Type == CmResourceTypePort &&
          (Cm->Flags & CM_RESOURCE_PORT_WINDOW_DECODE)) ||
         ((Cm->Type == CmResourceTypeMemory || Cm->Type == CmResourceTypeMemoryLarge) &&
          (Cm->Flags & CM_RESOURCE_MEMORY_WINDOW_DECODE))))
    {
        return FALSE;
    }

    switch (Cm->Type)
    {
        case CmResourceTypePort:
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            Length = RtlCmDecodeMemIoResource(Cm, &Start);
            if (Length == 0)
                return FALSE;

            return NT_SUCCESS(RtlIoEncodeMemIoResource(Io,
                                                       Cm->Type,
                                                       Length,
                                                       1,
                                                       Start,
                                                       Start + Length - 1));

        case CmResourceTypeInterrupt:
            Io->u.Interrupt.MinimumVector = Cm->u.Interrupt.Vector;
            Io->u.Interrupt.MaximumVector = Cm->u.Interrupt.Vector;
            return TRUE;

        case CmResourceTypeDma:
            Io->u.Dma.MinimumChannel = Cm->u.Dma.Channel;
            Io->u.Dma.MaximumChannel = Cm->u.Dma.Channel;
            return TRUE;

        case CmResourceTypeBusNumber:
            Io->u.BusNumber.Length = Cm->u.BusNumber.Length;
            Io->u.BusNumber.MinBusNumber = Cm->u.BusNumber.Start;
            Io->u.BusNumber.MaxBusNumber =
                Cm->u.BusNumber.Start + Cm->u.BusNumber.Length - 1;
            return TRUE;

        default:
            /* No arbiter for this type */
            return FALSE;
    }
}

/**
 * @brief
 * Reserves every descriptor of one resource type from a partial list, through
 * the arbiter's BootAllocation action.
 *
 * @param[in] Interface
 * The owning arbiter's interface.
 *
 * @param[in] ResourceType
 * The resource type this pass reserves.
 *
 * @param[in] PartialList
 * The assigned resources to reserve from.
 *
 * @param[in] Owner
 * The requesting PDO, or NULL for firmware and system resources.
 *
 * @param[in] InterfaceType
 * The bus interface type of the resource list.
 *
 * @param[in] BusNumber
 * The bus number of the resource list.
 *
 * @param[in] AllowForwardingWindow
 * Passed through to IopArbiterCmToFixedRequirement.
 *
 * @return
 * STATUS_SUCCESS, as reservation is best effort, or
 * STATUS_INSUFFICIENT_RESOURCES on allocation failure.
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

    /* Count the descriptors this arbiter owns */
    for (Index = 0; Index < PartialList->Count; Index++)
    {
        if (IopArbiterTypeMatches(PartialList->PartialDescriptors[Index].Type, ResourceType))
            Count++;
    }

    if (Count == 0)
        return STATUS_SUCCESS;

    Entries = ExAllocatePoolWithTag(PagedPool,
                                    Count * sizeof(ARBITER_LIST_ENTRY),
                                    TAG_IO_ARBITER);
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

    RtlZeroMemory(Entries, Count * sizeof(ARBITER_LIST_ENTRY));
    InitializeListHead(&ArbitrationList);

    Count = 0;
    for (Index = 0; Index < PartialList->Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];

        if (!IopArbiterTypeMatches(Cm->Type, ResourceType))
            continue;

        if (!IopArbiterCmToFixedRequirement(Cm, AllowForwardingWindow, &Descriptors[Count]))
            continue;

        Entries[Count].AlternativeCount = 1;
        Entries[Count].Alternatives = &Descriptors[Count];
        Entries[Count].PhysicalDeviceObject = Owner;
        Entries[Count].RequestSource = ArbiterRequestPnpEnumerated;
        Entries[Count].Flags = ARBITER_FLAG_BOOT_CONFIG;
        Entries[Count].InterfaceType = InterfaceType;
        Entries[Count].BusNumber = BusNumber;
        Entries[Count].Result = ArbiterResultUndefined;

        InsertTailList(&ArbitrationList, &Entries[Count].ListEntry);
        Count++;
    }

    if (Count != 0)
        IopArbiterInvoke(Interface, ArbiterActionBootAllocation, &ArbitrationList);

    ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
    ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reserves a device's firmware boot configuration in the arbiters, so that
 * those ranges are respected while other devices are assigned, and handed back
 * to this device when it is assigned itself.
 *
 * @param[in] DeviceNode
 * The device whose BootResources are reserved.
 *
 * @return
 * STATUS_SUCCESS. Reservation is best effort, and descriptors without an
 * arbiter are skipped.
 *
 * @remarks
 * Every descriptor goes to its owning arbiter as an ArbiterActionBootAllocation
 * request, and the arbiter records the ranges as owned by the device and tagged
 * ARBITER_RANGE_BOOT_ALLOCATED. A per arbiter failure is not fatal, since the
 * real assignment is what enforces conflicts. DNF_BOOT_CONFIG_RESERVED records
 * that the configuration is on record.
 */
NTSTATUS
NTAPI
IopArbiterReserveBootConfig(
    _In_ PDEVICE_NODE DeviceNode)
{
    PCM_RESOURCE_LIST BootResources = DeviceNode->BootResources;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    ULONG TypeIndex;

    PAGED_CODE();

    if (BootResources == NULL || BootResources->Count == 0)
        return STATUS_SUCCESS;

    PartialList = &BootResources->List[0].PartialResourceList;

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface;
        BOOLEAN AllowForwardingWindow;

        if (!NT_SUCCESS(IopFindArbiterForResourceType(DeviceNode, ResourceType, &Interface)))
            continue;

        /*
         * A forwarding window is reserved only when a real ancestor bus arbiter
         * owns it, as for a PCI to PCI bridge consuming its window from the bus
         * above. The root fallback is different: the top PCI bus's window is its
         * own allocatable pool, not a range it occupies.
         * IopFindArbiterForResourceType hands back the shared root interface for
         * the fallback, so a mismatch means an ancestor bus answered.
         */
        AllowForwardingWindow = (Interface != IopGetRootArbiterInterface(ResourceType));

        IopArbiterReserveType(Interface,
                              ResourceType,
                              PartialList,
                              DeviceNode->PhysicalDeviceObject,
                              BootResources->List[0].InterfaceType,
                              BootResources->List[0].BusNumber,
                              AllowForwardingWindow);
    }

    IopDeviceNodeSetFlag(DeviceNode, DNF_BOOT_CONFIG_RESERVED);

    return STATUS_SUCCESS;
}

/* DEFERRED BOOT CONFIGURATION RESERVATION **********************************/

/**
 * @brief
 * Reserves a boot configuration in the arbiters right away.
 *
 * @param[in] RequestSource
 * Who is claiming. A bus reported boot configuration uses
 * ArbiterRequestPnpEnumerated.
 *
 * @param[in] DeviceObject
 * The PDO the boot configuration belongs to, whose device node supplies the
 * BootResources that get reserved. NULL reserves a system range that no device
 * owns.
 *
 * @param[in] BootResources
 * The configuration to reserve. For a device it is adopted as the node's
 * BootResources when the node has none yet; for a NULL DeviceObject it is
 * reserved as it stands and remains the caller's.
 *
 * @return
 * STATUS_SUCCESS, as reservation is best effort, STATUS_NO_SUCH_DEVICE if the
 * PDO has no device node, or STATUS_INSUFFICIENT_RESOURCES.
 *
 * @remarks
 * A configuration already on record, marked DNF_BOOT_CONFIG_RESERVED, is left
 * alone. A device assigned before the deferred list was drained has reserved
 * and claimed its configuration itself, so reserving it again would only
 * duplicate the ranges. IopReserveBootConfigRoutine points here once the boot
 * drivers are up.
 */
NTSTATUS
NTAPI
IopReserveBootConfig(
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ PCM_RESOURCE_LIST BootResources)
{
    PDEVICE_NODE DeviceNode;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(RequestSource);

    IopLockResourceAssignment();

    if (DeviceObject == NULL)
    {
        /*
         * The root PDO owns the range, so root-enumerated devices may still
         * share it.
         */
        PVOID Owner = (IopRootDeviceNode != NULL)
                      ? IopRootDeviceNode->PhysicalDeviceObject : NULL;
        ULONG TypeIndex;

        for (TypeIndex = 0;
             BootResources != NULL && BootResources->Count != 0 &&
             TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes);
             TypeIndex++)
        {
            UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
            PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(ResourceType);

            if (Interface != NULL)
            {
                IopArbiterReserveType(Interface,
                                      ResourceType,
                                      &BootResources->List[0].PartialResourceList,
                                      Owner,
                                      BootResources->List[0].InterfaceType,
                                      BootResources->List[0].BusNumber,
                                      FALSE);
            }
        }

        IopUnlockResourceAssignment();
        return STATUS_SUCCESS;
    }

    Status = STATUS_SUCCESS;
    DeviceNode = IopGetDeviceNode(DeviceObject);

    if (DeviceNode == NULL)
    {
        Status = STATUS_NO_SUCH_DEVICE;
    }
    else if (!(DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED))
    {
        if (DeviceNode->BootResources == NULL)
        {
            ULONG Size = PnpDetermineResourceListSize(BootResources);

            if (Size != 0)
            {
                DeviceNode->BootResources = ExAllocatePool(PagedPool, Size);
                if (DeviceNode->BootResources != NULL)
                    RtlCopyMemory(DeviceNode->BootResources, BootResources, Size);
                else
                    Status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }

        if (NT_SUCCESS(Status) && DeviceNode->BootResources != NULL)
            Status = IopArbiterReserveBootConfig(DeviceNode);
    }

    IopUnlockResourceAssignment();

    return Status;
}

/**
 * @brief
 * Reserves a boot configuration before the boot drivers are up: a bus
 * enumerated device's right away, and a root-enumerated device's once
 * IopReserveDeferredBootConfigs runs.
 *
 * @param[in] RequestSource
 * Who is claiming, as for IopReserveBootConfig.
 *
 * @param[in] DeviceObject
 * The PDO the configuration belongs to, or NULL for a system range.
 *
 * @param[in] BootResources
 * The configuration. For a NULL DeviceObject a private copy is queued, so the
 * caller stays free to release its own list.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INSUFFICIENT_RESOURCES if the deferral entry cannot be
 * allocated, or IopReserveBootConfig's status for an immediate reservation.
 *
 * @remarks
 * Root-enumerated devices, marked DNF_MADEUP, describe resources of the legacy
 * buses, whose drivers and translators only exist once the boot bus extenders
 * have started, so their reservations wait for that point. A queued PDO is
 * referenced, and since the entry only names it, a node removed before the
 * drain is skipped. IopReserveBootConfigRoutine starts out pointing here.
 */
NTSTATUS
NTAPI
IopQueueBootConfig(
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ PCM_RESOURCE_LIST BootResources)
{
    PIOP_DEFERRED_BOOT_CONFIG Deferred;
    PCM_RESOURCE_LIST Copy = NULL;
    ULONG Size;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    Size = PnpDetermineResourceListSize(BootResources);
    if (Size == 0)
        return STATUS_SUCCESS;

    IopLockResourceAssignment();

    if (DeviceObject != NULL)
    {
        PDEVICE_NODE DeviceNode = IopGetDeviceNode(DeviceObject);

        if (DeviceNode == NULL)
        {
            Status = STATUS_NO_SUCH_DEVICE;
            goto Done;
        }

        /* A bus enumerated device's arbiters exist by the time it is enumerated */
        if (!(DeviceNode->Flags & DNF_MADEUP))
        {
            Status = IopReserveBootConfig(RequestSource, DeviceObject, BootResources);
            goto Done;
        }

        if (DeviceNode->BootResources == NULL)
        {
            DeviceNode->BootResources = ExAllocatePool(PagedPool, Size);
            if (DeviceNode->BootResources == NULL)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }

            RtlCopyMemory(DeviceNode->BootResources, BootResources, Size);
        }
    }
    else
    {
        Copy = ExAllocatePoolWithTag(PagedPool, Size, TAG_IO_ARBITER);
        if (Copy == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }

        RtlCopyMemory(Copy, BootResources, Size);
    }

    Deferred = ExAllocatePoolWithTag(PagedPool, sizeof(*Deferred), TAG_IO_ARBITER);
    if (Deferred == NULL)
    {
        if (Copy != NULL)
            ExFreePoolWithTag(Copy, TAG_IO_ARBITER);

        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    if (DeviceObject != NULL)
        ObReferenceObject(DeviceObject);

    Deferred->DeviceObject = DeviceObject;
    Deferred->ResourceList = Copy;
    InsertTailList(&IopDeferredBootConfigList, &Deferred->ListEntry);

Done:
    IopUnlockResourceAssignment();

    return Status;
}

/**
 * @brief
 * Reserves the deferred boot configurations in the arbiters, switches
 * IopReserveBootConfigRoutine to IopReserveBootConfig and sets
 * IopDeferredBootConfigsReserved.
 *
 * @remarks
 * Called once from IopInitializeBootDrivers, after every boot driver has
 * started and before the first tree wide enumeration pass. It has to run before
 * that pass, because a device assigned in it could otherwise take a boot
 * device's firmware ranges and bugcheck 0x7B INACCESSIBLE_BOOT_DEVICE. Every
 * resource type resolves through the ancestry walk that ends at the root
 * arbiters, so the whole list is drained at once rather than per legacy bus.
 */
VOID
NTAPI
IopReserveDeferredBootConfigs(VOID)
{
    PAGED_CODE();

    IopLockResourceAssignment();

    while (!IsListEmpty(&IopDeferredBootConfigList))
    {
        PLIST_ENTRY ListEntry = RemoveHeadList(&IopDeferredBootConfigList);
        PIOP_DEFERRED_BOOT_CONFIG Deferred =
            CONTAINING_RECORD(ListEntry, IOP_DEFERRED_BOOT_CONFIG, ListEntry);

        if (Deferred->DeviceObject != NULL)
        {
            PDEVICE_NODE DeviceNode = IopGetDeviceNode(Deferred->DeviceObject);

            if (DeviceNode != NULL && DeviceNode->BootResources != NULL)
            {
                DPRINT("Reserving the deferred boot config of %wZ\n",
                       &DeviceNode->InstancePath);

                IopReserveBootConfig(ArbiterRequestPnpEnumerated,
                                     Deferred->DeviceObject,
                                     DeviceNode->BootResources);
            }

            ObDereferenceObject(Deferred->DeviceObject);
        }
        else if (Deferred->ResourceList != NULL)
        {
            IopReserveBootConfig(ArbiterRequestPnpEnumerated, NULL, Deferred->ResourceList);
            ExFreePoolWithTag(Deferred->ResourceList, TAG_IO_ARBITER);
        }

        ExFreePoolWithTag(Deferred, TAG_IO_ARBITER);
    }

    IopReserveBootConfigRoutine = IopReserveBootConfig;
    IopDeferredBootConfigsReserved = TRUE;

    IopUnlockResourceAssignment();
}

/* FIRMWARE RESOURCE MAP SEEDING ********************************************/

/**
 * @brief
 * Reserves one resource map list in the root arbiters.
 *
 * @param[in] ResourceList
 * The resource map list to seed.
 *
 * @remarks
 * Root PDO ownership makes the seeded hardware shareable with root-enumerated
 * devices. The driver-exclusive share path then lets another root-enumerated
 * device claim the same hardware, as the kernel debugger's ports need, and the
 * conflict check treats the root owned range as shareable.
 */
CODE_SEG("INIT")
static
VOID
IopArbiterSeedResourceList(
    _In_ PCM_RESOURCE_LIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    PVOID Owner;
    ULONG TypeIndex;

    if (ResourceList->Count == 0)
        return;

    PartialList = &ResourceList->List[0].PartialResourceList;
    Owner = (IopRootDeviceNode != NULL) ? IopRootDeviceNode->PhysicalDeviceObject : NULL;

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(ResourceType);

        if (Interface != NULL)
        {
            /*
             * Firmware and HAL resources owned by the root PDO seed the root
             * arbiter. A root bus forwarding window is the arbiter's own pool
             * rather than an occupied range, so it stays skipped.
             */
            IopArbiterReserveType(Interface,
                                  ResourceType,
                                  PartialList,
                                  Owner,
                                  ResourceList->List[0].InterfaceType,
                                  ResourceList->List[0].BusNumber,
                                  FALSE);
        }
    }
}

/**
 * @brief
 * Walks one resource map registry key, reserving every raw CM_RESOURCE_LIST
 * value and skipping the ".Translated" twins, then recursing into the subkeys.
 *
 * @param[in] Key
 * The open resource map key to walk.
 *
 * @param[in] Depth
 * The current recursion depth.
 *
 * @remarks
 * The resource map is shallow, RESOURCEMAP\<Class>\<Driver>, so a modest depth
 * bound guards against a malformed hive. The buffers are generous and oversized
 * entries are skipped, as the seeding is best effort.
 */
CODE_SEG("INIT")
static
VOID
IopArbiterSeedFromKey(
    _In_ HANDLE Key,
    _In_ ULONG Depth)
{
    UNICODE_STRING Translated = RTL_CONSTANT_STRING(L".Translated");
    PKEY_VALUE_FULL_INFORMATION ValueInfo;
    PKEY_BASIC_INFORMATION KeyInfo;
    ULONG Length;
    ULONG Index;
    NTSTATUS Status;

    if (Depth > 8)
        return;

    ValueInfo = ExAllocatePoolWithTag(PagedPool, 2 * PAGE_SIZE, TAG_IO_ARBITER);
    KeyInfo = ExAllocatePoolWithTag(PagedPool, 512, TAG_IO_ARBITER);
    if (ValueInfo == NULL || KeyInfo == NULL)
    {
        if (ValueInfo != NULL)
            ExFreePoolWithTag(ValueInfo, TAG_IO_ARBITER);
        if (KeyInfo != NULL)
            ExFreePoolWithTag(KeyInfo, TAG_IO_ARBITER);

        return;
    }

    /* Reserve every raw resource list value on this key */
    for (Index = 0; ; Index++)
    {
        UNICODE_STRING Name;

        Status = ZwEnumerateValueKey(Key,
                                     Index,
                                     KeyValueFullInformation,
                                     ValueInfo,
                                     2 * PAGE_SIZE,
                                     &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            break;

        /* Too large for the buffer, or transient */
        if (!NT_SUCCESS(Status))
            continue;

        if (ValueInfo->Type != REG_RESOURCE_LIST ||
            ValueInfo->DataLength < sizeof(CM_RESOURCE_LIST) ||
            ValueInfo->NameLength > MAXUSHORT)
        {
            continue;
        }

        Name.Buffer = ValueInfo->Name;
        Name.Length = (USHORT)ValueInfo->NameLength;
        Name.MaximumLength = Name.Length;

        /* The raw twin of a ".Translated" value carries the same ranges */
        if (RtlEqualUnicodeString(&Translated, &Name, TRUE))
            continue;

        IopArbiterSeedResourceList(
            (PCM_RESOURCE_LIST)((PUCHAR)ValueInfo + ValueInfo->DataOffset));
    }

    /* Recurse into the subkeys */
    for (Index = 0; ; Index++)
    {
        UNICODE_STRING SubName;
        OBJECT_ATTRIBUTES ObjectAttributes;
        HANDLE SubKey;

        Status = ZwEnumerateKey(Key, Index, KeyBasicInformation, KeyInfo, 512, &Length);
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
 * Seeds the root arbiters with the firmware, HAL and loader resources published
 * under \Registry\Machine\HARDWARE\RESOURCEMAP.
 *
 * @remarks
 * These are the fixed system resources that belong to no PnP device's boot
 * configuration, so without this the root arbiters could hand a flexible
 * requirement a range the HAL already owns. Run once at arbiter registration,
 * before any device is enumerated, so that the map holds only system resources
 * and never a device that is still to be assigned.
 */
CODE_SEG("INIT")
static
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

/**
 * @brief
 * Advances to the next alternative IO_RESOURCE_LIST in a variable length
 * requirements list.
 *
 * @param[in] List
 * The current alternative configuration.
 *
 * @return
 * The next alternative, immediately past this one's descriptor array.
 */
static
PIO_RESOURCE_LIST
IopArbiterNextList(
    _In_ PIO_RESOURCE_LIST List)
{
    return (PIO_RESOURCE_LIST)&List->Descriptors[List->Count];
}

/**
 * @brief
 * Reports whether two single placement requirements of the same type ask for
 * exactly the same range.
 *
 * @param[in] A
 * The first requirement.
 *
 * @param[in] B
 * The second requirement.
 *
 * @return
 * TRUE if the type and the placement window match.
 */
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
            return (A->u.Generic.Length == B->u.Generic.Length &&
                    A->u.Generic.MinimumAddress.QuadPart ==
                    B->u.Generic.MinimumAddress.QuadPart &&
                    A->u.Generic.MaximumAddress.QuadPart ==
                    B->u.Generic.MaximumAddress.QuadPart);

        case CmResourceTypeInterrupt:
            return (A->u.Interrupt.MinimumVector == B->u.Interrupt.MinimumVector &&
                    A->u.Interrupt.MaximumVector == B->u.Interrupt.MaximumVector);

        case CmResourceTypeDma:
            return (A->u.Dma.MinimumChannel == B->u.Dma.MinimumChannel &&
                    A->u.Dma.MaximumChannel == B->u.Dma.MaximumChannel);

        case CmResourceTypeBusNumber:
            return (A->u.BusNumber.Length == B->u.BusNumber.Length &&
                    A->u.BusNumber.MinBusNumber == B->u.BusNumber.MinBusNumber &&
                    A->u.BusNumber.MaxBusNumber == B->u.BusNumber.MaxBusNumber);

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Reports whether a requirement group asks for one of the device's own
 * firmware boot configuration placements.
 *
 * @param[in] DeviceNode
 * The device whose BootResources are consulted.
 *
 * @param[in] Entry
 * The requirement group, a lead alternative plus its followers.
 *
 * @return
 * TRUE if any alternative of the group is the exact fixed placement of a
 * descriptor of the device's boot configuration.
 *
 * @remarks
 * Bus drivers report the firmware placement as a fixed, usually preferred,
 * alternative of the requirement, so it is recognized by value against
 * BootResources.
 */
static
BOOLEAN
IopArbiterIsBootRequirement(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PARBITER_LIST_ENTRY Entry)
{
    PCM_RESOURCE_LIST BootResources = DeviceNode->BootResources;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    ULONG Alt;
    ULONG Index;

    if (!(DeviceNode->Flags & DNF_HAS_BOOT_CONFIG) ||
        BootResources == NULL || BootResources->Count == 0)
    {
        return FALSE;
    }

    PartialList = &BootResources->List[0].PartialResourceList;

    for (Alt = 0; Alt < Entry->AlternativeCount; Alt++)
    {
        PIO_RESOURCE_DESCRIPTOR Requirement = &Entry->Alternatives[Alt];

        for (Index = 0; Index < PartialList->Count; Index++)
        {
            IO_RESOURCE_DESCRIPTOR Fixed;

            if (!IopArbiterCmToFixedRequirement(&PartialList->PartialDescriptors[Index],
                                                TRUE,
                                                &Fixed))
            {
                continue;
            }

            if (IopArbiterSamePlacement(Requirement, &Fixed))
                return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief
 * Names whatever already holds the range an arbiter just refused.
 *
 * @param[in] Interface
 * The arbiter that refused. This has to be one of the kernel's own root
 * arbiters, because only for those is Context known to be an ARBITER_INSTANCE.
 * A bus driver's arbiter hands back an opaque Context in whatever shape that
 * driver keeps privately, and walking it as ours reads a garbage Allocation
 * pointer. The caller does the check, since the resource type is not known
 * here.
 *
 * @param[in] Start
 * The first address of the requested range, in untranslated arbiter units.
 *
 * @param[in] End
 * The last address of the requested range.
 *
 * @remarks
 * An "ExternalConflict" result says a range was taken but not by whom, and the
 * owner is the whole answer. A boot reserved range, whose attributes carry
 * ARBITER_RANGE_BOOT_ALLOCATED, is reclaimable in phase 1 by the device that
 * owns it, whereas a range owned by another device is a genuine collision that
 * needs a different fix. Printing Owner and Attributes tells those two apart
 * without another boot.
 *
 * This reads Allocation, the committed list. PossibleAllocation is scratch and
 * is only meaningful in the middle of a transaction.
 */
CODE_SEG("PAGE")
static
VOID
IopArbiterReportOccupants(
    _In_ PARBITER_INTERFACE Interface,
    _In_ ULONGLONG Start,
    _In_ ULONGLONG End)
{
    PARBITER_INSTANCE Arbiter;
    RTL_RANGE_LIST_ITERATOR Iterator;
    PRTL_RANGE Range;
    BOOLEAN Any = FALSE;

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
        if (Range->Start > End || Start > Range->End)
            continue;

        Any = TRUE;
        DPRINT1("      occupied by %I64x..%I64x owner %p attr 0x%x flags 0x%x%s\n",
                Range->Start, Range->End, Range->Owner,
                Range->Attributes, Range->Flags,
                (Range->Attributes & ARBITER_RANGE_BOOT_ALLOCATED) ? " BOOT_ALLOCATED" : "");
    }

    if (!Any)
    {
        DPRINT1("      nothing in Allocation overlaps, so the refusal is not an "
                "occupancy conflict\n");
    }
}

/**
 * @brief
 * Reports why an arbiter refused a device's requirements.
 *
 * @param[in] Interface
 * The arbiter that refused.
 *
 * @param[in] ResourceType
 * The CmResourceType being arbitrated when the refusal happened.
 *
 * @param[in] Phase
 * 0 when boot reserved ranges are off limits, 1 when this device's own firmware
 * configuration may reclaim them.
 *
 * @param[in] DeviceNode
 * The device whose configuration failed.
 *
 * @param[in] ArbitrationList
 * The entries handed to the arbiter, each still carrying its requirement
 * alternatives and its Result.
 *
 * @remarks
 * Without this an arbitration failure is silent: IopArbiterInvoke returns an
 * error and the caller rolls back, so the only evidence is that all N
 * configurations failed, which says nothing about which requirement could not
 * be placed or why. That is not enough to act on, and every diagnosis then
 * costs a boot. A refusal is rare and is exactly the event worth seeing, so
 * this prints unconditionally.
 *
 * Read it together with the arbiter's ordering and reserved lists under
 * HKLM\SYSTEM\CurrentControlSet\Control\Arbiters. A fixed requirement that
 * overlaps a reserved range can only be placed in phase 1, and only if it
 * matched the device's boot configuration.
 */
CODE_SEG("PAGE")
static
VOID
IopArbiterReportFailure(
    _In_ PARBITER_INTERFACE Interface,
    _In_ UCHAR ResourceType,
    _In_ ULONG Phase,
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PLIST_ENTRY ArbitrationList)
{
    PLIST_ENTRY Link;
    BOOLEAN OwnArbiter;

    PAGED_CODE();

    /*
     * Occupancy can only be read out of an arbiter whose bookkeeping is ours,
     * as a bus driver's Context is opaque. See IopArbiterReportOccupants.
     */
    OwnArbiter = (Interface == IopGetRootArbiterInterface(ResourceType));

    DPRINT1("Arbitration failed: type %u, phase %lu, device %wZ%s\n",
            ResourceType, Phase, &DeviceNode->InstancePath,
            OwnArbiter ? "" : " (external arbiter)");

    for (Link = ArbitrationList->Flink; Link != ArbitrationList; Link = Link->Flink)
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
                    DPRINT1("    [%lu] type %u opt 0x%x flags 0x%x len 0x%lx align 0x%lx "
                            "%I64x..%I64x%s\n",
                            Alt, Desc->Type, Desc->Option, Desc->Flags,
                            Desc->u.Generic.Length, Desc->u.Generic.Alignment,
                            Desc->u.Generic.MinimumAddress.QuadPart,
                            Desc->u.Generic.MaximumAddress.QuadPart,
                            (Desc->u.Generic.MaximumAddress.QuadPart -
                             Desc->u.Generic.MinimumAddress.QuadPart + 1 ==
                             Desc->u.Generic.Length) ? " FIXED" : "");

                    if (OwnArbiter)
                    {
                        IopArbiterReportOccupants(Interface,
                                                  Desc->u.Generic.MinimumAddress.QuadPart,
                                                  Desc->u.Generic.MaximumAddress.QuadPart);
                    }
                    break;

                case CmResourceTypeBusNumber:
                    DPRINT1("    [%lu] bus opt 0x%x len 0x%lx %lx..%lx\n",
                            Alt, Desc->Option, Desc->u.BusNumber.Length,
                            Desc->u.BusNumber.MinBusNumber, Desc->u.BusNumber.MaxBusNumber);
                    break;

                case CmResourceTypeInterrupt:
                    DPRINT1("    [%lu] irq opt 0x%x flags 0x%x %lx..%lx\n",
                            Alt, Desc->Option, Desc->Flags,
                            Desc->u.Interrupt.MinimumVector,
                            Desc->u.Interrupt.MaximumVector);
                    break;

                default:
                    DPRINT1("    [%lu] type %u opt 0x%x (not decoded)\n",
                            Alt, Desc->Type, Desc->Option);
                    break;
            }
        }
    }
}

/**
 * @brief
 * Tries to place one alternative configuration, a single IO_RESOURCE_LIST,
 * through the arbiters.
 *
 * @param[in] DeviceNode
 * The device being assigned resources.
 *
 * @param[in] RequirementsList
 * The full requirements list, for the bus identity fields.
 *
 * @param[in] Configuration
 * The alternative configuration to try.
 *
 * @param[in] Phase
 * 0 to leave every other device's boot reserved range alone, 1 to let the
 * requirements that are this device's own firmware configuration take boot
 * reserved ranges.
 *
 * @param[in] RequestSource
 * Who is asking. A legacy request may take boot reserved ranges.
 *
 * @param[out] ResourceList
 * On success, receives the packed CM_RESOURCE_LIST of assignments, or NULL for
 * an empty configuration.
 *
 * @return
 * STATUS_SUCCESS when every resource type was placed and committed,
 * STATUS_CONFLICTING_ADDRESSES or the arbiter's own failure status when any
 * type could not be placed, or STATUS_INSUFFICIENT_RESOURCES.
 *
 * @remarks
 * The descriptors are grouped into requirements, a lead descriptor plus its
 * IO_RESOURCE_ALTERNATIVE followers, one ARBITER_LIST_ENTRY each. They are
 * grouped by resource type and handed to the owning arbiter's TestAllocation.
 * If every type is placed the arbiters commit and the packed assignments become
 * the returned CM_RESOURCE_LIST; if any type fails, the arbiters already tested
 * roll back and the caller tries the next alternative.
 *
 * Phase 0 never takes a range that another device's boot configuration
 * reserved. Only when that yields no solution does phase 1 tag the requirements
 * matching this device's own boot configuration with ARBITER_FLAG_BOOT_CONFIG.
 * The port and memory arbiters then treat boot reserved ranges as available,
 * since the firmware put both devices there.
 */
static
NTSTATUS
IopArbiterTryConfiguration(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ PIO_RESOURCE_LIST Configuration,
    _In_ ULONG Phase,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _Out_ PCM_RESOURCE_LIST *ResourceList)
{
    PARBITER_LIST_ENTRY Entries;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Assignments;
    PARBITER_INTERFACE Tested[RTL_NUMBER_OF(IopArbiterResourceTypes)];
    ULONG TestedCount = 0;
    ULONG GroupCount = 0;
    ULONG Index;
    ULONG Which;
    ULONG TypeIndex;
    NTSTATUS Status = STATUS_CONFLICTING_ADDRESSES;
    PCM_RESOURCE_LIST CmList;
    ULONG CmListSize;

    /* Count the requirement groups, as each non-alternative descriptor starts one */
    for (Index = 0; Index < Configuration->Count; Index++)
    {
        if (!(Configuration->Descriptors[Index].Option & IO_RESOURCE_ALTERNATIVE))
            GroupCount++;
    }

    if (GroupCount == 0)
    {
        /* An empty but otherwise valid configuration, so nothing to arbitrate */
        *ResourceList = NULL;
        return STATUS_SUCCESS;
    }

    Entries = ExAllocatePoolWithTag(PagedPool,
                                    GroupCount * sizeof(ARBITER_LIST_ENTRY),
                                    TAG_IO_ARBITER);
    Assignments = ExAllocatePoolWithTag(PagedPool,
                                        GroupCount * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR),
                                        TAG_IO_ARBITER);
    if (Entries == NULL || Assignments == NULL)
    {
        if (Entries != NULL)
            ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
        if (Assignments != NULL)
            ExFreePoolWithTag(Assignments, TAG_IO_ARBITER);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Entries, GroupCount * sizeof(ARBITER_LIST_ENTRY));
    RtlZeroMemory(Assignments, GroupCount * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

    /* Build one ARBITER_LIST_ENTRY per requirement group */
    Which = 0;
    for (Index = 0; Index < Configuration->Count; )
    {
        PARBITER_LIST_ENTRY Entry = &Entries[Which];
        ULONG AlternativeCount = 1;

        /* The group runs from this lead descriptor across its alternatives */
        while (Index + AlternativeCount < Configuration->Count &&
               (Configuration->Descriptors[Index + AlternativeCount].Option &
                IO_RESOURCE_ALTERNATIVE))
        {
            AlternativeCount++;
        }

        Entry->AlternativeCount = AlternativeCount;
        Entry->Alternatives = &Configuration->Descriptors[Index];
        Entry->PhysicalDeviceObject = DeviceNode->PhysicalDeviceObject;
        Entry->RequestSource = RequestSource;
        Entry->Flags = 0;

        if (Phase != 0 && IopArbiterIsBootRequirement(DeviceNode, Entry))
            Entry->Flags |= ARBITER_FLAG_BOOT_CONFIG;

        Entry->InterfaceType = RequirementsList->InterfaceType;
        Entry->SlotNumber = RequirementsList->SlotNumber;
        Entry->BusNumber = RequirementsList->BusNumber;
        Entry->Assignment = &Assignments[Which];
        Entry->Result = ArbiterResultUndefined;

        Index += AlternativeCount;
        Which++;
    }

    /* Arbitrate one resource type at a time */
    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface;
        LIST_ENTRY ArbitrationList;
        ULONG InList = 0;

        InitializeListHead(&ArbitrationList);

        for (Which = 0; Which < GroupCount; Which++)
        {
            if (IopArbiterTypeMatches(Entries[Which].Alternatives[0].Type, ResourceType))
            {
                InsertTailList(&ArbitrationList, &Entries[Which].ListEntry);
                InList++;
            }
        }

        if (InList == 0)
            continue;

        Status = IopFindArbiterForResourceType(DeviceNode, ResourceType, &Interface);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("No arbiter for resource type %u (device %wZ)\n",
                    ResourceType, &DeviceNode->InstancePath);
            goto Rollback;
        }

        Status = IopArbiterInvoke(Interface, ArbiterActionTestAllocation, &ArbitrationList);
        if (!NT_SUCCESS(Status))
        {
            IopArbiterReportFailure(Interface, ResourceType, Phase, DeviceNode, &ArbitrationList);
            goto Rollback;
        }

        Tested[TestedCount++] = Interface;
    }

    /* Every type is placed, so make the tentative allocations permanent */
    for (Index = 0; Index < TestedCount; Index++)
        IopArbiterInvoke(Tested[Index], ArbiterActionCommitAllocation, NULL);

    /*
     * Pass through any requirement whose type no arbiter owns, whose Assignment
     * is therefore still zeroed, copying the descriptor identity from its
     * preferred alternative so that the output list carries a well formed
     * descriptor rather than a null one.
     *
     * This is a common path rather than a rare one. Every PCI root bridge and
     * PCI to PCI bridge carries a CmResourceTypeDevicePrivate marker after each
     * producer window, and pci.sys emits CmResourceTypeNull placeholders to keep
     * descriptor positions aligned with BAR indices. Both are expected on every
     * successful arbitration of every bridge.
     */
    for (Which = 0; Which < GroupCount; Which++)
    {
        PIO_RESOURCE_DESCRIPTOR Lead = &Entries[Which].Alternatives[0];
        BOOLEAN Arbitrated = FALSE;

        for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
        {
            if (IopArbiterTypeMatches(Lead->Type, IopArbiterResourceTypes[TypeIndex]))
            {
                Arbitrated = TRUE;
                break;
            }
        }

        if (Arbitrated)
            continue;

        /*
         * The expected types stay quiet. A PCIe host bridge (PNP0A08) can carry
         * twenty-odd producer windows, so one line per marker per bridge floods
         * the debug port on a machine with many bridges. Over a 115200 serial
         * link that is seconds of stall per device, it reads as a hang, and it
         * buries whatever the machine did next under a screenful of messages
         * that only mean everything is normal.
         *
         * An unexpected type is still worth shouting about, because it means a
         * requirement reached the assignment list that no arbiter claimed and
         * that is not known to be benign.
         */
        if (Lead->Type == CmResourceTypeDevicePrivate ||
            Lead->Type == CmResourceTypeNull)
        {
            DPRINT("Passing through non-arbitrated resource type %u for %wZ\n",
                   Lead->Type, &DeviceNode->InstancePath);
        }
        else
        {
            DPRINT1("Passing through unexpected non-arbitrated resource type %u for %wZ\n",
                    Lead->Type, &DeviceNode->InstancePath);
        }

        Assignments[Which].Type = Lead->Type;
        Assignments[Which].Flags = Lead->Flags;
        Assignments[Which].ShareDisposition = Lead->ShareDisposition;

        /*
         * A DevicePrivate descriptor carries the bus driver's own opaque state,
         * which it round-trips from requirements through assigned resources to
         * IRP_MN_START_DEVICE. pci.sys uses it to remember how to map each
         * assigned range back onto hardware, in particular which assigned memory
         * range is a PCI to PCI bridge's forwarding window and therefore what to
         * write into the Memory and Prefetch Base/Limit registers. The payload
         * lives in u.DevicePrivate.Data, the same ULONG Data[3] shape in both the
         * IO_RESOURCE_DESCRIPTOR and the CM descriptor, and has to be copied
         * verbatim. Dropping it corrupts pci.sys's state, so it cannot program
         * the bridge window and leaves it disabled at 0xFFF0/0x0000, which makes
         * every device behind the bridge unreachable.
         */
        if (Lead->Type == CmResourceTypeDevicePrivate)
        {
            Assignments[Which].u.DevicePrivate.Data[0] = Lead->u.DevicePrivate.Data[0];
            Assignments[Which].u.DevicePrivate.Data[1] = Lead->u.DevicePrivate.Data[1];
            Assignments[Which].u.DevicePrivate.Data[2] = Lead->u.DevicePrivate.Data[2];
        }
    }

    /* Assemble the packed assignments into a resource list */
    CmListSize = sizeof(CM_RESOURCE_LIST) +
                 (GroupCount - 1) * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);

    CmList = ExAllocatePoolWithTag(PagedPool, CmListSize, TAG_IO_ARBITER);
    if (CmList == NULL)
    {
        /* The arbiters have already committed, so there is nothing to undo */
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlZeroMemory(CmList, CmListSize);
    CmList->Count = 1;
    CmList->List[0].InterfaceType = RequirementsList->InterfaceType;
    CmList->List[0].BusNumber = RequirementsList->BusNumber;
    CmList->List[0].PartialResourceList.Version = 1;
    CmList->List[0].PartialResourceList.Revision = 1;
    CmList->List[0].PartialResourceList.Count = GroupCount;
    RtlCopyMemory(CmList->List[0].PartialResourceList.PartialDescriptors,
                  Assignments,
                  GroupCount * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

    *ResourceList = CmList;
    Status = STATUS_SUCCESS;
    goto Cleanup;

Rollback:
    /* Discard the tentative allocations of every arbiter tested so far */
    for (Index = 0; Index < TestedCount; Index++)
        IopArbiterInvoke(Tested[Index], ArbiterActionRollbackAllocation, NULL);

Cleanup:
    ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
    ExFreePoolWithTag(Assignments, TAG_IO_ARBITER);

    return Status;
}

/**
 * @brief
 * Assigns a device's resources through the arbiters.
 *
 * @param[in] DeviceNode
 * The device being assigned resources.
 *
 * @param[in] RequirementsList
 * The device's resource requirements.
 *
 * @param[in] RequestSource
 * Who is asking.
 *
 * @param[out] ResourceList
 * On success, receives the CM_RESOURCE_LIST of assignments.
 *
 * @return
 * STATUS_SUCCESS when an alternative configuration was fully placed, or the
 * last failure status when every alternative failed.
 *
 * @remarks
 * Each alternative configuration is tried in turn until one is fully placed,
 * first without touching any boot reserved range in phase 0, then with the
 * device's own boot configuration requirements allowed to reclaim boot reserved
 * ranges in phase 1. Phase 1 only runs for a device that has a boot
 * configuration.
 */
static
NTSTATUS
IopArbiterAllocateResourcesEx(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ ARBITER_REQUEST_SOURCE RequestSource,
    _Out_ PCM_RESOURCE_LIST *ResourceList)
{
    PIO_RESOURCE_LIST Configuration;
    ULONG Index;
    ULONG Phase;
    ULONG PhaseCount;
    NTSTATUS Status = STATUS_CONFLICTING_ADDRESSES;

    PAGED_CODE();

    *ResourceList = NULL;

    PhaseCount = ((DeviceNode->Flags & DNF_HAS_BOOT_CONFIG) &&
                  DeviceNode->BootResources != NULL) ? 2 : 1;

    for (Phase = 0; Phase < PhaseCount; Phase++)
    {
        Configuration = &RequirementsList->List[0];

        for (Index = 0;
             Index < RequirementsList->AlternativeLists;
             Index++, Configuration = IopArbiterNextList(Configuration))
        {
            Status = IopArbiterTryConfiguration(DeviceNode,
                                                RequirementsList,
                                                Configuration,
                                                Phase,
                                                RequestSource,
                                                ResourceList);
            if (NT_SUCCESS(Status))
                return STATUS_SUCCESS;
        }
    }

    DPRINT1("All %lu configurations failed for %wZ\n",
            RequirementsList->AlternativeLists, &DeviceNode->InstancePath);

    return Status;
}

/**
 * @brief
 * Assigns a PnP enumerated device's resources through the arbiters.
 *
 * @param[in] DeviceNode
 * The device being assigned resources.
 *
 * @param[in] RequirementsList
 * The device's resource requirements.
 *
 * @param[out] ResourceList
 * On success, receives the CM_RESOURCE_LIST of assignments.
 *
 * @return
 * IopArbiterAllocateResourcesEx's status.
 *
 * @remarks
 * One line in and one line out per device. Resource assignment is a long silent
 * stretch of boot: the requirements are queried from the bus driver,
 * arbitrated, and then the device is started, and none of that prints anything
 * on success. When a machine wedges somewhere in there the last visible line is
 * whatever the bus driver happened to log beforehand, which says nothing about
 * which of the three stages it died in. Bracketing arbitration alone splits the
 * window three ways: no "arbitrating" line means it never got past building the
 * requirements list, an "arbitrating" line with no result means the arbiter
 * itself is stuck, and both lines mean the device's start is at fault.
 */
NTSTATUS
NTAPI
IopArbiterAllocateResources(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _Out_ PCM_RESOURCE_LIST *ResourceList)
{
    NTSTATUS Status;

    DPRINT1("Arbitrating resources for %wZ (%lu configuration(s))\n",
            &DeviceNode->InstancePath, RequirementsList->AlternativeLists);

    Status = IopArbiterAllocateResourcesEx(DeviceNode,
                                           RequirementsList,
                                           ArbiterRequestPnpEnumerated,
                                           ResourceList);

    DPRINT1("Arbitration for %wZ returned 0x%08lx\n",
            &DeviceNode->InstancePath, Status);

    return Status;
}

/**
 * @brief
 * Releases everything a device owns in one arbiter.
 *
 * @param[in] Interface
 * The arbiter to release from.
 *
 * @param[in] PhysicalDeviceObject
 * The owner whose ranges are released.
 *
 * @remarks
 * A TestAllocation whose entry has no alternatives makes the arbiter drop the
 * owner's ranges from its working copy without adding anything back, and the
 * commit makes that permanent. Going through the interface this way works for a
 * bus driver's arbiter as much as for the root ones. A failed test is rolled
 * back, so the committed state is left as it was found.
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
    Entry.AlternativeCount = 0;
    Entry.Alternatives = NULL;
    Entry.PhysicalDeviceObject = PhysicalDeviceObject;
    Entry.RequestSource = ArbiterRequestPnpEnumerated;
    Entry.Result = ArbiterResultUndefined;

    InitializeListHead(&ArbitrationList);
    InsertTailList(&ArbitrationList, &Entry.ListEntry);

    Status = IopArbiterInvoke(Interface, ArbiterActionTestAllocation, &ArbitrationList);
    if (NT_SUCCESS(Status))
    {
        IopArbiterInvoke(Interface, ArbiterActionCommitAllocation, NULL);
    }
    else
    {
        DPRINT1("Arbiter rejected the release request (Status 0x%08lx)\n", Status);
        IopArbiterInvoke(Interface, ArbiterActionRollbackAllocation, NULL);
    }
}

/**
 * @brief
 * Frees every arbiter range a device holds, both its committed allocations and
 * any reserved boot configuration, so that the ranges return to the free pool.
 *
 * @param[in] DeviceNode
 * The device being released, either removed or about to be reassigned.
 *
 * @remarks
 * A no-op for the resource types the device never held. DNF_BOOT_CONFIG_RESERVED
 * is cleared, since the boot reservation goes with the rest.
 */
VOID
NTAPI
IopArbiterReleaseResources(
    _In_ PDEVICE_NODE DeviceNode)
{
    ULONG TypeIndex;

    PAGED_CODE();

    if (DeviceNode->PhysicalDeviceObject == NULL)
        return;

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); TypeIndex++)
    {
        PARBITER_INTERFACE Interface;

        if (NT_SUCCESS(IopFindArbiterForResourceType(DeviceNode,
                                                     IopArbiterResourceTypes[TypeIndex],
                                                     &Interface)))
        {
            IopArbiterReleaseOwner(Interface, DeviceNode->PhysicalDeviceObject);
        }
    }

    IopDeviceNodeClearFlag(DeviceNode, DNF_BOOT_CONFIG_RESERVED);
}

/* REGISTRY MIRROR AND ASSIGNMENT *******************************************/

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

static
NTSTATUS
IopTranslateDeviceResources(
   IN PDEVICE_NODE DeviceNode)
{
   PCM_PARTIAL_RESOURCE_LIST pPartialResourceList;
   PCM_PARTIAL_RESOURCE_DESCRIPTOR DescriptorRaw, DescriptorTranslated;
   PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor;
   ULONG i, j, ListSize;
   NTSTATUS Status;

   if (!DeviceNode->ResourceList)
   {
      DeviceNode->ResourceListTranslated = NULL;
      return STATUS_SUCCESS;
   }

   /* That's easy to translate a resource list. Just copy the
    * untranslated one and change few fields in the copy
    */
   ListSize = PnpDetermineResourceListSize(DeviceNode->ResourceList);

   DeviceNode->ResourceListTranslated = ExAllocatePool(PagedPool, ListSize);
   if (!DeviceNode->ResourceListTranslated)
   {
      Status = STATUS_NO_MEMORY;
      goto cleanup;
   }
   RtlCopyMemory(DeviceNode->ResourceListTranslated, DeviceNode->ResourceList, ListSize);

   FullDescriptor = &DeviceNode->ResourceList->List[0];
   for (i = 0; i < DeviceNode->ResourceList->Count; i++)
   {
      pPartialResourceList = &FullDescriptor->PartialResourceList;
      FullDescriptor = CmiGetNextResourceDescriptor(FullDescriptor);

      for (j = 0; j < pPartialResourceList->Count; j++)
      {
        /* Partial resource descriptors can be of variable size (CmResourceTypeDeviceSpecific),
           but only one is allowed and it must be the last one in the list! */
         DescriptorRaw = &pPartialResourceList->PartialDescriptors[j];

         /* Calculate the location of the translated resource descriptor */
         DescriptorTranslated = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)(
             (PUCHAR)DeviceNode->ResourceListTranslated +
             ((PUCHAR)DescriptorRaw - (PUCHAR)DeviceNode->ResourceList));

         switch (DescriptorRaw->Type)
         {
            case CmResourceTypePort:
            {
               ULONG AddressSpace = 1; /* IO space */
               if (!HalTranslateBusAddress(
                  DeviceNode->ResourceList->List[i].InterfaceType,
                  DeviceNode->ResourceList->List[i].BusNumber,
                  DescriptorRaw->u.Port.Start,
                  &AddressSpace,
                  &DescriptorTranslated->u.Port.Start))
               {
                  Status = STATUS_UNSUCCESSFUL;
                  DPRINT1("Failed to translate port resource (Start: 0x%I64x)\n", DescriptorRaw->u.Port.Start.QuadPart);
                  goto cleanup;
               }

               if (AddressSpace == 0)
               {
                   DPRINT1("Guessed incorrect address space: 1 -> 0\n");

                   /* FIXME: I think all other CM_RESOURCE_PORT_XXX flags are
                    * invalid for this state but I'm not 100% sure */
                   DescriptorRaw->Flags =
                   DescriptorTranslated->Flags = CM_RESOURCE_PORT_MEMORY;
               }
               break;
            }
            case CmResourceTypeInterrupt:
            {
               KIRQL Irql;
               DescriptorTranslated->u.Interrupt.Vector = HalGetInterruptVector(
                  DeviceNode->ResourceList->List[i].InterfaceType,
                  DeviceNode->ResourceList->List[i].BusNumber,
                  DescriptorRaw->u.Interrupt.Level,
                  DescriptorRaw->u.Interrupt.Vector,
                  &Irql,
                  &DescriptorTranslated->u.Interrupt.Affinity);
               DescriptorTranslated->u.Interrupt.Level = Irql;
               if (!DescriptorTranslated->u.Interrupt.Vector)
               {
                   Status = STATUS_UNSUCCESSFUL;
                   DPRINT1("Failed to translate interrupt resource (Vector: 0x%x | Level: 0x%x)\n", DescriptorRaw->u.Interrupt.Vector,
                                                                                                   DescriptorRaw->u.Interrupt.Level);
                   goto cleanup;
               }
               break;
            }
            case CmResourceTypeMemory:
            {
               ULONG AddressSpace = 0; /* Memory space */
               if (!HalTranslateBusAddress(
                  DeviceNode->ResourceList->List[i].InterfaceType,
                  DeviceNode->ResourceList->List[i].BusNumber,
                  DescriptorRaw->u.Memory.Start,
                  &AddressSpace,
                  &DescriptorTranslated->u.Memory.Start))
               {
                  Status = STATUS_UNSUCCESSFUL;
                  DPRINT1("Failed to translate memory resource (Start: 0x%I64x)\n", DescriptorRaw->u.Memory.Start.QuadPart);
                  goto cleanup;
               }

               if (AddressSpace != 0)
               {
                   DPRINT1("Guessed incorrect address space: 0 -> 1\n");

                   /* This should never happen for memory space */
                   ASSERT(FALSE);
               }
            }

            case CmResourceTypeDma:
            case CmResourceTypeBusNumber:
            case CmResourceTypeDevicePrivate:
            case CmResourceTypeDeviceSpecific:
               /* Nothing to do */
               break;
            default:
               DPRINT1("Unknown resource descriptor type 0x%x\n", DescriptorRaw->Type);
               Status = STATUS_NOT_IMPLEMENTED;
               goto cleanup;
         }
      }
   }
   return STATUS_SUCCESS;

cleanup:
   /* Yes! Also delete ResourceList because ResourceList and
    * ResourceListTranslated should be a pair! */
   ExFreePool(DeviceNode->ResourceList);
   DeviceNode->ResourceList = NULL;
   if (DeviceNode->ResourceListTranslated)
   {
      ExFreePool(DeviceNode->ResourceListTranslated);
      DeviceNode->ResourceList = NULL;
   }
   return Status;
}

/**
 * @brief
 * Assigns a device its resources.
 *
 * @param[in] DeviceNode
 * The device to assign.
 *
 * @return
 * STATUS_SUCCESS once the device holds an arbitrated, translated and recorded
 * assignment, or a failure status with the device left with a problem code.
 *
 * @remarks
 * Anything a previous pass or a boot reservation holds is returned to the
 * arbiters first, so that re-arbitration does not see the device conflict with
 * itself. A device with no requirements keeps its firmware configuration as its
 * assignment; otherwise the requirements are arbitrated and the result replaces
 * it.
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

    /*
     * Give up any ranges a previous assignment or boot reservation holds, so
     * that re-arbitration does not see this device conflict with itself.
     */
    IopArbiterReleaseResources(DeviceNode);

    Status = IopFilterResourceRequirements(DeviceNode);
    if (!NT_SUCCESS(Status))
        goto Failure;

    if (DeviceNode->BootResources == NULL && DeviceNode->ResourceRequirements == NULL)
    {
        /* This device needs no resources at all */
        DeviceNode->ResourceList = NULL;
        DeviceNode->ResourceListTranslated = NULL;
        DeviceNode->Flags |= DNF_NO_RESOURCE_REQUIRED;
        PiSetDevNodeState(DeviceNode, DeviceNodeResourcesAssigned);

        IopUnlockResourceAssignment();
        return STATUS_SUCCESS;
    }

    if (DeviceNode->BootResources != NULL)
    {
        ListSize = PnpDetermineResourceListSize(DeviceNode->BootResources);

        DeviceNode->ResourceList = ExAllocatePool(PagedPool, ListSize);
        if (DeviceNode->ResourceList == NULL)
        {
            Status = STATUS_NO_MEMORY;
            goto Failure;
        }

        RtlCopyMemory(DeviceNode->ResourceList, DeviceNode->BootResources, ListSize);

        /*
         * Put the firmware boot configuration back on record in the arbiters.
         * Firmware configurations are trusted, and a real conflict is settled by
         * the arbitration below rather than by dropping the configuration here.
         */
        IopArbiterReserveBootConfig(DeviceNode);
    }
    else
    {
        /* The assignment is built from the requirements instead */
        DeviceNode->ResourceList = NULL;
    }

    /* With no requirements to arbitrate, the boot configuration is the answer */
    if (DeviceNode->ResourceRequirements == NULL)
        goto Translate;

    /* Let the HAL adjust the requirements for the platform */
    HalAdjustResourceList(&DeviceNode->ResourceRequirements);

    /* The boot copy, if there is one, is superseded by the arbitrated result */
    if (DeviceNode->ResourceList != NULL)
    {
        ExFreePool(DeviceNode->ResourceList);
        DeviceNode->ResourceList = NULL;
    }

    Status = IopArbiterAllocateResources(DeviceNode,
                                         DeviceNode->ResourceRequirements,
                                         &DeviceNode->ResourceList);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to arbitrate resources for %wZ (Status 0x%08lx)\n",
                &DeviceNode->InstancePath, Status);
        PiSetDevNodeProblem(DeviceNode, CM_PROB_NORMAL_CONFLICT);
        goto Failure;
    }

Translate:
    Status = IopTranslateDeviceResources(DeviceNode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to translate resources for %wZ (Status 0x%08lx)\n",
                &DeviceNode->InstancePath, Status);
        PiSetDevNodeProblem(DeviceNode, CM_PROB_TRANSLATION_FAILED);
        goto Failure;
    }

    /* Record the assignment under RESOURCEMAP and Control\AllocConfig */
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
    if (DeviceNode->ResourceList != NULL)
    {
        ExFreePool(DeviceNode->ResourceList);
        DeviceNode->ResourceList = NULL;
    }

    if (DeviceNode->ResourceListTranslated != NULL)
    {
        ExFreePool(DeviceNode->ResourceListTranslated);
        DeviceNode->ResourceListTranslated = NULL;
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


/**
 * @brief
 * Tree walk callback that lets a device which failed resource assignment try
 * again.
 *
 * @param[in] DeviceNode
 * The visited node.
 *
 * @param[in] Context
 * A PULONG counting the devices released for a retry.
 *
 * @return
 * STATUS_SUCCESS, to keep the walk going.
 *
 * @remarks
 * A device whose arbitration failed stays in DeviceNodeDriversAdded with
 * CM_PROB_NORMAL_CONFLICT or CM_PROB_TRANSLATION_FAILED. Clearing the problem
 * lets the next state machine pass call IopAssignDeviceResources for it again.
 */
static
NTSTATUS
IopClearResourceProblem(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PVOID Context)
{
    if (DeviceNode->State == DeviceNodeDriversAdded &&
        (DeviceNode->Flags & DNF_HAS_PROBLEM) &&
        (DeviceNode->Problem == CM_PROB_NORMAL_CONFLICT ||
         DeviceNode->Problem == CM_PROB_TRANSLATION_FAILED))
    {
        DPRINT("Retrying resource assignment for %wZ\n", &DeviceNode->InstancePath);
        PiClearDevNodeProblem(DeviceNode);
        (*(PULONG)Context)++;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Asks that every device which failed resource assignment be re-arbitrated.
 *
 * @remarks
 * Called once ranges have been returned to the arbiters. The failed devices'
 * problems are cleared and a tree pass is queued, so the state machine re-runs
 * their assignment.
 */
static
VOID
IopRequestResourceRetry(VOID)
{
    DEVICETREE_TRAVERSE_CONTEXT Context;
    ULONG Released = 0;

    if (IopRootDeviceNode == NULL)
        return;

    IopInitDeviceTreeTraverseContext(&Context,
                                     IopRootDeviceNode,
                                     IopClearResourceProblem,
                                     &Released);
    IopTraverseDeviceTree(&Context);

    if (Released != 0)
    {
        PiQueueDeviceAction(IopRootDeviceNode->PhysicalDeviceObject,
                            PiActionEnumDeviceTree,
                            NULL,
                            NULL);
    }
}

/**
 * @brief
 * Releases the resources a device holds: its arbiter ranges, its registry
 * mirror and its resource lists.
 *
 * @param[in] DeviceNode
 * The device being removed or restarted.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @remarks
 * Every owning arbiter releases the device's ranges, the Control\AllocConfig
 * value and the RESOURCEMAP pair are deleted, and the raw and translated lists
 * are freed.
 *
 * A root-enumerated device, marked DNF_MADEUP, keeps its firmware claim and has
 * its boot configuration re-reserved right away. Any other device's boot
 * configuration goes with the device, and the bus reports it again when the
 * device is re-enumerated. Finally the devices that failed assignment are asked
 * to retry, since what was just freed may be exactly what they were waiting for.
 */
NTSTATUS
NTAPI
IopReleaseDeviceResources(
    _In_ PDEVICE_NODE DeviceNode)
{
    BOOLEAN HeldResources;

    PAGED_CODE();

    IopLockResourceAssignment();

    HeldResources = (DeviceNode->ResourceList != NULL) ||
                    (DeviceNode->Flags & DNF_BOOT_CONFIG_RESERVED);

    if (HeldResources)
    {
        /* Return this device's assigned and reserved ranges to the arbiters */
        IopArbiterReleaseResources(DeviceNode);

        if (DeviceNode->ResourceList != NULL)
        {
            ExFreePool(DeviceNode->ResourceList);
            DeviceNode->ResourceList = NULL;
        }

        if (DeviceNode->ResourceListTranslated != NULL)
        {
            ExFreePool(DeviceNode->ResourceListTranslated);
            DeviceNode->ResourceListTranslated = NULL;
        }

        /* Clear the registry mirror, best effort */
        if (DeviceNode->PhysicalDeviceObject != NULL)
        {
            IopUpdateResourceMapForPnPDevice(DeviceNode);
            IopUpdateControlKeyWithResources(DeviceNode);
        }
    }

    if ((DeviceNode->Flags & (DNF_MADEUP | DNF_DEVICE_GONE)) == DNF_MADEUP)
    {
        /* A root-enumerated device keeps its firmware claim */
        if ((DeviceNode->Flags & DNF_HAS_BOOT_CONFIG) && DeviceNode->BootResources != NULL)
            IopArbiterReserveBootConfig(DeviceNode);
    }
    else
    {
        IopDeviceNodeClearFlag(DeviceNode, DNF_HAS_BOOT_CONFIG | DNF_BOOT_CONFIG_RESERVED);

        if (DeviceNode->BootResources != NULL)
        {
            ExFreePool(DeviceNode->BootResources);
            DeviceNode->BootResources = NULL;
        }
    }

    /* Whatever was freed may be what a conflicting device is waiting for */
    if (HeldResources)
        IopRequestResourceRetry();

    IopUnlockResourceAssignment();

    return STATUS_SUCCESS;
}
