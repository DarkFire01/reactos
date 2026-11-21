/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MSM8998 USB 3 controller and its first generation QUSB2 PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define MSM8998_CORE_BASE               0x0A800000ULL
#define MSM8998_QUSB2_BASE              0x0C012000ULL

/* Clock controller offsets */
#define MSM8998_GPLL0_MODE              0x00000
#define MSM8998_APCS_GPLL_VOTE          0x52000
#define MSM8998_APCS_GPLL0_VOTE         0x00000001
#define MSM8998_USB30_BCR               0xF000
#define MSM8998_USB30_GDSCR             0xF004
#define MSM8998_USB30_MASTER_CBCR       0xF008
#define MSM8998_USB30_SLEEP_CBCR        0xF00C
#define MSM8998_USB30_MOCK_UTMI_CBCR    0xF010
#define MSM8998_CFG_NOC_USB3_CBCR       0x5018
#define MSM8998_AHB2PHY_CBCR            0x6A004
#define MSM8998_AGGRE1_USB3_CBCR        0x82024
#define MSM8998_RX1_USB2_CLKREF_CBCR    0x88014
#define MSM8998_USB30_MASTER_CMD_RCGR   0xF014
#define MSM8998_USB30_MOCK_UTMI_CMD_RCGR 0xF028
#define MSM8998_QUSB2PHY_PRIM_BCR       0x12000

/* GPLL0 main divided by 5 for a 120 MHz master, the crystal undivided for mock UTMI */
#define MSM8998_MASTER_120MHZ           0x109
#define MSM8998_MOCK_UTMI_19_2MHZ       0x001

/* Fuse whose low nibble holds the factory transmit current trim, taken by TUNE1 */
#define MSM8998_QUSB2_TRIM_FUSE         0x0078423AULL
#define MSM8998_QUSB2_TUNE1             0x23C

/* This generation is only held by its power down bit while programmed */
#define MSM8998_QUSB2_HOLD              0x01

static const KDDWC_REG_WRITE Msm8998Qusb2Setup[] =
{
    { 0x004, 0x13 }, { 0x18C, 0x7C }, { 0x02C, 0x80 }, { 0x184, 0x0A },
    { 0x23C, 0xA5 }, { 0x240, 0x09 }, { 0x0B4, 0x19 }
};

static const ULONG Msm8998Branches[] =
{
    MSM8998_CFG_NOC_USB3_CBCR,
    MSM8998_USB30_MASTER_CBCR,
    MSM8998_AGGRE1_USB3_CBCR,
    MSM8998_USB30_SLEEP_CBCR,
    MSM8998_USB30_MOCK_UTMI_CBCR,
    MSM8998_AHB2PHY_CBCR,
    MSM8998_RX1_USB2_CLKREF_CBCR
};

static const KDDWC_CLOCK_PLAN Msm8998Clocks =
{
    KDDWC_GCC_BASE,
    MSM8998_GPLL0_MODE,
    MSM8998_APCS_GPLL_VOTE,
    MSM8998_APCS_GPLL0_VOTE,
    MSM8998_USB30_GDSCR,
    MSM8998_USB30_MASTER_CMD_RCGR,
    MSM8998_MASTER_120MHZ,
    MSM8998_USB30_MOCK_UTMI_CMD_RCGR,
    MSM8998_MOCK_UTMI_19_2MHZ,
    0,
    MSM8998_USB30_BCR,
    Msm8998Branches,
    RTL_NUMBER_OF(Msm8998Branches)
};

static const KDDWC_QUSB2_TRIM Msm8998Qusb2Trim =
{
    MSM8998_QUSB2_TRIM_FUSE,
    0,
    0x0F,
    MSM8998_QUSB2_TUNE1
};

/*
 * Firmware runs this controller in device mode, so its PHY is only set up
 * again when it is down. The PHY supplies are on RPM, which is not voted here.
 */
static
BOOLEAN
NTAPI
Msm8998PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Msm8998Clocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Msm8998StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcQusb2IsRunning(MSM8998_QUSB2_BASE))
        return TRUE;

    return KdDwcStartQusb2(MSM8998_QUSB2_BASE,
                           KDDWC_GCC_BASE + MSM8998_QUSB2PHY_PRIM_BCR,
                           MSM8998_QUSB2_HOLD,
                           Msm8998Qusb2Setup,
                           RTL_NUMBER_OF(Msm8998Qusb2Setup),
                           NULL,
                           0,
                           &Msm8998Qusb2Trim);
}

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Msm8998IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Msm8998Clocks);
}

const KDDWC_PLATFORM KdDwcPlatformMsm8998 =
{
    "MSM8998",
    { "MSM8998", NULL, NULL, NULL, NULL },
    MSM8998_CORE_BASE,
    Msm8998PowerUp,
    0,
    NULL,
    Msm8998IsPowered,
    Msm8998StartPhy
};
