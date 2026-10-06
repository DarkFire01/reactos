/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 configuration space layouts used by a connection manager
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * Layouts follow the USB4 specification version 2.0: router, adapter and path
 * configuration spaces in chapter 8, protocol adapter capabilities in chapter
 * 10. Offsets are dwords unless a name ends in _BYTES.
 */

/* Router configuration space (USB4 8.2.1) ************************************/

/* Basic router configuration space: ROUTER_CS_0 to ROUTER_CS_26 */
#define USB4DR_ROUTER_BASIC_DWORDS          27
#define USB4DR_ROUTER_HEADER_DWORDS         5   /**< ROUTER_CS_0..4, all a TBT3 router has */

#define USB4DR_ROUTER_CS_0                  0
#define USB4DR_ROUTER_CS0_VENDOR_MASK       0x0000FFFF
#define USB4DR_ROUTER_CS0_PRODUCT_MASK      0xFFFF0000

#define USB4DR_ROUTER_CS_1                  1
#define USB4DR_ROUTER_CS1_NEXT_CAP_MASK     0x000000FF
#define USB4DR_ROUTER_CS1_UPSTREAM_MASK     0x00003F00  /**< upstream lane 0 adapter */
#define USB4DR_ROUTER_CS1_MAX_ADAPTER_MASK  0x000FC000
#define USB4DR_ROUTER_CS1_DEPTH_MASK        0x00700000
#define USB4DR_ROUTER_CS1_REVISION_MASK     0xFF000000

#define USB4DR_ROUTER_CS_2                  2   /**< topology ID low */
#define USB4DR_ROUTER_CS_3                  3
#define USB4DR_ROUTER_CS3_TOPOLOGY_HIGH     0x00FFFFFF
#define USB4DR_ROUTER_CS3_TOPOLOGY_VALID    0x80000000

#define USB4DR_ROUTER_CS_4                  4
#define USB4DR_ROUTER_CS4_NOTIFY_TIMEOUT    0x000000FF
#define USB4DR_ROUTER_CS4_CM_VERSION_MASK   0x0000FF00
#define USB4DR_ROUTER_CS4_VERSION_MASK      0xFF000000  /**< USB4 version byte: major 7:5, minor 4:0 */

#define USB4DR_ROUTER_CS_5                  5
#define USB4DR_ROUTER_CS5_SLEEP             0x00000001
#define USB4DR_ROUTER_CS5_WAKE_PCIE         0x00000002
#define USB4DR_ROUTER_CS5_WAKE_USB3         0x00000004
#define USB4DR_ROUTER_CS5_WAKE_DP           0x00000008
#define USB4DR_ROUTER_CS5_RESERVED_23       0x00800000  /**< Windows always writes it as zero */
#define USB4DR_ROUTER_CS5_PCIE_TUNNELING    0x01000000
#define USB4DR_ROUTER_CS5_USB3_TUNNELING    0x02000000
#define USB4DR_ROUTER_CS5_HOST_CONTROLLER   0x04000000  /**< internal xHCI used instead of USB3 tunneling */
#define USB4DR_ROUTER_CS5_CONFIG_VALID      0x80000000

#define USB4DR_ROUTER_CS_6                  6
#define USB4DR_ROUTER_CS6_SLEEP_READY       0x00000001
#define USB4DR_ROUTER_CS6_HOST_CONTROLLER   0x00040000  /**< internal host controller implemented */
#define USB4DR_ROUTER_CS6_ROUTER_READY      0x01000000
#define USB4DR_ROUTER_CS6_CONFIG_READY      0x02000000

/* Router operations (USB4 8.3.1) */
#define USB4DR_ROUTER_CS_DATA               9   /**< ROUTER_CS_9..24, operation data */
#define USB4DR_ROUTER_DATA_DWORDS           16
#define USB4DR_ROUTER_CS_METADATA           25
#define USB4DR_ROUTER_CS_OPCODE             26
#define USB4DR_ROUTER_CS26_OPCODE_MASK      0x0000FFFF
#define USB4DR_ROUTER_CS26_STATUS_MASK      0x3F000000
#define USB4DR_ROUTER_CS26_NOT_SUPPORTED    0x40000000
#define USB4DR_ROUTER_CS26_VALID            0x80000000

