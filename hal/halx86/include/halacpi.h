#pragma once

#define MAX_PIC_IRQs 16

typedef struct _ISA_IRQ_TO_GSI_OVERRIDE {
    ULONG SourceIRQ;
    ULONG GlobalIRQ;
    ULONG Flags;
    ULONG HandledIrq;
} ISA_IRQ_TO_GSI_OVERRIDE, *PISA_IRQ_TO_GSI_OVERRIDE;

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
