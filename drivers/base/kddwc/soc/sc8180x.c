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
#define SC8180X_USB30_PRIM_GDSCR        0xF004
#define SC8180X_USB30_MASTER_CBCR       0xF010
#define SC8180X_USB30_SLEEP_CBCR        0xF014
#define SC8180X_USB30_MOCK_UTMI_CBCR    0xF018
#define SC8180X_CFG_NOC_USB3_CBCR       0xF078
#define SC8180X_AGGRE_USB3_CBCR         0xF07C
#define SC8180X_QUSB2PHY_PRIM_BCR       0x12000

static const ULONG Sc8180xBranches[] =
{
    SC8180X_USB30_MASTER_CBCR,
    SC8180X_USB30_SLEEP_CBCR,
    SC8180X_USB30_MOCK_UTMI_CBCR
};

/* The bus branches are checked rather than written, they come up with the firmware's USB stack */
static const ULONG Sc8180xBusBranches[] =
{
    SC8180X_CFG_NOC_USB3_CBCR,
    SC8180X_AGGRE_USB3_CBCR
};

static
BOOLEAN
NTAPI
Sc8180xPowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SC8180X_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sc8180xBranches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sc8180xBranches[i]))
            return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(Sc8180xBusBranches); i++)
    {
        if (!KdDwcVotedClockIsOn(KDDWC_GCC_BASE + Sc8180xBusBranches[i]))
            return FALSE;
    }

    return TRUE;
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

static const KDDWC_RPMH Sc8180xRpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x85F20000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc8180xBusNodes,
    RTL_NUMBER_OF(Sc8180xBusNodes),
    NULL,
    0
};

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Sc8180xIsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClocksRunning(KDDWC_GCC_BASE, SC8180X_USB30_PRIM_GDSCR, Sc8180xBranches, RTL_NUMBER_OF(Sc8180xBranches));
}

const KDDWC_PLATFORM KdDwcPlatformSc8180x =
{
    "SC8180X",
    { "SC8180X", "SC8180XP", "SDM8180", "SC8180" },
    SC8180X_CORE_BASE,
    Sc8180xPowerUp,
    0,
    &Sc8180xRpmh,
    Sc8180xIsPowered,
    Sc8180xStartPhy
};
