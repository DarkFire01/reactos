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
#define SC8280XP_USB30_PRIM_GDSCR       0xF004
#define SC8280XP_USB30_MASTER_CBCR      0xF010
#define SC8280XP_USB30_SLEEP_CBCR       0xF018
#define SC8280XP_USB30_MOCK_UTMI_CBCR   0xF01C
#define SC8280XP_AGGRE_USB3_CBCR        0xF080
#define SC8280XP_CFG_NOC_USB3_CBCR      0xF084
#define SC8280XP_QUSB2PHY_PRIM_BCR      0x12000

static const ULONG Sc8280xpBranches[] =
{
    SC8280XP_USB30_MASTER_CBCR,
    SC8280XP_USB30_SLEEP_CBCR,
    SC8280XP_USB30_MOCK_UTMI_CBCR
};

/* The bus branches and the USB NOC behind them stay with firmware, they are only checked */
static const ULONG Sc8280xpBusBranches[] =
{
    SC8280XP_AGGRE_USB3_CBCR,
    SC8280XP_CFG_NOC_USB3_CBCR
};

static
BOOLEAN
NTAPI
Sc8280xpPowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SC8280XP_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sc8280xpBranches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sc8280xpBranches[i]))
            return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(Sc8280xpBusBranches); i++)
    {
        if (!KdDwcVotedClockIsOn(KDDWC_GCC_BASE + Sc8280xpBusBranches[i]))
            return FALSE;
    }

    return TRUE;
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

static const KDDWC_RPMH Sc8280xpRpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80860000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc8280xpBusNodes,
    RTL_NUMBER_OF(Sc8280xpBusNodes),
    NULL,
    0
};

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Sc8280xpIsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClocksRunning(KDDWC_GCC_BASE, SC8280XP_USB30_PRIM_GDSCR, Sc8280xpBranches, RTL_NUMBER_OF(Sc8280xpBranches));
}

const KDDWC_PLATFORM KdDwcPlatformSc8280xp =
{
    "SC8280XP",
    { "SC8280XP", "SC8280X", "SC8280", NULL },
    SC8280XP_CORE_BASE,
    Sc8280xpPowerUp,
    0,
    &Sc8280xpRpmh,
    Sc8280xpIsPowered,
    Sc8280xpStartPhy
};
