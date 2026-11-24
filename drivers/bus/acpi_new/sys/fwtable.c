/*
 * PROJECT:     ReactOS ACPI driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The ACPI firmware table provider behind GetSystemFirmwareTable
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

#define NDEBUG
#include <debug.h>

#define UACPINT_FIRMWARE_PROVIDER_ACPI          'ACPI'
#define UACPINT_REGISTER_FIRMWARE_TABLE_HANDLER 75

NTSYSAPI
NTSTATUS
NTAPI
ZwSetSystemInformation(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength);

/* Every table uACPI knows of, the root table's entries as well as the DSDT and FACS */
static
NTSTATUS
UacpiNtEnumerateFirmwareTables(
    _Out_writes_bytes_to_(*Length, *Length) PULONG Signatures,
    _Inout_ PULONG Length)
{
    uacpi_size Count = uacpi_table_count();
    uacpi_table Table;
    ULONG Found = 0;
    ULONG Capacity = *Length / sizeof(ULONG);
    uacpi_size i;

    for (i = 0; i < Count; i++)
    {
        if (uacpi_unlikely_error(uacpi_table_get_by_index(i, &Table)))
            continue;

        if (Found < Capacity)
            RtlCopyMemory(&Signatures[Found], Table.hdr->signature, sizeof(ULONG));
        Found++;

        uacpi_table_unref(&Table);
    }

    *Length = Found * sizeof(ULONG);
    return (Found > Capacity) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
}

/* Copies the first table carrying Signature, header included */
static
NTSTATUS
UacpiNtGetFirmwareTable(
    _In_ ULONG Signature,
    _Out_writes_bytes_to_(*Length, *Length) PVOID Buffer,
    _Inout_ PULONG Length)
{
    CHAR Name[sizeof(ULONG) + 1] = { 0 };
    uacpi_table Table;
    ULONG TableLength;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlCopyMemory(Name, &Signature, sizeof(ULONG));
    if (uacpi_unlikely_error(uacpi_table_find_by_signature(Name, &Table)))
        return STATUS_NOT_FOUND;

    TableLength = Table.hdr->length;
    if (*Length < TableLength)
        Status = STATUS_BUFFER_TOO_SMALL;
    else
        RtlCopyMemory(Buffer, Table.ptr, TableLength);

    *Length = TableLength;
    uacpi_table_unref(&Table);
    return Status;
}

static
NTSTATUS
__cdecl
UacpiNtFirmwareTableHandler(
    _Inout_ PSYSTEM_FIRMWARE_TABLE_INFORMATION Info)
{
    switch (Info->Action)
    {
        case SystemFirmwareTable_Enumerate:
            return UacpiNtEnumerateFirmwareTables((PULONG)Info->TableBuffer, &Info->TableBufferLength);

        case SystemFirmwareTable_Get:
            return UacpiNtGetFirmwareTable(Info->TableID, Info->TableBuffer, &Info->TableBufferLength);

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/**
 * @brief Makes the ACPI tables readable through the ACPI firmware table provider.
 */
VOID
NTAPI
UacpiNtRegisterFirmwareTables(VOID)
{
    SYSTEM_FIRMWARE_TABLE_HANDLER Handler;
    NTSTATUS Status;

    RtlZeroMemory(&Handler, sizeof(Handler));
    Handler.ProviderSignature = UACPINT_FIRMWARE_PROVIDER_ACPI;
    Handler.Register = TRUE;
    Handler.FirmwareTableHandler = UacpiNtFirmwareTableHandler;
    Handler.DriverObject = GlobalAcpiDriverObj;

    Status = ZwSetSystemInformation(UACPINT_REGISTER_FIRMWARE_TABLE_HANDLER, &Handler, sizeof(Handler));
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_EXISTS)
        DPRINT1("uACPI-NT: the ACPI firmware table provider did not register, 0x%08lx\n", Status);
}
