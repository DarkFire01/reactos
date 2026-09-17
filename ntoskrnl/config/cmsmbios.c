/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Configuration Manager - What the firmware calls this machine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"

#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

/*
 * One registry value each, named after the field of the SMBIOS structure it
 * is taken from. The offset is of the byte holding the string number, which
 * only counts when the structure is long enough to have it.
 */
typedef struct _CMP_SMBIOS_VALUE
{
    PCWSTR ValueName;
    UCHAR StructureType;
    UCHAR Offset;
} CMP_SMBIOS_VALUE, *PCMP_SMBIOS_VALUE;

static const CMP_SMBIOS_VALUE CmpSmbiosValues[] =
{
    { L"BIOSVendor",            SMBIOS_TYPE_BIOS_INFORMATION,    4 },
    { L"BIOSVersion",           SMBIOS_TYPE_BIOS_INFORMATION,    5 },
    { L"BIOSReleaseDate",       SMBIOS_TYPE_BIOS_INFORMATION,    8 },
    { L"SystemManufacturer",    SMBIOS_TYPE_SYSTEM_INFORMATION,  4 },
    { L"SystemProductName",     SMBIOS_TYPE_SYSTEM_INFORMATION,  5 },
    { L"SystemVersion",         SMBIOS_TYPE_SYSTEM_INFORMATION,  6 },
    { L"SystemSKU",             SMBIOS_TYPE_SYSTEM_INFORMATION, 25 },
    { L"SystemFamily",          SMBIOS_TYPE_SYSTEM_INFORMATION, 26 },
    { L"BaseBoardManufacturer", SMBIOS_TYPE_BASEBOARD,           4 },
    { L"BaseBoardProduct",      SMBIOS_TYPE_BASEBOARD,           5 },
    { L"BaseBoardVersion",      SMBIOS_TYPE_BASEBOARD,           6 },
};

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Writes one SMBIOS string to the registry.
 *
 * @param[in] KeyHandle
 * The key the value belongs under.
 *
 * @param[in] ValueName
 * Name of the value to write.
 *
 * @param[in] String
 * The string to write.
 */
CODE_SEG("INIT")
static
VOID
CmpSetSmbiosValue(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _In_ PCSTR String)
{
    UNICODE_STRING ValueNameString, Data;
    ANSI_STRING AnsiString;
    NTSTATUS Status;

    RtlInitAnsiString(&AnsiString, String);
    if (AnsiString.Length == 0)
        return;

    Status = RtlAnsiStringToUnicodeString(&Data, &AnsiString, TRUE);
    if (!NT_SUCCESS(Status))
        return;

    RtlInitUnicodeString(&ValueNameString, ValueName);
    NtSetValueKey(KeyHandle,
                  &ValueNameString,
                  0,
                  REG_SZ,
                  Data.Buffer,
                  Data.Length + sizeof(UNICODE_NULL));

    RtlFreeUnicodeString(&Data);
}

/**
 * @brief
 * Puts what the firmware says about this machine under the BIOS key, where a
 * program that cannot read the SMBIOS structures itself goes looking for it.
 *
 * @return
 * STATUS_SUCCESS, or the reason the values could not be written.
 *
 * @remarks
 * A firmware that describes none of this leaves the key empty rather than
 * holding up the boot, since nothing the kernel does depends on it.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
CmpInitializeSmbiosConfiguration(VOID)
{
    PSMBIOS_STRUCTURE_HEADER Structures[SMBIOS_TYPE_BASEBOARD + 1] = {0};
    PSMBIOS_STRUCTURE_HEADER Header;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    HANDLE SystemHandle, BiosHandle;
    NTSTATUS Status;
    ULONG Disposition;
    ULONG Index;
    PCSTR String;

    if (ExpSmbiosTable.TableData == NULL)
        return STATUS_SUCCESS;

    /* Open the hardware description key the BIOS key lives under */
    RtlInitUnicodeString(&KeyName,
                         L"\\Registry\\Machine\\Hardware\\Description\\System");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = NtOpenKey(&SystemHandle, KEY_READ | KEY_WRITE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to open the hardware description key: 0x%lx\n", Status);
        return Status;
    }

    RtlInitUnicodeString(&KeyName, L"BIOS");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               SystemHandle,
                               NULL);
    Status = NtCreateKey(&BiosHandle,
                         KEY_READ | KEY_WRITE,
                         &ObjectAttributes,
                         0,
                         NULL,
                         0,
                         &Disposition);
    NtClose(SystemHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create the BIOS key: 0x%lx\n", Status);
        return Status;
    }

    /* Each of the three structures is looked up the once */
    for (Index = 0; Index < RTL_NUMBER_OF(Structures); Index++)
    {
        Structures[Index] = ExpFindSmbiosStructure((UCHAR)Index);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(CmpSmbiosValues); Index++)
    {
        Header = Structures[CmpSmbiosValues[Index].StructureType];
        if (Header == NULL)
            continue;

        /* A field the firmware's structure is too short to hold is not there */
        if (CmpSmbiosValues[Index].Offset >= Header->Length)
            continue;

        String = ExpGetSmbiosString(Header,
                                    ((PUCHAR)Header)[CmpSmbiosValues[Index].Offset]);
        if (String == NULL)
            continue;

        CmpSetSmbiosValue(BiosHandle, CmpSmbiosValues[Index].ValueName, String);
    }

    NtClose(BiosHandle);
    return STATUS_SUCCESS;
}

/* EOF */
