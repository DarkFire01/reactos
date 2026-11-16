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
#define SC7180_USB30_PRIM_GDSCR         0xF004
#define SC7180_USB30_MASTER_CBCR        0xF010
#define SC7180_USB30_SLEEP_CBCR         0xF014
#define SC7180_USB30_MOCK_UTMI_CBCR     0xF018
#define SC7180_CFG_NOC_USB3_CBCR        0x502C
#define SC7180_AHB2PHY_CBCR             0x6A004
#define SC7180_AGGRE_USB3_CBCR          0x8201C
#define SC7180_QUSB2PHY_PRIM_BCR        0x26000

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

static const ULONG Sc7180Branches[] =
{
    SC7180_CFG_NOC_USB3_CBCR,
    SC7180_USB30_MASTER_CBCR,
    SC7180_AGGRE_USB3_CBCR,
    SC7180_USB30_SLEEP_CBCR,
    SC7180_USB30_MOCK_UTMI_CBCR,
    SC7180_AHB2PHY_CBCR
};

/* Firmware runs this controller in device mode, so its PHY is only set up again when it is down */
static
BOOLEAN
NTAPI
Sc7180PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + SC7180_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Sc7180Branches); i++)
    {
        if (!KdDwcClockOn(KDDWC_GCC_BASE + Sc7180Branches[i]))
            return FALSE;
    }

    if (KdDwcQusb2IsRunning(SC7180_QUSB2_BASE))
        return TRUE;

    return KdDwcStartQusb2(SC7180_QUSB2_BASE,
                           KDDWC_GCC_BASE + SC7180_QUSB2PHY_PRIM_BCR,
                           SC7180_QUSB2_HOLD,
                           Sc7180Qusb2Setup,
                           RTL_NUMBER_OF(Sc7180Qusb2Setup));
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

static const KDDWC_RPMH Sc7180Rpmh =
{
    0x18220000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x80820000ULL,
    KDDWC_CMD_DB_BYTES,
    Sc7180BusNodes,
    RTL_NUMBER_OF(Sc7180BusNodes),
    NULL,
    0
};

const KDDWC_PLATFORM KdDwcPlatformSc7180 =
{
    "SC7180",
    { "SC7180", "SDM7180", NULL, NULL },
    SC7180_CORE_BASE,
    Sc7180PowerUp,
    0,
    &Sc7180Rpmh
};
