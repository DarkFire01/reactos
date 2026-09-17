/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rtl functions of Vista+
 * COPYRIGHT:   2026 Justin Miller (justin.miller@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>

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
