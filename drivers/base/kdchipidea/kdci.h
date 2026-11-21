/*
 * PROJECT:     ReactOS ChipIdea Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Definitions shared between the kdchipidea translation units
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "../kdcom/kddll.h"
#include <arc/arc.h>
#include <kddll.h>
#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>

typedef struct _KDCI_PLATFORM KDCI_PLATFORM, *PKDCI_PLATFORM;

typedef
BOOLEAN
(NTAPI *PKDCI_PLATFORM_ROUTINE)(
    _In_ const KDCI_PLATFORM* Platform);

/** What a SoC file tells the controller code. */
struct _KDCI_PLATFORM
{
    PCSTR Name;

    /* OEM table IDs of the ACPI tables firmware on this SoC reports */
    PCSTR AcpiIds[4];

    /* Physical base of the ChipIdea controller, its capability registers start 0x100 in */
    ULONG64 CoreBase;

    /* The PHY is on ULPI rather than UTMI */
    BOOLEAN UlpiPhy;

    /* Vendor bits set in USBCMD together with run, such as a forced session valid */
    ULONG CommandFlags;

    /* Turns on the controller's clocks and resets the block */
    PKDCI_PLATFORM_ROUTINE PowerUp;

    /* Programs the PHY after the controller reset, through the ULPI viewport */
    PKDCI_PLATFORM_ROUTINE StartPhy;

    /* TRUE while the controller's clocks run, read from the clock controller only */
    PKDCI_PLATFORM_ROUTINE IsPowered;
};

/* soc/<name>.c */
extern const KDCI_PLATFORM KdCiPlatformMsm8916;

/* Capability registers, from the controller base */
#define CI_CAP_OFFSET                   0x100
#define CI_CAPLENGTH                    0x00
#define CI_HCCPARAMS                    0x08
#define CI_HCCPARAMS_LPM                0x00020000
#define CI_DCCPARAMS                    0x24
#define CI_DCCPARAMS_DEVICE             0x00000080
#define CI_DCCPARAMS_ENDPOINTS(Value)   ((Value) & 0x1F)

/* Operational registers, from the end of the capability registers */
#define CI_USBCMD                       0x00
#define CI_USBCMD_RUN                   0x00000001
#define CI_USBCMD_RESET                 0x00000002
#define CI_USBCMD_SETUP_TRIPWIRE        0x00002000
#define CI_USBCMD_SESSION_VALID         0x02000000
#define CI_USBSTS                       0x04
#define CI_USBSTS_PORT_CHANGE           0x00000004
#define CI_USBSTS_RESET                 0x00000040
#define CI_USBSTS_SUSPEND               0x00000100
#define CI_USBINTR                      0x08
#define CI_DEVICEADDR                   0x14
#define CI_DEVICEADDR_ADVANCE           0x01000000
#define CI_DEVICEADDR_ADDRESS(Address)  ((ULONG)(Address) << 25)
#define CI_ENDPTLISTADDR                0x18
#define CI_ULPI_VIEWPORT                0x30
#define CI_ULPI_WAKEUP                  0x80000000
#define CI_ULPI_RUN                     0x40000000
#define CI_ULPI_WRITE                   0x20000000
#define CI_ULPI_SYNC                    0x08000000
#define CI_ULPI_ADDRESS(Address)        ((ULONG)(Address) << 16)
#define CI_ULPI_READ_DATA(Value)        (((Value) >> 8) & 0xFF)
#define CI_PORTSC                       0x44
#define CI_PORTSC_SPEED(Value)          (((Value) >> 26) & 0x3)
#define CI_PORTSC_PTS_MASK              0xC2000000
#define CI_PORTSC_PTS_ULPI              0x80000000
#define CI_DEVLC                        0x84
#define CI_DEVLC_SPEED(Value)           (((Value) >> 25) & 0x3)
#define CI_DEVLC_PTS_MASK               0xE0000000
#define CI_DEVLC_PTS_ULPI               0x40000000
#define CI_SPEED_FULL                   0
#define CI_SPEED_HIGH                   2

/* Registers that move up when the controller has the LPM extension */
#define CI_USBMODE(Lpm)                 ((Lpm) ? 0xC8 : 0x68)
#define CI_USBMODE_CM_MASK              0x00000003
#define CI_USBMODE_CM_DEVICE            0x00000002
#define CI_USBMODE_SETUP_LOCKOUT_OFF    0x00000008
#define CI_USBMODE_STREAM_DISABLE       0x00000010
#define CI_ENDPTSETUPSTAT(Lpm)          ((Lpm) ? 0xD8 : 0x6C)
#define CI_ENDPTPRIME(Lpm)              ((Lpm) ? 0xDC : 0x70)
#define CI_ENDPTFLUSH(Lpm)              ((Lpm) ? 0xE0 : 0x74)
#define CI_ENDPTCOMPLETE(Lpm)           ((Lpm) ? 0xE8 : 0x7C)
#define CI_ENDPTCTRL(Lpm, Ep)           (((Lpm) ? 0xEC : 0x80) + (Ep) * 4)
#define CI_ENDPTCTRL_RX_STALL           0x00000001
#define CI_ENDPTCTRL_RX_TYPE(Type)      ((ULONG)(Type) << 2)
#define CI_ENDPTCTRL_RX_TOGGLE_RESET    0x00000040
#define CI_ENDPTCTRL_RX_ENABLE          0x00000080
#define CI_ENDPTCTRL_TX_STALL           0x00010000
#define CI_ENDPTCTRL_TX_TYPE(Type)      ((ULONG)(Type) << 18)
#define CI_ENDPTCTRL_TX_TOGGLE_RESET    0x00400000
#define CI_ENDPTCTRL_TX_ENABLE          0x00800000
#define CI_ENDPOINT_BULK                2
#define CI_ENDPOINT_INTERRUPT           3

