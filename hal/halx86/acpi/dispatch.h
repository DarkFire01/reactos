/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI power-management dispatch tables shared with the ACPI driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * The ACPI bus driver and the HAL exchange two tables through
 * HalInitPowerManagement: the driver hands the HAL its callbacks (SCI/GPE
 * control), the HAL hands back the routines the driver needs for machine
 * state, PCI configuration access and raw ACPI table access. Both layouts
 * are a binary contract with the driver and must not be reordered.
 */

/* Per-state SLP_TYP values the driver hands to the HAL, one triple per
   sleep state S1..S5 */
typedef struct _HAL_ACPI_SLEEP_STATE
{
    UCHAR Supported;
    UCHAR SlpTypA;
    UCHAR SlpTypB;
} HAL_ACPI_SLEEP_STATE, *PHAL_ACPI_SLEEP_STATE;

#define HAL_ACPI_SLEEP_STATE_COUNT 5

/* Interrupt models reported back to the driver */
#define HAL_ACPI_INTERRUPT_MODEL_PIC    0
#define HAL_ACPI_INTERRUPT_MODEL_APIC   1

/* HAL_ACPI_DISPATCH_TABLE.Signature / Version */
#define HAL_ACPI_DISPATCH_SIGNATURE     0x48435049  /* 'IPCH' */
#define HAL_ACPI_DISPATCH_VERSION       3

typedef VOID
(NTAPI *PHAL_ACPI_TIMER_INIT)(
    _In_ PULONG TimerPort,
    _In_ BOOLEAN TimerValExt);

typedef VOID
(NTAPI *PHAL_ACPI_TIMER_CARRY)(
    VOID);

typedef VOID
(NTAPI *PHAL_ACPI_MACHINE_STATE_INIT)(
    _In_ PHAL_ACPI_SLEEP_STATE SleepStates,
    _Out_ PULONG InterruptModel);

typedef ULONG
(NTAPI *PHAL_ACPI_QUERY_FLAGS)(
    VOID);

typedef BOOLEAN
(NTAPI *PHAL_ACPI_PIC_STATE_INTACT)(
    VOID);

typedef VOID
(NTAPI *PHAL_ACPI_RESTORE_INTERRUPT_CONTROLLER)(
    VOID);

typedef ULONG
(NTAPI *PHAL_ACPI_PCI_CONFIG_ACCESS)(
    _In_ PBUS_HANDLER RootBusHandler,
    _In_ ULONG BusNumber,
    _In_ PCI_SLOT_NUMBER SlotNumber,
    _In_ PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

typedef ULONG
(NTAPI *PHAL_ACPI_GET_APIC_VERSION)(
    _In_ ULONG InterruptBase);

typedef VOID
(NTAPI *PHAL_ACPI_SET_MAX_LEGACY_PCI_BUS)(
    _In_ ULONG BusNumber);

typedef BOOLEAN
(NTAPI *PHAL_ACPI_IS_VECTOR_VALID)(
    _In_ ULONG Vector);

typedef PVOID
(NTAPI *PHAL_ACPI_GET_TABLE)(
    _In_ ULONG Signature,
    _In_opt_ PCSTR OemId,
    _In_opt_ PCSTR OemTableId);

typedef PVOID
(NTAPI *PHAL_ACPI_GET_POINTER)(
    VOID);

typedef struct _HAL_ACPI_DISPATCH_TABLE
{
    ULONG Signature;
    ULONG Version;
    PHAL_ACPI_TIMER_INIT TimerInit;
    PHAL_ACPI_TIMER_CARRY TimerCarry;
    PHAL_ACPI_MACHINE_STATE_INIT MachineStateInit;
    PHAL_ACPI_QUERY_FLAGS QueryFlags;
    PHAL_ACPI_PIC_STATE_INTACT PicStateIntact;
    PHAL_ACPI_RESTORE_INTERRUPT_CONTROLLER RestoreInterruptController;
    PHAL_ACPI_PCI_CONFIG_ACCESS PciReadConfig;
    PHAL_ACPI_PCI_CONFIG_ACCESS PciWriteConfig;
    PHAL_ACPI_GET_APIC_VERSION GetApicVersion;
    PHAL_ACPI_SET_MAX_LEGACY_PCI_BUS SetMaxLegacyPciBusNumber;
    PHAL_ACPI_IS_VECTOR_VALID IsVectorValid;
    PHAL_ACPI_GET_TABLE GetTable;
    PHAL_ACPI_GET_POINTER GetRsdp;
    PHAL_ACPI_GET_POINTER GetFacsMapping;
    PHAL_ACPI_GET_POINTER GetAllTables;
} HAL_ACPI_DISPATCH_TABLE, *PHAL_ACPI_DISPATCH_TABLE;

/* The driver's side of the handshake */
#define ACPI_DRIVER_DISPATCH_SIGNATURE  0x41435049  /* 'IPCA' */

typedef NTSTATUS
(NTAPI *PACPI_DRIVER_ENABLE_DISABLE_GPE)(
    _In_ ULONG Enable);

typedef NTSTATUS
(NTAPI *PACPI_DRIVER_INIT_ENABLE_ACPI)(
    _In_ ULONG Flags);

typedef NTSTATUS
(NTAPI *PACPI_DRIVER_GPE_ENABLE_WAKE)(
    _In_ ULONG Enable);

typedef VOID
(NTAPI *PACPI_DRIVER_MARK_HIBER_PHASE)(
    VOID);

typedef struct _ACPI_DRIVER_DISPATCH_TABLE
{
    ULONG Signature;
    ULONG Version;
    PACPI_DRIVER_ENABLE_DISABLE_GPE EnableDisableGpeEvents;
    PACPI_DRIVER_INIT_ENABLE_ACPI InitEnableAcpi;
    PACPI_DRIVER_GPE_ENABLE_WAKE GpeEnableWakeEvents;
    PACPI_DRIVER_MARK_HIBER_PHASE MarkHiberPhase;
} ACPI_DRIVER_DISPATCH_TABLE, *PACPI_DRIVER_DISPATCH_TABLE;

extern PACPI_DRIVER_DISPATCH_TABLE HalpAcpiDriverDispatch;

/* EOF */
