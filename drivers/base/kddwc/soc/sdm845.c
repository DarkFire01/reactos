/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SDM845 and SDM850 primary USB controller and its QUSB2 PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SDM845_CORE_BASE                0x0A600000ULL
#define SDM845_QUSB2_BASE               0x088E2000ULL

/* Clock controller offsets */
#define SDM845_USB30_PRIM_BCR           0xF000
#define SDM845_USB30_PRIM_GDSCR         0xF004
#define SDM845_USB30_MASTER_CBCR        0xF00C
#define SDM845_USB30_SLEEP_CBCR         0xF010
#define SDM845_USB30_MOCK_UTMI_CBCR     0xF014
#define SDM845_CFG_NOC_USB3_CBCR        0x502C
#define SDM845_AHB2PHY_CBCR             0x6A004
#define SDM845_AGGRE_USB3_CBCR          0x8201C
#define SDM845_QUSB2PHY_PRIM_BCR        0x12000

/* The master and mock UTMI roots, and the PLL the master runs from */
#define SDM845_USB30_MASTER_CMD_RCGR    0xF018
#define SDM845_USB30_MOCK_UTMI_CMD_RCGR 0xF030
#define SDM845_GPLL0_MODE               0x00000
#define SDM845_APCS_GPLL_VOTE           0x52000
#define SDM845_APCS_GPLL0_VOTE          0x00000001

/* GPLL0 main divided by 3 for a 200 MHz master, the board crystal undivided for mock UTMI */
#define SDM845_MASTER_200MHZ            0x105
#define SDM845_MOCK_UTMI_19_2MHZ        0x001

/* Held powered down, clamped and with the supply trim off while programmed */
#define SDM845_QUSB2_HOLD               0x23

/* Fuse byte whose bits 4:1 hold the factory transmit current trim of the PHY */
#define SDM845_QUSB2_TRIM_FUSE          0x007841EBULL

/*
 * OnePlus 6 tuning over the reference table: impedance offset 8, 21.6 mA
 * transmit current, 5 percent pre-emphasis held for half a bit.
 */
static const KDDWC_REG_MODIFY Sdm845Qusb2Tuning[] =
{
    { 0x220, 0x3F, 0x08 },
    { 0x240, 0xF7, 0x45 }
};

static const KDDWC_REG_WRITE Sdm845Qusb2Setup[] =
{
    { 0x004, 0x03 }, { 0x18C, 0x7C }, { 0x02C, 0x80 }, { 0x184, 0x0A },
    { 0x0B4, 0x19 }, { 0x194, 0x40 }, { 0x198, 0x20 }, { 0x214, 0x21 },
    { 0x220, 0x00 }, { 0x224, 0x58 }, { 0x240, 0x30 }, { 0x244, 0x29 },
    { 0x248, 0xCA }, { 0x24C, 0x04 }, { 0x250, 0x03 }, { 0x23C, 0x00 }
};

static const ULONG Sdm845Branches[] =
{
    SDM845_CFG_NOC_USB3_CBCR,
    SDM845_USB30_MASTER_CBCR,
    SDM845_AGGRE_USB3_CBCR,
    SDM845_USB30_SLEEP_CBCR,
    SDM845_USB30_MOCK_UTMI_CBCR,
    SDM845_AHB2PHY_CBCR
};

/*
 * Firmware's exit leaves the roots of the controller's clocks as it pleases,
 * so they are set again the way the clock driver sets them: the master at
 * 200 MHz from GPLL0 and mock UTMI from the crystal. The block is reset while
 * its branches are still off.
 */
static
BOOLEAN
NTAPI
Sdm845PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPllOn(KDDWC_GCC_BASE + SDM845_GPLL0_MODE,
                    KDDWC_GCC_BASE + SDM845_APCS_GPLL_VOTE,
                    SDM845_APCS_GPLL0_VOTE))
    {
        return FALSE;
    }

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SDM845_USB30_PRIM_GDSCR))
        return FALSE;

    if (!KdDwcConfigureRoot(KDDWC_GCC_BASE + SDM845_USB30_MASTER_CMD_RCGR, SDM845_MASTER_200MHZ) ||
        !KdDwcConfigureRoot(KDDWC_GCC_BASE + SDM845_USB30_MOCK_UTMI_CMD_RCGR, SDM845_MOCK_UTMI_19_2MHZ))
    {
        return FALSE;
    }

    KdDwcPulseReset(KDDWC_GCC_BASE + SDM845_USB30_PRIM_BCR);

    for (i = 0; i < RTL_NUMBER_OF(Sdm845Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sdm845Branches[i]))
            return FALSE;
    }

    return TRUE;
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sdm845StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    return KdDwcStartQusb2(SDM845_QUSB2_BASE,
                           KDDWC_GCC_BASE + SDM845_QUSB2PHY_PRIM_BCR,
                           SDM845_QUSB2_HOLD,
                           Sdm845Qusb2Setup,
                           RTL_NUMBER_OF(Sdm845Qusb2Setup),
                           Sdm845Qusb2Tuning,
                           RTL_NUMBER_OF(Sdm845Qusb2Tuning),
                           SDM845_QUSB2_TRIM_FUSE);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sdm845BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN11", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 4),
    KDDWC_MEMORY_NODE("MC0", 4, 4),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SN12", 8, 1),
    KDDWC_REGISTER_NODE("SN3", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 4, 1)
};

/* The supplies of the high speed PHY on the boards this SoC ships in */
static const KDDWC_SUPPLY Sdm845Supplies[] =
{
    { "ldoa1", 880 },
    { "ldoa12", 1800 },
    { "ldoa24", 3088 }
};

static const KDDWC_RPMH Sdm845Rpmh =
{
    0x179E0000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x85FE0000ULL,
    KDDWC_CMD_DB_BYTES,
    Sdm845BusNodes,
    RTL_NUMBER_OF(Sdm845BusNodes),
    Sdm845Supplies,
    RTL_NUMBER_OF(Sdm845Supplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Sdm845IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClocksRunning(KDDWC_GCC_BASE, SDM845_USB30_PRIM_GDSCR, Sdm845Branches, RTL_NUMBER_OF(Sdm845Branches));
}

const KDDWC_PLATFORM KdDwcPlatformSdm845 =
{
    "SDM845",
    { "SDM845", "SDM850", NULL, NULL },
    SDM845_CORE_BASE,
    Sdm845PowerUp,
    0,
    &Sdm845Rpmh,
    Sdm845IsPowered,
    Sdm845StartPhy
};
