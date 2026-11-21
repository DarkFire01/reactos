/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SC7280 (Snapdragon 7c+ Gen 3) primary USB controller and its high speed PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SC7280_CORE_BASE                0x0A600000ULL
#define SC7280_HSPHY_BASE               0x088E3000ULL

/* Clock controller offsets */
#define SC7280_GPLL0_MODE               0x00000
#define SC7280_APCS_GPLL_VOTE           0x52010
#define SC7280_APCS_GPLL0_VOTE          0x00000001
#define SC7280_USB30_PRIM_BCR           0xF000
#define SC7280_USB30_PRIM_GDSCR         0xF004
#define SC7280_USB30_MASTER_CMD_RCGR    0xF020
#define SC7280_USB30_MOCK_UTMI_CMD_RCGR 0xF038
#define SC7280_USB30_MOCK_UTMI_POSTDIV  0xF050
#define SC7280_CFG_NOC_USB3_CBCR        0xF07C
#define SC7280_USB30_MASTER_CBCR        0xF010
#define SC7280_AGGRE_USB3_CBCR          0xF080
#define SC7280_USB30_SLEEP_CBCR         0xF018
#define SC7280_USB30_MOCK_UTMI_CBCR     0xF01C
#define SC7280_QUSB2PHY_PRIM_BCR        0x12000

/* GPLL0 odd output undivided for a 200 MHz master, the crystal undivided for mock UTMI */
#define SC7280_MASTER_200MHZ            0x301
#define SC7280_MOCK_UTMI_19_2MHZ        0x001

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG Sc7280Branches[] =
{
    SC7280_CFG_NOC_USB3_CBCR,
    SC7280_USB30_MASTER_CBCR,
    SC7280_AGGRE_USB3_CBCR,
    SC7280_USB30_SLEEP_CBCR,
    SC7280_USB30_MOCK_UTMI_CBCR
};

static const KDDWC_CLOCK_PLAN Sc7280Clocks =
{
    KDDWC_GCC_BASE,
    SC7280_GPLL0_MODE,
    SC7280_APCS_GPLL_VOTE,
    SC7280_APCS_GPLL0_VOTE,
    SC7280_USB30_PRIM_GDSCR,
    SC7280_USB30_MASTER_CMD_RCGR,
    SC7280_MASTER_200MHZ,
    SC7280_USB30_MOCK_UTMI_CMD_RCGR,
    SC7280_MOCK_UTMI_19_2MHZ,
    SC7280_USB30_MOCK_UTMI_POSTDIV,
    SC7280_USB30_PRIM_BCR,
    Sc7280Branches,
    RTL_NUMBER_OF(Sc7280Branches)
};

/* Firmware can leave the roots parked on the crystal and the branches off at exit */
static
BOOLEAN
NTAPI
Sc7280PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sc7280Clocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sc7280StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcSnpsHsIsRunning(SC7280_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SC7280_HSPHY_BASE, KDDWC_GCC_BASE + SC7280_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sc7280BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN7", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 2),
    KDDWC_MEMORY_NODE("MC0", 4, 2),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH4", 32, 1),
    KDDWC_REGISTER_NODE("CN0", 16, 1),
    KDDWC_REGISTER_NODE("CN1", 8, 1),
    KDDWC_REGISTER_NODE("CN1", 4, 1)
};

/* The supplies of the high speed PHY on the IDP and the qcard Chromebooks */
static const KDDWC_SUPPLY Sc7280Supplies[] =
{
    { "ldoc10", 880 },
    { "ldoc1", 1800 },
    { "ldob2", 3072 }
};

static const KDDWC_RPMH Sc7280Rpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80860000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc7280BusNodes,
    RTL_NUMBER_OF(Sc7280BusNodes),
    Sc7280Supplies,
    RTL_NUMBER_OF(Sc7280Supplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

static
BOOLEAN
NTAPI
Sc7280IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sc7280Clocks);
}

const KDDWC_PLATFORM KdDwcPlatformSc7280 =
{
    "SC7280",
    { "SC7280", "SDM7280", NULL, NULL, NULL },
    SC7280_CORE_BASE,
    Sc7280PowerUp,
    0,
    &Sc7280Rpmh,
    Sc7280IsPowered,
    Sc7280StartPhy
};
