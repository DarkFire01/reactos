/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SC7180 (Snapdragon 7c) primary USB controller and its QUSB2 PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define SC7180_CORE_BASE                0x0A600000ULL
#define SC7180_QUSB2_BASE               0x088E3000ULL

/* Clock controller offsets */
#define SC7180_GPLL0_MODE               0x00000
#define SC7180_APCS_GPLL_VOTE           0x52010
#define SC7180_APCS_GPLL0_VOTE          0x00000001
#define SC7180_USB30_PRIM_BCR           0xF000
#define SC7180_USB30_PRIM_GDSCR         0xF004
#define SC7180_USB30_MASTER_CBCR        0xF010
#define SC7180_USB30_SLEEP_CBCR         0xF014
#define SC7180_USB30_MOCK_UTMI_CBCR     0xF018
#define SC7180_CFG_NOC_USB3_CBCR        0x502C
#define SC7180_AHB2PHY_CBCR             0x6A004
#define SC7180_AGGRE_USB3_CBCR          0x8201C
#define SC7180_USB30_MASTER_CMD_RCGR    0xF01C
#define SC7180_USB30_MOCK_UTMI_CMD_RCGR 0xF034
#define SC7180_QUSB2PHY_PRIM_BCR        0x26000

/* GPLL0 main divided by 3 for a 200 MHz master, the crystal undivided for mock UTMI */
#define SC7180_MASTER_200MHZ            0x105
#define SC7180_MOCK_UTMI_19_2MHZ        0x001

/* Fuse whose bits 3:1 hold the factory transmit current trim */
#define SC7180_QUSB2_TRIM_FUSE          0x0078425BULL

/* Held powered down, clamped and with the supply trim off while programmed */
#define SC7180_QUSB2_HOLD               0x23

/* Second generation QUSB2 setup, the same one the SDM845 uses */
static const KDDWC_REG_WRITE Sc7180Qusb2Setup[] =
{
    { 0x004, 0x03 }, { 0x18C, 0x7C }, { 0x02C, 0x80 }, { 0x184, 0x0A },
    { 0x0B4, 0x19 }, { 0x194, 0x40 }, { 0x198, 0x20 }, { 0x214, 0x21 },
    { 0x220, 0x00 }, { 0x224, 0x58 }, { 0x240, 0x30 }, { 0x244, 0x29 },
    { 0x248, 0xCA }, { 0x24C, 0x04 }, { 0x250, 0x03 }, { 0x23C, 0x00 }
};

/*
 * Tuning every SC7180 board uses: impedance offset 8, bias control 0x22,
 * charge pump 3, 15 percent pre-emphasis held for half a bit, no disconnect trim.
 */
static const KDDWC_REG_MODIFY Sc7180Qusb2Tuning[] =
{
    { 0x220, 0x3F, 0x08 },
    { 0x198, 0x3F, 0x22 },
    { 0x23C, 0x30, 0x30 },
    { 0x240, 0x07, 0x07 },
    { 0x244, 0x03, 0x00 }
};

static const KDDWC_QUSB2_TRIM Sc7180Qusb2Trim =
{
    SC7180_QUSB2_TRIM_FUSE,
    1,
    0x07,
    0x240
};

static const ULONG Sc7180Branches[] =
{
    SC7180_CFG_NOC_USB3_CBCR,
    SC7180_USB30_MASTER_CBCR,
    SC7180_AGGRE_USB3_CBCR,
    SC7180_USB30_SLEEP_CBCR,
    SC7180_USB30_MOCK_UTMI_CBCR,
    SC7180_AHB2PHY_CBCR
};

static const KDDWC_CLOCK_PLAN Sc7180Clocks =
{
    KDDWC_GCC_BASE,
    SC7180_GPLL0_MODE,
    SC7180_APCS_GPLL_VOTE,
    SC7180_APCS_GPLL0_VOTE,
    SC7180_USB30_PRIM_GDSCR,
    SC7180_USB30_MASTER_CMD_RCGR,
    SC7180_MASTER_200MHZ,
    SC7180_USB30_MOCK_UTMI_CMD_RCGR,
    SC7180_MOCK_UTMI_19_2MHZ,
    0,
    SC7180_USB30_PRIM_BCR,
    Sc7180Branches,
    RTL_NUMBER_OF(Sc7180Branches)
};

/* Firmware runs this controller in device mode, so its PHY is only set up again when it is down */
static
BOOLEAN
NTAPI
Sc7180PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcRunClockPlan(&Sc7180Clocks);
}

/* Runs after the core has stopped suspending its PHY, as the core driver orders it */
static
BOOLEAN
NTAPI
Sc7180StartPhy(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);

    if (KdDwcQusb2IsRunning(SC7180_QUSB2_BASE))
        return TRUE;

    return KdDwcStartQusb2(SC7180_QUSB2_BASE,
                           KDDWC_GCC_BASE + SC7180_QUSB2PHY_PRIM_BCR,
                           SC7180_QUSB2_HOLD,
                           Sc7180Qusb2Setup,
                           RTL_NUMBER_OF(Sc7180Qusb2Setup),
                           Sc7180Qusb2Tuning,
                           RTL_NUMBER_OF(Sc7180Qusb2Tuning),
                           &Sc7180Qusb2Trim);
}

/* Buses from the controller to memory and from the processor to its registers */
static const KDDWC_BUS_NODE Sc7180BusNodes[] =
{
    KDDWC_MEMORY_NODE("SN9", 16, 1),
    KDDWC_MEMORY_NODE("SN0", 16, 1),
    KDDWC_MEMORY_NODE("SH0", 16, 1),
    KDDWC_MEMORY_NODE("MC0", 4, 2),
    KDDWC_ACV_NODE,
    KDDWC_REGISTER_NODE("SH4", 16, 1),
    KDDWC_REGISTER_NODE("SN12", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 8, 1),
    KDDWC_REGISTER_NODE("CN0", 4, 1)
};

/* The supplies of the high speed PHY on Trogdor, the IDP and the Aspire 1 */
static const KDDWC_SUPPLY Sc7180Supplies[] =
{
    { "ldoa4", 880 },
    { "ldoa11", 1800 },
    { "ldoa17", 3088 }
};

static const KDDWC_RPMH Sc7180Rpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80820000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc7180BusNodes,
    RTL_NUMBER_OF(Sc7180BusNodes),
    Sc7180Supplies,
    RTL_NUMBER_OF(Sc7180Supplies),

    /* A 200 MHz master needs the logic rail at its nominal level */
    "cx.lvl",
    KDDWC_RAIL_NOMINAL
};

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Sc7180IsPowered(
    _In_ const KDDWC_PLATFORM* Platform)
{
    UNREFERENCED_PARAMETER(Platform);
    return KdDwcClockPlanRunning(&Sc7180Clocks);
}

const KDDWC_PLATFORM KdDwcPlatformSc7180 =
{
    "SC7180",
    { "SC7180", "SDM7180", NULL, NULL, NULL },
    SC7180_CORE_BASE,
    Sc7180PowerUp,
    0,
    &Sc7180Rpmh,
    Sc7180IsPowered,
    Sc7180StartPhy
};
