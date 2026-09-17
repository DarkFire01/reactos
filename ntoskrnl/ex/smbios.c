/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The firmware's own account of the machine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* The structures, kept whole once the firmware memory holding them is gone */
SMBIOS_TABLE_INFORMATION ExpSmbiosTable;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Adds up the bytes of an SMBIOS anchor, which is built to sum to zero.
 *
 * @param[in] Data
 * The anchor to add up.
 *
 * @param[in] Length
 * Length of @p Data, in bytes.
 *
 * @return
 * The sum of the bytes.
 */
CODE_SEG("INIT")
static
UCHAR
ExpSmbiosChecksum(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Checksum = 0;
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
    {
        Checksum += Data[Index];
    }

    return Checksum;
}

/**
 * @brief
 * Reads an anchor and says where the structures it describes are.
 *
 * @param[in] EntryPoint
 * The anchor to read.
 *
 * @param[out] TableAddress
 * Receives the physical address of the structure table.
 *
 * @param[out] TableLength
 * Receives the length of the structure table, in bytes.
 *
 * @return
 * TRUE when @p EntryPoint is an anchor that names a table, FALSE otherwise.
 *
 * @remarks
 * Also takes down what the anchor says about itself, so that a caller which
 * went looking for one can tell it found nothing from the address staying
 * zero.
 */
CODE_SEG("INIT")
static
BOOLEAN
ExpParseSmbiosEntryPoint(
    _In_ const UCHAR *EntryPoint,
    _Out_ PULONG64 TableAddress,
    _Out_ PULONG TableLength)
{
    PSMBIOS_TABLE_HEADER EntryPoint21;
    PSMBIOS3_TABLE_HEADER EntryPoint30;

    *TableAddress = 0;
    *TableLength = 0;

    /* Check for an SMBIOS 2.1 anchor */
    EntryPoint21 = (PSMBIOS_TABLE_HEADER)EntryPoint;
    if (RtlEqualMemory(EntryPoint21->Signature, "_SM_", 4))
    {
        if ((EntryPoint21->Length < FIELD_OFFSET(SMBIOS_TABLE_HEADER, Revision)) ||
            (EntryPoint21->Length > sizeof(SMBIOS_TABLE_HEADER)))
        {
            return FALSE;
        }

        if (ExpSmbiosChecksum(EntryPoint, EntryPoint21->Length) != 0)
            return FALSE;

        *TableAddress = EntryPoint21->StructureTableAddress;
        *TableLength = EntryPoint21->StructureTableLength;
        ExpSmbiosTable.MajorVersion = EntryPoint21->MajorVersion;
        ExpSmbiosTable.MinorVersion = EntryPoint21->MinorVersion;
        ExpSmbiosTable.DmiRevision = 2;
        return (*TableAddress != 0) && (*TableLength != 0);
    }

    /* Check for an SMBIOS 3.0 anchor */
    EntryPoint30 = (PSMBIOS3_TABLE_HEADER)EntryPoint;
    if (RtlEqualMemory(EntryPoint30->Signature, "_SM3_", 5))
    {
        if ((EntryPoint30->Length < sizeof(SMBIOS3_TABLE_HEADER)) ||
            (EntryPoint30->Length > sizeof(SMBIOS_TABLE_HEADER)))
        {
            return FALSE;
        }

        if (ExpSmbiosChecksum(EntryPoint, EntryPoint30->Length) != 0)
            return FALSE;

        *TableAddress = EntryPoint30->StructureTableAddress;
        *TableLength = EntryPoint30->StructureTableMaximumSize;
        ExpSmbiosTable.MajorVersion = EntryPoint30->MajorVersion;
        ExpSmbiosTable.MinorVersion = EntryPoint30->MinorVersion;
        ExpSmbiosTable.DmiRevision = 3;
        return (*TableAddress != 0) && (*TableLength != 0);
    }

    return FALSE;
}

