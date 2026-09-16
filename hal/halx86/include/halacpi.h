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

/* PM timer setup. TimerPort carries the port number; zero takes the FADT values. */
VOID
NTAPI
HaliAcpiTimerInit(
    _In_opt_ PULONG TimerPort,
    _In_ BOOLEAN TimerValExt
);

/* ACPI power management services (acpi/acpidisp.c) */
NTSTATUS
NTAPI
HaliInitPowerManagement(
    _In_ PPM_DISPATCH_TABLE PmDriverDispatchTable,
    _Out_ PPM_DISPATCH_TABLE *PmHalDispatchTable
);

VOID
NTAPI
HalpAcpiPollPowerButton(
    VOID
);

/* acpi/irqtrans.c */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpQueryPicLineTranslator(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length
);

/* The Fixed ACPI Description Table, parsed at phase 0 */
extern FADT HalpFixedAcpiDescTable;

CODE_SEG("INIT")
NTSTATUS
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