#define USB4DR_ROP_QUERY_DP_RESOURCE        0x10
#define USB4DR_ROP_ALLOCATE_DP_RESOURCE     0x11
#define USB4DR_ROP_DEALLOCATE_DP_RESOURCE   0x12
#define USB4DR_ROP_CONNECT_DP_OUT           0x13
#define USB4DR_ROP_NVM_WRITE                0x20
#define USB4DR_ROP_NVM_AUTH_WRITE           0x21
#define USB4DR_ROP_NVM_READ                 0x22
#define USB4DR_ROP_NVM_SET_OFFSET           0x23
#define USB4DR_ROP_DROM_READ                0x24
#define USB4DR_ROP_NVM_SECTOR_SIZE          0x25
#define USB4DR_ROP_PCIE_DOWNSTREAM_MAPPING  0x30
#define USB4DR_ROP_GET_CAPABILITIES         0x31
#define USB4DR_ROP_SET_CAPABILITIES         0x32
#define USB4DR_ROP_BUFFER_ALLOCATION        0x33
#define USB4DR_ROP_BLOCK_SIDEBAND           0x34
#define USB4DR_ROP_UNBLOCK_SIDEBAND         0x35
#define USB4DR_ROP_GET_CONTAINER_ID         0x36
#define USB4DR_ROP_GET_CONNECTORS           0x37

/* Windows polls Operation Valid every 1 ms, 120 times */
#define USB4DR_ROP_POLL_MS                  1
#define USB4DR_ROP_POLL_COUNT               120

/* DROM read metadata: dword address 14:2, dword count 19:15 (1 to 16) */
#define USB4DR_DROM_META_ADDRESS_SHIFT      2
#define USB4DR_DROM_META_ADDRESS_MASK       0x00007FFC
#define USB4DR_DROM_META_COUNT_SHIFT        15
#define USB4DR_DROM_META_COUNT_MASK         0x000F8000
#define USB4DR_DROM_HEADER_METADATA         0x00020000  /**< address 0, 4 dwords */

/* Buffer allocation request results: index 15:0, value 31:16 (at most 255) */
#define USB4DR_BUFFER_PARAM_INDEX_MASK      0x0000FFFF
#define USB4DR_BUFFER_PARAM_VALUE_MASK      0xFFFF0000
#define USB4DR_BUFFER_MAX_USB3              1
#define USB4DR_BUFFER_MIN_DP_AUX            2
#define USB4DR_BUFFER_MIN_DP_MAIN           3
#define USB4DR_BUFFER_MAX_PCIE              4
#define USB4DR_BUFFER_MAX_HI                5
#define USB4DR_BUFFER_MAX_USB3_GEN_T        6

/* Capability headers: next pointer 7:0, ID 15:8; a VSC adds VSC ID 23:16 and length 31:24 */
#define USB4DR_CAP_NEXT_MASK                0x000000FF
#define USB4DR_CAP_ID_MASK                  0x0000FF00
#define USB4DR_CAP_VSC_ID_MASK              0x00FF0000
#define USB4DR_CAP_VSC_LENGTH_MASK          0xFF000000

/* Windows stops a capability walk after this many headers */
#define USB4DR_MAX_CAPABILITIES             128

/* Router capability IDs */
#define USB4DR_ROUTER_CAP_TMU               0x03
#define USB4DR_ROUTER_CAP_VENDOR            0x05    /**< VSC, or VSEC when the length byte is 0 */
#define USB4DR_TBT3_VSC_1                   1
#define USB4DR_TBT3_VSEC_6                  6

/* Adapter configuration space (USB4 8.2.2) ***********************************/

#define USB4DR_ADAPTER_BASIC_DWORDS         9   /**< ADP_CS_0..8 */

#define USB4DR_ADAPTER_CS_0                 0
#define USB4DR_ADAPTER_CS_1                 1
#define USB4DR_ADAPTER_CS1_NEXT_CAP_MASK    0x000000FF

#define USB4DR_ADAPTER_CS_2                 2
#define USB4DR_ADAPTER_CS2_SUBTYPE_MASK     0x000000FF
#define USB4DR_ADAPTER_CS2_VERSION_MASK     0x0000FF00
#define USB4DR_ADAPTER_CS2_PROTOCOL_MASK    0x00FF0000

#define USB4DR_ADAPTER_CS_3                 3
#define USB4DR_ADAPTER_CS3_NUMBER_MASK      0x03F00000

#define USB4DR_ADAPTER_CS_4                 4
#define USB4DR_ADAPTER_CS4_TOTAL_BUFFERS    0x3FF00000  /**< lane adapters */
#define USB4DR_ADAPTER_CS4_PLUGGED          0x40000000
#define USB4DR_ADAPTER_CS4_LOCK             0x80000000

