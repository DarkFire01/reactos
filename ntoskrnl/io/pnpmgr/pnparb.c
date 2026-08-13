/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PnP manager resource-arbiter discovery
 * COPYRIGHT:   Copyright 2025-2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The PnP manager assigns each device its resources through the resource
 * arbiters.  An arbiter is owned by a *bus* (the ancestor that decodes the
 * resource), and is published through IRP_MN_QUERY_INTERFACE for
 * GUID_ARBITER_INTERFACE_STANDARD, keyed by resource type in
 * InterfaceSpecificData.  When no bus in the parent chain arbitrates a given
 * type, the five root arbiters (Port / Memory / Dma / Interrupt / BusNumber),
 * pre-registered on the root device node, are the terminal fallback.
 *
 * This file provides the discovery half of that machinery: registering the root
 * arbiters and walking a device's ancestry to find the arbiter for a resource
 * type (caching the result on the providing node, NT-style, via
 * DeviceArbiterList + Query/NoArbiterMask).  Driving the actual
 * TestAllocation/Commit transaction with the found arbiter is the job of the
 * resource-assignment path (pnpres.c).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wdmguid.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define TAG_IO_ARBITER 'AbrI'

/*
 * Seeding the root arbiters from \HARDWARE\RESOURCEMAP reserves the fixed
 * firmware/HAL/loader resources.  It is on by default; clear it (e.g. from the
 * boot debugger before the PnP phase runs, or here) to fall back to un-seeded
 * arbiters should the seeding prove to over-reserve on a given machine.
 */
BOOLEAN IopArbiterSeedResourceMap = TRUE;

/* Defined below; seeds the root arbiters from the firmware resource map. */
CODE_SEG("INIT") VOID NTAPI IopArbiterSeedFromResourceMap(VOID);

/* Defined below; the root arbiter's published interface for a resource type. */
static PARBITER_INTERFACE IopGetRootArbiterInterface(_In_ UCHAR ResourceType);

/* The five root arbiter instances (initialized by IopInitializeArbiters). */
extern ARBITER_INSTANCE IopRootBusNumberArbiter;
extern ARBITER_INSTANCE IopRootIrqArbiter;
extern ARBITER_INSTANCE IopRootDmaArbiter;
extern ARBITER_INSTANCE IopRootMemArbiter;
extern ARBITER_INSTANCE IopRootPortArbiter;

/*
 * The root arbiters live for the life of the system, so their ARBITER_INTERFACEs
 * are static and their reference/dereference callbacks are no-ops.
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

/* One published ARBITER_INTERFACE per root arbiter (same order as the table). */
static ARBITER_INTERFACE IopRootArbiterInterface[RTL_NUMBER_OF(IopRootArbiterTable)];

/*
 * A cached arbiter entry.  When an arbiter is discovered by querying a bus, its
 * ARBITER_INTERFACE is copied out of the transient QUERY_INTERFACE buffer into
 * the Interface member here so the cached PI_RESOURCE_ARBITER_ENTRY has stable
 * storage to point at.  (The root entries point at IopRootArbiterInterface[]
 * instead and leave Interface unused.)  PI_RESOURCE_ARBITER_ENTRY is the first
 * member, so a DeviceArbiterList link resolves to the whole IOP_ARBITER_ENTRY.
 */
typedef struct _IOP_ARBITER_ENTRY
{
    PI_RESOURCE_ARBITER_ENTRY Entry;
    ARBITER_INTERFACE Interface;
} IOP_ARBITER_ENTRY, *PIOP_ARBITER_ENTRY;

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Returns the cached arbiter entry of the given resource type on a
 * device node, if any.
 *
 * @param[in] Node
 * The device node whose DeviceArbiterList is searched.
 *
 * @param[in] ResourceType
 * The CmResourceType* the arbiter must own.
 *
 * @return
 * Returns the cached PI_RESOURCE_ARBITER_ENTRY, or NULL if this
 * node has none for the type.
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
 * Asks a bus PDO for the arbiter of a resource type via
 * IRP_MN_QUERY_INTERFACE for GUID_ARBITER_INTERFACE_STANDARD.
 *
 * @param[in] Pdo
 * The bus's physical device object to query.
 *
 * @param[in] ResourceType
 * The CmResourceType* passed as InterfaceSpecificData.
 *
 * @param[out] Interface
 * Receives the bus's ARBITER_INTERFACE on success.
 *
 * @return
 * Returns the IRP completion status; a bus that does not arbitrate
 * the type fails the IRP (STATUS_NOT_SUPPORTED default).
 *
 * @remarks
 * The GUID_ARBITER_INTERFACE_STANDARD interface is version 0 (the
 * ARBITER interface has never been revised).  pci.sys advertises
 * its per-bus arbiters with MinVersion == MaxVersion == 0 and
 * rejects any other version, so we must query for exactly 0 -
 * querying for 1 makes pci.sys decline, and every PCI device then
 * falls through to the root arbiter, whose grants never reach
 * pci.sys's own allocation.  That in turn makes pci.sys's
 * PciTranslateBusAddress hook reject the (now NULL-owned) range,
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
 * Registers the five root arbiters on the root device node so the
 * ancestry walk always terminates with a fallback arbiter for
 * every resource type.
 *
 * @param[in] RootNode
 * The root device node (IopRootDeviceNode).
 *
 * @return
 * Returns STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES if an
 * entry allocation fails.
 *
 * @remarks
 * Called once, from IopInitializePlugPlayServices, after the root
 * node exists.  Also seeds the freshly-published root arbiters
 * with the fixed firmware/HAL/loader resources, so a later
 * flexible requirement is never placed on a range the system
 * already owns; the seeding runs before any device is enumerated.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
