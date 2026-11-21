/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     X1E80100 (Snapdragon X Elite and X Plus) first USB controller, its eUSB2 PHY and repeater
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define X1E80100_CORE_BASE              0x0A600000ULL
#define X1E80100_EUSB2_BASE             0x00FD3000ULL

/* Clock controller offsets */
#define X1E80100_GPLL0_MODE             0x00000
#define X1E80100_APCS_GPLL_VOTE         0x52030
#define X1E80100_APCS_GPLL0_VOTE        0x00000001
#define X1E80100_QUSB2PHY_PRIM_BCR      0x12000
#define X1E80100_USB30_PRIM_BCR         0x39000
#define X1E80100_USB30_PRIM_GDSCR       0x39004
#define X1E80100_USB30_MASTER_CBCR      0x39018
#define X1E80100_USB30_SLEEP_CBCR       0x39024
#define X1E80100_USB30_MOCK_UTMI_CBCR   0x39028
#define X1E80100_USB30_MASTER_CMD_RCGR  0x3902C
#define X1E80100_USB30_MOCK_UTMI_CMD_RCGR 0x39044
#define X1E80100_CFG_NOC_USB3_CBCR      0x3908C
#define X1E80100_AGGRE_USB3_CBCR        0x39090
#define X1E80100_SYS_NOC_USB_AXI_CBCR   0x2D014
#define X1E80100_CFG_NOC_ANOC_AHB_CBCR  0x2D024
#define X1E80100_CFG_NOC_ANOC_NORTH_CBCR 0x2D028
#define X1E80100_CFG_NOC_ANOC_SOUTH_CBCR 0x2D02C
#define X1E80100_AGGRE_USB_NOC_AXI_CBCR 0x2D034
#define X1E80100_AGGRE_NOC_SOUTH_CBCR   0x2D174
#define X1E80100_AGGRE_NOC_NORTH_CBCR   0x2D17C
#define X1E80100_USB_0_PHY_GDSCR        0x50024

/* The USB NoC's AHB branches are voted on through the application processor's vote registers */
#define X1E80100_APCS_CLOCK_VOTE_1      0x52018
#define X1E80100_APCS_CLOCK_VOTE_2      0x52028
#define X1E80100_ANOC_SOUTH_VOTE        0x00000080
#define X1E80100_ANOC_AHB_VOTE          0x00200000
#define X1E80100_ANOC_NORTH_VOTE        0x00800000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define X1E80100_MASTER_200MHZ          0x105
#define X1E80100_MOCK_UTMI_19_2MHZ      0x001

/* The first port's eUSB2 repeater is in the SMB2360 at SID 7 of the second SPMI bus */
#define X1E80100_REPEATER_BUS           1
#define X1E80100_REPEATER_SID           7
#define X1E80100_REPEATER_BASE          0xFD00

/* Branches the controller and its path to memory need, in the order they are enabled */
static const ULONG X1e80100Branches[] =
{
    X1E80100_CFG_NOC_USB3_CBCR,
    X1E80100_USB30_MASTER_CBCR,
    X1E80100_AGGRE_USB3_CBCR,
    X1E80100_USB30_SLEEP_CBCR,
    X1E80100_USB30_MOCK_UTMI_CBCR,
    X1E80100_AGGRE_USB_NOC_AXI_CBCR,
    X1E80100_AGGRE_NOC_NORTH_CBCR,
    X1E80100_AGGRE_NOC_SOUTH_CBCR,
    X1E80100_SYS_NOC_USB_AXI_CBCR
};

static const struct
{
    ULONG Vote;
    ULONG Bit;
    ULONG Cbcr;
} X1e80100VotedBranches[] =
{
    { X1E80100_APCS_CLOCK_VOTE_2, X1E80100_ANOC_AHB_VOTE, X1E80100_CFG_NOC_ANOC_AHB_CBCR },
    { X1E80100_APCS_CLOCK_VOTE_2, X1E80100_ANOC_NORTH_VOTE, X1E80100_CFG_NOC_ANOC_NORTH_CBCR },
    { X1E80100_APCS_CLOCK_VOTE_1, X1E80100_ANOC_SOUTH_VOTE, X1E80100_CFG_NOC_ANOC_SOUTH_CBCR }
};

/* SMB2360 tuning of the repeater: amplitude, squelch threshold and pre-emphasis */
static const KDDWC_REG_WRITE X1e80100RepeaterTuning[] =
{
    { 0x51, 0x05 },
    { 0x54, 0x03 },
    { 0x57, 0x02 }
};

static const KDDWC_SPMI X1e80100Spmi =
{
    0x0C400000ULL,
    0x0C500000ULL,
    0x0C440000ULL,
    { 0x0C42D000ULL, 0x0C432000ULL }
};