#define USB4DR_ADAPTER_CS_5                 5
#define USB4DR_ADAPTER_CS5_MAX_IN_HOPID     0x0000007F
#define USB4DR_ADAPTER_CS5_MAX_OUT_HOPID    0x0003F800
#define USB4DR_ADAPTER_CS5_LINK_CREDITS     0x1FC00000

/* Adapter protocols (ADP_CS_2 23:16) and sub types (7:0) */
#define USB4DR_PROTOCOL_LANE                0x00
#define USB4DR_PROTOCOL_DP                  0x0E
#define USB4DR_PROTOCOL_PCIE                0x10
#define USB4DR_PROTOCOL_USB3                0x20
#define USB4DR_PROTOCOL_USB3_GEN_T          0x21
#define USB4DR_PROTOCOL_VENDOR              0xFF

#define USB4DR_SUBTYPE_LANE                 0x01    /**< protocol lane */
#define USB4DR_SUBTYPE_HOST_INTERFACE       0x02    /**< protocol lane */
#define USB4DR_SUBTYPE_DOWN                 0x01    /**< protocol adapters; DP IN */
#define USB4DR_SUBTYPE_UP                   0x02    /**< protocol adapters; DP OUT */

/* Adapter capability IDs */
#define USB4DR_ADAPTER_CAP_LANE             0x01
#define USB4DR_ADAPTER_CAP_TMU              0x03
#define USB4DR_ADAPTER_CAP_PROTOCOL         0x04    /**< USB3, PCIe and DP adapter capability */
#define USB4DR_ADAPTER_CAP_VENDOR           0x05
#define USB4DR_ADAPTER_CAP_USB4_PORT        0x06

/* Lane adapter capability, relative to its header dword */
#define USB4DR_LANE_CS_0                    0
#define USB4DR_LANE_CS0_SUPPORTED_SPEEDS    0x000F0000
#define USB4DR_LANE_CS0_SUPPORTED_WIDTHS    0x03F00000
#define USB4DR_LANE_CS0_CL0S_SUPPORT        0x04000000
#define USB4DR_LANE_CS0_CL1_SUPPORT         0x08000000
#define USB4DR_LANE_CS0_CL2_SUPPORT         0x10000000

#define USB4DR_LANE_CS_1                    1
#define USB4DR_LANE_CS1_TARGET_SPEED        0x0000000F
#define USB4DR_LANE_CS1_TARGET_WIDTH        0x000003F0
#define USB4DR_LANE_CS1_LANE_DISABLE        0x00004000
#define USB4DR_LANE_CS1_LANE_BONDING        0x00008000
#define USB4DR_LANE_CS1_CURRENT_SPEED       0x000F0000
#define USB4DR_LANE_CS1_NEGOTIATED_WIDTH    0x03F00000
#define USB4DR_LANE_CS1_ADAPTER_STATE       0x3C000000
#define USB4DR_LANE_CS1_PM_SECONDARY        0x40000000

/* Lane adapter states (LANE_ADP_CS_1 29:26) */
#define USB4DR_LANE_STATE_DISABLED          0
#define USB4DR_LANE_STATE_TRAINING          1
#define USB4DR_LANE_STATE_CL0               2

/* USB4 port capability, relative to its header dword */
#define USB4DR_PORT_CS_18                   18
#define USB4DR_PORT_CS18_BONDING_ENABLED    0x00000100
#define USB4DR_PORT_CS18_TBT3_COMPAT        0x00000200
#define USB4DR_PORT_CS18_CLX_SUPPORT        0x00000400
#define USB4DR_PORT_CS18_ROUTER_DETECTED    0x00002000
#define USB4DR_PORT_CS18_WAKE_DISCONNECT    0x00020000
#define USB4DR_PORT_CS_19                   19

/* Windows polls Router Detected every 100 ms after a plug */
#define USB4DR_ROUTER_DETECT_POLL_MS        100

/* Path configuration space (USB4 8.2.3): two dwords per ingress HopID ********/

#define USB4DR_PATH_DWORDS_PER_HOP          2
#define USB4DR_PATH_CS0_OUTPUT_HOPID_MASK   0x0000007F
#define USB4DR_PATH_CS0_OUTPUT_ADAPTER_MASK 0x0001F800
#define USB4DR_PATH_CS0_CREDITS_MASK        0x00FE0000  /**< HopID 0 holds the control buffers */
#define USB4DR_PATH_CS0_VALID               0x80000000