IopRegisterRootArbiters(
    _In_ PDEVICE_NODE RootNode)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(IopRootArbiterTable); ++Index)
    {
        PARBITER_INTERFACE Interface = &IopRootArbiterInterface[Index];
        PPI_RESOURCE_ARBITER_ENTRY Entry;

        Interface->Size = sizeof(ARBITER_INTERFACE);
        /* GUID_ARBITER_INTERFACE_STANDARD is version 0 (see IopQueryArbiterInterface). */
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

    if (IopArbiterSeedResourceMap)
        IopArbiterSeedFromResourceMap();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Answers the root PDO's IRP_MN_QUERY_INTERFACE for
 * GUID_ARBITER_INTERFACE_STANDARD with the root arbiter of the
 * requested resource type.
 *
 * @param[in] IoStack
 * The QUERY_INTERFACE stack location; InterfaceSpecificData
 * carries the CmResourceType*.
 *
 * @param[in] ExistingStatus
 * The IRP's current status, returned unchanged when the query is
 * declined so the IRP is left untouched per the QUERY_INTERFACE
 * protocol.
 *
 * @return
 * Returns STATUS_SUCCESS with the interface copied into the
 * caller's buffer, or ExistingStatus when the query is not for
 * this interface, is for an unknown resource type, asks for a
 * version other than 0, or provides too small a buffer.
 *
 * @remarks
 * This makes the root just another bus in the discovery walk (no
 * special "root" code path): the same IopQueryArbiterInterface
 * query that discovers a bus driver's arbiters discovers the root
 * arbiters when it reaches the root PDO.  The registration cache
 * on the root devnode normally answers first, so this handler is
 * the protocol-faithful backstop for queriers that come through
 * the IRP path.  A version other than 0 is declined, matching how
 * pci.sys treats the never-revised ARBITER interface.
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

    if (!IsEqualGUID(IoStack->Parameters.QueryInterface.InterfaceType,
                     &GUID_ARBITER_INTERFACE_STANDARD))
    {
        return ExistingStatus;   /* not ours - leave the IRP untouched */
    }

    if (IoStack->Parameters.QueryInterface.Version != 0 ||
        IoStack->Parameters.QueryInterface.Size < sizeof(ARBITER_INTERFACE) ||
        IoStack->Parameters.QueryInterface.Interface == NULL)
    {
        return ExistingStatus;   /* decline: wrong version or short buffer */
    }

    ResourceType = (UCHAR)(ULONG_PTR)IoStack->Parameters.QueryInterface.InterfaceSpecificData;
    Published = IopGetRootArbiterInterface(ResourceType);
    if (Published == NULL || Published->ArbiterHandler == NULL)
    {
        /* No root arbiter for the type, or registration has not run yet. */
        return ExistingStatus;
    }

    Out = (PARBITER_INTERFACE)IoStack->Parameters.QueryInterface.Interface;
    RtlCopyMemory(Out, Published, sizeof(ARBITER_INTERFACE));

    /* The provider references the interface before returning it (a no-op for
     * the immortal root arbiters, but the protocol requires the call). */
    Out->InterfaceReference(Out->Context);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finds the arbiter that owns ResourceType for a device by walking
 * its ancestry.
 *
 * @param[in] DeviceNode
 * The device whose resource is being arbitrated.
 *
 * @param[in] ResourceType
 * The CmResourceType* to find an arbiter for.
 *
 * @param[out] ArbiterInterface
 * Receives the owning arbiter's interface.
 *
 * @return
 * Returns STATUS_SUCCESS with the interface,
 * STATUS_INSUFFICIENT_RESOURCES if caching an answer fails, or
 * STATUS_NOT_FOUND if no arbiter exists for the type (only
 * possible before the root arbiters are registered).
 *
 * @remarks
 * Arbitration is provided by an ancestor bus, never the device
 * itself, so the walk starts at the parent.  The first cached
 * entry wins; otherwise each bus's PDO is queried exactly once
 * (Query/NoArbiterMask) and the answer is cached on the providing
 * node.  The walk terminates at the root node, whose
 * pre-registered arbiters cover every resource type, so this
 * succeeds for any in-tree device.
 */
NTSTATUS
NTAPI
IopFindArbiterForResourceType(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ UCHAR ResourceType,
    _Out_ PARBITER_INTERFACE *ArbiterInterface)
{
    PDEVICE_NODE Node;
    USHORT Mask = (USHORT)(1 << ResourceType);

    PAGED_CODE();

    *ArbiterInterface = NULL;

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

        /* This bus is already known not to arbitrate the type: keep walking up. */
        if (Node->NoArbiterMask & Mask)
            continue;

        /* Query this bus for the arbiter exactly once. */
        Node->QueryArbiterMask |= Mask;

        /*
         * A bus that does not arbitrate this type leaves the IRP at its
         * STATUS_NOT_SUPPORTED default, so a failure is the normal answer.  Zero
         * the buffer and require a handler on success, so a driver that reports
         * success without filling the interface cannot be cached and later called.
         */
        RtlZeroMemory(&Queried, sizeof(Queried));
        if (Node->PhysicalDeviceObject != NULL &&
            NT_SUCCESS(IopQueryArbiterInterface(Node->PhysicalDeviceObject, ResourceType, &Queried)) &&
            Queried.ArbiterHandler != NULL)
        {
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
             * Diagnostic: which ANCESTOR bus arbitrates this device's resource.
             * For a device behind a PCI-PCI (AGP) bridge, a memory BAR must
             * resolve to the BRIDGE's arbiter (so the child is carved out of the
             * bridge's forwarding window); if it instead falls through to the
             * root arbiter, the child occupies root space and the bridge's own
             * window can no longer be placed -> bridge window left closed.
             */
            DPRINT("ARB-ROUTE: dev %wZ res-type %u -> arbiter on ancestor %wZ\n",
                   &DeviceNode->InstancePath, ResourceType, &Node->InstancePath);

            *ArbiterInterface = &NewEntry->Interface;
            return STATUS_SUCCESS;
        }

        /* No arbiter here - don't ask again. */
        Node->NoArbiterMask |= Mask;
    }

    /*
     * No bus in the ancestry arbitrates this type: fall back to the root
     * arbiter, which owns every type.  This also covers the case where the walk
     * did not reach the root node (e.g. a device whose parent chain is short or
     * not yet fully linked) - the published interface is static, so it is always
     * available once the root arbiters have been registered.
     */
    *ArbiterInterface = IopGetRootArbiterInterface(ResourceType);
    if (*ArbiterInterface != NULL && (*ArbiterInterface)->Context != NULL)
    {
        /*
         * Diagnostic: no ancestor bus arbitrated this type, so it falls to the
         * ROOT arbiter.  For a device on bus 0 this is correct; for a device
         * BEHIND a bridge (e.g. an AGP GPU on bus 1) a memory resource landing
         * here means the parent bridge did NOT expose an arbiter - the child
         * will occupy root space and starve the bridge's own forwarding window.
         */
        DPRINT("ARB-ROUTE: dev %wZ res-type %u -> ROOT arbiter (no ancestor bus arbiter)\n",
               &DeviceNode->InstancePath, ResourceType);
        return STATUS_SUCCESS;
    }

    DPRINT1("IopFindArbiterForResourceType: no arbiter for type %u\n", ResourceType);
    return STATUS_NOT_FOUND;
}

