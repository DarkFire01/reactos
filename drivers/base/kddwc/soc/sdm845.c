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

/* Held powered down, clamped and with the supply trim off while programmed */
#define SDM845_QUSB2_HOLD               0x23

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

/* The whole block is reset and brought up from scratch, the same way the host driver does it */
static
BOOLEAN
NTAPI
Sdm845PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SDM845_USB30_PRIM_GDSCR))
        return FALSE;

    KdDwcPulseReset(KDDWC_GCC_BASE + SDM845_USB30_PRIM_BCR);

    for (i = 0; i < RTL_NUMBER_OF(Sdm845Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sdm845Branches[i]))
            return FALSE;
    }

    return KdDwcStartQusb2(SDM845_QUSB2_BASE,
                           KDDWC_GCC_BASE + SDM845_QUSB2PHY_PRIM_BCR,
                           SDM845_QUSB2_HOLD,
                           Sdm845Qusb2Setup,
                           RTL_NUMBER_OF(Sdm845Qusb2Setup));
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
    RTL_NUMBER_OF(Sdm845Supplies)
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
    Sdm845IsPowered
};
