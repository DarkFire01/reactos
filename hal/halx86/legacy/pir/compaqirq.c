/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Compaq PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* The OSB and CMC-2 route registers sit behind an index and data port pair */
#define CPQ_INDEX_PORT          0xC00
#define CPQ_DATA_PORT           0xC01

/* OSB links 1 to 4 are the indexes 4 to 7, with the IRQ in the low nibble */
#define CPQ_OSB_LINKS           4
#define CPQ_OSB_FIRST_INDEX     4

/* CMC-2 links 1 to 6 are scattered, with the IRQ in the high nibble */
#define CPQ_CMC2_LINKS          6

static const UCHAR HalpCompaqCmc2Indexes[CPQ_CMC2_LINKS] = { 0x00, 0x01, 0x06, 0x07, 0x04, 0x05 };

/* A MISC-3 link picks an entry of the 0xAE/0xAF pair on one of two functions */
#define CPQ_MISC3_INDEX         0xAE
#define CPQ_MISC3_DATA          0xAF
#define CPQ_MISC3_DISABLED      0x01
#define CPQ_MISC3_ROUTER_FIRST  0x0A
#define CPQ_MISC3_ROUTER_LAST   0x0C
#define CPQ_MISC3_BRIDGE_FIRST  0x14
#define CPQ_MISC3_BRIDGE_LAST   0x19
#define CPQ_MISC3_TABLE_FIRST   0x08
#define CPQ_MISC3_BRIDGE_DEVICE 15

/* The index port is shared, so its previous contents are put back */
static
UCHAR
HalpCompaqIndexedRead(
    _In_ UCHAR Index)
{
    UCHAR Saved = __inbyte(CPQ_INDEX_PORT);
    UCHAR Value;

    __outbyte(CPQ_INDEX_PORT, Index);
    Value = __inbyte(CPQ_DATA_PORT);
    __outbyte(CPQ_INDEX_PORT, Saved);

    return Value;
}

static
VOID
HalpCompaqIndexedUpdate(
    _In_ UCHAR Index,
    _In_ BOOLEAN High,
    _In_ UCHAR Irq)
{
    UCHAR Saved = __inbyte(CPQ_INDEX_PORT);

    __outbyte(CPQ_INDEX_PORT, Index);
    __outbyte(CPQ_DATA_PORT, HalpPirSetNibble(__inbyte(CPQ_DATA_PORT), High, Irq));
    __outbyte(CPQ_INDEX_PORT, Saved);
}

/* OSB ***********************************************************************/

static
NTSTATUS
NTAPI
HalpCompaqOsbValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= CPQ_OSB_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpCompaqOsbGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Index;

    if (Link == 0 || Link > CPQ_OSB_LINKS)
        return STATUS_INVALID_PARAMETER;

    Index = (UCHAR)(CPQ_OSB_FIRST_INDEX + Link - 1);

    *Irq = HalpPirGetNibble(HalpCompaqIndexedRead(Index), FALSE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpCompaqOsbSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Index;

    if (Link == 0 || Link > CPQ_OSB_LINKS)
        return STATUS_INVALID_PARAMETER;

    Index = (UCHAR)(CPQ_OSB_FIRST_INDEX + Link - 1);

    HalpCompaqIndexedUpdate(Index, FALSE, Irq);
    return STATUS_SUCCESS;
}

/* CMC-2 *********************************************************************/

static
NTSTATUS
NTAPI
HalpCompaqCmc2ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= CPQ_CMC2_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpCompaqCmc2GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (Link == 0 || Link > CPQ_CMC2_LINKS)
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpPirGetNibble(HalpCompaqIndexedRead(HalpCompaqCmc2Indexes[Link - 1]), TRUE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpCompaqCmc2SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (Link == 0 || Link > CPQ_CMC2_LINKS)
        return STATUS_INVALID_PARAMETER;

    HalpCompaqIndexedUpdate(HalpCompaqCmc2Indexes[Link - 1], TRUE, Irq);
    return STATUS_SUCCESS;
}

/* MISC-3 ********************************************************************/