/* RESOURCE ALLOCATION TRANSACTION ******************************************/

/*
 * The resource types that have an arbiter, in the order they are arbitrated.
 * A "MemoryLarge" (type 7) requirement is arbitrated by the Memory arbiter.
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
 * Reports whether a requirement of EntryType is handled by the
 * ArbType arbiter.
 *
 * @param[in] EntryType
 * The requirement descriptor's resource type.
 *
 * @param[in] ArbType
 * The arbiter's resource type.
 *
 * @return
 * Returns TRUE if the arbiter owns the requirement; the Memory
 * arbiter also owns the 64-bit "large memory" requirements.
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
 * Advances to the next alternative IO_RESOURCE_LIST in a
 * variable-length requirements list.
 *
 * @param[in] List
 * The current alternative configuration.
 *
 * @return
 * Returns the next alternative, immediately past this one's
 * descriptor array.
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
 * The list of ARBITER_LIST_ENTRYs for list-bearing actions, or
 * NULL for Commit / Rollback.
 *
 * @return
 * Returns the arbiter's completion status.
 *
 * @remarks
 * TestAllocation / BootAllocation read ArbitrationList from the
 * first union member; Commit / Rollback ignore Parameters
 * entirely.  The list-bearing parameter blocks share the same
 * leading ArbitrationList field, so one assignment serves
 * whichever action is dispatched.
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
 * Tries to place one alternative configuration (a single
 * IO_RESOURCE_LIST) through the arbiters.
 *
 * @param[in] DeviceNode
 * The device being assigned resources.
 *
 * @param[in] RequirementsList
 * The full requirements list (for bus identity fields).
 *
 * @param[in] Configuration
 * The alternative configuration to try.
 *
 * @param[out] ResourceList
 * On success, receives the packed CM_RESOURCE_LIST of assignments
 * (NULL for an empty configuration).
 *
 * @return
 * Returns STATUS_SUCCESS when every resource type was placed and
 * committed, STATUS_CONFLICTING_ADDRESSES (or the arbiter's
 * failure status) when any type could not be placed, or
 * STATUS_INSUFFICIENT_RESOURCES on allocation failure.
 *
 * @remarks
 * The descriptors are grouped into requirements (a lead descriptor
 * plus its IO_RESOURCE_ALTERNATIVE followers), one
 * ARBITER_LIST_ENTRY each, grouped by resource type and handed to
 * the owning arbiter's TestAllocation.  If every type is placed,
 * the arbiters commit and the packed assignments become the
 * returned CM_RESOURCE_LIST; if any type fails, the already-tested
 * arbiters roll back and the caller tries the next alternative.
 */
