/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel PCI IRQ routers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>

#define NDEBUG
#include <debug.h>

#define INTEL_VENDOR_ID             0x8086

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

typedef struct _HALP_INTEL_ROUTER_ID
{
    USHORT DeviceId;
    PHALP_IRQ_ROUTER Router;
} HALP_INTEL_ROUTER_ID;

static ULONG HalpIntelRouterBus;
static PCI_SLOT_NUMBER HalpIntelRouterSlot;

static
VOID
HalpIntelLinkRange(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _Out_ PUCHAR Lowest,
    _Out_ PUCHAR Highest)
{
    PSLOT_INFO Slot;
    PSLOT_INFO End = (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
    ULONG Pin;

    *Lowest = MAXUCHAR;
    *Highest = 0;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = 0; Pin < RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            UCHAR Link = Slot->PinInfo[Pin].Link;

            if (Link == 0)
                continue;

            *Lowest = (UCHAR)min(*Lowest, Link);
            *Highest = (UCHAR)max(*Highest, Link);
        }
    }

    if (*Highest == 0)
        *Lowest = 0;
}

static
NTSTATUS
NTAPI
HalpPiixValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpIntelLinkRange(Table, &Lowest, &Highest);

    return (Lowest >= PIIX_LOWEST_ROUTE_REGISTER) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpPiixGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Route = PIIX_ROUTE_DISABLED;

    if (Link < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    HalGetBusDataByOffset(PCIConfiguration,
                          HalpIntelRouterBus,
                          HalpIntelRouterSlot.u.AsULONG,
                          &Route,
                          Link,
                          sizeof(Route));

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
    UCHAR Route = (Irq != 0) ? Irq : (UCHAR)PIIX_ROUTE_DISABLED;

    if (Link < PIIX_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    HalSetBusDataByOffset(PCIConfiguration,
                          HalpIntelRouterBus,
                          HalpIntelRouterSlot.u.AsULONG,
                          &Route,
                          Link,
                          sizeof(Route));
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

    HalpIntelLinkRange(Table, &Lowest, &Highest);

    if (Highest <= ESC_LINK_COUNT)
        return STATUS_SUCCESS;

    if (Lowest < ESC_PIRQ_REGISTER || Highest >= ESC_PIRQ_REGISTER + ESC_LINK_COUNT)
        return STATUS_UNSUCCESSFUL;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = 0; Pin < RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            if (Slot->PinInfo[Pin].Link != 0)
                Slot->PinInfo[Pin].Link = (UCHAR)(Slot->PinInfo[Pin].Link - (ESC_PIRQ_REGISTER - 1));
        }
    }

    return STATUS_SUCCESS;
}

static
UCHAR
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

static HALP_IRQ_ROUTER HalpPiixRouter =
{
    HalpPiixValidateTable,
    HalpPiixGetIrq,
    HalpPiixSetIrq
};

static HALP_IRQ_ROUTER HalpEscRouter =
{
    HalpEscValidateTable,
    HalpEscGetIrq,
    HalpEscSetIrq
};

static const HALP_INTEL_ROUTER_ID HalpIntelRouters[] =
{
    { 0x0482, &HalpEscRouter },  /* 82375EB/SB PCEB */
    { 0x0484, &HalpPiixRouter }, /* 82378ZB SIO */
    { 0x122E, &HalpPiixRouter }, /* 82371FB PIIX */
    { 0x1234, &HalpPiixRouter }, /* 82371MX MPIIX */
    { 0x7000, &HalpPiixRouter }, /* 82371SB PIIX3 */
    { 0x7110, &HalpPiixRouter }, /* 82371AB/EB/MB PIIX4 */
    { 0x7198, &HalpPiixRouter }, /* 82443MX */
    { 0x2410, &HalpPiixRouter }, /* 82801AA ICH */
    { 0x2420, &HalpPiixRouter }, /* 82801AB ICH0 */
    { 0x2440, &HalpPiixRouter }, /* 82801BA ICH2 */
    { 0x244C, &HalpPiixRouter }, /* 82801BAM ICH2-M */
    { 0x2450, &HalpPiixRouter }, /* 82801E C-ICH */
    { 0x2480, &HalpPiixRouter }, /* 82801CA ICH3-S */
    { 0x248C, &HalpPiixRouter }, /* 82801CAM ICH3-M */
    { 0x24C0, &HalpPiixRouter }, /* 82801DB ICH4 */
    { 0x24CC, &HalpPiixRouter }, /* 82801DBM ICH4-M */
    { 0x24D0, &HalpPiixRouter }, /* 82801EB ICH5 */
    { 0x25A1, &HalpPiixRouter }, /* 6300ESB */
    { 0x2640, &HalpPiixRouter }, /* ICH6 */
    { 0x2641, &HalpPiixRouter }, /* ICH6-M */
    { 0x2670, &HalpPiixRouter }, /* 631xESB/632xESB */
    { 0x27B0, &HalpPiixRouter }, /* ICH7DH */
    { 0x27B8, &HalpPiixRouter }, /* ICH7 */
    { 0x27B9, &HalpPiixRouter }, /* ICH7-M */
    { 0x27BD, &HalpPiixRouter }, /* ICH7-MDH */
    { 0x2810, &HalpPiixRouter }, /* ICH8 */
    { 0x2811, &HalpPiixRouter }, /* ICH8M-E */
    { 0x2812, &HalpPiixRouter }, /* ICH8DH */
    { 0x2814, &HalpPiixRouter }, /* ICH8DO */
    { 0x2815, &HalpPiixRouter }, /* ICH8M */
    { 0x2912, &HalpPiixRouter }, /* ICH9DH */
    { 0x2913, &HalpPiixRouter }, /* ICH9 engineering sample */
    { 0x2914, &HalpPiixRouter }, /* ICH9DO */
    { 0x2916, &HalpPiixRouter }, /* ICH9R */
    { 0x2918, &HalpPiixRouter }, /* ICH9 */
    { 0x2919, &HalpPiixRouter }, /* ICH9M */
    { 0x3A14, &HalpPiixRouter }, /* ICH10DO */
    { 0x3A16, &HalpPiixRouter }, /* ICH10R */
    { 0x3A18, &HalpPiixRouter }, /* ICH10 */
    { 0x3A1A, &HalpPiixRouter }, /* ICH10D */
};

