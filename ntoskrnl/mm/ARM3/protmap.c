/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Page protection based mapping and contiguous allocation interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define MI_CACHE_PROTECT_MASK       (PAGE_NOCACHE | PAGE_WRITECOMBINE)

/* Splits a PAGE_* value into its access part and a caching type, FALSE if the pair is not allowed */
static
BOOLEAN
MiProtectToCacheType(
    _In_ ULONG Protect,
    _In_ ULONG AllowedAccess,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    ULONG Access = Protect & ~MI_CACHE_PROTECT_MASK;

    if (!(Access & AllowedAccess) || (Access & (Access - 1)))
        return FALSE;

    switch (Protect & MI_CACHE_PROTECT_MASK)
    {
        case 0:
            *CacheType = MmCached;
            return TRUE;

        case PAGE_NOCACHE:
            *CacheType = MmNonCached;
            return TRUE;

        case PAGE_WRITECOMBINE:
            *CacheType = MmWriteCombined;
            return TRUE;

        default:
            return FALSE;
    }
}

/**
 * @brief Maps device memory with the caching taken from a page protection value.
 *
 * @param[in] PhysicalAddress Start of the physical range.
 * @param[in] NumberOfBytes Length of the range.
 * @param[in] Protect PAGE_READWRITE or PAGE_READONLY, optionally with PAGE_NOCACHE or PAGE_WRITECOMBINE.
 *
 * @return The mapping, or NULL for an executable or malformed protection.
 *
 * @remarks Read only requests get a writable mapping, the system PTEs used for I/O space carry no separate read only form.
 */
PVOID
NTAPI
MmMapIoSpaceEx(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Protect)
{
    MEMORY_CACHING_TYPE CacheType;

    if (!MiProtectToCacheType(Protect, PAGE_READWRITE | PAGE_READONLY, &CacheType))
        return NULL;

    return MmMapIoSpace(PhysicalAddress, NumberOfBytes, CacheType);
}

/**
 * @brief Allocates physically contiguous nonpaged memory, the node preference is ignored on this single node system.
 */
PVOID
NTAPI
MmAllocateContiguousNodeMemory(
    _In_ SIZE_T NumberOfBytes,
    _In_ PHYSICAL_ADDRESS LowestAcceptableAddress,
    _In_ PHYSICAL_ADDRESS HighestAcceptableAddress,
    _In_opt_ PHYSICAL_ADDRESS BoundaryAddressMultiple,
    _In_ ULONG Protect,
    _In_ ULONG PreferredNode)
{
    MEMORY_CACHING_TYPE CacheType;

    UNREFERENCED_PARAMETER(PreferredNode);

    if (!MiProtectToCacheType(Protect, PAGE_READWRITE | PAGE_EXECUTE_READWRITE, &CacheType))
        return NULL;

    return MmAllocateContiguousMemorySpecifyCache(NumberOfBytes,
                                                  LowestAcceptableAddress,
                                                  HighestAcceptableAddress,
                                                  BoundaryAddressMultiple,
                                                  CacheType);
}
