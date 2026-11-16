/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SM8150 primary USB controller and its Synopsys high speed PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SM8150_CORE_BASE                0x0A600000ULL
#define SM8150_HSPHY_BASE               0x088E2000ULL

/* Clock controller offsets */
#define SM8150_USB30_PRIM_GDSCR         0xF004
#define SM8150_USB30_MASTER_CBCR        0xF010
#define SM8150_USB30_SLEEP_CBCR         0xF014
#define SM8150_USB30_MOCK_UTMI_CBCR     0xF018
#define SM8150_CFG_NOC_USB3_CBCR        0xF078
#define SM8150_AGGRE_USB3_CBCR          0xF07C
#define SM8150_QUSB2PHY_PRIM_BCR        0x12000

static const ULONG Sm8150Branches[] =
{
    SM8150_CFG_NOC_USB3_CBCR,
    SM8150_USB30_MASTER_CBCR,
    SM8150_AGGRE_USB3_CBCR,
    SM8150_USB30_SLEEP_CBCR,
    SM8150_USB30_MOCK_UTMI_CBCR
};

/* Firmware runs this controller in device mode, so its PHY is only set up again when it is down */
static
BOOLEAN
NTAPI
Sm8150PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SM8150_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sm8150Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sm8150Branches[i]))
            return FALSE;
    }

    if (KdDwcSnpsHsIsRunning(SM8150_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SM8150_HSPHY_BASE, KDDWC_GCC_BASE + SM8150_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sm8150BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN9", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 4),
    KDDWC_MEMORY_NODE("MC0", 4, 4),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH5", 32, 2),
    KDDWC_REGISTER_NODE("SH2", 8, 1),
    KDDWC_REGISTER_NODE("SN15", 8, 1),
    KDDWC_REGISTER_NODE("SN3", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 4, 1)
};

static const KDDWC_RPMH Sm8150Rpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x85F20000ULL,
    KDDWC_CMD_DB_BYTES,
    Sm8150BusNodes,
    RTL_NUMBER_OF(Sm8150BusNodes),
    NULL,
    0
};

const KDDWC_PLATFORM KdDwcPlatformSm8150 =
{
    "SM8150",
    { "SDM8150", "SM8150", NULL, NULL },
    SM8150_CORE_BASE,
    Sm8150PowerUp,
    0,
    &Sm8150Rpmh
};