static
NTSTATUS
IopArbiterTryConfiguration(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _In_ PIO_RESOURCE_LIST Configuration,
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

    /* Count requirement groups: each non-alternative descriptor starts one. */
    for (Index = 0; Index < Configuration->Count; ++Index)
    {
        if (!(Configuration->Descriptors[Index].Option & IO_RESOURCE_ALTERNATIVE))
            ++GroupCount;
    }

    if (GroupCount == 0)
    {
        /* Nothing to arbitrate - an empty (but valid) configuration. */
        *ResourceList = NULL;
        return STATUS_SUCCESS;
    }

    Entries = ExAllocatePoolWithTag(PagedPool, GroupCount * sizeof(ARBITER_LIST_ENTRY), TAG_IO_ARBITER);
    Assignments = ExAllocatePoolWithTag(PagedPool, GroupCount * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR), TAG_IO_ARBITER);
    if (Entries == NULL || Assignments == NULL)
    {
        if (Entries) ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
        if (Assignments) ExFreePoolWithTag(Assignments, TAG_IO_ARBITER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Entries, GroupCount * sizeof(ARBITER_LIST_ENTRY));
    RtlZeroMemory(Assignments, GroupCount * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

    /* Build one ARBITER_LIST_ENTRY per requirement group. */
    Which = 0;
    for (Index = 0; Index < Configuration->Count; )
    {
        PARBITER_LIST_ENTRY Entry = &Entries[Which];
        ULONG AlternativeCount = 1;

        /* The group runs from this lead descriptor across its alternatives. */
        while (Index + AlternativeCount < Configuration->Count &&
               (Configuration->Descriptors[Index + AlternativeCount].Option & IO_RESOURCE_ALTERNATIVE))
        {
            ++AlternativeCount;
        }

        Entry->AlternativeCount = AlternativeCount;
        Entry->Alternatives = &Configuration->Descriptors[Index];
        Entry->PhysicalDeviceObject = DeviceNode->PhysicalDeviceObject;
        Entry->RequestSource = ArbiterRequestPnpEnumerated;
        Entry->Flags = 0;
        Entry->InterfaceType = RequirementsList->InterfaceType;
        Entry->SlotNumber = RequirementsList->SlotNumber;
        Entry->BusNumber = RequirementsList->BusNumber;
        Entry->Assignment = &Assignments[Which];
        Entry->Result = ArbiterResultUndefined;

        Index += AlternativeCount;
        ++Which;
    }

    /* Arbitrate one resource type at a time. */
    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); ++TypeIndex)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface;
        LIST_ENTRY ArbitrationList;
        ULONG InList = 0;

        InitializeListHead(&ArbitrationList);
        for (Which = 0; Which < GroupCount; ++Which)
        {
            if (IopArbiterTypeMatches(Entries[Which].Alternatives[0].Type, ResourceType))
            {
                InsertTailList(&ArbitrationList, &Entries[Which].ListEntry);
                ++InList;
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
            DPRINT("Arbiter type %u could not place %wZ (status 0x%08lx)\n",
                   ResourceType, &DeviceNode->InstancePath, Status);
            goto Rollback;
        }

        Tested[TestedCount++] = Interface;
    }

    /* Every type placed - make the tentative allocations permanent. */
    for (Index = 0; Index < TestedCount; ++Index)
        IopArbiterInvoke(Tested[Index], ArbiterActionCommitAllocation, NULL);

    /*
     * Pass through any requirement whose type no arbiter owns (its Assignment is
     * still zeroed): copy the descriptor identity from its preferred alternative
     * so the output list carries a well-formed descriptor rather than a null one.
     * PnP requirement lists normally contain only arbitrated types, so this is a
     * rare best-effort path.
     */
    for (Which = 0; Which < GroupCount; ++Which)
    {
        PIO_RESOURCE_DESCRIPTOR Lead = &Entries[Which].Alternatives[0];
        BOOLEAN Arbitrated = FALSE;

        for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); ++TypeIndex)
        {
            if (IopArbiterTypeMatches(Lead->Type, IopArbiterResourceTypes[TypeIndex]))
            {
                Arbitrated = TRUE;
                break;
            }
        }

        if (!Arbitrated)
        {
            DPRINT1("Passing through non-arbitrated resource type %u for %wZ\n",
                    Lead->Type, &DeviceNode->InstancePath);
            Assignments[Which].Type = Lead->Type;
            Assignments[Which].Flags = Lead->Flags;
            Assignments[Which].ShareDisposition = Lead->ShareDisposition;

            /*
             * A DevicePrivate descriptor carries the bus driver's own opaque
             * state, which it round-trips requirements -> assigned resources ->
             * IRP_MN_START_DEVICE.  pci.sys uses it to remember how to map each
             * assigned range back onto hardware - in particular which assigned
             * memory range is a PCI-PCI bridge's forwarding window and thus what
             * to write into the Memory/Prefetch Base/Limit registers.  The
             * payload lives in u.DevicePrivate.Data[] (same ULONG Data[3] shape
             * in both the IO_RESOURCE_DESCRIPTOR and the CM descriptor); it MUST
             * be copied verbatim.  Dropping it (leaving zeros) corrupts pci.sys's
             * state so it cannot program the bridge window and leaves it disabled
             * (0xFFF0/0x0000), which makes devices behind the bridge unreachable.
             */
            if (Lead->Type == CmResourceTypeDevicePrivate)
            {
                Assignments[Which].u.DevicePrivate.Data[0] = Lead->u.DevicePrivate.Data[0];
                Assignments[Which].u.DevicePrivate.Data[1] = Lead->u.DevicePrivate.Data[1];
                Assignments[Which].u.DevicePrivate.Data[2] = Lead->u.DevicePrivate.Data[2];
            }
        }
    }

    /* Assemble the packed assignments into a resource list. */
    CmListSize = sizeof(CM_RESOURCE_LIST) +
                 (GroupCount - 1) * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    CmList = ExAllocatePoolWithTag(PagedPool, CmListSize, TAG_IO_ARBITER);
    if (CmList == NULL)
    {
        /* The arbiters have already committed; nothing to undo but the report. */
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
    /* Discard the tentative allocations of every arbiter tested so far. */
    for (Index = 0; Index < TestedCount; ++Index)
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
 * @param[out] ResourceList
 * On success, receives the CM_RESOURCE_LIST of assignments.
 *
 * @return
 * Returns STATUS_SUCCESS when an alternative configuration was
 * fully placed, or the last failure status when every alternative
 * failed.
 *
 * @remarks
 * Each alternative configuration in the requirements list is tried
 * in turn until one is fully placed.
 */
NTSTATUS
NTAPI
IopArbiterAllocateResources(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList,
    _Out_ PCM_RESOURCE_LIST *ResourceList)
{
    PIO_RESOURCE_LIST Configuration;
    ULONG Index;
    NTSTATUS Status = STATUS_CONFLICTING_ADDRESSES;

    PAGED_CODE();

    *ResourceList = NULL;

    Configuration = &RequirementsList->List[0];
    for (Index = 0;
         Index < RequirementsList->AlternativeLists;
         ++Index, Configuration = IopArbiterNextList(Configuration))
    {
        Status = IopArbiterTryConfiguration(DeviceNode, RequirementsList, Configuration, ResourceList);
        if (NT_SUCCESS(Status))
            return STATUS_SUCCESS;
    }

    DPRINT1("IopArbiterAllocateResources: all %lu configurations failed for %wZ\n",
            RequirementsList->AlternativeLists, &DeviceNode->InstancePath);
    return Status;
}

/* BOOT-CONFIGURATION RESERVATION *******************************************/

/**
 * @brief
 * Builds a single fixed IO requirement from an assigned CM
 * resource descriptor.
 *
 * @param[in] Cm
 * The assigned descriptor to convert.
 *
 * @param[in] AllowForwardingWindow
 * TRUE to convert a bridge forwarding window
 * (CM_RESOURCE_*_WINDOW_DECODE), FALSE to skip it.
 *
 * @param[out] Io
 * Receives the fixed single-placement requirement.
 *
 * @return
 * Returns TRUE with the requirement, FALSE for a type with no
 * arbiter or a skipped forwarding window.
 *
 * @remarks
 * A *forwarding window* is the address range a bus decodes and
 * hands OUT to the devices behind it, not space the bus itself
 * consumes.  Whether it must be reserved depends on WHICH arbiter
 * owns it - and that mirrors the Windows arbiter hierarchy:
 *
 * A PCI-PCI (e.g. AGP) bridge's window is a bounded sub-range that
 * the bridge CONSUMES from its PARENT bus's arbiter and then
 * sub-arbitrates to its own children.  It is boot-allocated in the
 * parent arbiter (ARBITER_FLAG_BOOT_CONFIG) so the firmware window
 * is preserved and re-handed to the bridge, which then programs
 * its Memory/Prefetch Base/Limit OPEN and seeds its child arbiter
 * with it.  If it is NOT reserved, the bridge reaches START with
 * no window, pci.sys writes the disabled 0xFFF0/0x0000 state, its
 * child arbiter is never seeded, and the devices behind it go dark
 * (an AGP GPU's linear framebuffer becomes unreachable).
 * -> reserved (AllowForwardingWindow == TRUE, an ancestor-bus
 * arbiter).
 *
 * The ROOT PCI bus (PNP0A03) IS the top arbiter; its window
 * defines the arbiter's own allocatable pool, not a range consumed
 * from something above.  Reserving it as OWNED would mark the
 * entire pool taken and fail every child allocation (a root that
 * forwards 0-0xFFFF would fail every port request).  Its window
 * resolves to the ntoskrnl ROOT fallback arbiter (the devnode
 * self-skip keeps a bus from arbitrating its own resource).
 * -> never reserved (AllowForwardingWindow == FALSE).
 */
static
BOOLEAN
IopArbiterCmToFixedRequirement(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm,
    _In_ BOOLEAN AllowForwardingWindow,
    _Out_ PIO_RESOURCE_DESCRIPTOR Io)
{
    RtlZeroMemory(Io, sizeof(*Io));
    Io->Option = 0;                 /* required, single placement */
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
            Io->u.Port.Length = Cm->u.Port.Length;
            Io->u.Port.Alignment = 1;
            Io->u.Port.MinimumAddress = Cm->u.Port.Start;
            Io->u.Port.MaximumAddress.QuadPart =
                Cm->u.Port.Start.QuadPart + Cm->u.Port.Length - 1;
            return TRUE;

        case CmResourceTypeMemory:
            Io->u.Memory.Length = Cm->u.Memory.Length;
            Io->u.Memory.Alignment = 1;
            Io->u.Memory.MinimumAddress = Cm->u.Memory.Start;
            Io->u.Memory.MaximumAddress.QuadPart =
                Cm->u.Memory.Start.QuadPart + Cm->u.Memory.Length - 1;
            return TRUE;

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
            Io->u.BusNumber.MaxBusNumber = Cm->u.BusNumber.Start + Cm->u.BusNumber.Length - 1;
            return TRUE;

        default:
            return FALSE;   /* no arbiter for this type */
    }
}

