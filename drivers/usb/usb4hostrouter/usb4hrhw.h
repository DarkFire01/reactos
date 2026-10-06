/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 hardware definitions (host interface, control packets, configuration spaces)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * Layouts follow the USB4 specification version 2.0. The host interface
 * register map is chapter 12, control packets section 6.4, configuration
 * spaces chapter 8, protocol adapters chapter 10.
 */

/* Host interface registers (USB4 12.6) ***************************************/

/* Smallest BAR 0 that holds every register this driver touches */
#define USB4HR_MMIO_MIN_LENGTH              0x0003989C

/* Rings: descriptors are 16 bytes per ring, tables 32 bytes per ring */
#define USB4HR_TX_RING_BASE                 0x00000
#define USB4HR_RX_RING_BASE                 0x08000
#define USB4HR_RING_STRIDE                  16
#define USB4HR_RX_RING_CONTROL              0x18C00
#define USB4HR_RX_RING_STATUS               0x19400
#define USB4HR_TX_TABLE_BASE                0x19800
#define USB4HR_RX_TABLE_BASE                0x29800
#define USB4HR_TABLE_STRIDE                 32

/* Ring descriptor dwords, relative to the ring's descriptor */
#define USB4HR_RING_ADDRESS_LOW             0x00
#define USB4HR_RING_ADDRESS_HIGH            0x04
#define USB4HR_RING_INDEXES                 0x08    /**< consumer 15:0, producer 31:16 */
#define USB4HR_RING_SIZE                    0x0C    /**< entries 15:0, RX data buffer bytes 27:16 */

#define USB4HR_RING_CONSUMER_MASK           0x0000FFFF
#define USB4HR_RING_PRODUCER_SHIFT          16
#define USB4HR_RX_BUFFER_SIZE_SHIFT         16
#define USB4HR_RX_BUFFER_SIZE_MASK          0x0FFF0000

/* Ring table dword 0 */
#define USB4HR_TABLE_E2E_HOPID_SHIFT        12      /**< RX only, bits 22:12 */
#define USB4HR_TABLE_E2E_HOPID_MASK         0x007FF000
#define USB4HR_TABLE_E2E_FLOW_CONTROL       0x10000000
#define USB4HR_TABLE_NO_SNOOP               0x20000000
#define USB4HR_TABLE_RAW_MODE               0x40000000
#define USB4HR_TABLE_VALID                  0x80000000

/* Ring table dword 1 (RX): SOF PDF mask 15:0, EOF PDF mask 31:16. TX dword 1 is the start timestamp. */
#define USB4HR_TABLE_PDF_MASKS              0x04
#define USB4HR_TABLE_EOF_MASK_SHIFT         16

/* Interrupt status: TX ring n is cause n, RX ring n is cause paths + n */
#define USB4HR_INTERRUPT_STATUS             0x37800
#define USB4HR_INTERRUPT_STATUS_CLEAR       0x37808
#define USB4HR_INTERRUPT_STATUS_CLEAR_ALT   0x3780C     /**< shim flag InterruptStatusClearAt3780C */
#define USB4HR_INTERRUPT_MASK               0x38200
#define USB4HR_INTERRUPT_MASK_CLEAR         0x38208     /**< write 1 to disable a cause */
#define USB4HR_INTERRUPT_MASK_SET           0x38210     /**< write 1 to enable a cause */

/* Above this path count the status and mask registers are 64 bits wide */
#define USB4HR_NARROW_INTERRUPT_PATHS       10

/* 16 throttling registers, one per message; Windows writes 125 */
#define USB4HR_INTERRUPT_THROTTLING         0x38C00
#define USB4HR_INTERRUPT_THROTTLING_COUNT   16
#define USB4HR_INTERRUPT_THROTTLING_DEFAULT 125

/* 8 vector allocation dwords, 4 bits of message number per cause */
#define USB4HR_INTERRUPT_VECTOR_ALLOCATION  0x38C40
#define USB4HR_INTERRUPT_VECTOR_REGISTERS   8
#define USB4HR_INTERRUPT_VECTOR_BITS        4
#define USB4HR_INTERRUPT_VECTOR_MASK        0x0000000F

/* Host interface capabilities: total paths 10:0, version 23:16 */
#define USB4HR_HOST_CAPABILITIES            0x39640
#define USB4HR_HOST_CAPS_PATHS_MASK         0x000007FF
#define USB4HR_HOST_CAPS_VERSION_SHIFT      16
#define USB4HR_HOST_CAPS_VERSION_MASK       0x00FF0000
#define USB4HR_HOST_VERSION_2               0x40    /**< host interface for USB4 version 2 */

