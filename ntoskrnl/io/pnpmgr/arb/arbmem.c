/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PnP manager Root Memory Arbiter
 * COPYRIGHT:   Copyright 2025-2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern ARBITER_INSTANCE IopRootMemArbiter;

/* The memory arbiter uses the resource-type-agnostic callbacks from arbgen.c. */
NTSTATUS NTAPI IopSharedUnpackRequirement(PIO_RESOURCE_DESCRIPTOR, PUINT64, PUINT64, PUINT64, PUINT64);
NTSTATUS NTAPI IopSharedPackResource(PIO_RESOURCE_DESCRIPTOR, UINT64, PCM_PARTIAL_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR, PUINT64, PUINT64);
INT32 NTAPI IopSharedScoreRequirement(PIO_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedTranslateOrdering(PIO_RESOURCE_DESCRIPTOR, PIO_RESOURCE_DESCRIPTOR);

#define TAG_ARB_MEM 'MbrA'

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Determines whether a device is a PCI host bridge - the only
 * device the ECAM conflict filter applies to.
 *
 * @param[in] PhysicalDeviceObject
 * The device whose hardware IDs are examined.
 *
 * @param[out] IsRootBus
 * Receives TRUE if the hardware IDs include the ACPI PCI or PCIe
 * root-bus IDs, FALSE otherwise.
 *
 * @return
 * Returns STATUS_SUCCESS, or the failure status of reading the
 * hardware IDs.
 */
static
NTSTATUS
IopMemIsPciRootBus(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PBOOLEAN IsRootBus)
{
    PWSTR HardwareIds;
    PWSTR Id;
    ULONG Length = 0;
    NTSTATUS Status;

    PAGED_CODE();

    *IsRootBus = FALSE;

    Status = IoGetDeviceProperty(PhysicalDeviceObject, DevicePropertyHardwareID,
                                 0, NULL, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        return Status;

    HardwareIds = ExAllocatePoolWithTag(PagedPool, Length, TAG_ARB_MEM);
    if (HardwareIds == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoGetDeviceProperty(PhysicalDeviceObject, DevicePropertyHardwareID,
                                 Length, HardwareIds, &Length);
    if (NT_SUCCESS(Status))
    {
        /* HardwareID is a REG_MULTI_SZ: walk the NUL-separated strings. */
        for (Id = HardwareIds; *Id != UNICODE_NULL; Id += wcslen(Id) + 1)
        {
            if (_wcsicmp(Id, L"ACPI\\PNP0A03") == 0 ||   /* PCI host bridge  */
                _wcsicmp(Id, L"ACPI\\PNP0A08") == 0)     /* PCIe host bridge */
            {
                *IsRootBus = TRUE;
                break;
            }
        }
    }

    ExFreePoolWithTag(HardwareIds, TAG_ARB_MEM);
    return Status;
}

/**
 * @brief
 * Drops every reported conflict that falls inside the ECAM
 * window, for a PCI host bridge only.
 *
 * @param[in] PhysicalDeviceObject
 * The device the conflicts were reported for.
 *
 * @param[in,out] ConflictCount
 * The conflict count, reduced as entries are removed.
 *
 * @param[in,out] Conflicts
 * The conflict array, compacted in place (swap-remove).
 *
 * @remarks
 * The bridge legitimately owns a memory aperture that CONTAINS
 * the config-space window the arbiter itself boot-reserved, so
 * that overlap is not a real conflict. The window is recorded by
 * ArbiterLibAddMmConfigRangeAsBootReserved and tested through
 * ArbiterLibIsConflictWithMmConfigRange.
 */
static
VOID
IopMemFilterEcamConflicts(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Inout_ PULONG ConflictCount,
    _Inout_ PARBITER_CONFLICT_INFO *Conflicts)
{
    PARBITER_CONFLICT_INFO List = *Conflicts;
    BOOLEAN IsRootBus = FALSE;
    ULONG Count = *ConflictCount;
    ULONG Index;

    PAGED_CODE();

    if (List == NULL || Count == 0)
        return;

    /* The filter applies only to PCI host bridges. */
    if (!NT_SUCCESS(IopMemIsPciRootBus(PhysicalDeviceObject, &IsRootBus)) || !IsRootBus)
        return;

    /* Swap-remove every conflict that lies within the ECAM window. */
    Index = 0;
    while (Index < Count)
    {
        if (ArbiterLibIsConflictWithMmConfigRange(List[Index].Start, List[Index].End))
        {
            List[Index] = List[Count - 1];
            --Count;
        }
        else
        {
            ++Index;
        }
    }

    *ConflictCount = Count;
}

/**
 * @brief
 * The memory arbiter's QueryConflict override: runs the library
 * default, then suppresses the false ECAM conflicts a PCI host
 * bridge's aperture would otherwise report.
 *
 * @param[in] Arbiter
 * The memory arbiter instance.
 *
 * @param[in,out] Parameters
 * The action parameters (pre-Vista builds receive the query
 * arguments directly).
 *
 * @return
 * Returns the library QueryConflict status.
 */
#if (NTDDI_VERSION >= NTDDI_VISTA)
static
NTSTATUS
NTAPI
IopMemQueryConflict(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_QUERY_CONFLICT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = ArbiterLibQueryConflict(Arbiter, Parameters);
    if (!NT_SUCCESS(Status))
        return Status;

    IopMemFilterEcamConflicts(Parameters->PhysicalDeviceObject,
                              Parameters->ConflictCount,
                              Parameters->Conflicts);
    return STATUS_SUCCESS;
}
#else
static
NTSTATUS
NTAPI
IopMemQueryConflict(
    _In_ PARBITER_INSTANCE Arbiter,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ PIO_RESOURCE_DESCRIPTOR ConflictingResource,
    _Out_ PULONG ConflictCount,
    _Out_ PARBITER_CONFLICT_INFO *Conflicts)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = ArbiterLibQueryConflict(Arbiter, PhysicalDeviceObject, ConflictingResource,
                                     ConflictCount, Conflicts);
    if (!NT_SUCCESS(Status))
        return Status;

    IopMemFilterEcamConflicts(PhysicalDeviceObject, ConflictCount, Conflicts);
    return STATUS_SUCCESS;
}
#endif

/**
 * @brief
 * Initializes the Root Memory arbiter: the shared port/memory
 * callbacks, the ECAM conflict filter, and the special memory
 * reservations.
 *
 * @return
 * Returns STATUS_SUCCESS, or the failure status of the instance
 * initialization or the page-0 reservation.
 *
 * @remarks
 * Three reservations distinguish the memory arbiter:
 * - physical page 0, never available to any requester;
 * - the PCIe ECAM / MMCONFIG window (boot-reserved, and recorded
 *   for the conflict filter) - absent on a legacy machine;
 * - the firmware-reported inaccessible ranges (everything above
 *   the CPU's physical address width, once the kernel publishes
 *   InaccessibleRange\PhysicalAddress) - carved out as unowned
 *   blocking ranges.
 * The latter two are best-effort: missing registry data reserves
 * nothing.
 */
NTSTATUS
NTAPI
IopArbMemInitialize(VOID)
{
    NTSTATUS Status;

    PAGED_CODE();

    IopRootMemArbiter.Name = L"RootMemory";

    /* Shared port/memory callbacks (arbgen.c). */
    IopRootMemArbiter.UnpackRequirement = IopSharedUnpackRequirement;
    IopRootMemArbiter.PackResource = IopSharedPackResource;
    IopRootMemArbiter.UnpackResource = IopSharedUnpackResource;
    IopRootMemArbiter.ScoreRequirement = IopSharedScoreRequirement;

    /* Memory-specific override: the ECAM conflict filter. */
    IopRootMemArbiter.QueryConflict = IopMemQueryConflict;

    Status = ArbiterLibInitializeInstance(&IopRootMemArbiter,
                                          NULL,
                                          CmResourceTypeMemory,
                                          IopRootMemArbiter.Name,
                                          L"Root",
                                          IopSharedTranslateOrdering);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopArbMemInitialize: Failed with %X\n", Status);
        return Status;
    }

    /* Reserve physical page 0 (never available to any requester). */
    Status = RtlAddRange(IopRootMemArbiter.Allocation, 0ULL, 0xFFFULL, 0, 0, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopArbMemInitialize: reserving page 0 failed %X\n", Status);
        return Status;
    }

    /*
     * Reserve the PCIe ECAM / MMCONFIG window as boot-reserved and record it for
     * the conflict filter above.  Best-effort: a legacy machine has no window.
     */
    Status = ArbiterLibAddMmConfigRangeAsBootReserved(&IopRootMemArbiter,
                                                      IopRootMemArbiter.Allocation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopArbMemInitialize: reserving MmConfigRange failed %X\n", Status);
    }

    /*
     * Carve out the firmware-reported inaccessible ranges (above-MAXPHYADDR).
     * Best-effort: nothing is published until the kernel writes
     * InaccessibleRange\PhysicalAddress at PnP initialization.
     */
    Status = ArbiterLibAddInaccessibleAllocationRange(&IopRootMemArbiter,
                                                      L"Root",
                                                      IopRootMemArbiter.Allocation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopArbMemInitialize: reserving inaccessible ranges failed %X\n", Status);
    }

    return Status;
}
