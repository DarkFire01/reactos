/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SC8180X (Snapdragon 8cx, SQ1 and SQ2) primary USB controller and its high speed PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SC8180X_CORE_BASE               0x0A600000ULL
#define SC8180X_HSPHY_BASE              0x088E2000ULL

/* Clock controller offsets */
#define SC8180X_GPLL0_MODE              0x00000
#define SC8180X_APCS_GPLL_VOTE          0x52000
#define SC8180X_APCS_GPLL0_VOTE         0x00000001
#define SC8180X_USB30_PRIM_BCR          0xF000
#define SC8180X_USB30_PRIM_GDSCR        0xF004
#define SC8180X_USB30_MASTER_CMD_RCGR   0xF01C
#define SC8180X_USB30_MOCK_UTMI_CMD_RCGR 0xF034
#define SC8180X_CFG_NOC_USB3_CBCR       0xF078
#define SC8180X_USB30_MASTER_CBCR       0xF010
#define SC8180X_AGGRE_USB3_CBCR         0xF07C
#define SC8180X_USB30_SLEEP_CBCR        0xF014
#define SC8180X_USB30_MOCK_UTMI_CBCR    0xF018
#define SC8180X_USB3_SEC_CLKREF_CBCR    0x8C028
#define SC8180X_QUSB2PHY_PRIM_BCR       0x12000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define SC8180X_MASTER_200MHZ           0x105
#define SC8180X_MOCK_UTMI_19_2MHZ       0x001

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG Sc8180xBranches[] =
{
    SC8180X_CFG_NOC_USB3_CBCR,
    SC8180X_USB30_MASTER_CBCR,
    SC8180X_AGGRE_USB3_CBCR,
    SC8180X_USB30_SLEEP_CBCR,
    SC8180X_USB30_MOCK_UTMI_CBCR,
    SC8180X_USB3_SEC_CLKREF_CBCR
};

static const KDDWC_CLOCK_PLAN Sc8180xClocks =
{
    KDDWC_GCC_BASE,
    SC8180X_GPLL0_MODE,
    SC8180X_APCS_GPLL_VOTE,
    SC8180X_APCS_GPLL0_VOTE,
    SC8180X_USB30_PRIM_GDSCR,
    SC8180X_USB30_MASTER_CMD_RCGR,
    SC8180X_MASTER_200MHZ,
    SC8180X_USB30_MOCK_UTMI_CMD_RCGR,
    SC8180X_MOCK_UTMI_19_2MHZ,
    0,
    SC8180X_USB30_PRIM_BCR,
    Sc8180xBranches,
    RTL_NUMBER_OF(Sc8180xBranches)
};

/* Firmware can leave the roots parked on the crystal and the branches off at exit */
static
BOOLEAN
NTAPI
Sc8180xPowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sc8180xClocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sc8180xStartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcSnpsHsIsRunning(SC8180X_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SC8180X_HSPHY_BASE, KDDWC_GCC_BASE + SC8180X_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sc8180xBusNodes[] =
{
    KDDWC_MEMORY_NODE("SN9", 32, 1),
    KDDWC_MEMORY_NODE("SN0", 32, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 8),
    KDDWC_MEMORY_NODE("MC0", 4, 8),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH3", 64, 4),
    KDDWC_REGISTER_NODE("SH2", 8, 1),
    KDDWC_REGISTER_NODE("SN15", 8, 1),
    KDDWC_REGISTER_NODE("SN3", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 4, 1)
};

/* The supplies of the high speed PHY on Primus and the Flex 5G */
static const KDDWC_SUPPLY Sc8180xSupplies[] =
{
    { "ldoe5", 880 },
    { "ldoa12", 1800 },
    { "ldoe16", 3072 }
};

static const KDDWC_RPMH Sc8180xRpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x85F20000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc8180xBusNodes,
    RTL_NUMBER_OF(Sc8180xBusNodes),
    Sc8180xSupplies,
    RTL_NUMBER_OF(Sc8180xSupplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

static
BOOLEAN
NTAPI
Sc8180xIsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sc8180xClocks);
}

const KDDWC_PLATFORM KdDwcPlatformSc8180x =
{
    "SC8180X",
    { "SC8180X", "SC8180XP", "SDM8180", "SC8180", NULL },
    SC8180X_CORE_BASE,
    Sc8180xPowerUp,
    0,
    &Sc8180xRpmh,
    Sc8180xIsPowered,
    Sc8180xStartPhy
};
