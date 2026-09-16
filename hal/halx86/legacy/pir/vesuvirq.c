/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PicoPower Vesuvius PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/*
 * The route registers sit behind the 0x24/0x26 index and data pair. Links 1
 * to 4 are the nibbles of indexes 0x10 and 0x11, lowest link first, and bits
 * 0 to 3 of index 0x12 are cleared for the links that are level triggered.
 */
#define VESUVIUS_INDEX_PORT     0x24
#define VESUVIUS_DATA_PORT      0x26
#define VESUVIUS_ROUTE_INDEX    0x10
#define VESUVIUS_EDGE_INDEX     0x12
#define VESUVIUS_EDGE_LINKS     0x0F
#define VESUVIUS_LINKS          4

/* The index port is put back afterwards, since other code may use the pair */
static
UCHAR
NTAPI
HalpVesuviusAccess(
    _In_ UCHAR Index,
    _In_ BOOLEAN Write,
    _In_ UCHAR Value)
{
    UCHAR PreviousIndex = __inbyte(VESUVIUS_INDEX_PORT);

    __outbyte(VESUVIUS_INDEX_PORT, Index);

    if (Write)
        __outbyte(VESUVIUS_DATA_PORT, Value);
    else
        Value = __inbyte(VESUVIUS_DATA_PORT);

    __outbyte(VESUVIUS_INDEX_PORT, PreviousIndex);
    return Value;
}

/* Returns the IRQs of all four links, one nibble each with link 1 lowest */
static
USHORT
NTAPI
HalpVesuviusReadRoutes(VOID)
{
    USHORT Routes;

    Routes = HalpVesuviusAccess(VESUVIUS_ROUTE_INDEX, FALSE, 0);
    Routes |= (USHORT)(HalpVesuviusAccess(VESUVIUS_ROUTE_INDEX + 1, FALSE, 0) << 8);

    return Routes;
}

static
UCHAR
NTAPI
HalpVesuviusRouteOf(
    _In_ USHORT Routes,
    _In_ ULONG LinkIndex)
{
    return (UCHAR)((Routes >> (LinkIndex * 4)) & 0x0F);
}

static
NTSTATUS
NTAPI
HalpVesuviusValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= VESUVIUS_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpVesuviusGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (Link == 0 || Link > VESUVIUS_LINKS)
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpVesuviusRouteOf(HalpVesuviusReadRoutes(), Link - 1);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpVesuviusSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Index, Shift, Value;

    if (Link == 0 || Link > VESUVIUS_LINKS)
        return STATUS_INVALID_PARAMETER;

    Index = (UCHAR)(VESUVIUS_ROUTE_INDEX + (Link - 1) / 2);
    Shift = ((Link - 1) % 2) ? 4 : 0;

    Value = HalpVesuviusAccess(Index, FALSE, 0);
    Value = (UCHAR)((Value & ~(0x0F << Shift)) | ((Irq & 0x0F) << Shift));

    HalpVesuviusAccess(Index, TRUE, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpVesuviusGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    UCHAR Edge = HalpVesuviusAccess(VESUVIUS_EDGE_INDEX, FALSE, 0);
    USHORT Routes = HalpVesuviusReadRoutes();
    USHORT Level = 0;
    ULONG LinkIndex;
    UCHAR Irq;

    for (LinkIndex = 0; LinkIndex < VESUVIUS_LINKS; LinkIndex++)
    {
        Irq = HalpVesuviusRouteOf(Routes, LinkIndex);

        if (Irq != 0 && !(Edge & (1 << LinkIndex)))
            Level |= 1 << Irq;
    }

    *LevelIrqs = Level;
    return STATUS_SUCCESS;
}

/*
 * Only the links can be level triggered, so every level IRQ has to belong to
 * one. The upper bits of the edge register are written back as zero.
 */
static
NTSTATUS
NTAPI
HalpVesuviusSetTrigger(
    _In_ USHORT LevelIrqs)
{
    UCHAR Edge = HalpVesuviusAccess(VESUVIUS_EDGE_INDEX, FALSE, 0) & VESUVIUS_EDGE_LINKS;
    USHORT Routes = HalpVesuviusReadRoutes();
    USHORT Covered = 0;
    ULONG LinkIndex;
    UCHAR Irq;

    for (LinkIndex = 0; LinkIndex < VESUVIUS_LINKS; LinkIndex++)
    {
        Irq = HalpVesuviusRouteOf(Routes, LinkIndex);

        if (Irq != 0 && (LevelIrqs & (1 << Irq)))
        {
            Edge &= ~(1 << LinkIndex);
            Covered |= 1 << Irq;
        }
        else
        {
            Edge |= 1 << LinkIndex;
        }
    }

    if (LevelIrqs & ~Covered)
        return STATUS_UNSUCCESSFUL;

    HalpVesuviusAccess(VESUVIUS_EDGE_INDEX, TRUE, Edge);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpVesuviusRouter =
{
    HalpVesuviusValidateTable,
    HalpVesuviusGetIrq,
    HalpVesuviusSetIrq,
    HalpVesuviusGetTrigger,
    HalpVesuviusSetTrigger
};
