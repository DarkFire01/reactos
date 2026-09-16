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

/* Route register layout shared by the PIIX and the ESC */
#define INTEL_ROUTE_DISABLED        0x80
#define INTEL_ROUTE_IRQ             0x0F

/* ESC route registers behind the 0x22/0x23 index pair */
#define ESC_INDEX_PORT              0x22
#define ESC_DATA_PORT               0x23
#define ESC_ID_REGISTER             0x02
#define ESC_CONFIG_UNLOCK           0x0F
#define ESC_CONFIG_LOCK             0x00
#define ESC_PIRQ_REGISTER           0x60
#define ESC_LINK_COUNT              4

static
UCHAR
NTAPI
HalpIntelRouteToIrq(
    _In_ UCHAR Route)
{
    if (Route & INTEL_ROUTE_DISABLED)
        return 0;

    return (UCHAR)(Route & INTEL_ROUTE_IRQ);
}

static
UCHAR
NTAPI
HalpIntelIrqToRoute(
    _In_ UCHAR Irq)
{
    return (Irq == 0) ? (UCHAR)INTEL_ROUTE_DISABLED : Irq;
}

/* PIIX **************************************************************************/

/* The links are the offsets of the route registers themselves */
static
NTSTATUS
NTAPI
HalpPiixValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    if (Lowest < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpPiixGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    *Irq = 0;

    if (Link < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpIntelRouteToIrq(HalpPirReadRouterByte(Link));
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

    HalpPirWriteRouterByte(Link, HalpIntelIrqToRoute(Irq));
    return STATUS_SUCCESS;
}

/* ESC ***************************************************************************/

static
BOOLEAN
NTAPI
HalpEscLinkValid(
    _In_ UCHAR Link)
{
    return (Link >= 1 && Link <= ESC_LINK_COUNT);
}

/* Tables name links 1 to 4, or the route register offsets which become 1 to 4 */
static
NTSTATUS
NTAPI
HalpEscValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PSLOT_INFO Slot;
    PSLOT_INFO End = (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
    UCHAR Lowest, Highest;
    PPIN_INFO Pin;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    if (Highest <= ESC_LINK_COUNT)
        return STATUS_SUCCESS;

    if (Lowest < ESC_PIRQ_REGISTER || Highest > ESC_PIRQ_REGISTER + ESC_LINK_COUNT - 1)
        return STATUS_UNSUCCESSFUL;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = Slot->PinInfo; Pin < Slot->PinInfo + RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            if (Pin->Link != 0)
                Pin->Link = (UCHAR)(Pin->Link - ESC_PIRQ_REGISTER + 1);
        }
    }

    return STATUS_SUCCESS;
}

/* Reads or writes the route register of a link with configuration access opened around it */
static
UCHAR
NTAPI
HalpEscAccessRoute(
    _In_ UCHAR Link,
    _In_ BOOLEAN Write,
    _In_ UCHAR Route)
{
    __outbyte(ESC_INDEX_PORT, ESC_ID_REGISTER);
    __outbyte(ESC_DATA_PORT, ESC_CONFIG_UNLOCK);

    __outbyte(ESC_INDEX_PORT, (UCHAR)(ESC_PIRQ_REGISTER + Link - 1));
    if (Write)
        __outbyte(ESC_DATA_PORT, Route);
    else
        Route = __inbyte(ESC_DATA_PORT);

    __outbyte(ESC_INDEX_PORT, ESC_ID_REGISTER);
    __outbyte(ESC_DATA_PORT, ESC_CONFIG_LOCK);

    return Route;
}

static
NTSTATUS
NTAPI
HalpEscGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    *Irq = 0;

    if (!HalpEscLinkValid(Link))
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpIntelRouteToIrq(HalpEscAccessRoute(Link, FALSE, 0));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpEscSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (!HalpEscLinkValid(Link))
        return STATUS_INVALID_PARAMETER;

    HalpEscAccessRoute(Link, TRUE, HalpIntelIrqToRoute(Irq));
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