/**
 * @brief
 * Reserves every descriptor of one resource type from a partial
 * list in the arbiter, via BootAllocation.
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
 * The requesting PDO, or NULL for firmware/system resources.
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
 * Returns STATUS_SUCCESS (reservation is best-effort), or
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

    /* Count descriptors this arbiter owns. */
    for (Index = 0; Index < PartialList->Count; ++Index)
    {
        if (IopArbiterTypeMatches(PartialList->PartialDescriptors[Index].Type, ResourceType))
            ++Count;
    }

    if (Count == 0)
        return STATUS_SUCCESS;

    Entries = ExAllocatePoolWithTag(PagedPool, Count * sizeof(ARBITER_LIST_ENTRY), TAG_IO_ARBITER);
    Descriptors = ExAllocatePoolWithTag(PagedPool, Count * sizeof(IO_RESOURCE_DESCRIPTOR), TAG_IO_ARBITER);
    if (Entries == NULL || Descriptors == NULL)
    {
        if (Entries) ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
        if (Descriptors) ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Entries, Count * sizeof(ARBITER_LIST_ENTRY));

    InitializeListHead(&ArbitrationList);
    Count = 0;
    for (Index = 0; Index < PartialList->Count; ++Index)
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
        ++Count;
    }

    if (Count != 0)
        IopArbiterInvoke(Interface, ArbiterActionBootAllocation, &ArbitrationList);

    ExFreePoolWithTag(Entries, TAG_IO_ARBITER);
    ExFreePoolWithTag(Descriptors, TAG_IO_ARBITER);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reserves a device's firmware boot configuration in the arbiters,
 * so those ranges are respected (and handed back to this device)
 * when other devices are assigned.
 *
 * @param[in] DeviceNode
 * The device whose BootResources are reserved.
 *
 * @return
 * Returns STATUS_SUCCESS; reservation is best-effort and
 * descriptors without an arbiter are skipped.
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

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); ++TypeIndex)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface;
        BOOLEAN AllowForwardingWindow;

        if (!NT_SUCCESS(IopFindArbiterForResourceType(DeviceNode, ResourceType, &Interface)))
            continue;

        /*
         * Reserve a forwarding window only when it is owned by a real ANCESTOR-bus
         * arbiter (a PCI-PCI bridge consuming its window from the bus above it), not
         * by the ntoskrnl ROOT fallback (the top PCI bus, whose window IS its own
         * allocatable pool).  IopFindArbiterForResourceType returns the shared root
         * interface for the fallback, so a mismatch means an ancestor bus answered.
         */
        AllowForwardingWindow = (Interface != IopGetRootArbiterInterface(ResourceType));

        IopArbiterReserveType(Interface, ResourceType, PartialList,
                              DeviceNode->PhysicalDeviceObject,
                              BootResources->List[0].InterfaceType,
                              BootResources->List[0].BusNumber,
                              AllowForwardingWindow);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees every arbiter range a device holds - its committed
 * allocations and any reserved boot config, all owned by its PDO
 * across the arbiters - when the device is removed, so the ranges
 * return to the free pool.
 *
 * @param[in] DeviceNode
 * The removed device.
 *
 * @remarks
 * Best-effort per type; harmless (a no-op) for types the device
 * never held.
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

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); ++TypeIndex)
    {
        PARBITER_INTERFACE Interface;

        if (NT_SUCCESS(IopFindArbiterForResourceType(DeviceNode,
                                                     IopArbiterResourceTypes[TypeIndex],
                                                     &Interface)))
        {
            ArbiterLibReleaseResources((PARBITER_INSTANCE)Interface->Context,
                                       DeviceNode->PhysicalDeviceObject);
        }
    }
}