static
PHALP_IRQ_ROUTER
HalpIntelLookupRouter(
    _In_ USHORT VendorId,
    _In_ USHORT DeviceId)
{
    ULONG Index;

    if (VendorId != INTEL_VENDOR_ID)
        return NULL;

    for (Index = 0; Index < RTL_NUMBER_OF(HalpIntelRouters); Index++)
    {
        if (HalpIntelRouters[Index].DeviceId == DeviceId)
            return HalpIntelRouters[Index].Router;
    }

    return NULL;
}

static
PHALP_IRQ_ROUTER
HalpIntelProbeRouter(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot)
{
    USHORT Id[2] = { PCI_INVALID_VENDORID, PCI_INVALID_VENDORID };

    HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, Id, 0, sizeof(Id));

    return HalpIntelLookupRouter(Id[0], Id[1]);
}

static
PHALP_IRQ_ROUTER
HalpIntelScanBusZero(
    _Out_ PPCI_SLOT_NUMBER Found)
{
    PHALP_IRQ_ROUTER Router;
    PCI_SLOT_NUMBER Slot;
    UCHAR HeaderType;
    ULONG Device, Function;

    Slot.u.AsULONG = 0;
    for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
    {
        Slot.u.bits.DeviceNumber = Device;

        for (Function = 0; Function < PCI_MAX_FUNCTION; Function++)
        {
            Slot.u.bits.FunctionNumber = Function;

            Router = HalpIntelProbeRouter(0, Slot);
            if (Router)
            {
                *Found = Slot;
                return Router;
            }

            if (Function != 0)
                continue;

            HeaderType = MAXUCHAR;
            HalGetBusDataByOffset(PCIConfiguration,
                                  0,
                                  Slot.u.AsULONG,
                                  &HeaderType,
                                  FIELD_OFFSET(PCI_COMMON_HEADER, HeaderType),
                                  sizeof(HeaderType));
            if (!(HeaderType & PCI_MULTIFUNCTION))
                break;
        }
    }

    return NULL;
}

/**
 * @brief
 * Finds the Intel PIRQ router, using the $PIR router fields before
 * scanning bus 0.
 */
CODE_SEG("PAGE")
PHALP_IRQ_ROUTER
NTAPI
HalpIntelFindRouter(
    _In_opt_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PHALP_IRQ_ROUTER Router = NULL;
    PCI_SLOT_NUMBER Slot;
    ULONG Bus = 0;

    PAGED_CODE();

    Slot.u.AsULONG = 0;

    if (Table)
    {
        Bus = Table->RouterBus;
        Slot.u.bits.DeviceNumber = Table->RouterDevFunc >> 3;
        Slot.u.bits.FunctionNumber = Table->RouterDevFunc & 7;

        Router = HalpIntelProbeRouter(Bus, Slot);
        if (!Router)
        {
            Router = HalpIntelLookupRouter((USHORT)Table->CompatibleRouter,
                                           (USHORT)(Table->CompatibleRouter >> 16));
        }
    }

    if (!Router)
    {
        Bus = 0;
        Router = HalpIntelScanBusZero(&Slot);
    }

    if (Router)
    {
        HalpIntelRouterBus = Bus;
        HalpIntelRouterSlot = Slot;
    }

    return Router;
}
