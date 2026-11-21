/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SM8350 primary USB controller and its Synopsys high speed PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SM8350_CORE_BASE                0x0A600000ULL
#define SM8350_HSPHY_BASE               0x088E3000ULL

/* Clock controller offsets */
#define SM8350_GPLL0_MODE               0x00000
#define SM8350_APCS_GPLL_VOTE           0x52018
#define SM8350_APCS_GPLL0_VOTE          0x00000001
#define SM8350_USB30_PRIM_BCR           0xF000
#define SM8350_USB30_PRIM_GDSCR         0xF004
#define SM8350_USB30_MASTER_CMD_RCGR    0xF020
#define SM8350_USB30_MOCK_UTMI_CMD_RCGR 0xF038
#define SM8350_USB30_MOCK_UTMI_POSTDIV  0xF050
#define SM8350_CFG_NOC_USB3_CBCR        0xF07C
#define SM8350_USB30_MASTER_CBCR        0xF010
#define SM8350_AGGRE_USB3_CBCR          0xF080
#define SM8350_USB30_SLEEP_CBCR         0xF018
#define SM8350_USB30_MOCK_UTMI_CBCR     0xF01C
#define SM8350_QUSB2PHY_PRIM_BCR        0x12000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define SM8350_MASTER_200MHZ            0x105
#define SM8350_MOCK_UTMI_19_2MHZ        0x001

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG Sm8350Branches[] =
{
    SM8350_CFG_NOC_USB3_CBCR,
    SM8350_USB30_MASTER_CBCR,
    SM8350_AGGRE_USB3_CBCR,
    SM8350_USB30_SLEEP_CBCR,
    SM8350_USB30_MOCK_UTMI_CBCR
};

static const KDDWC_CLOCK_PLAN Sm8350Clocks =
{
    KDDWC_GCC_BASE,
    SM8350_GPLL0_MODE,
    SM8350_APCS_GPLL_VOTE,
    SM8350_APCS_GPLL0_VOTE,
    SM8350_USB30_PRIM_GDSCR,
    SM8350_USB30_MASTER_CMD_RCGR,
    SM8350_MASTER_200MHZ,
    SM8350_USB30_MOCK_UTMI_CMD_RCGR,
    SM8350_MOCK_UTMI_19_2MHZ,
    SM8350_USB30_MOCK_UTMI_POSTDIV,
    SM8350_USB30_PRIM_BCR,
    Sm8350Branches,
    RTL_NUMBER_OF(Sm8350Branches)
};

/* Firmware can leave the roots parked on the crystal and the branches off at exit */
static
BOOLEAN
NTAPI
Sm8350PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sm8350Clocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sm8350StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcSnpsHsIsRunning(SM8350_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SM8350_HSPHY_BASE, KDDWC_GCC_BASE + SM8350_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sm8350BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN7", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 4),
    KDDWC_MEMORY_NODE("MC0", 4, 4),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH4", 32, 2),
    KDDWC_REGISTER_NODE("CN0", 16, 1),
    KDDWC_REGISTER_NODE("CN1", 4, 1)
};

/* The supplies of the high speed PHY on the HDK and the MTP */
static const KDDWC_SUPPLY Sm8350Supplies[] =
{
    { "ldob5", 880 },
    { "ldoc1", 1800 },
    { "ldob2", 3072 }
};

static const KDDWC_RPMH Sm8350Rpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80860000ULL,
    KDDWC_CMD_DB_BYTES,
    Sm8350BusNodes,
    RTL_NUMBER_OF(Sm8350BusNodes),
    Sm8350Supplies,
    RTL_NUMBER_OF(Sm8350Supplies)
};

static
BOOLEAN
NTAPI
Sm8350IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sm8350Clocks);
}

const KDDWC_PLATFORM KdDwcPlatformSm8350 =
{
    "SM8350",
    { "SM8350", "SDM8350", NULL, NULL, NULL },
    SM8350_CORE_BASE,
    Sm8350PowerUp,
    0,
    &Sm8350Rpmh,
    Sm8350IsPowered,
    Sm8350StartPhy
};