/* Endpoint bits of the prime, flush and complete registers */
#define CI_OUT_BIT(Ep)                  (1UL << (Ep))
#define CI_IN_BIT(Ep)                   (1UL << (16 + (Ep)))

/* Queue head, one per endpoint and direction, OUT first */
typedef struct _CI_QUEUE_HEAD
{
    ULONG Capabilities;
    ULONG CurrentTd;
    ULONG NextTd;
    ULONG Token;
    ULONG Buffer[5];
    ULONG Reserved;
    UCHAR Setup[8];
    ULONG Pad[4];
} CI_QUEUE_HEAD, *PCI_QUEUE_HEAD;

C_ASSERT(sizeof(CI_QUEUE_HEAD) == 64);

#define CI_QH_INTERRUPT_ON_SETUP        0x00008000
#define CI_QH_MAX_PACKET(Size)          ((ULONG)(Size) << 16)
#define CI_QH_NO_ZERO_LENGTH            0x20000000

/* Transfer descriptor */
typedef struct _CI_TRANSFER
{
    ULONG NextTd;
    ULONG Token;
    ULONG Buffer[5];
    ULONG Reserved;
} CI_TRANSFER, *PCI_TRANSFER;

C_ASSERT(sizeof(CI_TRANSFER) == 32);

#define CI_TD_TERMINATE                 0x00000001
#define CI_TD_ACTIVE                    0x00000080
#define CI_TD_ERRORS                    0x00000068
#define CI_TD_INTERRUPT                 0x00008000
#define CI_TD_BYTES(Count)              ((ULONG)(Count) << 16)
#define CI_TD_REMAINING(Token)          (((Token) >> 16) & 0x7FFF)

/* Endpoint numbers on the bus */
#define KDCI_EP_DATA                    1
#define KDCI_EP_NOTIFY                  2
#define KDCI_USB_DATA_OUT               0x01
#define KDCI_USB_DATA_IN                0x81
#define KDCI_USB_NOTIFY_IN              0x82

/* Queue head slots: OUT then IN of each endpoint */
#define KDCI_QH_EP0_OUT                 0
#define KDCI_QH_EP0_IN                  1
#define KDCI_QH_DATA_OUT                2
#define KDCI_QH_DATA_IN                 3
#define KDCI_QH_NOTIFY_OUT              4
#define KDCI_QH_NOTIFY_IN               5
#define KDCI_QH_COUNT                   6

/* Endpoint packet sizes */
#define KDCI_EP0_PACKET                 64
#define KDCI_BULK_PACKET_HIGH           512
#define KDCI_BULK_PACKET_FULL           64
#define KDCI_NOTIFY_PACKET              16

/* Sizes of the pieces of the DMA block */
#define KDCI_EP0_BYTES                  512
#define KDCI_RX_BYTES                   512
#define KDCI_TX_BYTES                   4096

/* Ring of output waiting for the host, and ring of input from it */
#define KDCI_RING_BYTES                 (128 * 1024)
#define KDCI_INPUT_BYTES                1024

/* The block the controller reads and writes, the queue heads first since they need 2 KB alignment */
typedef struct _KDCI_DMA
{
    CI_QUEUE_HEAD QueueHeads[KDCI_QH_COUNT];
    CI_TRANSFER Ep0Td;
    CI_TRANSFER RxTd;
    CI_TRANSFER TxTd;
    CI_TRANSFER Reserved;
    UCHAR Ep0Buffer[KDCI_EP0_BYTES];
    UCHAR RxBuffer[KDCI_RX_BYTES];
    UCHAR TxBuffer[KDCI_TX_BYTES];
} KDCI_DMA, *PKDCI_DMA;

/* When the controller last saw anything from the host */
extern ULONG64 KdCiLastEvent;

/* kdci.c */
ULONG64
NTAPI
KdCiMicroseconds(VOID);

VOID
NTAPI
KdCiStall(
    _In_ ULONG Microseconds);

/* chipidea.c */
NTSTATUS
NTAPI
KdCiStartController(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ const KDCI_PLATFORM* Platform,
    _In_ ULONG64 CoreBase);

VOID
NTAPI
KdCiService(VOID);

VOID
NTAPI
KdCiPowerDown(VOID);

NTSTATUS
NTAPI
KdCiPowerUp(VOID);

VOID
NTAPI
KdCiDmaRange(
    _Out_ PULONG64 Base,
    _Out_ PULONG Bytes);

BOOLEAN
NTAPI
KdCiIsConfigured(VOID);

BOOLEAN
NTAPI
KdCiIsHostListening(VOID);

BOOLEAN
NTAPI
KdCiIsSending(VOID);

ULONG
NTAPI
KdCiSend(
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length);

BOOLEAN
NTAPI
KdCiTakeInput(
    _Out_ PUCHAR Byte);

BOOLEAN
NTAPI
KdCiUlpiWrite(
    _In_ UCHAR Address,
    _In_ UCHAR Value);

PUCHAR
NTAPI
KdCiMapRegister(
    _In_ ULONG64 PhysicalAddress);

ULONG
NTAPI
KdCiRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset);

VOID
NTAPI
KdCiWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value);

VOID
NTAPI
KdCiModify(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set);

BOOLEAN
NTAPI
KdCiWaitFor(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _In_ ULONG TimeoutUs);

/* platform.c */
const KDCI_PLATFORM*
NTAPI
KdCiFindPlatform(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_opt_ PCSTR Name);