/* HopIDs 0 to 7 are reserved; protocol paths start at 8 */
#define USB4DR_FIRST_PATH_HOPID             8
#define USB4DR_PROTOCOL_INPUT_HOPID         8
#define USB4DR_PROTOCOL_OUTPUT_HOPID        9

/* Protocol adapter capabilities (USB4 10.3), relative to the capability dword */

#define USB4DR_PCIE_CS_0                    0
#define USB4DR_PCIE_CS0_LTSSM_MASK          0x1E000000  /**< 0 is Detect */
#define USB4DR_PCIE_CS0_PATH_ENABLE         0x80000000
#define USB4DR_PCIE_CS_1                    1
#define USB4DR_PCIE_CS1_EXTENDED_ENCAP      0x00000001

#define USB4DR_USB3_CS_0                    0
#define USB4DR_USB3_CS0_VALID               0x40000000
#define USB4DR_USB3_CS0_PATH_ENABLE         0x80000000
#define USB4DR_USB3_CS_4                    4
#define USB4DR_USB3_CS4_MAX_RATE_MASK       0x0007F000
#define USB4DR_USB3_MAX_RATE_GEN2X2         1   /**< max supported link rate field: 20 Gbps class */

/* USB3 adapter vendor capability (VSC ID 0), dword 1: back pressure bits 11:8 */
#define USB4DR_USB3_VSC_CS_1                1
#define USB4DR_USB3_VSC1_BACK_PRESSURE_MASK 0x00000F00
#define USB4DR_USB3_VSC1_BACK_PRESSURE_MIN  0x00000800

/* PCIe adapters wait for LTSSM Detect before asking for a tunnel: 100 ms timer, 50 retries */
#define USB4DR_LTSSM_POLL_MS                100
#define USB4DR_LTSSM_POLL_COUNT             50

/* A USB3 down adapter waits 500 ms after start before it is disconnected */
#define USB4DR_USB3_DOWN_SETTLE_MS          500

/* Router startup *************************************************************/

/* Notification timeout a USB4 router writes into its own ROUTER_CS_4, and a TBT3 one */
#define USB4DR_NOTIFY_TIMEOUT_USB4          0xFF
#define USB4DR_NOTIFY_TIMEOUT_TBT3          0xFE

/* Connection manager USB4 version written into ROUTER_CS_4 bits 15:8 */
#define USB4DR_CM_VERSION_BYTE_1            0x10
#define USB4DR_CM_VERSION_BYTE_2            0x20

/* Router Ready and Configuration Ready: 10 ms for 10 polls, then 100 ms, 60 polls in all */
#define USB4DR_READY_POLL_FAST_MS           10
#define USB4DR_READY_POLL_SLOW_MS           100
#define USB4DR_READY_POLL_FAST_COUNT        10
#define USB4DR_READY_POLL_COUNT             60

/* DROM, the same format the USB hub driver reads ***************************/

#define USB4DR_DROM_HEADER_BYTES            16
#define USB4DR_DROM_TBT3_HEADER_BYTES       22
#define USB4DR_DROM_REVISION_OFFSET         13      /**< 1 or 2: TBT3 header, 3 and up: USB4 */
#define USB4DR_DROM_LENGTH_OFFSET           14      /**< 12 bit data length */
#define USB4DR_DROM_LENGTH_MASK             0x0FFF
#define USB4DR_DROM_LENGTH_MIN              3
#define USB4DR_DROM_SIZE_EXTRA              13      /**< total DROM bytes = length + 13 */

/* Entry header: byte 0 length (0 or 1 ends the list), byte 1 bit 7 adapter entry */
#define USB4DR_DROM_ENTRY_ADAPTER           0x80
#define USB4DR_DROM_ENTRY_TYPE_MASK         0x3F
#define USB4DR_DROM_ENTRY_VENDOR_NAME       1
#define USB4DR_DROM_ENTRY_MODEL_NAME        2
#define USB4DR_DROM_ENTRY_TMU_MODE          8
#define USB4DR_DROM_ENTRY_PRODUCT           9
#define USB4DR_DROM_ENTRY_SERIAL            10
#define USB4DR_DROM_ENTRY_USB_PORT_MAP      11
#define USB4DR_DROM_ENTRY_VENDOR_NAME_UTF16 12
#define USB4DR_DROM_ENTRY_MODEL_NAME_UTF16  13
#define USB4DR_DROM_PRODUCT_MIN_LENGTH      15
#define USB4DR_DROM_SERIAL_MIN_LENGTH       5