/* Links 8 and 9 are accepted in the table, but they cannot be steered */
static
BOOLEAN
NTAPI
HalpCompaqMisc3IsTableLink(
    _In_ UCHAR Link)
{
    return (Link >= CPQ_MISC3_TABLE_FIRST && Link <= CPQ_MISC3_ROUTER_LAST) ||
           (Link >= CPQ_MISC3_BRIDGE_FIRST && Link <= CPQ_MISC3_BRIDGE_LAST);
}

/* The second set of entries lives on bus 0, device 15, function 0 */
static
UCHAR
HalpCompaqMisc3Read(
    _In_ BOOLEAN OnBridge,
    _In_ ULONG Offset)
{
    PCI_SLOT_NUMBER Slot;
    UCHAR Value = MAXUCHAR;

    if (!OnBridge)
        return HalpPirReadRouterByte(Offset);

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = CPQ_MISC3_BRIDGE_DEVICE;
    HalGetBusDataByOffset(PCIConfiguration, 0, Slot.u.AsULONG, &Value, Offset, sizeof(Value));

    return Value;
}

static
VOID
HalpCompaqMisc3Write(
    _In_ BOOLEAN OnBridge,
    _In_ ULONG Offset,
    _In_ UCHAR Value)
{
    PCI_SLOT_NUMBER Slot;

    if (!OnBridge)
    {
        HalpPirWriteRouterByte(Offset, Value);
        return;
    }

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = CPQ_MISC3_BRIDGE_DEVICE;
    HalSetBusDataByOffset(PCIConfiguration, 0, Slot.u.AsULONG, &Value, Offset, sizeof(Value));
}

/* Points the index register at the entry of the link */
static
BOOLEAN
HalpCompaqMisc3Select(
    _In_ UCHAR Link,
    _Out_ PBOOLEAN OnBridge)
{
    if (Link >= CPQ_MISC3_ROUTER_FIRST && Link <= CPQ_MISC3_ROUTER_LAST)
    {
        *OnBridge = FALSE;
        HalpCompaqMisc3Write(FALSE, CPQ_MISC3_INDEX, (UCHAR)(Link - CPQ_MISC3_ROUTER_FIRST));
        return TRUE;
    }

    if (Link >= CPQ_MISC3_BRIDGE_FIRST && Link <= CPQ_MISC3_BRIDGE_LAST)
    {
        *OnBridge = TRUE;
        HalpCompaqMisc3Write(TRUE, CPQ_MISC3_INDEX, (UCHAR)(Link - CPQ_MISC3_BRIDGE_FIRST));
        return TRUE;
    }

    return FALSE;
}

static
NTSTATUS
NTAPI
HalpCompaqMisc3ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpCompaqMisc3IsTableLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpCompaqMisc3GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    BOOLEAN OnBridge;

    if (!HalpCompaqMisc3Select(Link, &OnBridge))
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpPirGetNibble(HalpCompaqMisc3Read(OnBridge, CPQ_MISC3_DATA), TRUE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpCompaqMisc3SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    BOOLEAN OnBridge;
    UCHAR Value;

    if (!HalpCompaqMisc3Select(Link, &OnBridge))
        return STATUS_INVALID_PARAMETER;

    Value = (Irq != 0) ? (UCHAR)(Irq << 4) : CPQ_MISC3_DISABLED;

    HalpCompaqMisc3Write(OnBridge, CPQ_MISC3_DATA, Value);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpCompaqOsbRouter =
{
    HalpCompaqOsbValidateTable,
    HalpCompaqOsbGetIrq,
    HalpCompaqOsbSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};

HALP_IRQ_ROUTER HalpCompaqCmc2Router =
{
    HalpCompaqCmc2ValidateTable,
    HalpCompaqCmc2GetIrq,
    HalpCompaqCmc2SetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};

HALP_IRQ_ROUTER HalpCompaqMisc3Router =
{
    HalpCompaqMisc3ValidateTable,
    HalpCompaqMisc3GetIrq,
    HalpCompaqMisc3SetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
