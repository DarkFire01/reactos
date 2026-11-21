/*
 * PROJECT:     ReactOS ChipIdea Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MSM8916 and APQ8016 high speed USB controller and its ULPI PHY
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kdci.h"

#define MSM8916_CORE_BASE               0x078D9000ULL
#define MSM8916_GCC_BASE                0x01800000ULL

/* Clock controller offsets */
#define MSM8916_GPLL0_STATUS            0x2101C
#define MSM8916_GPLL0_ACTIVE            0x00020000
#define MSM8916_APCS_GPLL_VOTE          0x45000
#define MSM8916_APCS_GPLL0_VOTE         0x00000001
#define MSM8916_USB_HS_BCR              0x41000
#define MSM8916_USB_HS_SYSTEM_CBCR      0x41004
#define MSM8916_USB_HS_AHB_CBCR         0x41008
#define MSM8916_USB_HS_SYSTEM_CMD_RCGR  0x41010
#define MSM8916_USB2A_PHY_BCR           0x41028
#define MSM8916_USB2A_PHY_SLEEP_CBCR    0x4102C

/* GPLL0 at 800 MHz divided by 10 for the 80 MHz system clock */
#define MSM8916_SYSTEM_80MHZ            0x113

#define GCC_CBCR_ENABLE                 0x00000001
#define GCC_CBCR_CLK_OFF                0x80000000
#define GCC_RCG_UPDATE                  0x00000001
#define GCC_RCG_CFG_MASK                0x0000071F

/* Qualcomm registers next to the controller's own, offsets from its base */
#define MSM8916_SBUSCFG                 0x090
#define MSM8916_SBUSCFG_AHB_BURST_MASK  0x00000007
#define MSM8916_AHB_MODE                0x098
#define MSM8916_AHB_MODE_TRANSACTOR     0x00000008
#define MSM8916_GENCONFIG               0x09C
#define MSM8916_GENCONFIG_TXFIFO_IDLE_FORCE_DIS 0x00000010
#define MSM8916_GENCONFIG_2             0x0A0
#define MSM8916_GENCONFIG_2_SESSION_VALID_EN    0x00000080
#define MSM8916_GENCONFIG_2_ULPI_TX_PKT_FIX     0x00080000
#define MSM8916_HS_PHY_CTRL             0x240
#define MSM8916_HS_PHY_CTRL_POR         0x00000001

/* ULPI registers of the PHY, set and clear forms are one and two above the register */
#define ULPI_PWR_CLK_MNG_SET            0x89
#define ULPI_PWR_OTG_COMP_DISABLE       0x01
#define ULPI_MISC_A_SET                 0x97
#define ULPI_MISC_A_VBUS_VALID_EXT_SEL  0x02
#define ULPI_MISC_A_VBUS_VALID_EXT      0x01

#define KDCI_CLOCK_TIMEOUT_US           10000

/* Vendor registers 0x80 to 0x83 of the PHY, as the board files set them */
static const UCHAR Msm8916PhySetup[][2] =
{
    { 0x80, 0x44 },
    { 0x81, 0x6B },
    { 0x82, 0x24 },
    { 0x83, 0x13 }
};

static const ULONG Msm8916Branches[] =
{
    MSM8916_USB_HS_AHB_CBCR,
    MSM8916_USB_HS_SYSTEM_CBCR,
    MSM8916_USB2A_PHY_SLEEP_CBCR
};

static
BOOLEAN
Msm8916ClockOn(
    _In_ ULONG Cbcr)
{
    PUCHAR Register = KdCiMapRegister(MSM8916_GCC_BASE + Cbcr);

    if (Register == NULL)
        return FALSE;

    if (!(KdCiRead(Register, 0) & GCC_CBCR_CLK_OFF))
        return TRUE;

    KdCiModify(Register, 0, 0, GCC_CBCR_ENABLE);
    return KdCiWaitFor(Register, 0, GCC_CBCR_CLK_OFF, 0, KDCI_CLOCK_TIMEOUT_US);
}

static
VOID
Msm8916PulseReset(
    _In_ ULONG Bcr,
    _In_ ULONG HoldUs)
{
    PUCHAR Register = KdCiMapRegister(MSM8916_GCC_BASE + Bcr);

    if (Register == NULL)
        return;

    KdCiModify(Register, 0, 0, 1);
    KdCiStall(HoldUs);
    KdCiModify(Register, 0, 1, 0);
    KdCiStall(100);
}

/*
 * GPLL0 is voted on and the system clock set to 80 MHz from it, the block is
 * reset with its branches still off and then the controller and PHY clocks
 * are enabled. The PHY supplies are on RPM and stay as firmware left them.
 */
