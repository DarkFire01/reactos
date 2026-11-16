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
#define SC7280_USB30_PRIM_GDSCR         0xF004
#define SC7280_USB30_MASTER_CBCR        0xF010
#define SC7280_USB30_SLEEP_CBCR         0xF018
#define SC7280_USB30_MOCK_UTMI_CBCR     0xF01C
#define SC7280_CFG_NOC_USB3_CBCR        0xF07C
#define SC7280_AGGRE_USB3_CBCR          0xF080
#define SC7280_QUSB2PHY_PRIM_BCR        0x12000

static const ULONG Sc7280Branches[] =
{
    SC7280_USB30_MASTER_CBCR,
    SC7280_USB30_SLEEP_CBCR,
    SC7280_USB30_MOCK_UTMI_CBCR
};

/* Voted branches, enabled through the vote register and only checked here */
static const ULONG Sc7280BusBranches[] =
{
    SC7280_CFG_NOC_USB3_CBCR,
    SC7280_AGGRE_USB3_CBCR
};

static
BOOLEAN
NTAPI
Sc7280PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SC7280_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sc7280Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sc7280Branches[i]))
            return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(Sc7280BusBranches); i++)
    {
        if (!KdDwcVotedClockIsOn(KDDWC_GCC_BASE + Sc7280BusBranches[i]))
            return FALSE;
    }

    if (KdDwcSnpsHsIsRunning(SC7280_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SC7280_HSPHY_BASE, KDDWC_GCC_BASE + SC7280_QUSB2PHY_PRIM_BCR);
}

const KDDWC_PLATFORM KdDwcPlatformSc7280 =
{
    "SC7280",
    { "SC7280", NULL, NULL, NULL },
    SC7280_CORE_BASE,
    Sc7280PowerUp,
    0
};
