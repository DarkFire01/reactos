/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private HAL definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* GIC CPU interface registers */
#define ARM64_ICC_PMR_EL1       ARM64_SYSREG(3, 0, 4, 6, 0)
#define ARM64_ICC_RPR_EL1       ARM64_SYSREG(3, 0, 12, 11, 3)
#define ARM64_ICC_SGI1R_EL1     ARM64_SYSREG(3, 0, 12, 11, 5)
#define ARM64_ICC_IAR1_EL1      ARM64_SYSREG(3, 0, 12, 12, 0)
#define ARM64_ICC_EOIR1_EL1     ARM64_SYSREG(3, 0, 12, 12, 1)
#define ARM64_ICC_BPR1_EL1      ARM64_SYSREG(3, 0, 12, 12, 3)
#define ARM64_ICC_CTLR_EL1      ARM64_SYSREG(3, 0, 12, 12, 4)
#define ARM64_ICC_SRE_EL1       ARM64_SYSREG(3, 0, 12, 12, 5)
#define ARM64_ICC_IGRPEN1_EL1   ARM64_SYSREG(3, 0, 12, 12, 7)

/* Interrupt IDs */
#define GIC_SGI_COUNT           16
#define GIC_PPI_FIRST           16
#define GIC_SPI_FIRST           32
#define GIC_MAX_INTID           1019
#define GIC_SPURIOUS_INTID      1023

/* Default generic timer interrupt when the GTDT is missing */
#define HALP_VIRTUAL_TIMER_INTID    27

/* Every interrupt gets a GIC priority from its IRQL, higher IRQL is more urgent */
#define HalpIrqlToPriority(Irql)    ((UCHAR)((HIGH_LEVEL - (Irql)) << 4))

/* 10 ms clock, in 100 ns units */
#define HALP_CLOCK_INCREMENT        100000

/* ACPI tables the HAL consumes, see the ACPI 6.5 specification */
#define MADT_SIGNATURE  'CIPA'
#define GTDT_SIGNATURE  'TDTG'

#define MADT_TYPE_GICC          0x0B
#define MADT_TYPE_GICD          0x0C
#define MADT_TYPE_GICR          0x0E

#define FADT_ARM_PSCI_COMPLIANT 0x0001
#define FADT_ARM_PSCI_USE_HVC   0x0002

#include <pshpack1.h>
typedef struct _HALP_MADT
{
    DESCRIPTION_HEADER Header;
    ULONG LocalInterruptControllerAddress;
    ULONG Flags;
    UCHAR Entries[ANYSIZE_ARRAY];
} HALP_MADT, *PHALP_MADT;

typedef struct _HALP_MADT_ENTRY
{
    UCHAR Type;
    UCHAR Length;
} HALP_MADT_ENTRY, *PHALP_MADT_ENTRY;

typedef struct _HALP_MADT_GICC
{
    UCHAR Type;
    UCHAR Length;
    USHORT Reserved;
    ULONG CpuInterfaceNumber;
    ULONG AcpiProcessorUid;
    ULONG Flags;
    ULONG ParkingProtocolVersion;
    ULONG PerformanceInterruptGsiv;
    ULONG64 ParkedAddress;
    ULONG64 PhysicalBaseAddress;
    ULONG64 Gicv;
    ULONG64 Gich;
    ULONG VgicMaintenanceInterrupt;
    ULONG64 GicrBaseAddress;
    ULONG64 Mpidr;
} HALP_MADT_GICC, *PHALP_MADT_GICC;

typedef struct _HALP_MADT_GICD
{
    UCHAR Type;
    UCHAR Length;
    USHORT Reserved;
    ULONG GicId;
    ULONG64 PhysicalBaseAddress;
    ULONG SystemVectorBase;
    UCHAR GicVersion;
    UCHAR Reserved2[3];
} HALP_MADT_GICD, *PHALP_MADT_GICD;

typedef struct _HALP_MADT_GICR
{
    UCHAR Type;
    UCHAR Length;
    USHORT Reserved;
    ULONG64 DiscoveryRangeBaseAddress;
    ULONG DiscoveryRangeLength;
} HALP_MADT_GICR, *PHALP_MADT_GICR;

