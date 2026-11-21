/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SC8280XP (Snapdragon 8cx Gen 3, SQ3) first USB controller and its high speed PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SC8280XP_CORE_BASE              0x0A600000ULL
#define SC8280XP_HSPHY_BASE             0x088E5000ULL

/* Clock controller offsets */
#define SC8280XP_GPLL0_MODE             0x00000
#define SC8280XP_APCS_GPLL_VOTE         0x52028
#define SC8280XP_APCS_GPLL0_VOTE        0x00000001
#define SC8280XP_USB30_PRIM_BCR         0xF000
#define SC8280XP_USB30_PRIM_GDSCR       0xF004
#define SC8280XP_USB30_MASTER_CMD_RCGR  0xF020
#define SC8280XP_USB30_MOCK_UTMI_CMD_RCGR 0xF038
#define SC8280XP_USB30_MOCK_UTMI_POSTDIV 0xF050
#define SC8280XP_AHB2PHY0_CBCR          0x6A004
#define SC8280XP_CFG_NOC_USB3_CBCR      0xF084
#define SC8280XP_USB30_MASTER_CBCR      0xF010
#define SC8280XP_AGGRE_USB3_CBCR        0xF080
#define SC8280XP_USB30_SLEEP_CBCR       0xF018
#define SC8280XP_USB30_MOCK_UTMI_CBCR   0xF01C
#define SC8280XP_AGGRE_USB_NOC_AXI_CBCR 0x5D024
#define SC8280XP_AGGRE_NOC_NORTH_CBCR   0x5D020
#define SC8280XP_AGGRE_NOC_SOUTH_CBCR   0x5D01C
#define SC8280XP_SYS_NOC_USB_AXI_CBCR   0x5D000
#define SC8280XP_QUSB2PHY_PRIM_BCR      0x12000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define SC8280XP_MASTER_200MHZ          0x105
#define SC8280XP_MOCK_UTMI_19_2MHZ      0x001

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG Sc8280xpBranches[] =
{
    SC8280XP_AHB2PHY0_CBCR,
    SC8280XP_CFG_NOC_USB3_CBCR,
    SC8280XP_USB30_MASTER_CBCR,
    SC8280XP_AGGRE_USB3_CBCR,
    SC8280XP_USB30_SLEEP_CBCR,
    SC8280XP_USB30_MOCK_UTMI_CBCR,
    SC8280XP_AGGRE_USB_NOC_AXI_CBCR,
    SC8280XP_AGGRE_NOC_NORTH_CBCR,
    SC8280XP_AGGRE_NOC_SOUTH_CBCR,
    SC8280XP_SYS_NOC_USB_AXI_CBCR
};

static const KDDWC_CLOCK_PLAN Sc8280xpClocks =
{
    KDDWC_GCC_BASE,
    SC8280XP_GPLL0_MODE,
    SC8280XP_APCS_GPLL_VOTE,
    SC8280XP_APCS_GPLL0_VOTE,
    SC8280XP_USB30_PRIM_GDSCR,
    SC8280XP_USB30_MASTER_CMD_RCGR,
    SC8280XP_MASTER_200MHZ,
    SC8280XP_USB30_MOCK_UTMI_CMD_RCGR,
    SC8280XP_MOCK_UTMI_19_2MHZ,
    SC8280XP_USB30_MOCK_UTMI_POSTDIV,
    SC8280XP_USB30_PRIM_BCR,
    Sc8280xpBranches,
    RTL_NUMBER_OF(Sc8280xpBranches)
};

/* Firmware can leave the roots parked on the crystal and the branches off at exit */
static
BOOLEAN
NTAPI
Sc8280xpPowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sc8280xpClocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sc8280xpStartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcSnpsHsIsRunning(SC8280XP_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SC8280XP_HSPHY_BASE, KDDWC_GCC_BASE + SC8280XP_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sc8280xpBusNodes[] =
{
    KDDWC_MEMORY_NODE("SN5", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 8),
    KDDWC_MEMORY_NODE("MC0", 4, 8),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH2", 32, 2),
    KDDWC_REGISTER_NODE("CN0", 16, 1),
    KDDWC_REGISTER_NODE("CN1", 4, 1)
};

/* The supplies of the high speed PHY on the Windows Dev Kit 2023, the X13s and the CRD */
static const KDDWC_SUPPLY Sc8280xpSupplies[] =
{
    { "ldod9", 912 },
    { "ldoc1", 1800 },
    { "ldod7", 3072 }
};

static const KDDWC_RPMH Sc8280xpRpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80860000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc8280xpBusNodes,
    RTL_NUMBER_OF(Sc8280xpBusNodes),
    Sc8280xpSupplies,
    RTL_NUMBER_OF(Sc8280xpSupplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

static
BOOLEAN
NTAPI
Sc8280xpIsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sc8280xpClocks);
}

const KDDWC_PLATFORM KdDwcPlatformSc8280xp =
{
    "SC8280XP",
    { "SC8280XP", "SC8280X", "SC8280", "SDM8280", NULL },
    SC8280XP_CORE_BASE,
    Sc8280xpPowerUp,
    0,
    &Sc8280xpRpmh,
    Sc8280xpIsPowered,
    Sc8280xpStartPhy
};
