/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     X1E80100 (Snapdragon X Elite and X Plus) first USB controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kddwc.h"

#define X1E80100_CORE_BASE              0x0A600000ULL

/* Clock controller offsets */
#define X1E80100_USB30_PRIM_GDSCR       0x39004
#define X1E80100_USB30_MASTER_CBCR      0x39018
#define X1E80100_USB30_SLEEP_CBCR       0x39024
#define X1E80100_USB30_MOCK_UTMI_CBCR   0x39028
#define X1E80100_CFG_NOC_USB3_CBCR      0x3908C
#define X1E80100_AGGRE_USB3_CBCR        0x39090

static const ULONG X1e80100Branches[] =
{
    X1E80100_USB30_MASTER_CBCR,
    X1E80100_USB30_SLEEP_CBCR,
    X1E80100_USB30_MOCK_UTMI_CBCR,
    X1E80100_CFG_NOC_USB3_CBCR,
    X1E80100_AGGRE_USB3_CBCR
};

/*
 * The high speed side is an eUSB2 PHY behind a repeater in the PMIC, which is
 * reached over SPMI. Firmware sets both up for its own USB function, so the
 * controller is used as firmware left it and nothing here is written but the
 * power domain when it is collapsed.
 */
static
BOOLEAN
NTAPI
X1e80100PowerUp(
    _In_ const KDDWC_PLATFORM* Platform)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (!KdDwcPowerDomainOn(KDDWC_GCC_BASE + X1E80100_USB30_PRIM_GDSCR))
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(X1e80100Branches); i++)
    {
        if (!KdDwcVotedClockIsOn(KDDWC_GCC_BASE + X1e80100Branches[i]))
            return FALSE;
    }

    return TRUE;
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

static const KDDWC_RPMH X1e80100Rpmh =
{
    0x17520000ULL,
    KDDWC_RPMH_DRV2_TCS_OFFSET,
    KDDWC_RPMH_FIRST_ACTIVE_TCS,
    0x81C60000ULL,
    KDDWC_CMD_DB_BYTES,
    X1e80100BusNodes,
    RTL_NUMBER_OF(X1e80100BusNodes),
    NULL,
    0
};

const KDDWC_PLATFORM KdDwcPlatformX1e80100 =
{
    "X1E80100",
    { "X1E80100", "SC8380XP", "SC8380", "X1E" },
    X1E80100_CORE_BASE,
    X1e80100PowerUp,
    KDDWC_PLATFORM_KEEP_PHY,
    &X1e80100Rpmh
};