typedef struct _HALP_GTDT
{
    DESCRIPTION_HEADER Header;
    ULONG64 CntControlBasePhysicalAddress;
    ULONG Reserved;
    ULONG SecureEl1TimerGsiv;
    ULONG SecureEl1TimerFlags;
    ULONG NonSecureEl1TimerGsiv;
    ULONG NonSecureEl1TimerFlags;
    ULONG VirtualEl1TimerGsiv;
    ULONG VirtualEl1TimerFlags;
} HALP_GTDT, *PHALP_GTDT;
#include <poppack.h>

/* Distributor registers, and one redistributor frame pair without virtual LPIs */
#define HALP_GICD_SIZE          0x10000
#define HALP_GICR_FRAME_SIZE    0x20000

/* GICv2 memory mapped CPU interface, GICC_DIR sits in the second page */
#define HALP_GICC_SIZE          0x2000

/* One MCFG allocation */
#define HALP_MAX_ECAM_WINDOWS   16

typedef struct _HALP_ECAM_WINDOW
{
    PHYSICAL_ADDRESS Base;
    USHORT Segment;
    UCHAR StartBus;
    UCHAR EndBus;
} HALP_ECAM_WINDOW, *PHALP_ECAM_WINDOW;

/* What the ACPI tables told us about the platform */
typedef struct _HALP_PLATFORM
{
    PHYSICAL_ADDRESS GicdBase;
    PHYSICAL_ADDRESS GicrBase;
    ULONG GicrLength;
    /* Without a GICR range every GICC entry names the redistributor of its processor */
    ULONG64 GiccMpidr[MAXIMUM_PROCESSORS];
    ULONG64 GiccGicr[MAXIMUM_PROCESSORS];
    ULONG GiccCount;
    PHYSICAL_ADDRESS GiccBase;
    UCHAR GicVersion;
    ULONG TimerIntId;
    USHORT ArmBootFlags;
    PHYSICAL_ADDRESS RootTable;
    HALP_ECAM_WINDOW Ecam[HALP_MAX_ECAM_WINDOWS];
    ULONG EcamCount;
} HALP_PLATFORM, *PHALP_PLATFORM;

extern HALP_PLATFORM HalpPlatform;
extern ULONG HalpBusType;
extern ULONG64 HalpTimerFrequency;
extern ULONG64 HalpProcessorMpidr[MAXIMUM_PROCESSORS];

/* The loader console with DEBUGPORT=SCREEN, only while the boot processor is set up */
extern ULONG (*HalpEarlyPrint)(const char *Format, ...);

#define HalpEarlyTrace(...) \
    do { if (HalpEarlyPrint) HalpEarlyPrint(__VA_ARGS__); } while (0)

/* acpi.c */
VOID
NTAPI
HalpAcpiInitialize(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock);

PVOID
NTAPI
HalAcpiGetTable(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ ULONG Signature);

/* halpnp.c */
#define HALP_INTERRUPT_CONTROLLER_PIC   0
#define HALP_INTERRUPT_CONTROLLER_APIC  1
extern ULONG HalpInterruptControllerType;
extern PWCHAR HalHardwareIdString;

NTSTATUS
NTAPI
HaliInitPowerManagement(
    _In_ PPM_DISPATCH_TABLE PmDriverDispatchTable,
    _Out_ PPM_DISPATCH_TABLE *PmHalDispatchTable);

NTSTATUS
NTAPI
HalpQueryAcpiResourceRequirements(
    _Out_ PIO_RESOURCE_REQUIREMENTS_LIST *Requirements);

NTSTATUS
NTAPI
HalpQueryPicLineTranslator(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length);

/* halpnpdd.c */
NTSTATUS
NTAPI
HaliInitPnpDriver(VOID);

/* mmconfig.c */
NTSTATUS
NTAPI
HalpPublishMmConfigRanges(VOID);

/* cache.s */
ULONG64
HalpCallEfiFirmware(
    _In_ PVOID Function,
    _In_ ULONG64 Argument1,
    _In_ ULONG64 Argument2,
    _In_ ULONG64 Argument3,
    _In_ ULONG64 Argument4,
    _In_ ULONG64 Argument5);

VOID
HalpSweepDcacheRange(
    _In_ PVOID BaseAddress,
    _In_ SIZE_T Length);

