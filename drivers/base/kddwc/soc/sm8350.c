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
#define SM8350_USB30_PRIM_GDSCR         0xF004
#define SM8350_USB30_MASTER_CBCR        0xF010
#define SM8350_USB30_SLEEP_CBCR         0xF018
#define SM8350_USB30_MOCK_UTMI_CBCR     0xF01C
#define SM8350_CFG_NOC_USB3_CBCR        0xF07C
#define SM8350_AGGRE_USB3_CBCR          0xF080
#define SM8350_QUSB2PHY_PRIM_BCR        0x12000

static const ULONG Sm8350Branches[] =
{
    SM8350_USB30_MASTER_CBCR,
    SM8350_USB30_SLEEP_CBCR,
    SM8350_USB30_MOCK_UTMI_CBCR
};

/* Voted branches, enabled through the vote register and only checked here */
static const ULONG Sm8350BusBranches[] =
{
    SM8350_CFG_NOC_USB3_CBCR,
    SM8350_AGGRE_USB3_CBCR
};

static
BOOLEAN
NTAPI
Sm8350PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SM8350_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sm8350Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sm8350Branches[i]))
            return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(Sm8350BusBranches); i++)
    {
        if (!KdDwcVotedClockIsOn(KDDWC_GCC_BASE + Sm8350BusBranches[i]))
            return FALSE;
    }

    if (KdDwcSnpsHsIsRunning(SM8350_HSPHY_BASE))
        return TRUE;

    return KdDwcStartSnpsHs(SM8350_HSPHY_BASE, KDDWC_GCC_BASE + SM8350_QUSB2PHY_PRIM_BCR);
}

const KDDWC_PLATFORM KdDwcPlatformSm8350 =
{
    "SM8350",
    { "SM8350", "SDM8350", NULL, NULL },
    SM8350_CORE_BASE,
    Sm8350PowerUp,
    0
};
