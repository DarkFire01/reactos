/*
 * PROJECT:     ReactOS Qualcomm xHCI lower filter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>

#define XHCIF_POOL_TAG                  'fHcQ'

/* Controller windows the role switch hands down, they also tell the SoCs apart */
#define XHCIF_CONTROLLER_BASE           0xF9200000UL
#define XHCIF_SDM845_CONTROLLER_BASE    0x0A600000UL

/* The window has to reach the end of the Qualcomm wrapper */
#define XHCIF_CONTROLLER_MIN_SIZE       0xF8C00

typedef enum _XHCIF_SOC
{
    XhcifSocMsm8994,
    XhcifSocSdm845
} XHCIF_SOC;

/* Fixed MSM8992 and MSM8994 blocks outside the controller window */
#define XHCIF_GCC_BASE                  0xFC400000UL
#define XHCIF_GCC_SIZE                  0x2000
#define XHCIF_TCSR_USB_BASE             0xFD4AB000UL
#define XHCIF_TCSR_USB_SIZE             PAGE_SIZE
#define XHCIF_SS_PHY_BASE               0xF9B38000UL
#define XHCIF_SS_PHY_SIZE               0x7000
#define XHCIF_JTAG_ID_ADDRESS           0xFD512028UL

/* JTAG part number of the MSM8992, which takes its own SuperSpeed PHY tuning */
#define XHCIF_JTAG_PART(Id)             (((Id) >> 12) & 0xFFFF)
#define XHCIF_JTAG_PART_MSM8992         0x969

/* xHCI port status of the high speed and SuperSpeed ports */
#define XHCIF_PORTSC_HS                 0x420
#define XHCIF_PORTSC_SS                 0x430
#define XHCIF_PORTSC_CCS                0x00000001
#define XHCIF_PORTSC_PR                 0x00000010
#define XHCIF_PORTSC_PLS_MASK           0x000001E0
#define XHCIF_PORTSC_PLS_U3             0x00000060

/* DWC3 global and device registers */
#define DWC3_GCTL                       0xC110
#define DWC3_GCTL_DSBLCLKGTNG           0x00000001
#define DWC3_GCTL_CORESOFTRESET         0x00000800
#define DWC3_GCTL_PRTCAPDIR_MASK        0x00003000
#define DWC3_GCTL_PRTCAPDIR_HOST        0x00001000
#define DWC3_GUCTL1                     0xC11C
#define DWC3_GUCTL1_PARKMODE_DISABLE_SS 0x00020000
#define DWC3_GDBGLTSSM                  0xC164
#define DWC3_GDBGLTSSM_STATE_MASK       0x03C00000
#define DWC3_GDBGLTSSM_STATE_U3         0x00C00000
#define DWC3_GDBGLTSSM_STATE_DISABLED   0x01000000
#define DWC3_GUSB2PHYCFG                0xC200
#define DWC3_GUSB2PHYCFG_SUSPEND        0x00000140
#define DWC3_GUSB2PHYCFG_PHYSOFTRST     0x80000000
#define DWC3_GUSB3PIPECTL               0xC2C0
#define DWC3_GUSB3PIPECTL_ELASTIC_MODE  0x00000001
#define DWC3_GUSB3PIPECTL_SUSPHY        0x00020000
#define DWC3_GUSB3PIPECTL_DELAYP1TRANS  0x00040000
#define DWC3_GUSB3PIPECTL_PHYSOFTRST    0x80000000
#define DWC3_DSTS                       0xC70C
#define DWC3_DSTS_LINK_STATE(Value)     (((Value) >> 18) & 0xF)

/* Qualcomm wrapper registers behind the DWC3 core */
#define QSCR(Offset)                    (0xF8800 + (Offset))
#define QSCR_CTRL                       QSCR(0x04)
#define QSCR_CTRL_HS_SUSPEND            0x00000003
#define QSCR_GENERAL_CFG                QSCR(0x08)
#define QSCR_GENERAL_CFG_UTMI_AS_PIPE   0x00000001
#define QSCR_GENERAL_CFG_PHYSTATUS_SW   0x00000008
#define QSCR_GENERAL_CFG_PIPE_CLK_GATE  0x00000100
#define QSCR_HS_PHY_CTRL                QSCR(0x10)
#define QSCR_HS_PHY_CTRL_SLEEP_CLAMP    0x00000001
#define QSCR_HS_PHY_CTRL_RETENTION      0x00400000
#define QSCR_HS_PHY_CTRL_SUSPEND        0x00800000
#define QSCR_HS_PHY_PARAMS              QSCR(0x14)
#define QSCR_HS_PHY_PARAMS_DEFAULT      0x00D191E4
#define QSCR_HS_PHY_PARAMS_BUS_MASK     0x03C00000
#define QSCR_CHARGING_DET_CTRL          QSCR(0x18)
#define QSCR_ALT_INTERRUPT_EN           QSCR(0x20)
#define QSCR_HS_PHY_IRQ_STAT            QSCR(0x24)
#define QSCR_SS_PHY_CTRL                QSCR(0x30)
#define QSCR_SS_PHY_CTRL_REF_ENABLE     0x11000100
#define QSCR_SS_PHY_PARAMS              QSCR(0x34)
#define QSCR_PWR_EVNT_IRQ_STAT          QSCR(0x58)
#define QSCR_PWR_EVNT_LPM_IN_L2         0x00000010
#define QSCR_PWR_EVNT_P3_ENTERED        0x00000004
#define QSCR_PWR_EVNT_IRQ_MASK          QSCR(0x5C)
#define QSCR_HS_PHY_COMMON              QSCR(0xEC)

