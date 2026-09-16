/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* PIIX, SIO and ICH route registers in the bridge config space */
#define PIIX_LOWEST_ROUTE_REGISTER  0x40
#define PIIX_ROUTE_DISABLED         0x80
#define PIIX_ROUTE_IRQ              0x0F

/* ESC route registers behind the 0x22/0x23 index pair */
#define ESC_INDEX_PORT              0x22
#define ESC_DATA_PORT               0x23
#define ESC_ID_REGISTER             0x02
#define ESC_CONFIG_UNLOCK           0x0F
#define ESC_CONFIG_LOCK             0x00
#define ESC_PIRQ_REGISTER           0x60
#define ESC_LINK_COUNT              4

static
NTSTATUS
NTAPI
HalpPiixValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Lowest >= PIIX_LOWEST_ROUTE_REGISTER) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpPiixGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Route;

    if (Link < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    Route = HalpPirReadRouterByte(Link);

    *Irq = (Route & PIIX_ROUTE_DISABLED) ? 0 : (UCHAR)(Route & PIIX_ROUTE_IRQ);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpPiixSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (Link < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    HalpPirWriteRouterByte(Link, (Irq != 0) ? Irq : (UCHAR)PIIX_ROUTE_DISABLED);
    return STATUS_SUCCESS;
}

/* Accepts links 1 to 4, or their register offsets */
static
NTSTATUS
NTAPI
HalpEscValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PSLOT_INFO Slot;
    PSLOT_INFO End = (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
    UCHAR Lowest, Highest;
    ULONG Pin;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    if (Highest <= ESC_LINK_COUNT)
        return STATUS_SUCCESS;

    if (Lowest < ESC_PIRQ_REGISTER || Highest >= ESC_PIRQ_REGISTER + ESC_LINK_COUNT)
        return STATUS_UNSUCCESSFUL;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = 0; Pin < RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            if (Slot->PinInfo[Pin].Link != 0)
                Slot->PinInfo[Pin].Link =
                    (UCHAR)(Slot->PinInfo[Pin].Link - (ESC_PIRQ_REGISTER - 1));
        }
    }

    return STATUS_SUCCESS;
}

static
UCHAR
NTAPI
HalpEscSelectLink(
    _In_ UCHAR Link)
{
    __outbyte(ESC_INDEX_PORT, ESC_ID_REGISTER);
    __outbyte(ESC_DATA_PORT, ESC_CONFIG_UNLOCK);
    __outbyte(ESC_INDEX_PORT, (UCHAR)(ESC_PIRQ_REGISTER + Link - 1));

    return __inbyte(ESC_DATA_PORT);
}

static
VOID
NTAPI
HalpEscRelease(VOID)
{
    __outbyte(ESC_INDEX_PORT, ESC_ID_REGISTER);
    __outbyte(ESC_DATA_PORT, ESC_CONFIG_LOCK);
}

static
NTSTATUS
NTAPI
HalpEscGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Route;

    if (Link == 0 || Link > ESC_LINK_COUNT)
        return STATUS_INVALID_PARAMETER;

    Route = HalpEscSelectLink(Link);
    HalpEscRelease();

    *Irq = (Route & PIIX_ROUTE_DISABLED) ? 0 : (UCHAR)(Route & PIIX_ROUTE_IRQ);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpEscSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (Link == 0 || Link > ESC_LINK_COUNT)
        return STATUS_INVALID_PARAMETER;

    HalpEscSelectLink(Link);
    __outbyte(ESC_DATA_PORT, (Irq != 0) ? Irq : (UCHAR)PIIX_ROUTE_DISABLED);
    HalpEscRelease();

    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpPiixRouter =
{
    HalpPiixValidateTable,
    HalpPiixGetIrq,
    HalpPiixSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};

HALP_IRQ_ROUTER HalpEscRouter =
{
    HalpEscValidateTable,
    HalpEscGetIrq,
    HalpEscSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
