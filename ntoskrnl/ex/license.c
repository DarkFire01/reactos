/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Product policy (license value) queries
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Reads a named value from the product policy.
 *
 * @param[in] ValueName
 * The name of the policy value.
 *
 * @param[out] Type
 * Optionally receives the registry type of the value.
 *
 * @param[out] Data
 * Optionally receives the value data.
 *
 * @param[in] DataSize
 * The size of @p Data, in bytes.
 *
 * @param[out] ResultDataSize
 * Receives the size of the value data.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a missing name or output size, otherwise
 * STATUS_OBJECT_NAME_NOT_FOUND since ReactOS loads no product policy.
 */
NTSTATUS
NTAPI
ZwQueryLicenseValue(
    _In_ PUNICODE_STRING ValueName,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_to_opt_(DataSize, *ResultDataSize) PVOID Data,
    _In_ ULONG DataSize,
    _Out_ PULONG ResultDataSize)
{
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(DataSize);

    PAGED_CODE();

    if ((ValueName == NULL) || (ResultDataSize == NULL))
        return STATUS_INVALID_PARAMETER;

    if ((ValueName->Buffer == NULL) || (ValueName->Length < sizeof(WCHAR)))
        return STATUS_INVALID_PARAMETER;

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* EOF */