#define USB4HR_HOST_INTERFACE_RESET         0x39858
#define USB4HR_HOST_INTERFACE_CONTROL       0x39864
#define USB4HR_HOST_CONTROL_NO_AUTO_CLEAR   0x00020000  /**< interrupt status is not cleared on read */
#define USB4HR_HOST_ROUTER_RESET            0x39898
#define USB4HR_HOST_ROUTER_RESET_BIT        0x00000001

/* Ring zero ******************************************************************/

#define USB4HR_MAX_PATHS                    21
#define USB4HR_RING_ZERO_ENTRIES            64
#define USB4HR_FRAME_SIZE                   256

/** Transmit and receive buffer descriptor, 16 bytes (USB4 12.4). */
typedef struct _USB4HR_BUFFER_DESCRIPTOR
{
    ULONG AddressLow;
    ULONG AddressHigh;
    ULONG Control;
    ULONG Timestamp;
} USB4HR_BUFFER_DESCRIPTOR, *PUSB4HR_BUFFER_DESCRIPTOR;

C_ASSERT(sizeof(USB4HR_BUFFER_DESCRIPTOR) == 16);

#define USB4HR_DESC_LENGTH_MASK             0x00000FFF
#define USB4HR_DESC_EOF_PDF_SHIFT           12
#define USB4HR_DESC_EOF_PDF_MASK            0x0000F000
#define USB4HR_DESC_SOF_PDF_SHIFT           16
#define USB4HR_DESC_SOF_PDF_MASK            0x000F0000
#define USB4HR_DESC_ISOCH                   0x00100000  /**< TX */
#define USB4HR_DESC_CRC_ERROR               0x00100000  /**< RX */
#define USB4HR_DESC_DONE                    0x00200000
#define USB4HR_DESC_REQUEST_STATUS          0x00400000
#define USB4HR_DESC_INTERRUPT_ENABLE        0x00800000

/* Control packets (USB4 6.4) *************************************************/

/* Protocol defined field values */
#define USB4HR_PDF_READ                     1
#define USB4HR_PDF_WRITE                    2
#define USB4HR_PDF_NOTIFICATION             3
#define USB4HR_PDF_NOTIFICATION_ACK         4
#define USB4HR_PDF_HOT_PLUG                 5
#define USB4HR_PDF_XDOMAIN_REQUEST          6
#define USB4HR_PDF_XDOMAIN_RESPONSE         7
#define USB4HR_PDF_COUNT                    8

/*
 * Every packet starts with the route string, high dword first, and ends with
 * a CRC-32C dword. On the ring the dwords are big endian.
 */
#define USB4HR_ROUTE_HIGH                   0
#define USB4HR_ROUTE_LOW                    1
#define USB4HR_ROUTE_CM                     0x80000000  /**< route high bit 31, cleared when echoing */
#define USB4HR_CRC32C_REFLECTED             0x82F63B78
#define USB4HR_CRC_BYTES                    4

/* Read and write request and response, dword 2 */
#define USB4HR_PACKET_HEADER_DWORDS         3
#define USB4HR_CFG_OFFSET_MASK              0x00001FFF
#define USB4HR_CFG_LENGTH_SHIFT             13
#define USB4HR_CFG_LENGTH_MASK              0x0007E000
#define USB4HR_CFG_ADAPTER_SHIFT            19
#define USB4HR_CFG_ADAPTER_MASK             0x01F80000
#define USB4HR_CFG_SPACE_SHIFT              25
#define USB4HR_CFG_SPACE_MASK               0x06000000
#define USB4HR_CFG_SEQUENCE_SHIFT           27
#define USB4HR_CFG_SEQUENCE_MASK            0x18000000

/* Notification packet, dword 2 */
#define USB4HR_NOTIFY_EVENT_MASK            0x000000FF
#define USB4HR_NOTIFY_ADAPTER_SHIFT         8
#define USB4HR_NOTIFY_ADAPTER_MASK          0x00003F00
#define USB4HR_NOTIFY_PG_SHIFT              30
#define USB4HR_NOTIFY_PG_PLUG               2
#define USB4HR_NOTIFY_PG_UNPLUG             3

