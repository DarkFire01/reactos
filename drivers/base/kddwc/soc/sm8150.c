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
#define SM8150_GPLL0_MODE               0x00000
#define SM8150_APCS_GPLL_VOTE           0x52000
#define SM8150_APCS_GPLL0_VOTE          0x00000001
#define SM8150_USB30_PRIM_BCR           0xF000
#define SM8150_USB30_PRIM_GDSCR         0xF004
#define SM8150_USB30_MASTER_CMD_RCGR    0xF01C
#define SM8150_USB30_MOCK_UTMI_CMD_RCGR 0xF034
#define SM8150_CFG_NOC_USB3_CBCR        0xF078
#define SM8150_USB30_MASTER_CBCR        0xF010
#define SM8150_AGGRE_USB3_CBCR          0xF07C
#define SM8150_USB30_SLEEP_CBCR         0xF014
#define SM8150_USB30_MOCK_UTMI_CBCR     0xF018
#define SM8150_USB3_SEC_CLKREF_CBCR     0x8C028
#define SM8150_QUSB2PHY_PRIM_BCR        0x12000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define SM8150_MASTER_200MHZ            0x105
#define SM8150_MOCK_UTMI_19_2MHZ        0x001

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG Sm8150Branches[] =
{
    SM8150_CFG_NOC_USB3_CBCR,
    SM8150_USB30_MASTER_CBCR,
    SM8150_AGGRE_USB3_CBCR,
    SM8150_USB30_SLEEP_CBCR,
    SM8150_USB30_MOCK_UTMI_CBCR,
    SM8150_USB3_SEC_CLKREF_CBCR
};

static const KDDWC_CLOCK_PLAN Sm8150Clocks =
{
    KDDWC_GCC_BASE,
    SM8150_GPLL0_MODE,
    SM8150_APCS_GPLL_VOTE,
    SM8150_APCS_GPLL0_VOTE,
    SM8150_USB30_PRIM_GDSCR,
    SM8150_USB30_MASTER_CMD_RCGR,
    SM8150_MASTER_200MHZ,
    SM8150_USB30_MOCK_UTMI_CMD_RCGR,
    SM8150_MOCK_UTMI_19_2MHZ,
    0,
    SM8150_USB30_PRIM_BCR,
    Sm8150Branches,
    RTL_NUMBER_OF(Sm8150Branches)
};

/* Firmware can leave the roots parked on the crystal and the branches off at exit */
static
BOOLEAN
NTAPI
Sm8150PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sm8150Clocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sm8150StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

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

/* The supplies of the high speed PHY on the MTP and the Surface Duo */
static const KDDWC_SUPPLY Sm8150Supplies[] =
{
    { "ldoa5", 880 },
    { "ldoa12", 1800 },
    { "ldoa2", 3072 }
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
    Sm8150Supplies,
    RTL_NUMBER_OF(Sm8150Supplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

static
BOOLEAN
NTAPI
Sm8150IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sm8150Clocks);
}

const KDDWC_PLATFORM KdDwcPlatformSm8150 =
{
    "SM8150",
    { "SDM8150", "SM8150", NULL, NULL, NULL },
    SM8150_CORE_BASE,
    Sm8150PowerUp,
    0,
    &Sm8150Rpmh,
    Sm8150IsPowered,
    Sm8150StartPhy
};