/* misc.c */
CODE_SEG("INIT")
VOID
HalpInitializeEfiRuntime(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock);

NTSTATUS
NTAPI
HalpOpenRegistryKey(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE RootKey,
    _In_ PUNICODE_STRING KeyName,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ BOOLEAN Create);

/* kdpci.c, shared with the x86 HALs. It keeps one of these per debug device. */
typedef union _PCI_TYPE1_CFG_CYCLE_BITS
{
    struct
    {
        ULONG InUse:2;
        ULONG RegisterNumber:6;
        ULONG FunctionNumber:3;
        ULONG DeviceNumber:5;
        ULONG BusNumber:8;
        ULONG Reserved2:8;
    };
    ULONG AsULONG;
} PCI_TYPE1_CFG_CYCLE_BITS, *PPCI_TYPE1_CFG_CYCLE_BITS;

CODE_SEG("INIT")
NTSTATUS
NTAPI
HalpSetupPciDeviceForDebugging(
    _In_opt_ PVOID LoaderBlock,
    _Inout_ PDEBUG_DEVICE_DESCRIPTOR PciDevice);

CODE_SEG("INIT")
NTSTATUS
NTAPI
HalpReleasePciDeviceForDebugging(
    _Inout_ PDEBUG_DEVICE_DESCRIPTOR PciDevice);

CODE_SEG("INIT")
ULONG
NTAPI
HalpGetPciDataByOffset(
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

CODE_SEG("INIT")
ULONG
NTAPI
HalpSetPciDataByOffset(
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

CODE_SEG("INIT")
VOID
NTAPI
HalpRegisterPciDebuggingDeviceInfo(VOID);

/* pci.c */
ULONG
NTAPI
HalpPciRawConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Write);

ULONG
NTAPI
HalpPciReadConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

ULONG
NTAPI
HalpPciWriteConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

extern BOOLEAN HalpPCIConfigInitialized;

CODE_SEG("INIT")
ULONG
HalpPhase0GetPciDataByOffset(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER PciSlot,
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

CODE_SEG("INIT")
ULONG
HalpPhase0SetPciDataByOffset(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER PciSlot,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

/* gic.c */
VOID
NTAPI
HalpGicInitializeDistributor(VOID);

VOID
NTAPI
HalpGicInitializeCpu(
    _In_ ULONG ProcessorNumber);

VOID
NTAPI
HalpGicSetPriority(
    _In_ ULONG IntId,
    _In_ UCHAR Priority);

BOOLEAN
NTAPI
HalpGicEnableInterrupt(
    _In_ ULONG IntId,
    _In_ KINTERRUPT_MODE Mode);

VOID
NTAPI
HalpGicDisableInterrupt(
    _In_ ULONG IntId);

VOID
NTAPI
HalpGicSendSgi(
    _In_ ULONG IntId,
    _In_ KAFFINITY TargetSet);

ULONG
NTAPI
HalpGicAcknowledge(VOID);

VOID
NTAPI
HalpGicEndOfInterrupt(
    _In_ ULONG IntId);

VOID
NTAPI
HalpGicSetPriorityMask(
    _In_ UCHAR Priority);

/* irql.c */
VOID
NTAPI
HalpSetPriorityMask(
    _In_ KIRQL Irql);

/* memory.c */
CODE_SEG("INIT")
ULONG64
NTAPI
HalpAllocPhysicalMemory(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ ULONG64 MaxAddress,
    _In_ PFN_NUMBER PageCount,
    _In_ BOOLEAN Aligned);

PVOID
NTAPI
HalpMapPhysicalMemory(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount,
    _In_ BOOLEAN Device);

PVOID
NTAPI
HalpMapPhysicalMemory64(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount);

PVOID
NTAPI
HalpMapDebugMemory(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount);

VOID
NTAPI
HalpUnmapVirtualAddress(
    _In_ PVOID VirtualAddress,
    _In_ PFN_COUNT PageCount);

/* Kernel exports the HAL drives */
VOID
FASTCALL
KeUpdateSystemTime(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ ULONG Increment,
    _In_ KIRQL OldIrql);

/* timer.c */
VOID
NTAPI
HalpInitializeClock(VOID);

VOID
NTAPI
HalpStartClockInterrupt(VOID);