/* ROOT-ARBITER SEEDING FROM THE FIRMWARE RESOURCE MAP **********************/

/**
 * @brief
 * Returns the published ARBITER_INTERFACE of the root arbiter for
 * a resource type.
 *
 * @param[in] ResourceType
 * The CmResourceType* to look up; MemoryLarge resolves to the
 * Memory arbiter.
 *
 * @return
 * Returns the static root interface, or NULL for a type with no
 * root arbiter.
 */
CODE_SEG("PAGE")
static
PARBITER_INTERFACE
IopGetRootArbiterInterface(
    _In_ UCHAR ResourceType)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(IopRootArbiterTable); ++Index)
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
 * Records a device's assigned CM_RESOURCE_LIST in the root
 * arbiters as ordinary (blocking) allocations owned by Owner, so
 * later PnP arbitration sees them.
 *
 * @param[in] ResourceList
 * The assigned resources to record.
 *
 * @param[in] Owner
 * The owning device object, or NULL.
 *
 * @remarks
 * Used for resources assigned outside the PnP transaction (a
 * legacy device's IoAssignResources / IoReportResourceUsage).
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

    for (Index = 0; Index < PartialList->Count; ++Index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(Cm->Type);
        PARBITER_INSTANCE Arbiter;
        ULONGLONG Start = 0;
        ULONGLONG Length = 0;

        if (Interface == NULL)
            continue;

        Arbiter = (PARBITER_INSTANCE)Interface->Context;
        if (Arbiter == NULL || Arbiter->UnpackResource == NULL)
            continue;   /* arbiters not registered yet, or type not unpackable */

        Arbiter->UnpackResource(Cm, &Start, &Length);
        if (Length == 0)
            continue;

        ArbiterLibReserveRange(Arbiter, Start, Start + Length - 1, Owner,
                               Cm->ShareDisposition == CmResourceShareShared);
    }
}

