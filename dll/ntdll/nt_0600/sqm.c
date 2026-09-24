/*
 * PROJECT:     ReactOS NT Layer/System API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Software Quality Metrics stream reporting
 */

#include "ntdll_vista.h"

/* FUNCTIONS *****************************************************************/

/**
 * @brief Adds a row to an SQM data stream.
 *
 * There is no SQM event consumer on ReactOS, so the row is never enabled
 * for writing and is dropped, same as Windows with SQM tracing disabled.
 */
VOID
NTAPI
WinSqmAddToStreamEx(
    _In_opt_ PVOID SessionHandle,
    _In_ ULONG DatapointId,
    _In_ ULONG EntryCount,
    _In_reads_opt_(EntryCount) PVOID Entries,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(SessionHandle);
    UNREFERENCED_PARAMETER(DatapointId);
    UNREFERENCED_PARAMETER(EntryCount);
    UNREFERENCED_PARAMETER(Entries);
    UNREFERENCED_PARAMETER(Flags);
}
