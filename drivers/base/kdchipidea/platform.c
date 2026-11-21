/*
 * PROJECT:     ReactOS ChipIdea Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SoC selection from the load options or the firmware's ACPI tables
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdci.h"
#include <drivers/acpi/acpi.h>

static const KDCI_PLATFORM* const KdCiPlatforms[] =
{
    &KdCiPlatformMsm8916
};

/* Compares a space padded ACPI header field with a name */
static
BOOLEAN
KdCiSameId(
    _In_reads_(8) const CHAR* Field,
    _In_ PCSTR Name)
{
    ULONG i;

    for (i = 0; i < 8 && Name[i] != ANSI_NULL; i++)
    {
        if (RtlUpperChar(Field[i]) != RtlUpperChar(Name[i]))
            return FALSE;
    }

    return (i == 8) || (Field[i] == ' ') || (Field[i] == ANSI_NULL);
}

static
const KDCI_PLATFORM*
KdCiMatchTable(
    _In_ const DESCRIPTION_HEADER* Header)
{
    ULONG i, j;

    for (i = 0; i < RTL_NUMBER_OF(KdCiPlatforms); i++)
    {
        for (j = 0; j < RTL_NUMBER_OF(KdCiPlatforms[i]->AcpiIds); j++)
        {
            if (KdCiPlatforms[i]->AcpiIds[j] != NULL &&
                KdCiSameId((const CHAR*)Header->OEMTableID, KdCiPlatforms[i]->AcpiIds[j]))
            {
                return KdCiPlatforms[i];
            }
        }
    }

    return NULL;
}

/* The DSDT is reached through the FADT; only its header is read, a byte at a time */
static
BOOLEAN
KdCiReadDsdtHeader(
    _In_ const FADT* Fadt,
    _Out_ PDESCRIPTION_HEADER Header)
{
    PHYSICAL_ADDRESS Address;
    volatile UCHAR* Mapped;
    PUCHAR Copy = (PUCHAR)Header;
    ULONG i;

    Address.QuadPart = 0;
    if (Fadt->Header.Length >= FIELD_OFFSET(FADT, x_dsdt) + sizeof(Fadt->x_dsdt))
        Address = Fadt->x_dsdt;
    if (Address.QuadPart == 0)
        Address.QuadPart = Fadt->dsdt;
    if (Address.QuadPart == 0)
        return FALSE;

    Mapped = KdMapPhysicalMemory64(Address, 1);
    if (Mapped == NULL)
        return FALSE;

    for (i = 0; i < sizeof(*Header); i++)
        Copy[i] = Mapped[i];

    KdUnmapVirtualAddress((PVOID)Mapped, 1);
    return Header->Signature == DSDT_SIGNATURE;
}

/**
 * @brief Picks the SoC by name, or by what its firmware's ACPI tables call it.
 *
 * @param[in] Name
 * A SoC name from the load options, or NULL to look at the ACPI tables.
 */
const KDCI_PLATFORM*
NTAPI
KdCiFindPlatform(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_opt_ PCSTR Name)
{
    const KDCI_PLATFORM* Platform = NULL;
    DESCRIPTION_HEADER Dsdt;
    PFADT Fadt;
    ULONG i, j;

    if (Name != NULL)
    {
        for (i = 0; i < RTL_NUMBER_OF(KdCiPlatforms); i++)
        {
            for (j = 0; KdCiPlatforms[i]->Name[j] != ANSI_NULL; j++)
            {
                if (RtlUpperChar(Name[j]) != KdCiPlatforms[i]->Name[j])
                    break;
            }

            if (KdCiPlatforms[i]->Name[j] == ANSI_NULL &&
                (Name[j] == ANSI_NULL || Name[j] == ' ' || Name[j] == '/'))
            {
                return KdCiPlatforms[i];
            }
        }

        return NULL;
    }

    Fadt = KdGetAcpiTablePhase0(LoaderBlock, FADT_SIGNATURE);
    if (Fadt == NULL)
        return NULL;

    if (KdCiReadDsdtHeader(Fadt, &Dsdt))
        Platform = KdCiMatchTable(&Dsdt);
    if (Platform == NULL)
        Platform = KdCiMatchTable(&Fadt->Header);

    KdUnmapVirtualAddress(Fadt, BYTES_TO_PAGES(Fadt->Header.Length));
    return Platform;
}
