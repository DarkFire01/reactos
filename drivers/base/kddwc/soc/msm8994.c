/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MSM8994 and MSM8992 USB 3 controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define MSM8994_CORE_BASE               0xF9200000ULL

/* This generation has its clock controller in the old peripheral range */
#define MSM8994_GCC_BASE                0xFC400000ULL
#define MSM8994_USB30_GDSCR             0x03C4
#define MSM8994_USB30_MASTER_CBCR       0x03C8
#define MSM8994_USB30_SLEEP_CBCR        0x03CC
#define MSM8994_USB30_MOCK_UTMI_CBCR    0x03D0
#define MSM8994_SYS_NOC_USB3_CBCR       0x03FC

static const ULONG Msm8994Branches[] =
{
    MSM8994_USB30_MASTER_CBCR,
    MSM8994_SYS_NOC_USB3_CBCR,
    MSM8994_USB30_SLEEP_CBCR,
    MSM8994_USB30_MOCK_UTMI_CBCR
};

/*
 * The boot firmware of these phones brings the high speed PHY up for its own
 * USB function and nothing here knows its setup, so it is used as firmware
 * left it. The core takes its pipe clock from UTMI, as on the phones' own setup.
 */
static
BOOLEAN
NTAPI
Msm8994PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(MSM8994_GCC_BASE + MSM8994_USB30_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Msm8994Branches); i++)
    {
        if (!KdDwcClockOn(MSM8994_GCC_BASE + Msm8994Branches[i]))
            return FALSE;
    }

    return TRUE;
}

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Msm8994IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClocksRunning(MSM8994_GCC_BASE, MSM8994_USB30_GDSCR, Msm8994Branches, RTL_NUMBER_OF(Msm8994Branches));
}

const KDDWC_PLATFORM KdDwcPlatformMsm8994 =
{
    "MSM8994",
    { "MSM8994", "MSM8992", NULL, NULL },
    MSM8994_CORE_BASE,
    Msm8994PowerUp,
    KDDWC_PLATFORM_KEEP_PHY,
    NULL,
    Msm8994IsPowered
};