/*
 * Firmware's exit can leave the controller and its PHY power domain collapsed
 * and the USB NoC clocks off, so all of them are brought up the way the clock
 * driver does it, with the roots set again and the block reset while its
 * branches are still off.
 */
static
BOOLEAN
NTAPI
X1e80100PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPllOn(KDDWC_GCC_BASE + X1E80100_GPLL0_MODE,
                    KDDWC_GCC_BASE + X1E80100_APCS_GPLL_VOTE,
                    X1E80100_APCS_GPLL0_VOTE))
    {
        return FALSE;
    }

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + X1E80100_USB30_PRIM_GDSCR) ||
        !KdDwcPowerDomainOn(KDDWC_GCC_BASE + X1E80100_USB_0_PHY_GDSCR))
    {
        return FALSE;
    }

    for (i = 0; i < RTL_NUMBER_OF(X1e80100VotedBranches); i++)
    {
        if (!KdDwcVoteClockOn(KDDWC_GCC_BASE + X1e80100VotedBranches[i].Vote,
                              X1e80100VotedBranches[i].Bit,
                              KDDWC_GCC_BASE + X1e80100VotedBranches[i].Cbcr))
        {
            return FALSE;
        }
    }

    if (!KdDwcConfigureRoot(KDDWC_GCC_BASE + X1E80100_USB30_MASTER_CMD_RCGR, X1E80100_MASTER_200MHZ) ||
        !KdDwcConfigureRoot(KDDWC_GCC_BASE + X1E80100_USB30_MOCK_UTMI_CMD_RCGR, X1E80100_MOCK_UTMI_19_2MHZ))
    {
        return FALSE;
    }

    KdDwcPulseReset(KDDWC_GCC_BASE + X1E80100_USB30_PRIM_BCR);

    for (i = 0; i < RTL_NUMBER_OF(X1e80100Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + X1e80100Branches[i]))
            return FALSE;
    }

    return TRUE;
}

/* The repeater comes up before the PHY leaves reset, then the PHY itself */
static
BOOLEAN
NTAPI
X1e80100StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcStartEusb2Repeater(&X1e80100Spmi,
                                 X1E80100_REPEATER_BUS,
                                 X1E80100_REPEATER_SID,
                                 X1E80100_REPEATER_BASE,
                                 X1e80100RepeaterTuning,
                                 RTL_NUMBER_OF(X1e80100RepeaterTuning)))
    {
        return FALSE;
    }

    return KdDwcStartEusb2(X1E80100_EUSB2_BASE, KDDWC_GCC_BASE + X1E80100_QUSB2PHY_PRIM_BCR);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE X1e80100BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN4", 64, 1),
    KDDWC_MEMORY_NODE("SN0", 64, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 8),
    KDDWC_MEMORY_NODE("MC0", 4, 8),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH1", 32, 6),
    KDDWC_REGISTER_NODE("SH1", 16, 1),
    KDDWC_REGISTER_NODE("CN0", 16, 1),
    KDDWC_REGISTER_NODE("CN0", 4, 1)
};

/*
 * What the first port's PEP component votes: the eUSB2 PHY, its repeater and
 * the combo PHY's supplies, and the PMIC clock buffer the port runs from.
 */
static const KDDWC_SUPPLY X1e80100Supplies[] =
{
    { "ldod3", 1800 },
    { "ldoi1", 1800 },
    { "ldoj1", 912 },
    { "ldob2", 3072 },
    { "ldoj2", 1256 },
    { "ldoj3", 880 },
    { "ldod1", 880 },
    { "clka6", 0 }
};

static const KDDWC_RPMH X1e80100Rpmh =
{
    0x17520000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x81C60000ULL,
    KDDWC_CMD_DB_BYTES,
    X1e80100BusNodes,
    RTL_NUMBER_OF(X1e80100BusNodes),
    X1e80100Supplies,
    RTL_NUMBER_OF(X1e80100Supplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
X1e80100IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClocksRunning(KDDWC_GCC_BASE, X1E80100_USB_0_PHY_GDSCR, NULL, 0) &&
           KdDwcClocksRunning(KDDWC_GCC_BASE, X1E80100_USB30_PRIM_GDSCR, X1e80100Branches, RTL_NUMBER_OF(X1e80100Branches));
}

const KDDWC_PLATFORM KdDwcPlatformX1e80100 =
{
    "X1E80100",
    { "X1E80100", "SC8380XP", "SC8380", "SDM8380", "X1E" },
    X1E80100_CORE_BASE,
    X1e80100PowerUp,
    0,
    &X1e80100Rpmh,
    X1e80100IsPowered,
    X1e80100StartPhy
};
