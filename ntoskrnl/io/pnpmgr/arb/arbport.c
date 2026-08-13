/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PnP manager Root Port Arbiter
 * COPYRIGHT:   Copyright 2025-2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern ARBITER_INSTANCE IopRootPortArbiter;

/* The port arbiter uses the resource-type-agnostic callbacks from arbgen.c. */
NTSTATUS NTAPI IopSharedUnpackRequirement(PIO_RESOURCE_DESCRIPTOR, PUINT64, PUINT64, PUINT64, PUINT64);
NTSTATUS NTAPI IopSharedPackResource(PIO_RESOURCE_DESCRIPTOR, UINT64, PCM_PARTIAL_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR, PUINT64, PUINT64);
INT32 NTAPI IopSharedScoreRequirement(PIO_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedTranslateOrdering(PIO_RESOURCE_DESCRIPTOR, PIO_RESOURCE_DESCRIPTOR);

/*
 * A card that decodes only the low 10 or 12 address lines answers not just to
 * its own I/O range but to every "alias" a multiple of 0x400 / 0x1000 away, up
 * to the top of the 64 KB port space.  The arbiter must reserve those aliases
 * too, or another device could be handed a port this card silently shadows.
 */
#define PORT_ALIAS_STRIDE_10_BIT    0x400
#define PORT_ALIAS_STRIDE_12_BIT    0x1000
#define PORT_MAX_ADDRESS            0xFFFF

/* Range-list attribute marking a phantom decode alias rather than a primary
 * allocation (boot-reserved ranges use the engine's shared
 * ARBITER_RANGE_BOOT_ALLOCATED attribute from arbiter.h). */
#define ARBITER_ATTRIBUTE_ALIAS     0x10

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Walks the decode-alias chain of a port range: given the last
 * alias (or the base start on the first call), produces the next
 * one.
 *
 * @param[in] DescriptorFlags
 * The requirement's flags; only 10-bit and 12-bit decode
 * requirements have aliases.
 *
 * @param[in] LastAlias
 * The previous alias start, or the primary range's start on the
 * first call.
 *
 * @param[out] NextAlias
 * Receives the next alias start.
 *
 * @return
 * Returns TRUE with the next alias, FALSE when the card decodes
 * fully (no aliases) or the next alias leaves I/O space.
 */
static
BOOLEAN
IopPortGetNextAlias(
    _In_ ULONG DescriptorFlags,
    _In_ ULONGLONG LastAlias,
    _Out_ PULONGLONG NextAlias)
{
    ULONG Stride;
    ULONGLONG Next;

    PAGED_CODE();

    if (DescriptorFlags & CM_RESOURCE_PORT_10_BIT_DECODE)
        Stride = PORT_ALIAS_STRIDE_10_BIT;
    else if (DescriptorFlags & CM_RESOURCE_PORT_12_BIT_DECODE)
        Stride = PORT_ALIAS_STRIDE_12_BIT;
    else
        return FALSE;

    Next = LastAlias + Stride;
    if (Next > PORT_MAX_ADDRESS)
        return FALSE;

    *NextAlias = Next;
    return TRUE;
}

/**
 * @brief
 * The port arbiter's FindSuitableRange override: the generic
 * search with the port twist that legacy and boot-config requests
 * may reclaim boot-reserved space.
 *
 * @param[in] Arbiter
 * The port arbiter instance.
 *
 * @param[in,out] ArbState
 * The allocation state of the entry being placed. On success,
 * Start and End receive the placement.
 *
 * @return
 * Returns TRUE if a placement was found (or granted by the
 * inherited OverrideConflict - a fixed requirement reclaiming its
 * own boot range), FALSE otherwise.
 */
static
BOOLEAN
NTAPI
IopPortFindSuitableRange(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE ArbState)
{
    PARBITER_ALTERNATIVE Alternative = ArbState->CurrentAlternative;
    PARBITER_LIST_ENTRY Entry = ArbState->Entry;
    UCHAR AvailableAttributes = 0;

    PAGED_CODE();

    /* A zero-length "null request" just anchors at the window base. */
    if (Alternative->Length == 0)
    {
        ArbState->Start = ArbState->CurrentMinimum;
        ArbState->End = ArbState->CurrentMinimum;
        return TRUE;
    }

    if (Entry->RequestSource == ArbiterRequestLegacyReported ||
        Entry->RequestSource == ArbiterRequestLegacyAssigned ||
        (Entry->Flags & ARBITER_FLAG_BOOT_CONFIG))
    {
        AvailableAttributes = ARBITER_RANGE_BOOT_ALLOCATED;
    }

    if (ArbState->CurrentMinimum > ArbState->CurrentMaximum)
        return FALSE;

    if (NT_SUCCESS(RtlFindRange(Arbiter->PossibleAllocation,
                                ArbState->CurrentMinimum,
                                ArbState->CurrentMaximum,
                                (ULONG)Alternative->Length,
                                (ULONG)(Alternative->Alignment ? Alternative->Alignment : 1),
                                (Alternative->Flags & ARBITER_ALTERNATIVE_FLAG_SHARED) ? RTL_RANGE_LIST_SHARED_OK : 0,
                                AvailableAttributes,
                                Arbiter->ConflictCallbackContext,
                                Arbiter->ConflictCallback,
                                &ArbState->Start)) ||
        Arbiter->OverrideConflict(Arbiter, ArbState))
    {
        ArbState->End = ArbState->Start + Alternative->Length - 1;
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * The port arbiter's AddAllocation override: commits the granted
 * range into the scratch list, plus - for a 10-/12-bit ISA card -
 * every decode alias, so nothing else is placed on a port this
 * card would shadow.
 *
 * @param[in] Arbiter
 * The port arbiter instance.
 *
 * @param[in,out] ArbState
 * The allocation state carrying the granted Start/End. Alias
 * ranges are tagged ARBITER_ATTRIBUTE_ALIAS and owned by the same
 * device.
 */
static
VOID
NTAPI
IopPortAddAllocation(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE ArbState)
{
    PARBITER_ALTERNATIVE Alternative = ArbState->CurrentAlternative;
    PVOID Owner = ArbState->Entry->PhysicalDeviceObject;
    ULONG AddFlags;
    ULONGLONG Alias;
    ULONGLONG LastAlias;

    PAGED_CODE();

    AddFlags = RTL_RANGE_LIST_ADD_IF_CONFLICT |
               ((Alternative->Flags & ARBITER_ALTERNATIVE_FLAG_SHARED) ? RTL_RANGE_LIST_ADD_SHARED : 0);

    RtlAddRange(Arbiter->PossibleAllocation,
                ArbState->Start,
                ArbState->End,
                ArbState->RangeAttributes,
                AddFlags,
                NULL,
                Owner);

    LastAlias = ArbState->Start;
    while (IopPortGetNextAlias(Alternative->Descriptor->Flags, LastAlias, &Alias))
    {
        RtlAddRange(Arbiter->PossibleAllocation,
                    Alias,
                    Alias + Alternative->Length - 1,
                    ArbState->RangeAttributes | ARBITER_ATTRIBUTE_ALIAS,
                    AddFlags,
                    NULL,
                    Owner);

        LastAlias = Alias;
    }
}

/**
 * @brief
 * The port arbiter's BacktrackAllocation override: the exact
 * inverse of IopPortAddAllocation.
 *
 * @param[in] Arbiter
 * The port arbiter instance.
 *
 * @param[in,out] ArbState
 * The allocation state whose placement is being withdrawn.
 *
 * @remarks
 * The alias chain is regenerated and every alias range removed
 * BEFORE the primary range - without this the aliases would leak
 * into the scratch list on every backtrack and corrupt all later
 * placement decisions.
 */
static
VOID
NTAPI
IopPortBacktrackAllocation(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE ArbState)
{
    PARBITER_ALTERNATIVE Alternative = ArbState->CurrentAlternative;
    PVOID Owner = ArbState->Entry->PhysicalDeviceObject;
    ULONGLONG Alias;
    ULONGLONG LastAlias;

    PAGED_CODE();

    LastAlias = ArbState->Start;
    while (IopPortGetNextAlias(Alternative->Descriptor->Flags, LastAlias, &Alias))
    {
        RtlDeleteRange(Arbiter->PossibleAllocation,
                       Alias,
                       Alias + Alternative->Length - 1,
                       Owner);

        LastAlias = Alias;
    }

    RtlDeleteRange(Arbiter->PossibleAllocation,
                   ArbState->Start,
                   ArbState->End,
                   Owner);
}

/**
 * @brief
 * Initializes the Root Port arbiter: the shared port/memory
 * callbacks plus the port-specific placement overrides (boot
 * leniency and ISA decode aliasing).
 *
 * @return
 * Returns STATUS_SUCCESS, or the instance initialization failure
 * status.
 */
NTSTATUS
NTAPI
IopArbPortInitialize(VOID)
{
    NTSTATUS Status;

    PAGED_CODE();

    IopRootPortArbiter.Name = L"RootPort";

    /* Shared port/memory callbacks (arbgen.c). */
    IopRootPortArbiter.UnpackRequirement = IopSharedUnpackRequirement;
    IopRootPortArbiter.PackResource = IopSharedPackResource;
    IopRootPortArbiter.UnpackResource = IopSharedUnpackResource;
    IopRootPortArbiter.ScoreRequirement = IopSharedScoreRequirement;

    /* Port-specific placement: boot leniency + ISA decode aliasing. */
    IopRootPortArbiter.FindSuitableRange = IopPortFindSuitableRange;
    IopRootPortArbiter.AddAllocation = IopPortAddAllocation;
    IopRootPortArbiter.BacktrackAllocation = IopPortBacktrackAllocation;

    Status = ArbiterLibInitializeInstance(&IopRootPortArbiter,
                                          NULL,
                                          CmResourceTypePort,
                                          IopRootPortArbiter.Name,
                                          L"Root",
                                          IopSharedTranslateOrdering);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopArbPortInitialize: Failed with %X\n", Status);
    }

    return Status;
}
