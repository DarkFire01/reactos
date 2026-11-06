/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC and ARM64X loader support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Returns the CHPE metadata of an ARM64X or ARM64EC image, if it has one.
 */
PIMAGE_ARM64EC_METADATA
NTAPI
LdrpGetArm64EcMetadata(
    _In_ PVOID ImageBase)
{
    PIMAGE_LOAD_CONFIG_DIRECTORY64 LoadConfig;
    ULONG Size;

    LoadConfig = RtlImageDirectoryEntryToData(ImageBase,
                                              TRUE,
                                              IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG,
                                              &Size);
    if (!LoadConfig)
        return NULL;

    if (Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) ||
        LoadConfig->Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer))
    {
        return NULL;
    }

    return (PIMAGE_ARM64EC_METADATA)(ULONG_PTR)LoadConfig->CHPEMetadataPointer;
}

/**
 * @brief
 * Tells whether an address inside an image is ARM64 or ARM64EC code.
 */
static
BOOLEAN
LdrpIsEcCodeInImage(
    _In_ PVOID ImageBase,
    _In_ ULONG Rva)
{
    PIMAGE_ARM64EC_METADATA Metadata;
    PIMAGE_NT_HEADERS NtHeader;
    PIMAGE_CHPE_RANGE_ENTRY Range;
    ULONG i, Start;

    Metadata = LdrpGetArm64EcMetadata(ImageBase);
    if (!Metadata)
    {
        /* Without a code map the whole image is of its machine type */
        NtHeader = RtlImageNtHeader(ImageBase);
        return NtHeader && NtHeader->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64;
    }

    Range = (PIMAGE_CHPE_RANGE_ENTRY)((ULONG_PTR)ImageBase + Metadata->CodeMap);
    for (i = 0; i < Metadata->CodeMapCount; i++)
    {
        Start = Range[i].StartOffset & ~3UL;
        if (Rva >= Start && Rva - Start < Range[i].Length)
            return (Range[i].StartOffset & 3) != IMAGE_CHPE_RANGE_AMD64;
    }

    return FALSE;
}

/**
 * @brief
 * Tells whether code at an address runs natively, as ARM64 or ARM64EC, or has to be emulated as x64.
 *
 * @param[in] CodePointer
 * The code address.
 *
 * @return
 * TRUE for ARM64 and ARM64EC code, FALSE for x64 code.
 */
BOOLEAN
NTAPI
RtlIsEcCode(
    _In_ ULONG64 CodePointer)
{
    PPEB_LDR_DATA Ldr = NtCurrentPeb()->Ldr;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY ListHead, Entry;
    ULONG_PTR Base;

    if (!Ldr)
        return FALSE;

    /* FIXME: Use a code bitmap, this does not see code outside of images */
    ListHead = &Ldr->InMemoryOrderModuleList;
    for (Entry = ListHead->Flink; Entry != ListHead; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        Base = (ULONG_PTR)LdrEntry->DllBase;

        if (CodePointer >= Base && CodePointer - Base < LdrEntry->SizeOfImage)
            return LdrpIsEcCodeInImage(LdrEntry->DllBase, (ULONG)(CodePointer - Base));
    }

    return FALSE;
}

/* EOF */