/**
 * @brief
 * Reports whether any descriptor of ResourceList conflicts with an
 * already-committed range in the root arbiters.
 *
 * @param[in] ResourceList
 * The resources to check.
 *
 * @param[out] ConflictingDescriptor
 * If given, receives the first conflicting descriptor.
 *
 * @return
 * Returns TRUE on the first real conflict, FALSE otherwise.
 *
 * @remarks
 * The arbiter-backed replacement for the old registry-scanning
 * conflict detector.  A candidate overlap is NOT a conflict when
 * the two are both shareable, when the committed range belongs to
 * the system (the root PDO owns the firmware/HAL hardware the
 * device also decodes), or when the range is boot-reserved /
 * driver-exclusive-shared.  Owned resources of the requesting
 * device are not yet committed when this runs, so there is no self
 * conflict (unlike the registry scan, which matched a device's own
 * stale entry).
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

    for (Index = 0; Index < PartialList->Count; ++Index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(Cm->Type);
        PARBITER_INSTANCE Arbiter;
        RTL_RANGE_LIST_ITERATOR Iterator;
        PRTL_RANGE Range;
        ULONGLONG Start = 0;
        ULONGLONG Length = 0;

        if (Interface == NULL)
            continue;   /* no arbiter owns this type - not ours to judge */

        Arbiter = (PARBITER_INSTANCE)Interface->Context;
        if (Arbiter == NULL || Arbiter->UnpackResource == NULL)
            continue;   /* arbiters not registered yet, or type not unpackable */

        Arbiter->UnpackResource(Cm, &Start, &Length);
        if (Length == 0)
            continue;

        if (!NT_SUCCESS(RtlGetFirstRange(Arbiter->Allocation, &Iterator, &Range)))
            continue;

        while (Range != NULL)
        {
            if (Range->Start <= (Start + Length - 1) && Range->End >= Start)
            {
                BOOLEAN Allowed = FALSE;

                if (Cm->ShareDisposition == CmResourceShareShared &&
                    (Range->Flags & RTL_RANGE_SHARED))
                {
                    Allowed = TRUE;   /* both shareable */
                }
                else if (RootPdo != NULL && Range->Owner == RootPdo)
                {
                    Allowed = TRUE;   /* the system owns this hardware; the device shares it */
                }
                else if (Range->Attributes & (ARBITER_RANGE_BOOT_ALLOCATED | ARBITER_RANGE_SHARED_DRIVER))
                {
                    Allowed = TRUE;   /* boot-reserved / already driver-exclusive-shared */
                }

                if (!Allowed)
                {
                    if (ConflictingDescriptor != NULL)
                        *ConflictingDescriptor = *Cm;
                    return TRUE;
                }
            }

            if (!NT_SUCCESS(RtlGetNextRange(&Iterator, &Range, TRUE)))
                break;
        }
    }

    return FALSE;
}

/**
 * @brief
 * Device-aware conflict query: for a device that owns
 * PhysicalDeviceObject, asks the owning arbiter
 * (ArbiterActionQueryConflict) whether each resource conflicts
 * with what is already committed.
 *
 * @param[in] PhysicalDeviceObject
 * The device the resources belong to; its own committed ranges are
 * excluded from the check.
 *
 * @param[in] ResourceList
 * The resources to check.
 *
 * @param[out] ConflictingDescriptor
 * If given, receives the first conflicting descriptor.
 *
 * @return
 * Returns TRUE on the first real conflict, FALSE otherwise.
 *
 * @remarks
 * The faithful mechanism (as NT's IopQueryConflictListInternal
 * does): the arbiter's QueryConflict reuses FindSuitableRange, so
 * the full share / driver-exclusive / boot semantics apply.
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
    ULONG Index;

    PAGED_CODE();

    if (ResourceList == NULL || ResourceList->Count == 0 || PhysicalDeviceObject == NULL)
        return FALSE;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    PartialList = &ResourceList->List[0].PartialResourceList;

    for (Index = 0; Index < PartialList->Count; ++Index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm = &PartialList->PartialDescriptors[Index];
        IO_RESOURCE_DESCRIPTOR IoDescriptor;
        PARBITER_INTERFACE Interface = NULL;
        ARBITER_PARAMETERS Parameters;
        ULONG Count = 0;
        PARBITER_CONFLICT_INFO Conflicts = NULL;

        /* Build the fixed IO requirement the arbiter's QueryConflict expects.  A
         * forwarding window is not a leaf range to conflict-check, so keep skipping
         * it here (AllowForwardingWindow == FALSE). */
        if (!IopArbiterCmToFixedRequirement(Cm, FALSE, &IoDescriptor))
            continue;   /* not an arbitrated (or forwarding-window) type */

        /* Discover the owning arbiter (an ancestor bus), falling back to root. */
        if (DeviceNode == NULL ||
            !NT_SUCCESS(IopFindArbiterForResourceType(DeviceNode, Cm->Type, &Interface)))
        {
            Interface = IopGetRootArbiterInterface(Cm->Type);
        }
        if (Interface == NULL || Interface->Context == NULL)
            continue;

        RtlZeroMemory(&Parameters, sizeof(Parameters));
        Parameters.Parameters.QueryConflict.PhysicalDeviceObject = PhysicalDeviceObject;
        Parameters.Parameters.QueryConflict.ConflictingResource = &IoDescriptor;
        Parameters.Parameters.QueryConflict.ConflictCount = &Count;
        Parameters.Parameters.QueryConflict.Conflicts = &Conflicts;

        if (NT_SUCCESS(Interface->ArbiterHandler(Interface->Context,
                                                 ArbiterActionQueryConflict, &Parameters)) &&
            Count > 0)
        {
            if (Conflicts != NULL)
                ExFreePool(Conflicts);
            if (ConflictingDescriptor != NULL)
                *ConflictingDescriptor = *Cm;
            return TRUE;
        }

        if (Conflicts != NULL)
            ExFreePool(Conflicts);
    }

    return FALSE;
}

