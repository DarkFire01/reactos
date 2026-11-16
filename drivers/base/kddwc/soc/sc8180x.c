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

    if (KdDwcSnpsHsIsRunning(SC8180X_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SC8180X_HSPHY_BASE, KDDWC_GCC_BASE + SC8180X_QUSB2PHY_PRIM_BCR);
}

const KDDWC_PLATFORM KdDwcPlatformSc8180x =
{
    "SC8180X",
    { "SC8180X", "SC8180XP", "SDM8180", "SC8180" },
    SC8180X_CORE_BASE,
    Sc8180xPowerUp,
    0
};
