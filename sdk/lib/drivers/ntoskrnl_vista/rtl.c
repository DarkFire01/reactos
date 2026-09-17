/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rtl functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Counts the bits that are set in a pointer sized value.
 *
 * @param[in] Target
 * The value whose bits are counted.
 *
 * @return
 * The number of bits set in @p Target.
 */
ULONG
NTAPI
RtlNumberOfSetBitsUlongPtr(
    _In_ ULONG_PTR Target)
{
    ULONG Count = 0;

    /* Clearing the lowest set bit each round visits only the bits that are set */
    while (Target != 0)
    {
        Target &= (Target - 1);
        Count++;
    }

    return Count;
}

/**
 * @brief
 * Queries a set of registry values in one call.
 *
 * @param[in] RelativeTo
 * The registry root @p Path is taken from.
 *
 * @param[in] Path
 * The key path to query.
 *
 * @param[in,out] QueryTable
 * The table describing the values to read.
 *
 * @param[in] Context
 * Optional context handed to the query callbacks.
 *
 * @param[in] Environment
 * Optional environment used to expand REG_EXPAND_SZ values.
 *
 * @return
 * The status returned by RtlQueryRegistryValues().
 *
 * @remarks
 * Callers are expected to pair every RTL_QUERY_REGISTRY_DIRECT entry with
 * RTL_QUERY_REGISTRY_TYPECHECK, which is what sets this apart from the plain
 * RtlQueryRegistryValues().
 */
NTSTATUS
NTAPI
RtlQueryRegistryValuesEx(
    _In_ ULONG RelativeTo,
    _In_ PCWSTR Path,
    _Inout_ PRTL_QUERY_REGISTRY_TABLE QueryTable,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Environment)
{
    return RtlQueryRegistryValues(RelativeTo, Path, QueryTable, Context, Environment);
}

/**
 * @brief
 * Tells whether the running system implements a given NTDDI version.
 *
 * @param[in] Version
 * An NTDDI_* constant. Values carrying a service pack level are rejected.
 *
 * @return
 * TRUE when the running system is at least @p Version, FALSE otherwise.
 */
BOOLEAN
NTAPI
RtlIsNtDdiVersionAvailable(
    _In_ ULONG Version)
{
    /* The low word holds the service pack level, which is never accepted */
    if ((Version & 0xFFFF) != 0)
        return FALSE;

    return Version <= ((SharedUserData->NtMajorVersion << 24) |
                       (SharedUserData->NtMinorVersion << 16));
}

/* EOF */
