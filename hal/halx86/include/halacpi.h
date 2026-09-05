#pragma once

//
// Internal HAL structure
//
typedef struct _ACPI_CACHED_TABLE
{
    LIST_ENTRY Links;
    DESCRIPTION_HEADER Header;
    // table follows
    // ...
} ACPI_CACHED_TABLE, *PACPI_CACHED_TABLE;

NTSTATUS
NTAPI
HalpAcpiTableCacheInit(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);

PVOID
NTAPI
HalpAcpiGetTable(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN ULONG Signature
);

/*
 * The ACPI power management timer setup (acpi/halacpi.c). A NULL TimerPort
 * means the values come from the FADT.
 */
VOID
NTAPI
HaliAcpiTimerInit(
    _In_opt_ PULONG TimerPort,
    _In_ BOOLEAN TimerValExt
);

/* The Fixed ACPI Description Table, parsed at phase 0 */
extern FADT HalpFixedAcpiDescTable;

/*
 * Publishes this machine's PCIe MMCONFIG (ECAM) windows under the arbiters'
 * ReservedResources key, so that the root memory arbiter boot-reserves them.
 */
VOID
NTAPI
HalpPublishMmConfigRanges(
    VOID
);

CODE_SEG("INIT")
NTSTATUS
NTAPI
HalpSetupAcpiPhase0(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);

PVOID
NTAPI
HalAcpiGetTable(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN ULONG Signature
);

/* EOF */
