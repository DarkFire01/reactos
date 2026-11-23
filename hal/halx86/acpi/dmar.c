/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Turn off DMA remapping the firmware left on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <hal.h>
#include <debug.h>

/* GLOBALS ******************************************************************/

#define DMAR_SIGNATURE                  'RAMD'

/* Remapping structure types */
#define DMAR_TYPE_HARDWARE_UNIT         0

/* Remapping hardware unit registers, from the Intel VT-d specification */
#define VTD_REG_CAPABILITY              0x08
#define VTD_REG_GLOBAL_COMMAND          0x18
#define VTD_REG_GLOBAL_STATUS           0x1C
#define VTD_REG_PROTECTED_MEMORY_ENABLE 0x64

#define VTD_CAP_LOW_PMR                 (1ULL << 5)
#define VTD_CAP_HIGH_PMR                (1ULL << 6)

#define VTD_STATUS_TRANSLATION          (1UL << 31)
#define VTD_STATUS_QUEUED_INVALIDATION  (1UL << 26)
#define VTD_STATUS_INTERRUPT_REMAPPING  (1UL << 25)
#define VTD_STATUS_COMPAT_FORMAT        (1UL << 23)

/* GSTS bits that mirror state kept in GCMD, the others report one-shot commands */
#define VTD_STATUS_PERSISTENT           (VTD_STATUS_TRANSLATION | VTD_STATUS_QUEUED_INVALIDATION | \
                                         VTD_STATUS_INTERRUPT_REMAPPING | VTD_STATUS_COMPAT_FORMAT)

#define VTD_PMR_ENABLE                  (1UL << 31)
#define VTD_PMR_STATUS                  (1UL << 0)

/* How long a unit gets to acknowledge a command, in microseconds */
#define VTD_COMMAND_TIMEOUT             100000

#include <pshpack1.h>
typedef struct _DMAR_TABLE
{
    DESCRIPTION_HEADER Header;
    UCHAR HostAddressWidth;
    UCHAR Flags;
    UCHAR Reserved[10];
} DMAR_TABLE, *PDMAR_TABLE;

typedef struct _DMAR_STRUCTURE_HEADER
{
    USHORT Type;
    USHORT Length;
} DMAR_STRUCTURE_HEADER, *PDMAR_STRUCTURE_HEADER;

typedef struct _DMAR_HARDWARE_UNIT
{
    DMAR_STRUCTURE_HEADER Header;
    UCHAR Flags;
    UCHAR Size;
    USHORT Segment;
    ULONGLONG RegisterBase;
} DMAR_HARDWARE_UNIT, *PDMAR_HARDWARE_UNIT;
#include <poppack.h>

C_ASSERT(sizeof(DMAR_TABLE) == 48);
C_ASSERT(sizeof(DMAR_HARDWARE_UNIT) == 16);

/* PRIVATE FUNCTIONS ********************************************************/

/**
 * @brief Waits until the masked status bits of a unit read as Expected.
 *
 * @return TRUE when they did before the timeout.
 */
static
BOOLEAN
HalpWaitVtdStatus(
    _In_ volatile ULONG *Register,
    _In_ ULONG Mask,
    _In_ ULONG Expected)
{
    ULONG Elapsed;

    for (Elapsed = 0; Elapsed < VTD_COMMAND_TIMEOUT; Elapsed += 10)
    {
        if ((READ_REGISTER_ULONG(Register) & Mask) == Expected)
            return TRUE;

        KeStallExecutionProcessor(10);
    }

    return FALSE;
}

/**
 * @brief Reports one remapping unit and turns off translation and protected memory on it.
 */