/* Error notifications that are acknowledged with a PDF 4 packet: ErrLink, ErrHec, ErrFc, ErrPlug, DP and PCIe events */
#define USB4HR_NOTIFY_ACK_EVENTS            0x0000003900007002ULL

/* Hot plug event packet, dword 2 */
#define USB4HR_HOT_PLUG_ADAPTER_MASK        0x0000003F
#define USB4HR_HOT_PLUG_UNPLUG              0x80000000

/* Router configuration space (USB4 8.2.1) ************************************/

#define USB4HR_ROUTER_CS_0                  0x00    /**< vendor 15:0, product 31:16 */
#define USB4HR_ROUTER_CS_1                  0x01
#define USB4HR_ROUTER_CS1_NEXT_CAP_MASK     0x000000FF
#define USB4HR_ROUTER_CS1_UPSTREAM_SHIFT    8
#define USB4HR_ROUTER_CS1_UPSTREAM_MASK     0x00003F00
#define USB4HR_ROUTER_CS1_MAX_ADAPTER_SHIFT 14
#define USB4HR_ROUTER_CS1_MAX_ADAPTER_MASK  0x000FC000
#define USB4HR_ROUTER_CS1_DEPTH_SHIFT       20
#define USB4HR_ROUTER_CS1_DEPTH_MASK        0x00700000
#define USB4HR_ROUTER_CS1_REVISION_SHIFT    24
#define USB4HR_ROUTER_CS_2                  0x02    /**< topology ID low */
#define USB4HR_ROUTER_CS_3                  0x03    /**< topology ID high 23:0, valid 31 */
#define USB4HR_ROUTER_CS3_TOPOLOGY_VALID    0x80000000
#define USB4HR_ROUTER_CS_4                  0x04    /**< notification timeout 7:0, CM USB4 version 15:8, USB4 version 31:24 */
#define USB4HR_ROUTER_CS4_USB4_VERSION_SHIFT 24
#define USB4HR_ROUTER_CS_5                  0x05
#define USB4HR_ROUTER_CS5_PCIE_TUNNELING    0x01000000
#define USB4HR_ROUTER_CS5_USB3_TUNNELING    0x02000000
#define USB4HR_ROUTER_CS5_CONFIG_VALID      0x80000000
#define USB4HR_ROUTER_CS_6                  0x06
#define USB4HR_ROUTER_CS6_ROUTER_READY      0x01000000
#define USB4HR_ROUTER_CS6_CONFIG_READY      0x02000000

/* Adapter configuration space (USB4 8.2.2) ***********************************/

#define USB4HR_ADAPTER_CS_0                 0x00    /**< vendor 15:0, product 31:16 */
#define USB4HR_ADAPTER_CS_1                 0x01    /**< next capability 7:0, revision 31:24 */
#define USB4HR_ADAPTER_CS_2                 0x02    /**< adapter type 23:0 */
#define USB4HR_ADAPTER_TYPE_MASK            0x00FFFFFF
#define USB4HR_ADAPTER_TYPE_LANE            0x00000001
#define USB4HR_ADAPTER_TYPE_HOST_INTERFACE  0x00000002
#define USB4HR_ADAPTER_TYPE_PCIE_DOWN       0x00100101
#define USB4HR_ADAPTER_TYPE_PCIE_UP         0x00100102
#define USB4HR_ADAPTER_TYPE_DP_IN           0x000E0101
#define USB4HR_ADAPTER_TYPE_DP_OUT          0x000E0102
#define USB4HR_ADAPTER_TYPE_USB3_DOWN       0x00200101
#define USB4HR_ADAPTER_TYPE_USB3_UP         0x00200102
#define USB4HR_ADAPTER_CS_4                 0x04
#define USB4HR_ADAPTER_CS4_TOTAL_BUFFERS_SHIFT 10
#define USB4HR_ADAPTER_CS4_TOTAL_BUFFERS_MASK  0x000FFC00
#define USB4HR_ADAPTER_CS_5                 0x05
#define USB4HR_ADAPTER_CS5_LINK_CREDITS_SHIFT  22
#define USB4HR_ADAPTER_CS5_LINK_CREDITS_MASK   0x1FC00000

/* Lane adapter capability, relative to the capability dword */
#define USB4HR_LANE_CS_1                    0x01
#define USB4HR_LANE_CS1_SPEED_SHIFT         16
#define USB4HR_LANE_CS1_SPEED_MASK          0x000F0000
#define USB4HR_LANE_CS1_WIDTH_SHIFT         20
#define USB4HR_LANE_CS1_WIDTH_MASK          0x03F00000

