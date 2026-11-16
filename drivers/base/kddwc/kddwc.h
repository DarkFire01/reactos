/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Definitions shared between the kddwc translation units
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "../kdcom/kddll.h"
#include <arc/arc.h>
#include <kddll.h>
#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>

/* Core register page and the Qualcomm glue page, relative to the controller base */
#define KDDWC_CORE_PAGE_OFFSET          0xC000
#define KDDWC_GLUE_PAGE_OFFSET          0xF8000

/* Clock controller base shared by every SoC from the MSM8996 on */
#define KDDWC_GCC_BASE                  0x00100000ULL

/** One register write of a PHY setup table. */
typedef struct _KDDWC_REG_WRITE
{
    USHORT Offset;
    UCHAR Value;
} KDDWC_REG_WRITE, *PKDDWC_REG_WRITE;

typedef struct _KDDWC_PLATFORM KDDWC_PLATFORM, *PKDDWC_PLATFORM;

typedef
BOOLEAN
(NTAPI *PKDDWC_POWER_UP)(
    _In_ const KDDWC_PLATFORM* Platform);

/** What a SoC file tells the controller code. */
struct _KDDWC_PLATFORM
{
    PCSTR Name;

    /* OEM table IDs of the ACPI tables firmware on this SoC reports */
    PCSTR AcpiIds[4];

    /* Physical base of the DWC3 controller that faces the USB-C port */
    ULONG64 CoreBase;

    /* Powers the controller and its high speed PHY, or makes sure firmware left them up */
    PKDDWC_POWER_UP PowerUp;

    ULONG Flags;
};

/* The PHY only works as firmware set it up, so the core never resets it */
#define KDDWC_PLATFORM_KEEP_PHY         0x00000001

/* soc/<name>.c */
extern const KDDWC_PLATFORM KdDwcPlatformMsm8994;
extern const KDDWC_PLATFORM KdDwcPlatformMsm8998;
extern const KDDWC_PLATFORM KdDwcPlatformSdm845;
extern const KDDWC_PLATFORM KdDwcPlatformSm8150;
extern const KDDWC_PLATFORM KdDwcPlatformSc8180x;
extern const KDDWC_PLATFORM KdDwcPlatformSc7180;
extern const KDDWC_PLATFORM KdDwcPlatformSc7280;
extern const KDDWC_PLATFORM KdDwcPlatformSm8350;
extern const KDDWC_PLATFORM KdDwcPlatformSc8280xp;
extern const KDDWC_PLATFORM KdDwcPlatformX1e80100;

/* DWC3 global registers, offsets inside the core register page */
#define DWC_GCTL                        0x110
#define DWC_GCTL_CORESOFTRESET          0x00000800
#define DWC_GCTL_PRTCAPDIR_MASK         0x00003000
#define DWC_GCTL_PRTCAPDIR_DEVICE       0x00002000
#define DWC_GUCTL1                      0x11C
#define DWC_GUCTL1_PARKMODE_DISABLE_SS  0x00020000
#define DWC_GSNPSID                     0x120
#define DWC_GUSB2PHYCFG                 0x200
#define DWC_GUSB2PHYCFG_SUSPEND         0x00000140
#define DWC_GUSB2PHYCFG_PHYSOFTRST      0x80000000
#define DWC_GEVNTADRLO                  0x400
#define DWC_GEVNTADRHI                  0x404
#define DWC_GEVNTSIZ                    0x408
#define DWC_GEVNTSIZ_INTMASK            0x80000000
#define DWC_GEVNTCOUNT                  0x40C
#define DWC_GEVNTCOUNT_MASK             0x0000FFFC

/* DWC3 device registers */
#define DWC_DCFG                        0x700
#define DWC_DCFG_SPEED_MASK             0x00000007
#define DWC_DCFG_SPEED_HIGH             0x00000000
#define DWC_DCFG_ADDRESS_MASK           0x000003F8
#define DWC_DCFG_ADDRESS(Address)       ((ULONG)(Address) << 3)
#define DWC_DCTL                        0x704
#define DWC_DCTL_RUN_STOP               0x80000000
#define DWC_DCTL_CSFTRST                0x40000000
#define DWC_DEVTEN                      0x708
#define DWC_DEVTEN_DISCONNECT           0x00000001
#define DWC_DEVTEN_RESET                0x00000002
#define DWC_DEVTEN_CONNECT_DONE         0x00000004
#define DWC_DSTS                        0x70C
#define DWC_DSTS_SPEED_MASK             0x00000007
#define DWC_DSTS_SPEED_FULL             0x00000001
#define DWC_DSTS_DEVCTRLHLT             0x00400000
#define DWC_DALEPENA                    0x720