static
BOOLEAN
NTAPI
Msm8916PowerUp(
    _In_ const KDCI_PLATFORM* Platform)
{
    PUCHAR Vote = KdCiMapRegister(MSM8916_GCC_BASE + MSM8916_APCS_GPLL_VOTE);
    PUCHAR Pll = KdCiMapRegister(MSM8916_GCC_BASE + MSM8916_GPLL0_STATUS);
    PUCHAR Root = KdCiMapRegister(MSM8916_GCC_BASE + MSM8916_USB_HS_SYSTEM_CMD_RCGR);
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    if (Vote == NULL || Pll == NULL || Root == NULL)
        return FALSE;

    KdCiModify(Vote, 0, 0, MSM8916_APCS_GPLL0_VOTE);
    if (!KdCiWaitFor(Pll, 0, MSM8916_GPLL0_ACTIVE, MSM8916_GPLL0_ACTIVE, KDCI_CLOCK_TIMEOUT_US))
        return FALSE;

    KdCiModify(Root, 4, GCC_RCG_CFG_MASK, MSM8916_SYSTEM_80MHZ);
    KdCiModify(Root, 0, 0, GCC_RCG_UPDATE);
    if (!KdCiWaitFor(Root, 0, GCC_RCG_UPDATE, 0, KDCI_CLOCK_TIMEOUT_US))
        return FALSE;

    Msm8916PulseReset(MSM8916_USB_HS_BCR, 10000);
    Msm8916PulseReset(MSM8916_USB2A_PHY_BCR, 100);

    for (i = 0; i < RTL_NUMBER_OF(Msm8916Branches); i++)
    {
        if (!Msm8916ClockOn(Msm8916Branches[i]))
            return FALSE;
    }

    return TRUE;
}

/*
 * The PHY gets its board values, then a power on reset through the
 * controller's PHY control register. The glue then takes AHB transfers, skips
 * the TX FIFO idle force and the ULPI packet fix, and VBUS is reported valid
 * by the PHY's external select since nothing else reports it.
 */
static
BOOLEAN
NTAPI
Msm8916StartPhy(
    _In_ const KDCI_PLATFORM* Platform)
{
    PUCHAR Base = KdCiMapRegister(Platform->CoreBase);
    ULONG i;

    if (Base == NULL)
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Msm8916PhySetup); i++)
    {
        if (!KdCiUlpiWrite(Msm8916PhySetup[i][0], Msm8916PhySetup[i][1]))
            return FALSE;
    }

    KdCiModify(Base, MSM8916_HS_PHY_CTRL, 0, MSM8916_HS_PHY_CTRL_POR);
    KdCiStall(15);
    KdCiModify(Base, MSM8916_HS_PHY_CTRL, MSM8916_HS_PHY_CTRL_POR, 0);
    KdCiStall(100);

    KdCiModify(Base, MSM8916_SBUSCFG, MSM8916_SBUSCFG_AHB_BURST_MASK, 0);
    KdCiWrite(Base, MSM8916_AHB_MODE, MSM8916_AHB_MODE_TRANSACTOR);
    KdCiModify(Base, MSM8916_GENCONFIG, MSM8916_GENCONFIG_TXFIFO_IDLE_FORCE_DIS, 0);
    KdCiModify(Base,
               MSM8916_GENCONFIG_2,
               MSM8916_GENCONFIG_2_ULPI_TX_PKT_FIX,
               MSM8916_GENCONFIG_2_SESSION_VALID_EN);

    return KdCiUlpiWrite(ULPI_PWR_CLK_MNG_SET, ULPI_PWR_OTG_COMP_DISABLE) &&
           KdCiUlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUS_VALID_EXT_SEL | ULPI_MISC_A_VBUS_VALID_EXT);
}

/* Reads only the clock controller, so it can run whatever state the controller is in */
static
BOOLEAN
NTAPI
Msm8916IsPowered(
    _In_ const KDCI_PLATFORM* Platform)
{
    PUCHAR Register;
    ULONG i;

    UNREFERENCED_PARAMETER(Platform);

    for (i = 0; i < RTL_NUMBER_OF(Msm8916Branches); i++)
    {
        Register = KdCiMapRegister(MSM8916_GCC_BASE + Msm8916Branches[i]);
        if (Register == NULL || (KdCiRead(Register, 0) & GCC_CBCR_CLK_OFF))
            return FALSE;
    }

    return TRUE;
}

const KDCI_PLATFORM KdCiPlatformMsm8916 =
{
    "MSM8916",
    { "MSM8916", "APQ8016", "SDM8916", NULL },
    MSM8916_CORE_BASE,
    TRUE,
    CI_USBCMD_SESSION_VALID,
    Msm8916PowerUp,
    Msm8916StartPhy,
    Msm8916IsPowered
};