/* Path configuration space (USB4 8.2.3): two dwords per ingress HopID ********/

#define USB4HR_PATH_DWORDS_PER_HOP          2
#define USB4HR_PATH_CS0_OUTPUT_HOPID_MASK   0x0000007F
#define USB4HR_PATH_CS0_OUTPUT_ADAPTER_SHIFT 11
#define USB4HR_PATH_CS0_OUTPUT_ADAPTER_MASK 0x0001F800
#define USB4HR_PATH_CS0_CREDITS_SHIFT       17
#define USB4HR_PATH_CS0_CREDITS_MASK        0x00FE0000
#define USB4HR_PATH_CS0_PM_PACKETS          0x01000000
#define USB4HR_PATH_CS0_VALID               0x80000000
#define USB4HR_PATH_CS1_WEIGHT_MASK         0x0000000F
#define USB4HR_PATH_CS1_PRIORITY_SHIFT      8
#define USB4HR_PATH_CS1_PRIORITY_MASK       0x00000700
#define USB4HR_PATH_CS1_COUNTER_MASK        0x00FFF000
#define USB4HR_PATH_CS1_INGRESS_FC          0x01000000
#define USB4HR_PATH_CS1_EGRESS_FC           0x02000000
#define USB4HR_PATH_CS1_INGRESS_SHARED      0x04000000
#define USB4HR_PATH_CS1_EGRESS_SHARED       0x08000000
#define USB4HR_PATH_CS1_PENDING_PACKETS     0x10000000

/* Path arbitration for protocol to lane and lane to protocol hops */
#define USB4HR_USB3_PATH_PRIORITY           3
#define USB4HR_USB3_PATH_WEIGHT_OUT         2   /**< protocol adapter to lane adapter */
#define USB4HR_USB3_PATH_WEIGHT_IN          1   /**< lane adapter to protocol adapter */
#define USB4HR_PCIE_PATH_PRIORITY           3
#define USB4HR_PCIE_PATH_WEIGHT             1

/* Protocol adapter capabilities (USB4 10.3), relative to the capability dword */

/* PCIe adapter */
#define USB4HR_PCIE_CS_0                    0x00
#define USB4HR_PCIE_CS0_PATH_ENABLE         0x80000000
#define USB4HR_PCIE_CS_1                    0x01
#define USB4HR_PCIE_CS1_EXTENDED_ENCAP      0x00000001

/* USB3 adapter */
#define USB4HR_USB3_CS_0                    0x00
#define USB4HR_USB3_CS0_VALID               0x40000000
#define USB4HR_USB3_CS0_PATH_ENABLE         0x80000000
#define USB4HR_USB3_CS_1                    0x01    /**< consumed bandwidth */
#define USB4HR_USB3_CS1_UP_MASK             0x00000FFF
#define USB4HR_USB3_CS1_DOWN_SHIFT          12
#define USB4HR_USB3_CS1_DOWN_MASK           0x00FFF000
#define USB4HR_USB3_CS1_HCA                 0x80000000  /**< adapter acknowledged the CM request */
#define USB4HR_USB3_CS_2                    0x02    /**< allocated bandwidth */
#define USB4HR_USB3_CS2_UP_MASK             0x00000FFF
#define USB4HR_USB3_CS2_DOWN_SHIFT          12
#define USB4HR_USB3_CS2_DOWN_MASK           0x00FFF000
#define USB4HR_USB3_CS2_CMR                 0x80000000  /**< connection manager request */
#define USB4HR_USB3_CS_3                    0x03
#define USB4HR_USB3_CS3_SCALE_MASK          0x0000003F
#define USB4HR_USB3_CS_4                    0x04
#define USB4HR_USB3_CS4_ACTUAL_RATE_MASK    0x0000007F
#define USB4HR_USB3_CS4_LINK_VALID          0x00000080
#define USB4HR_USB3_CS4_MAX_RATE_SHIFT      12
#define USB4HR_USB3_CS4_MAX_RATE_MASK       0x0007F000

/* Vendor PCI configuration dwords used by the force power flow ***************/

#define USB4HR_PCI_FORCE_POWER              0xFC
#define USB4HR_PCI_FORCE_POWER_BITS         0x22000002  /**< bits 1, 25 and 29 */
#define USB4HR_PCI_FORCE_POWER_STATUS       0xC8
#define USB4HR_PCI_FORCE_POWER_DONE         0x80000000