/**
 * @brief
 * Seeds one resource list of firmware/HAL resources into the root
 * arbiters, owned by the root PDO and tagged boot-reserved.
 *
 * @param[in] ResourceList
 * The resource-map list to seed.
 *
 * @remarks
 * Root-PDO ownership makes the seeded hardware shareable with
 * root-enumerated devices: the driver-exclusive share path then
 * lets another root-enumerated device claim the same hardware
 * (e.g. the ports the kernel debugger reserves), and the conflict
 * check treats the root-owned range as shareable.
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

    for (TypeIndex = 0; TypeIndex < RTL_NUMBER_OF(IopArbiterResourceTypes); ++TypeIndex)
    {
        UCHAR ResourceType = IopArbiterResourceTypes[TypeIndex];
        PARBITER_INTERFACE Interface = IopGetRootArbiterInterface(ResourceType);

        if (Interface != NULL)
        {
            /* Firmware/HAL resources owned by the root PDO seed the ROOT arbiter;
             * a root-bus forwarding window is the arbiter's own pool, never an owned
             * range - keep skipping it (AllowForwardingWindow == FALSE). */
            IopArbiterReserveType(Interface, ResourceType, PartialList, Owner,
                                  ResourceList->List[0].InterfaceType,
                                  ResourceList->List[0].BusNumber,
                                  FALSE);
        }
    }
}

/**
 * @brief
 * Walks one resource-map registry key: reserves every raw
 * CM_RESOURCE_LIST value (skipping the ".Translated" twins), then
 * recurses into the subkeys.
 *
 * @param[in] Key
 * The open resource-map key to walk.
 *
 * @param[in] Depth
 * The current recursion depth.
 *
 * @remarks
 * The resource map is shallow (RESOURCEMAP\<Class>\<Driver>), so a
 * modest depth bound guards against a malformed hive.  Generous
 * fixed buffers; oversized entries are skipped (best-effort).
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
        if (ValueInfo) ExFreePoolWithTag(ValueInfo, TAG_IO_ARBITER);
        if (KeyInfo) ExFreePoolWithTag(KeyInfo, TAG_IO_ARBITER);
        return;
    }

    /* Reserve every raw resource-list value on this key. */
    for (Index = 0; ; ++Index)
    {
        UNICODE_STRING Name;

        Status = ZwEnumerateValueKey(Key, Index, KeyValueFullInformation,
                                     ValueInfo, 2 * PAGE_SIZE, &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(Status))
            continue;   /* too large for the buffer, or transient - skip it */

        if (ValueInfo->Type != REG_RESOURCE_LIST ||
            ValueInfo->DataLength < sizeof(CM_RESOURCE_LIST))
        {
            continue;
        }

        Name.Buffer = ValueInfo->Name;
        Name.Length = (USHORT)ValueInfo->NameLength;
        Name.MaximumLength = Name.Length;
        if (RtlEqualUnicodeString(&Translated, &Name, TRUE))
            continue;   /* the raw twin carries the same ranges */

        IopArbiterSeedResourceList((PCM_RESOURCE_LIST)((PUCHAR)ValueInfo + ValueInfo->DataOffset));
    }

    /* Recurse into subkeys. */
    for (Index = 0; ; ++Index)
    {
        UNICODE_STRING SubName;
        OBJECT_ATTRIBUTES ObjectAttributes;
        HANDLE SubKey;

        Status = ZwEnumerateKey(Key, Index, KeyBasicInformation,
                                KeyInfo, 512, &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(Status))
            continue;

        SubName.Buffer = KeyInfo->Name;
        SubName.Length = (USHORT)KeyInfo->NameLength;
        SubName.MaximumLength = SubName.Length;

        InitializeObjectAttributes(&ObjectAttributes, &SubName,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Key, NULL);
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
 * Seeds the root arbiters with the firmware/HAL/loader resources
 * published under \Registry\Machine\HARDWARE\RESOURCEMAP.
 *
 * @remarks
 * These are the fixed system resources that belong to no PnP
 * device's boot config, so without this the root arbiters could
 * hand a flexible requirement a range the HAL already owns.  Run
 * once, at arbiter registration, before any device is enumerated
 * (so the map holds only system resources, never yet-to-be-assigned
 * PnP devices).
 */
CODE_SEG("INIT")
VOID
NTAPI
IopArbiterSeedFromResourceMap(VOID)
{
    UNICODE_STRING KeyName =
        RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Key;

    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &ObjectAttributes)))
    {
        DPRINT1("Arbiter seeding: RESOURCEMAP not present\n");
        return;
    }

    IopArbiterSeedFromKey(Key, 0);
    ZwClose(Key);
}