static
VOID
HalpQuiesceVtdUnit(
    _In_ PDMAR_HARDWARE_UNIT Unit)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    ULONGLONG Capability;
    ULONG Status, Pmr;
    PUCHAR Registers;

    PhysicalAddress.QuadPart = Unit->RegisterBase;
    Registers = MmMapIoSpace(PhysicalAddress, PAGE_SIZE, MmNonCached);
    if (Registers == NULL)
    {
        DPRINT1("DMAR: cannot map the unit at %I64x\n", Unit->RegisterBase);
        return;
    }

    Capability = READ_REGISTER_ULONG((PULONG)(Registers + VTD_REG_CAPABILITY)) |
                 ((ULONGLONG)READ_REGISTER_ULONG((PULONG)(Registers + VTD_REG_CAPABILITY + 4)) << 32);
    Status = READ_REGISTER_ULONG((PULONG)(Registers + VTD_REG_GLOBAL_STATUS));
    Pmr = (Capability & (VTD_CAP_LOW_PMR | VTD_CAP_HIGH_PMR)) ?
          READ_REGISTER_ULONG((PULONG)(Registers + VTD_REG_PROTECTED_MEMORY_ENABLE)) : 0;

    DPRINT1("DMAR: unit %I64x segment %u flags 0x%x, status 0x%08lx, protected memory 0x%08lx\n",
            Unit->RegisterBase, Unit->Segment, Unit->Flags, Status, Pmr);

    /* Nothing in ReactOS programs the page tables, so a live translation only blocks DMA */
    if (Status & VTD_STATUS_TRANSLATION)
    {
        WRITE_REGISTER_ULONG((PULONG)(Registers + VTD_REG_GLOBAL_COMMAND),
                             Status & VTD_STATUS_PERSISTENT & ~VTD_STATUS_TRANSLATION);
        if (HalpWaitVtdStatus((PULONG)(Registers + VTD_REG_GLOBAL_STATUS), VTD_STATUS_TRANSLATION, 0))
            DPRINT1("DMAR: unit %I64x translation turned off\n", Unit->RegisterBase);
        else
            DPRINT1("DMAR: unit %I64x kept translating\n", Unit->RegisterBase);
    }

    /* Pre-boot DMA protection leaves memory regions that every device is blocked from */
    if (Pmr & VTD_PMR_ENABLE)
    {
        WRITE_REGISTER_ULONG((PULONG)(Registers + VTD_REG_PROTECTED_MEMORY_ENABLE), 0);
        if (HalpWaitVtdStatus((PULONG)(Registers + VTD_REG_PROTECTED_MEMORY_ENABLE), VTD_PMR_STATUS, 0))
            DPRINT1("DMAR: unit %I64x protected memory turned off\n", Unit->RegisterBase);
        else
            DPRINT1("DMAR: unit %I64x kept its protected memory\n", Unit->RegisterBase);
    }

    MmUnmapIoSpace(Registers, PAGE_SIZE);
}

/* FUNCTIONS ****************************************************************/

/**
 * @brief Leaves DMA untranslated on every remapping unit, the way an OS that does
 * not use the IOMMU has to find it.
 *
 * @remarks Firmware with pre-boot DMA protection can hand over with translation or
 * protected memory regions still on, which silently drops device DMA to OS memory.
 */
CODE_SEG("INIT")
VOID
NTAPI
HalpQuiesceDmaRemapping(VOID)
{
    PDMAR_STRUCTURE_HEADER Structure;
    PDMAR_TABLE Dmar;
    PUCHAR Cursor, End;

    Dmar = HalAcpiGetTable(NULL, DMAR_SIGNATURE);
    if (Dmar == NULL || Dmar->Header.Length < sizeof(*Dmar))
        return;

    DPRINT1("DMAR: host address width %u, flags 0x%x\n", Dmar->HostAddressWidth + 1, Dmar->Flags);

    Cursor = (PUCHAR)(Dmar + 1);
    End = (PUCHAR)Dmar + Dmar->Header.Length;
    while (Cursor + sizeof(*Structure) <= End)
    {
        Structure = (PDMAR_STRUCTURE_HEADER)Cursor;
        if (Structure->Length < sizeof(*Structure) || Cursor + Structure->Length > End)
            break;

        if (Structure->Type == DMAR_TYPE_HARDWARE_UNIT && Structure->Length >= sizeof(DMAR_HARDWARE_UNIT))
            HalpQuiesceVtdUnit((PDMAR_HARDWARE_UNIT)Structure);

        Cursor += Structure->Length;
    }
}

/* EOF */