/**
 * @brief
 * Goes looking for an anchor in the area a PC BIOS leaves one in.
 *
 * @param[out] TableAddress
 * Receives the physical address of the structure table.
 *
 * @param[out] TableLength
 * Receives the length of the structure table, in bytes.
 *
 * @return
 * TRUE when an anchor turned up, FALSE otherwise.
 */
CODE_SEG("INIT")
static
BOOLEAN
ExpSearchSmbiosEntryPoint(
    _Out_ PULONG64 TableAddress,
    _Out_ PULONG TableLength)
{
    static const SIZE_T SearchBase = 0xF0000;
    static const SIZE_T SearchSize = 0x10000;
    PHYSICAL_ADDRESS PhysicalAddress;
    BOOLEAN Found = FALSE;
    PUCHAR Mapping;
    ULONG Offset;

    *TableAddress = 0;
    *TableLength = 0;

    PhysicalAddress.QuadPart = SearchBase;
    Mapping = MmMapIoSpace(PhysicalAddress, SearchSize, MmCached);
    if (Mapping == NULL)
    {
        DPRINT1("Failed to map the range an SMBIOS anchor would be in\n");
        return FALSE;
    }

    /* An anchor sits on a 16 byte boundary */
    for (Offset = 0; Offset <= (SearchSize - sizeof(SMBIOS_TABLE_HEADER)); Offset += 16)
    {
        if (ExpParseSmbiosEntryPoint(Mapping + Offset, TableAddress, TableLength))
        {
            Found = TRUE;
            break;
        }
    }

    MmUnmapIoSpace(Mapping, SearchSize);
    return Found;
}

/**
 * @brief
 * Takes a copy of the SMBIOS structures while they are still there to copy.
 *
 * @param[in] LoaderBlock
 * What the boot loader handed over.
 *
 * @remarks
 * A loader that came up on EFI firmware passes the anchor along, as there is
 * no ROM area to go looking in. Anything else is a machine whose BIOS leaves
 * one where it has always been.
 */
CODE_SEG("INIT")
VOID
NTAPI
ExpInitializeSMBIOS(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    PSMBIOS_TABLE_HEADER EntryPoint;
    ULONG64 TableAddress;
    ULONG TableLength;
    PVOID Mapping;

    RtlZeroMemory(&ExpSmbiosTable, sizeof(ExpSmbiosTable));

    EntryPoint = (LoaderBlock->Extension != NULL)
               ? LoaderBlock->Extension->SMBiosEPSHeader
               : NULL;

    if (EntryPoint != NULL)
    {
        if (!ExpParseSmbiosEntryPoint((const UCHAR *)EntryPoint,
                                      &TableAddress,
                                      &TableLength))
        {
            DPRINT1("The loader handed over an SMBIOS anchor that does not hold up\n");
            return;
        }
    }
    else if (!ExpSearchSmbiosEntryPoint(&TableAddress, &TableLength))
    {
        DPRINT1("No SMBIOS anchor was found\n");
        return;
    }

    ExpSmbiosTable.TableData = ExAllocatePoolWithTag(PagedPool, TableLength, TAG_SMBIOS);
    if (ExpSmbiosTable.TableData == NULL)
    {
        DPRINT1("Failed to allocate %lu bytes for the SMBIOS structures\n", TableLength);
        return;
    }

    PhysicalAddress.QuadPart = TableAddress;
    Mapping = MmMapIoSpace(PhysicalAddress, TableLength, MmCached);
    if (Mapping == NULL)
    {
        DPRINT1("Failed to map the SMBIOS structures at %I64x\n", TableAddress);
        ExFreePoolWithTag(ExpSmbiosTable.TableData, TAG_SMBIOS);
        ExpSmbiosTable.TableData = NULL;
        return;
    }

    RtlCopyMemory(ExpSmbiosTable.TableData, Mapping, TableLength);
    MmUnmapIoSpace(Mapping, TableLength);

    ExpSmbiosTable.TableLength = TableLength;

    DPRINT("SMBIOS %u.%u, %lu bytes of structures at %I64x\n",
           ExpSmbiosTable.MajorVersion,
           ExpSmbiosTable.MinorVersion,
           TableLength,
           TableAddress);
}