/* Clock controller resets */
#define GCC_USB30_BCR                   0x03C0
#define GCC_USB2_PHY_BCR                0x04A8
#define GCC_USB3_PHY_BCR                0x1400
#define GCC_USB3_PHY_PHY_BCR            0x1404

/* SuperSpeed PHY registers used outside the init table */
#define SSPHY_SW_RESET                  0x0600
#define SSPHY_START_CONTROL             0x0608
#define SSPHY_PCS_STATUS                0x0728
#define SSPHY_PCS_STATUS_PHYSTATUS      0x00000040
#define SSPHY_AUTONOMOUS_MODE           0x06BC
#define SSPHY_LFPS_RXTERM_IRQ_CLEAR     0x06C0
#define SSPHY_LOW_POWER_A               0x2078
#define SSPHY_LOW_POWER_B               0x2090
#define SSPHY_LOW_POWER_C               0x20C4
#define SSPHY_LOW_POWER_D               0x20E8
#define SSPHY_LOW_POWER_E               0x20EC

/* SDM845 clock controller, only the primary USB 3 controller is driven */
#define XHCIF_SDM845_GCC_BASE           0x00100000UL
#define XHCIF_SDM845_GCC_SIZE           0x83000
#define SDM845_GCC_USB30_PRIM_BCR       0xF000
#define SDM845_GCC_USB30_PRIM_GDSCR     0xF004
#define SDM845_GCC_USB30_MASTER_CBCR    0xF00C
#define SDM845_GCC_USB30_SLEEP_CBCR     0xF010
#define SDM845_GCC_USB30_MOCK_UTMI_CBCR 0xF014
#define SDM845_GCC_CFG_NOC_USB3_CBCR    0x502C
#define SDM845_GCC_AHB2PHY_CBCR         0x6A004
#define SDM845_GCC_AGGRE_USB3_CBCR      0x8201C
#define SDM845_GCC_QUSB2PHY_PRIM_BCR    0x12000

#define GCC_GDSC_SW_COLLAPSE            0x00000001
#define GCC_GDSC_PWR_ON                 0x80000000
#define GCC_CBCR_ENABLE                 0x00000001
#define GCC_CBCR_CLK_OFF                0x80000000

/* SDM845 high speed PHY of the primary port, the board has no SuperSpeed lanes wired */
#define XHCIF_SDM845_QUSB2_BASE         0x088E2000UL
#define XHCIF_SDM845_QUSB2_SIZE         0x400
#define QUSB2_PLL_STATUS                0x01A0
#define QUSB2_PLL_STATUS_CORE_READY     0x00000001
#define QUSB2_POWERDOWN                 0x0210
#define QUSB2_POWERDOWN_POWER_DOWN      0x00000001
#define QUSB2_POWERDOWN_CLAMP_N_EN      0x00000002
#define QUSB2_POWERDOWN_VREF_TRIM       0x00000020

#define XHCIF_POLL_TRIES                3

/* Method that hands out board tuning for the high speed PHY */
#define XHCIF_PHY_METHOD                "PHYC"
#define XHCIF_PHY_ENTRY_WRITE           0
#define XHCIF_PHY_ENTRY_FIELDS          3
#define XHCIF_PHY_EVAL_STEP             1024
#define XHCIF_PHY_EVAL_ATTEMPTS         4

typedef struct _XHCIF_DEVICE
{
    WDFDEVICE WdfDevice;
    PUCHAR Controller;
    SIZE_T ControllerSize;
    PUCHAR Gcc;
    PUCHAR TcsrUsb;
    PUCHAR SsPhy;
    PUCHAR Qusb2;
    XHCIF_SOC Soc;
    BOOLEAN Msm8992;

    /* Set once the PHYs were left in their suspended setup, cleared when D0 undoes it */
    BOOLEAN PhySuspended;
} XHCIF_DEVICE, *PXHCIF_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCIF_DEVICE, XhcifGetDevice)

/* phy.c */
VOID
NTAPI
XhcifInitializeHardware(
    _In_ PXHCIF_DEVICE Device);

VOID
NTAPI
XhcifSuspendPhys(
    _In_ PXHCIF_DEVICE Device);

VOID
NTAPI
XhcifResumePhys(
    _In_ PXHCIF_DEVICE Device);