/* Endpoint command registers, one block per physical endpoint */
#define DWC_DEPCMDPAR2(Ep)              (0x800 + (Ep) * 0x10)
#define DWC_DEPCMDPAR1(Ep)              (0x804 + (Ep) * 0x10)
#define DWC_DEPCMDPAR0(Ep)              (0x808 + (Ep) * 0x10)
#define DWC_DEPCMD(Ep)                  (0x80C + (Ep) * 0x10)
#define DWC_DEPCMD_SETEPCONFIG          0x00000001
#define DWC_DEPCMD_SETTRANSFRESOURCE    0x00000002
#define DWC_DEPCMD_SETSTALL             0x00000004
#define DWC_DEPCMD_CLEARSTALL           0x00000005
#define DWC_DEPCMD_STARTTRANSFER        0x00000006
#define DWC_DEPCMD_ENDTRANSFER          0x00000008
#define DWC_DEPCMD_DEPSTARTCFG          0x00000009
#define DWC_DEPCMD_CMDACT               0x00000400
#define DWC_DEPCMD_FORCERM              0x00000800
#define DWC_DEPCMD_STATUS(Value)        (((Value) >> 12) & 0xF)
#define DWC_DEPCMD_PARAM(Value)         ((ULONG)(Value) << 16)
#define DWC_DEPCMD_RESOURCE(Value)      (((Value) >> 16) & 0x7F)

/* Endpoint configuration parameters */
#define DWC_EPCFG0_TYPE(Type)           ((ULONG)(Type) << 1)
#define DWC_EPCFG0_MPS(Size)            ((ULONG)(Size) << 3)
#define DWC_EPCFG0_FIFO(Fifo)           ((ULONG)(Fifo) << 17)
#define DWC_EPCFG0_ACTION_MODIFY        0x80000000
#define DWC_EPCFG1_XFER_COMPLETE        0x00000100
#define DWC_EPCFG1_XFER_NOT_READY       0x00000400
#define DWC_EPCFG1_NUMBER(Ep)           ((ULONG)(Ep) << 25)

/* Qualcomm glue registers, offsets inside the glue page */
#define QSCR_GENERAL_CFG                0x808
#define QSCR_GENERAL_CFG_UTMI_AS_PIPE   0x00000001
#define QSCR_GENERAL_CFG_PHYSTATUS_SW   0x00000008
#define QSCR_GENERAL_CFG_PIPE_CLK_GATE  0x00000100
#define QSCR_HS_PHY_CTRL                0x810
#define QSCR_HS_PHY_CTRL_VBUS_VALID     0x00100000
#define QSCR_HS_PHY_CTRL_SESSVLD_SEL    0x10000000
#define QSCR_SS_PHY_CTRL                0x830
#define QSCR_SS_PHY_CTRL_LANE0_PWR      0x01000000

/* Transfer request block */
typedef struct _DWC_TRB
{
    ULONG BufferLow;
    ULONG BufferHigh;
    ULONG Size;
    ULONG Control;
} DWC_TRB, *PDWC_TRB;

#define DWC_TRB_SIZE_MASK               0x00FFFFFF
#define DWC_TRB_CTRL_HWO                0x00000001
#define DWC_TRB_CTRL_LST                0x00000002
#define DWC_TRB_CTRL_ISP_IMI            0x00000400
#define DWC_TRB_CTRL_IOC                0x00000800
#define DWC_TRB_TYPE_NORMAL             0x00000010
#define DWC_TRB_TYPE_CONTROL_SETUP      0x00000020
#define DWC_TRB_TYPE_CONTROL_STATUS2    0x00000030
#define DWC_TRB_TYPE_CONTROL_STATUS3    0x00000040
#define DWC_TRB_TYPE_CONTROL_DATA       0x00000050

/* Event buffer entries */
#define DWC_EVENT_IS_DEVICE             0x00000001
#define DWC_EVENT_DEVICE_TYPE(Event)    (((Event) >> 8) & 0xF)
#define DWC_DEVICE_EVENT_DISCONNECT     0
#define DWC_DEVICE_EVENT_RESET          1
#define DWC_DEVICE_EVENT_CONNECT_DONE   2
#define DWC_EVENT_ENDPOINT(Event)       (((Event) >> 1) & 0x1F)
#define DWC_EVENT_ENDPOINT_TYPE(Event)  (((Event) >> 6) & 0xF)
#define DWC_EVENT_ENDPOINT_STATUS(Event) (((Event) >> 12) & 0xF)
#define DWC_ENDPOINT_EVENT_COMPLETE     1
#define DWC_ENDPOINT_EVENT_NOT_READY    3
#define DWC_NOT_READY_STATUS_PHASE      2

/* Physical endpoints: control out and in, then the serial data and notification pipes */
#define KDDWC_EP0_OUT                   0
#define KDDWC_EP0_IN                    1
#define KDDWC_EP_DATA_OUT               2
#define KDDWC_EP_DATA_IN                3
#define KDDWC_EP_NOTIFY_IN              5