/**
 * @brief
 * Walks to the structure that follows a given one.
 *
 * @param[in] Header
 * The structure to walk past.
 *
 * @return
 * The next structure, or NULL once the end of the table is reached.
 *
 * @remarks
 * A structure is its formatted area followed by the strings its fields point
 * at, and the last of those strings is followed by a second terminator. A
 * structure with no strings carries that pair of terminators on its own.
 */
static
PSMBIOS_STRUCTURE_HEADER
ExpNextSmbiosStructure(
    _In_ PSMBIOS_STRUCTURE_HEADER Header)
{
    PUCHAR End = (PUCHAR)ExpSmbiosTable.TableData + ExpSmbiosTable.TableLength;
    PUCHAR Current = (PUCHAR)Header + Header->Length;

    if (Current >= End)
        return NULL;

    while ((Current + 1) < End)
    {
        if ((Current[0] == ANSI_NULL) && (Current[1] == ANSI_NULL))
        {
            Current += 2;
            break;
        }

        Current++;
    }

    if ((Current + sizeof(SMBIOS_STRUCTURE_HEADER)) > End)
        return NULL;

    return (PSMBIOS_STRUCTURE_HEADER)Current;
}

/**
 * @brief
 * Finds the first structure of a given type.
 *
 * @param[in] Type
 * The structure type to look for.
 *
 * @return
 * The structure, or NULL when the firmware describes none of that type.
 */
PSMBIOS_STRUCTURE_HEADER
NTAPI
ExpFindSmbiosStructure(
    _In_ UCHAR Type)
{
    PSMBIOS_STRUCTURE_HEADER Header;
    PUCHAR End;

    if (ExpSmbiosTable.TableData == NULL)
        return NULL;

    End = (PUCHAR)ExpSmbiosTable.TableData + ExpSmbiosTable.TableLength;
    Header = ExpSmbiosTable.TableData;

    while ((Header != NULL) && (((PUCHAR)Header + sizeof(*Header)) <= End))
    {
        /* Type 127 closes the table */
        if (Header->Type == SMBIOS_TYPE_END_OF_TABLE)
            break;

        /* A structure the table has no room for is where the walk stops */
        if ((Header->Length < sizeof(*Header)) ||
            (((PUCHAR)Header + Header->Length) > End))
        {
            break;
        }

        if (Header->Type == Type)
            return Header;

        Header = ExpNextSmbiosStructure(Header);
    }

    return NULL;
}

/**
 * @brief
 * Picks one of the strings a structure carries.
 *
 * @param[in] Header
 * The structure the string belongs to.
 *
 * @param[in] Index
 * The string number, as a field of the structure gives it. Numbering starts
 * at one, and zero means the field names no string.
 *
 * @return
 * The string, or NULL when the structure does not carry that many.
 *
 * @remarks
 * The string is read straight out of the table, so it lives as long as the
 * table does and must not be freed.
 */
PCSTR
NTAPI
ExpGetSmbiosString(
    _In_ PSMBIOS_STRUCTURE_HEADER Header,
    _In_ UCHAR Index)
{
    PUCHAR End = (PUCHAR)ExpSmbiosTable.TableData + ExpSmbiosTable.TableLength;
    PUCHAR Current = (PUCHAR)Header + Header->Length;

    if (Index == 0)
        return NULL;

    while (Current < End)
    {
        PUCHAR String = Current;

        /* An empty string where one was expected closes the set */
        if (*Current == ANSI_NULL)
            return NULL;

        /* Step over this string, terminator included */
        while ((Current < End) && (*Current != ANSI_NULL))
        {
            Current++;
        }

        /* A string the table has no room to terminate is not one to hand out */
        if (Current == End)
            return NULL;

        if (--Index == 0)
            return (PCSTR)String;

        Current++;
    }

    return NULL;
}

/* EOF */