/* USB endpoint addresses that go with them */
#define KDDWC_USB_DATA_OUT              0x01
#define KDDWC_USB_DATA_IN               0x81
#define KDDWC_USB_NOTIFY_IN             0x82

/* Endpoint packet sizes */
#define KDDWC_EP0_PACKET                64
#define KDDWC_BULK_PACKET_HIGH          512
#define KDDWC_BULK_PACKET_FULL          64
#define KDDWC_NOTIFY_PACKET             16

/* Sizes of the pieces of the DMA block */
#define KDDWC_EVENT_BYTES               4096
#define KDDWC_EP0_BYTES                 512
#define KDDWC_RX_BYTES                  512
#define KDDWC_TX_BYTES                  4096

/* Ring of output waiting for the host, kept while no terminal reads it */
#define KDDWC_RING_BYTES                (128 * 1024)

/* Ring of input from the host */
#define KDDWC_INPUT_BYTES               1024

/* The block the controller reads and writes: event buffer, TRBs and data buffers */
typedef struct _KDDWC_DMA
{
    ULONG Events[KDDWC_EVENT_BYTES / sizeof(ULONG)];
    DWC_TRB Ep0Trb;
    DWC_TRB RxTrb;
    DWC_TRB TxTrb;
    DWC_TRB Reserved;
    UCHAR Setup[64];
    UCHAR Ep0Buffer[KDDWC_EP0_BYTES];
    UCHAR RxBuffer[KDDWC_RX_BYTES];
    UCHAR TxBuffer[KDDWC_TX_BYTES];
} KDDWC_DMA, *PKDDWC_DMA;

typedef
ULONG
(*PKDDWC_BOOT_PRINT)(
    _In_ PCSTR Format,
    ...);

/* Set only while phase 0 runs with /FRLDRSCREENLOG; prints on the loader's screen */
extern PKDDWC_BOOT_PRINT KdDwcBootPrint;

#define KdDwcTrace(...)                             \
    do                                              \
    {                                               \
        if (KdDwcBootPrint != NULL)                 \
            KdDwcBootPrint("kddwc: " __VA_ARGS__);  \
    } while (0)

/* dwc3.c */
NTSTATUS
NTAPI
KdDwcStartController(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ const KDDWC_PLATFORM* Platform,
    _In_ ULONG64 CoreBase);

ULONG
NTAPI
KdDwcRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset);

VOID
NTAPI
KdDwcWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value);

VOID
NTAPI
KdDwcModify(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set);

BOOLEAN
NTAPI
KdDwcWaitFor(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _In_ ULONG TimeoutUs);

PUCHAR
NTAPI
KdDwcMapRegister(
    _In_ ULONG64 PhysicalAddress);

/* platform.c */
const KDDWC_PLATFORM*
NTAPI
KdDwcFindPlatform(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_opt_ PCSTR Name);

BOOLEAN
NTAPI
KdDwcPowerDomainOn(
    _In_ ULONG64 Gdscr);

BOOLEAN
NTAPI
KdDwcClockOn(
    _In_ ULONG64 Cbcr);

BOOLEAN
NTAPI
KdDwcVotedClockIsOn(
    _In_ ULONG64 Cbcr);

VOID
NTAPI
KdDwcPulseReset(
    _In_ ULONG64 Bcr);

BOOLEAN
NTAPI
KdDwcQusb2IsRunning(
    _In_ ULONG64 Phy);

BOOLEAN
NTAPI
KdDwcStartQusb2(
    _In_ ULONG64 Phy,
    _In_ ULONG64 PhyBcr,
    _In_ UCHAR PowerDownBits,
    _In_reads_(Count) const KDDWC_REG_WRITE* Table,
    _In_ ULONG Count);

BOOLEAN
NTAPI
KdDwcSnpsHsIsRunning(
    _In_ ULONG64 Phy);

BOOLEAN
NTAPI
KdDwcStartSnpsHs(
    _In_ ULONG64 Phy,
    _In_ ULONG64 PhyBcr);

VOID
NTAPI
KdDwcService(VOID);

VOID
NTAPI
KdDwcTraceState(VOID);

BOOLEAN
NTAPI
KdDwcIsConfigured(VOID);

BOOLEAN
NTAPI
KdDwcIsHostListening(VOID);

BOOLEAN
NTAPI
KdDwcIsSending(VOID);

ULONG
NTAPI
KdDwcSend(
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length);

BOOLEAN
NTAPI
KdDwcTakeInput(
    _Out_ PUCHAR Byte);

/* kddwc.c */
VOID
NTAPI
KdDwcStall(
    _In_ ULONG Microseconds);

ULONG64
NTAPI
KdDwcMicroseconds(VOID);
