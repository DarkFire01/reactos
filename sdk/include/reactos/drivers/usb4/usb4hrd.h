/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Contract between the USB4 host router and the USB4 device router drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * Every value here (IOCTL codes, GUIDs, sizes and offsets) matches what the
 * Windows 11 drivers exchange, so either side can be paired with the Windows
 * counterpart. Layouts are given for x64; the drivers only ship for x64.
 */

#ifndef FILE_DEVICE_USB4
#define FILE_DEVICE_USB4                    0x00000060
#endif

/* Device interfaces **********************************************************/

/* Virtual power coordination PDO; the reference string names the host router */
DEFINE_GUID(GUID_USB4HR_VIRTUAL_POWER_INTERFACE,
    0x9c0d3eae, 0xfba6, 0x4e08, 0x9e, 0x8d, 0x18, 0x1c, 0xbb, 0x39, 0x97, 0xd6);

/* Root router PDO, the depth 0 router the device router driver binds to */
DEFINE_GUID(GUID_USB4HR_ROOT_ROUTER_INTERFACE,
    0x3b6d80c9, 0x039c, 0x44ca, 0xba, 0x0e, 0x33, 0xba, 0x62, 0xa5, 0xcf, 0xb8);

/* Hardware services; also the query interface GUID of USB4HR_HARDWARE_SERVICES */
DEFINE_GUID(GUID_USB4HR_HARDWARE_SERVICES,
    0xd830231d, 0x81ce, 0x409b, 0x84, 0xfe, 0xec, 0xe5, 0xc2, 0x4a, 0x47, 0x9f);

/* Query interface GUID of USB4HR_PARENT_INTERFACE */
DEFINE_GUID(GUID_USB4HR_PARENT_INTERFACE,
    0xab69991b, 0x3f22, 0x457a, 0x94, 0xb4, 0x52, 0x3e, 0xe3, 0x4f, 0x94, 0xf5);

/* Property set shared by the USB4 drivers for the device properties they publish */
DEFINE_GUID(GUID_USB4HR_PROPERTY_SET,
    0x5df7e321, 0x1c1b, 0x4ce2, 0xb4, 0xfa, 0x55, 0xf4, 0xa5, 0xbc, 0x2c, 0xb6);

#define USB4HR_PROPERTY_D3COLD_SUPPORT      3   /**< DEVPROP_TYPE_BOOLEAN, host router PDO */
#define USB4HR_PROPERTY_RESET_SUPPORT       4   /**< DEVPROP_TYPE_BOOLEAN, host router PDO */
#define USB4HR_PROPERTY_OSC_CONTROL         6   /**< DEVPROP_TYPE_UINT32, host router PDO */

/* Set by the device router driver on its router PDO */
#define USB4HR_PROPERTY_ADAPTER_SUPPORT     2   /**< DEVPROP_TYPE_UINT32, USB4HR_ADAPTER_SUPPORT_* */
#define USB4HR_PROPERTY_IS_TBT3             8   /**< DEVPROP_TYPE_BOOLEAN */
#define USB4HR_PROPERTY_VENDOR_NAME         9   /**< DEVPROP_TYPE_STRING, from the DROM */
#define USB4HR_PROPERTY_MODEL_NAME          10  /**< DEVPROP_TYPE_STRING, from the DROM */
#define USB4HR_PROPERTY_TOPOLOGY_ID         11  /**< DEVPROP_TYPE_BINARY, Port[0..6] */
#define USB4HR_PROPERTY_DOMAIN_ID           12  /**< DEVPROP_TYPE_UINT32 */
#define USB4HR_PROPERTY_SILICON_VENDOR_ID   16  /**< DEVPROP_TYPE_UINT16, ROUTER_CS_0 */
#define USB4HR_PROPERTY_SILICON_PRODUCT_ID  17  /**< DEVPROP_TYPE_UINT16, ROUTER_CS_0 */
#define USB4HR_PROPERTY_SILICON_REVISION    18  /**< DEVPROP_TYPE_UINT16, ROUTER_CS_1 revision */
#define USB4HR_PROPERTY_USB4_VERSION        19  /**< DEVPROP_TYPE_UINT32, ROUTER_CS_4 version byte */

#define USB4HR_ADAPTER_SUPPORT_LANE         0x00000001
#define USB4HR_ADAPTER_SUPPORT_PCIE         0x00000002
#define USB4HR_ADAPTER_SUPPORT_DP           0x00000004
#define USB4HR_ADAPTER_SUPPORT_USB3         0x00000008

/* Status codes ***************************************************************/

/* USB4_STATUS values (public usb4dbgioctl.h): event codes of USB4 Table 6-11 plus software codes */
typedef ULONG USB4HR_STATUS, *PUSB4HR_STATUS;

#define USB4HR_STATUS_ERR_CONN              0
#define USB4HR_STATUS_ERR_LINK              1
#define USB4HR_STATUS_ERR_ADDR              2
#define USB4HR_STATUS_ERR_ADP               4
#define USB4HR_STATUS_HP_ACK                7
#define USB4HR_STATUS_ERR_ENUM              8
#define USB4HR_STATUS_ERR_NUA               9
#define USB4HR_STATUS_ERR_LEN               11
#define USB4HR_STATUS_ERR_HEC               12
#define USB4HR_STATUS_ERR_FC                13
#define USB4HR_STATUS_ERR_PLUG              14
#define USB4HR_STATUS_ERR_LOCK              15
#define USB4HR_STATUS_DP_BW                 32
#define USB4HR_STATUS_ROP_CMPLT             33
#define USB4HR_STATUS_POP_CMPLT             34
#define USB4HR_STATUS_PCIE_WAKE             35
#define USB4HR_STATUS_DP_CON_CHANGE         36
#define USB4HR_STATUS_DPTX_DISCOVERY        37
#define USB4HR_STATUS_LINK_RECOVERY         38
#define USB4HR_STATUS_ASYM_LINK             39
#define USB4HR_STATUS_POLLING_SKIPPED       252
#define USB4HR_STATUS_POLLING_TIMEOUT       253
#define USB4HR_STATUS_SUCCESS               254
#define USB4HR_STATUS_FAILURE               255

/* Configuration spaces (USB4 Table 6-2, public USB4_CONFIG_SPACE_TYPE) */
#define USB4HR_SPACE_PATH                   0
#define USB4HR_SPACE_ADAPTER                1
#define USB4HR_SPACE_ROUTER                 2
#define USB4HR_SPACE_COUNTERS               3

/* Largest dword count of one configuration read or write */
#define USB4HR_MAX_CONFIG_DWORDS            60

/* Deepest router below the host router (USB4 6.1) */
#define USB4HR_MAX_DEPTH                    6

/* Internal IOCTLs ************************************************************/

/*
 * All are IRP_MJ_INTERNAL_DEVICE_CONTROL, METHOD_NEITHER, sent to the root
 * router PDO. Buffers are the request's input and output buffers; the config
 * access data buffer is a separate caller pointer used in place.
 */
#define USB4HR_IOCTL(Function) \
    CTL_CODE(FILE_DEVICE_USB4, (Function), METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_USB4HR_READ_CONFIG                    USB4HR_IOCTL(0x407)
#define IOCTL_USB4HR_WRITE_CONFIG                   USB4HR_IOCTL(0x408)
#define IOCTL_USB4HR_CREATE_TUNNEL                  USB4HR_IOCTL(0x40A)
#define IOCTL_USB4HR_DESTROY_TUNNEL                 USB4HR_IOCTL(0x40B)
#define IOCTL_USB4HR_WAIT_ROUTER_EVENT              USB4HR_IOCTL(0x40D)
#define IOCTL_USB4HR_WAIT_ADAPTER_EVENT             USB4HR_IOCTL(0x40E)
#define IOCTL_USB4HR_REBUILD_TUNNEL                 USB4HR_IOCTL(0x412)
#define IOCTL_USB4HR_READ_CONFIG_EX                 USB4HR_IOCTL(0x415)
#define IOCTL_USB4HR_SEND_INTER_DOMAIN_REQUEST      USB4HR_IOCTL(0x417)
#define IOCTL_USB4HR_SEND_INTER_DOMAIN_RESPONSE     USB4HR_IOCTL(0x418)
#define IOCTL_USB4HR_WAIT_INTER_DOMAIN_PACKET       USB4HR_IOCTL(0x419)
#define IOCTL_USB4HR_CREATE_INTER_DOMAIN_PATH       USB4HR_IOCTL(0x41C)
#define IOCTL_USB4HR_DESTROY_INTER_DOMAIN_PATH      USB4HR_IOCTL(0x41D)
#define IOCTL_USB4HR_ALLOCATE_DP_BANDWIDTH          USB4HR_IOCTL(0x421)
#define IOCTL_USB4HR_RELEASE_DP_BANDWIDTH           USB4HR_IOCTL(0x422)
#define IOCTL_USB4HR_REGISTER_DOMAIN_POLICY         USB4HR_IOCTL(0x428)
#define IOCTL_USB4HR_REPORT_TMU_REQUIREMENT         USB4HR_IOCTL(0x429)
#define IOCTL_USB4HR_HOST_ROUTER_RESET              USB4HR_IOCTL(0x42B)

C_ASSERT(IOCTL_USB4HR_READ_CONFIG == 0x0060101F);
C_ASSERT(IOCTL_USB4HR_WRITE_CONFIG == 0x00601023);
C_ASSERT(IOCTL_USB4HR_CREATE_TUNNEL == 0x0060102B);
C_ASSERT(IOCTL_USB4HR_DESTROY_TUNNEL == 0x0060102F);
C_ASSERT(IOCTL_USB4HR_WAIT_ROUTER_EVENT == 0x00601037);
C_ASSERT(IOCTL_USB4HR_WAIT_ADAPTER_EVENT == 0x0060103B);
C_ASSERT(IOCTL_USB4HR_REBUILD_TUNNEL == 0x0060104B);
C_ASSERT(IOCTL_USB4HR_READ_CONFIG_EX == 0x00601057);
C_ASSERT(IOCTL_USB4HR_SEND_INTER_DOMAIN_REQUEST == 0x0060105F);
C_ASSERT(IOCTL_USB4HR_SEND_INTER_DOMAIN_RESPONSE == 0x00601063);
C_ASSERT(IOCTL_USB4HR_WAIT_INTER_DOMAIN_PACKET == 0x00601067);
C_ASSERT(IOCTL_USB4HR_CREATE_INTER_DOMAIN_PATH == 0x00601073);
C_ASSERT(IOCTL_USB4HR_DESTROY_INTER_DOMAIN_PATH == 0x00601077);
C_ASSERT(IOCTL_USB4HR_ALLOCATE_DP_BANDWIDTH == 0x00601087);
C_ASSERT(IOCTL_USB4HR_RELEASE_DP_BANDWIDTH == 0x0060108B);
C_ASSERT(IOCTL_USB4HR_REGISTER_DOMAIN_POLICY == 0x006010A3);
C_ASSERT(IOCTL_USB4HR_REPORT_TMU_REQUIREMENT == 0x006010A7);
C_ASSERT(IOCTL_USB4HR_HOST_ROUTER_RESET == 0x006010AF);

/* Router to router requests ***************************************************/

/*
 * A device router sends these to its own PDO. The PDO forwards each to a queue
 * of the parent router's downstream port; the parent fills in its half or passes
 * the request on unchanged to its own PDO, up to the host router. CREATE, DESTROY
 * and REBUILD_TUNNEL above travel this way too.
 */
#define IOCTL_USB4HR_CLIENT_CREATE_INTER_DOMAIN_TUNNEL  USB4HR_IOCTL(0x005)
#define IOCTL_USB4HR_CLIENT_DESTROY_INTER_DOMAIN_TUNNEL USB4HR_IOCTL(0x006)
#define IOCTL_USB4HR_ALLOCATE_DP_IN_ADAPTER         USB4HR_IOCTL(0x40F)
#define IOCTL_USB4HR_FREE_DP_IN_ADAPTER             USB4HR_IOCTL(0x410)
#define IOCTL_USB4HR_EXCHANGE_DP_CAPABILITIES       USB4HR_IOCTL(0x411)
#define IOCTL_USB4HR_INITIATE_LANE_BONDING          USB4HR_IOCTL(0x414)
#define IOCTL_USB4HR_READ_GRANDMASTER_TIME          USB4HR_IOCTL(0x416)
#define IOCTL_USB4HR_ENABLE_UNI_TMU                 USB4HR_IOCTL(0x41E)
#define IOCTL_USB4HR_DISABLE_UNI_TMU                USB4HR_IOCTL(0x41F)
#define IOCTL_USB4HR_ENABLE_TIME_SYNC               USB4HR_IOCTL(0x420)
#define IOCTL_USB4HR_DISABLE_CLX                    USB4HR_IOCTL(0x424)
#define IOCTL_USB4HR_SET_PORT_CONFIGURED            USB4HR_IOCTL(0x425)
#define IOCTL_USB4HR_CLEAR_PORT_CONFIGURED          USB4HR_IOCTL(0x426)
#define IOCTL_USB4HR_GET_LAST_PORT_RESET_TIME       USB4HR_IOCTL(0x427)
#define IOCTL_USB4HR_DISABLE_TIME_SYNC              USB4HR_IOCTL(0x42A)
#define IOCTL_USB4HR_ENABLE_ENHANCED_UNI_TMU        USB4HR_IOCTL(0x42C)
#define IOCTL_USB4HR_SWITCH_TO_ASYMMETRIC           USB4HR_IOCTL(0x42D)
#define IOCTL_USB4HR_SWITCH_TO_SYMMETRIC            USB4HR_IOCTL(0x42E)
#define IOCTL_USB4HR_VALIDATE_LINK_WIDTH            USB4HR_IOCTL(0x42F)
#define IOCTL_USB4HR_ENABLE_CLX                     USB4HR_IOCTL(0x430)
#define IOCTL_USB4HR_QUERY_RETIMERS                 USB4HR_IOCTL(0x431)

C_ASSERT(IOCTL_USB4HR_CLIENT_CREATE_INTER_DOMAIN_TUNNEL == 0x00600017);
C_ASSERT(IOCTL_USB4HR_CLIENT_DESTROY_INTER_DOMAIN_TUNNEL == 0x0060001B);
C_ASSERT(IOCTL_USB4HR_ALLOCATE_DP_IN_ADAPTER == 0x0060103F);
C_ASSERT(IOCTL_USB4HR_FREE_DP_IN_ADAPTER == 0x00601043);
C_ASSERT(IOCTL_USB4HR_EXCHANGE_DP_CAPABILITIES == 0x00601047);
C_ASSERT(IOCTL_USB4HR_INITIATE_LANE_BONDING == 0x00601053);
C_ASSERT(IOCTL_USB4HR_READ_GRANDMASTER_TIME == 0x0060105B);
C_ASSERT(IOCTL_USB4HR_ENABLE_UNI_TMU == 0x0060107B);
C_ASSERT(IOCTL_USB4HR_DISABLE_UNI_TMU == 0x0060107F);
C_ASSERT(IOCTL_USB4HR_ENABLE_TIME_SYNC == 0x00601083);
C_ASSERT(IOCTL_USB4HR_DISABLE_CLX == 0x00601093);
C_ASSERT(IOCTL_USB4HR_SET_PORT_CONFIGURED == 0x00601097);
C_ASSERT(IOCTL_USB4HR_CLEAR_PORT_CONFIGURED == 0x0060109B);
C_ASSERT(IOCTL_USB4HR_GET_LAST_PORT_RESET_TIME == 0x0060109F);
C_ASSERT(IOCTL_USB4HR_DISABLE_TIME_SYNC == 0x006010AB);
C_ASSERT(IOCTL_USB4HR_ENABLE_ENHANCED_UNI_TMU == 0x006010B3);
C_ASSERT(IOCTL_USB4HR_SWITCH_TO_ASYMMETRIC == 0x006010B7);
C_ASSERT(IOCTL_USB4HR_SWITCH_TO_SYMMETRIC == 0x006010BB);
C_ASSERT(IOCTL_USB4HR_VALIDATE_LINK_WIDTH == 0x006010BF);
C_ASSERT(IOCTL_USB4HR_ENABLE_CLX == 0x006010C3);
C_ASSERT(IOCTL_USB4HR_QUERY_RETIMERS == 0x006010C7);

/*
 * Smallest input and output buffers each router to router request must carry;
 * a router completes a smaller one with STATUS_BUFFER_TOO_SMALL.
 */
#define USB4HR_R2R_MIN_CREATE_TUNNEL_IN             472
#define USB4HR_R2R_MIN_CREATE_TUNNEL_OUT            24
#define USB4HR_R2R_MIN_DESTROY_TUNNEL_IN            8
#define USB4HR_R2R_MIN_DESTROY_TUNNEL_OUT           4
#define USB4HR_R2R_MIN_REBUILD_TUNNEL_IN            16
#define USB4HR_R2R_MIN_REBUILD_TUNNEL_OUT           4
#define USB4HR_R2R_MIN_ALLOCATE_DP_IN_IN            8
#define USB4HR_R2R_MIN_ALLOCATE_DP_IN_OUT           24
#define USB4HR_R2R_MIN_FREE_DP_IN_IN                24
#define USB4HR_R2R_MIN_FREE_DP_IN_OUT               4
#define USB4HR_R2R_MIN_EXCHANGE_DP_CAPS_IN          32
#define USB4HR_R2R_MIN_EXCHANGE_DP_CAPS_OUT         16
#define USB4HR_R2R_MIN_LANE_BONDING_IN              1
#define USB4HR_R2R_MIN_GRANDMASTER_TIME_OUT         12
#define USB4HR_R2R_MIN_CLIENT_XD_CREATE_IN          32
#define USB4HR_R2R_MIN_CLIENT_XD_CREATE_OUT         48
#define USB4HR_R2R_MIN_XD_CREATE_IN                 472
#define USB4HR_R2R_MIN_XD_CREATE_OUT                64
#define USB4HR_R2R_MIN_CLIENT_XD_DESTROY_IN         8
#define USB4HR_R2R_MIN_XD_DESTROY_IN                8
#define USB4HR_R2R_MIN_XD_DESTROY_OUT               4
#define USB4HR_R2R_MIN_ENABLE_CLX_IN                16
#define USB4HR_R2R_MIN_ENABLE_CLX_OUT               12
#define USB4HR_R2R_MIN_LAST_PORT_RESET_OUT          8

/** Input of INITIATE_LANE_BONDING. */
typedef struct _USB4HR_LANE_BONDING_INPUT
{
    BOOLEAN DisableLane1;
} USB4HR_LANE_BONDING_INPUT, *PUSB4HR_LANE_BONDING_INPUT;

/** Input of ENABLE_CLX: CL0s, CL1 and CL2 support, then the requested mode. */
typedef struct _USB4HR_ENABLE_CLX_INPUT
{
    ULONG Cl0sSupported;
    ULONG Cl1Supported;
    ULONG Cl2Supported;
    ULONG Mode;
} USB4HR_ENABLE_CLX_INPUT, *PUSB4HR_ENABLE_CLX_INPUT;

/** Output of GET_LAST_PORT_RESET_TIME. */
typedef struct _USB4HR_LAST_PORT_RESET_OUTPUT
{
    LARGE_INTEGER Timestamp;
} USB4HR_LAST_PORT_RESET_OUTPUT, *PUSB4HR_LAST_PORT_RESET_OUTPUT;

C_ASSERT(sizeof(USB4HR_LANE_BONDING_INPUT) == USB4HR_R2R_MIN_LANE_BONDING_IN);
C_ASSERT(sizeof(USB4HR_ENABLE_CLX_INPUT) == USB4HR_R2R_MIN_ENABLE_CLX_IN);
C_ASSERT(sizeof(USB4HR_LAST_PORT_RESET_OUTPUT) == USB4HR_R2R_MIN_LAST_PORT_RESET_OUT);

/* Handles and topology *******************************************************/

/** Router or adapter handle from the hardware services; tunnel handle from CREATE_TUNNEL. */
typedef PVOID USB4HR_HANDLE, *PUSB4HR_HANDLE;

/** Router position: Port[n] is the downstream adapter taken at depth n + 1. */
typedef struct _USB4HR_TOPOLOGY_ID
{
    ULONG Depth;
    UCHAR Port[USB4HR_MAX_DEPTH + 1];
    UCHAR Reserved;
} USB4HR_TOPOLOGY_ID, *PUSB4HR_TOPOLOGY_ID;

C_ASSERT(sizeof(USB4HR_TOPOLOGY_ID) == 12);

/* Domain ID: bits 2:0 the format, then format specific fields */
#define USB4HR_DOMAIN_FORMAT_MASK           0x00000007
#define USB4HR_DOMAIN_FORMAT_PCI            1   /**< bus 15:8, device 20:16, function 23:21 */
#define USB4HR_DOMAIN_FORMAT_ACPI           2   /**< connection manager ID 13:8 */
#define USB4HR_DOMAIN_PCI_BUS_SHIFT         8
#define USB4HR_DOMAIN_PCI_DEVICE_SHIFT      16
#define USB4HR_DOMAIN_PCI_FUNCTION_SHIFT    21
#define USB4HR_DOMAIN_CMID_SHIFT            8

/* Router types for USB4HR_REPORT_ROUTER_FAMILY */
typedef enum _USB4HR_ROUTER_FAMILY
{
    Usb4HrRouterUnknown = 0,
    Usb4HrRouterTbt3AlpineRidge = 1,
    Usb4HrRouterTbt3TitanRidge = 2,
    Usb4HrRouterUsb4 = 3
} USB4HR_ROUTER_FAMILY;

/* Configuration access (READ_CONFIG, READ_CONFIG_EX, WRITE_CONFIG) ***********/

/** Input of the three configuration IOCTLs, 40 bytes. */
typedef struct _USB4HR_CONFIG_INPUT
{
    USB4HR_HANDLE Handle;   /**< router handle for the router space, adapter handle otherwise */
    ULONG Space;            /**< USB4HR_SPACE_* */
    ULONG Reserved0;
    ULONG DwordOffset;
    ULONG Reserved1;
    ULONG DwordCount;       /**< 1 to USB4HR_MAX_CONFIG_DWORDS */
    ULONG Reserved2;
    PULONG Buffer;          /**< read destination or write source, DwordCount dwords */
} USB4HR_CONFIG_INPUT, *PUSB4HR_CONFIG_INPUT;

/** Output of READ_CONFIG and WRITE_CONFIG. */
typedef struct _USB4HR_CONFIG_OUTPUT
{
    USB4HR_STATUS Status;
} USB4HR_CONFIG_OUTPUT, *PUSB4HR_CONFIG_OUTPUT;

/** Output of READ_CONFIG_EX. */
typedef struct _USB4HR_CONFIG_EX_OUTPUT
{
    USB4HR_STATUS Status;
    ULONG AdapterNumber;    /**< adapter field of the read response */
} USB4HR_CONFIG_EX_OUTPUT, *PUSB4HR_CONFIG_EX_OUTPUT;

/* Events (WAIT_ROUTER_EVENT, WAIT_ADAPTER_EVENT) *****************************/

/** Input of both WAIT IOCTLs and of WAIT_INTER_DOMAIN_PACKET. */
typedef struct _USB4HR_WAIT_INPUT
{
    USB4HR_HANDLE Handle;
} USB4HR_WAIT_INPUT, *PUSB4HR_WAIT_INPUT;

/** Output of both WAIT IOCTLs: one hot plug event. */
typedef struct _USB4HR_WAIT_OUTPUT
{
    USB4HR_STATUS Status;   /**< USB4HR_STATUS_SUCCESS */
    ULONG Unplugged;        /**< 1 when the hot plug packet had the unplug bit */
} USB4HR_WAIT_OUTPUT, *PUSB4HR_WAIT_OUTPUT;

/* Tunnels (CREATE_TUNNEL, DESTROY_TUNNEL, REBUILD_TUNNEL) ********************/

#define USB4HR_TUNNEL_USB3                  0
#define USB4HR_TUNNEL_PCIE                  1
#define USB4HR_TUNNEL_DISPLAYPORT           2
#define USB4HR_TUNNEL_INTER_DOMAIN          3

#define USB4HR_USB3_RATE_10G                0
#define USB4HR_USB3_RATE_20G                1

#define USB4HR_PROTOCOL_PATH_HOPS           2
#define USB4HR_LONG_PATH_HOPS               6

/** One hop of a path: the ingress side of an adapter and where it forwards to. */
typedef struct _USB4HR_TUNNEL_SEGMENT
{
    USB4HR_HANDLE AdapterHandle;    /**< ingress adapter */
    UCHAR CapabilityOffset;         /**< protocol adapter capability dword, protocol segments only */
    UCHAR Reserved0;
    USHORT IngressHopId;
    USHORT OutputHopId;
    UCHAR OutputAdapter;
    UCHAR PathCredits;
    BOOLEAN IngressFlowControl;
    BOOLEAN EgressFlowControl;
    BOOLEAN IngressSharedBuffering;
    BOOLEAN EgressSharedBuffering;
    USHORT EgressMaxOutputHopId;
    BOOLEAN PmPacketSupport;
    UCHAR Reserved1;
} USB4HR_TUNNEL_SEGMENT, *PUSB4HR_TUNNEL_SEGMENT;

/**
 * USB3 or PCIe tunnel over one link. In each path segment 0 is the protocol adapter hop and
 * segment 1 the lane adapter hop: Outbound[0] is the parent's down adapter, Inbound[0] the
 * child's up adapter.
 */
typedef struct _USB4HR_PROTOCOL_TUNNEL
{
    USB4HR_TUNNEL_SEGMENT Inbound[USB4HR_PROTOCOL_PATH_HOPS];
    USB4HR_TUNNEL_SEGMENT Outbound[USB4HR_PROTOCOL_PATH_HOPS];
    ULONG Usb3MaxLinkRate;              /**< USB4HR_USB3_RATE_* */
    BOOLEAN PcieExtendedEncapsulation;
} USB4HR_PROTOCOL_TUNNEL, *PUSB4HR_PROTOCOL_TUNNEL;

/** DisplayPort tunnel; segment n is the router at depth n. Not handled this round. */
typedef struct _USB4HR_DP_TUNNEL
{
    ULONG SourceDepth;
    ULONG SinkDepth;
    PVOID DisconnectCallback;
    PVOID DisconnectContext;
    USB4HR_TUNNEL_SEGMENT MainPath[USB4HR_LONG_PATH_HOPS];
    USB4HR_TUNNEL_SEGMENT DpInAuxPath[USB4HR_LONG_PATH_HOPS];
    USB4HR_TUNNEL_SEGMENT DpOutAuxPath[USB4HR_LONG_PATH_HOPS];
    USHORT AllocatedBandwidth;          /**< 100 Mbps units */
    UCHAR NrdMaxLinkRate;
    UCHAR NrdMaxLaneCount;
} USB4HR_DP_TUNNEL, *PUSB4HR_DP_TUNNEL;

/** Host to host DMA path. Not handled this round. */
typedef struct _USB4HR_INTER_DOMAIN_TUNNEL
{
    PVOID ClientContext;
    PVOID FramesTransmittedCallback;
    PVOID FramesReceivedCallback;
    ULONG LinkDepth;
    BOOLEAN EndToEndFlowControl;
    USB4HR_TUNNEL_SEGMENT Inbound[USB4HR_LONG_PATH_HOPS];
    USB4HR_TUNNEL_SEGMENT Outbound[USB4HR_LONG_PATH_HOPS];
} USB4HR_INTER_DOMAIN_TUNNEL, *PUSB4HR_INTER_DOMAIN_TUNNEL;

/** Input of CREATE_TUNNEL and CREATE_INTER_DOMAIN_PATH, 472 bytes. */
typedef struct _USB4HR_TUNNEL_BUILD_REQUEST
{
    ULONG TunnelType;   /**< USB4HR_TUNNEL_* */
    union
    {
        USB4HR_PROTOCOL_TUNNEL Protocol;
        USB4HR_DP_TUNNEL DisplayPort;
        USB4HR_INTER_DOMAIN_TUNNEL InterDomain;
    };
} USB4HR_TUNNEL_BUILD_REQUEST, *PUSB4HR_TUNNEL_BUILD_REQUEST;

/** Output of CREATE_TUNNEL, 24 bytes. */
typedef struct _USB4HR_TUNNEL_BUILD_RESULT
{
    USB4HR_STATUS Status;
    USB4HR_HANDLE TunnelHandle;
    BOOLEAN ModeEnabled;    /**< DP: DPTX bandwidth allocation mode, PCIe: extended encapsulation */
} USB4HR_TUNNEL_BUILD_RESULT, *PUSB4HR_TUNNEL_BUILD_RESULT;

/** Input of DESTROY_TUNNEL and DESTROY_INTER_DOMAIN_PATH. */
typedef struct _USB4HR_TUNNEL_TEARDOWN_REQUEST
{
    USB4HR_HANDLE TunnelHandle;
} USB4HR_TUNNEL_TEARDOWN_REQUEST, *PUSB4HR_TUNNEL_TEARDOWN_REQUEST;

/** Input of REBUILD_TUNNEL, 16 bytes. */
typedef struct _USB4HR_TUNNEL_RESTORE_REQUEST
{
    USB4HR_HANDLE TunnelHandle;
    UCHAR NrdMaxLinkRate;
    UCHAR NrdMaxLaneCount;
} USB4HR_TUNNEL_RESTORE_REQUEST, *PUSB4HR_TUNNEL_RESTORE_REQUEST;

/** Output of DESTROY_TUNNEL, REBUILD_TUNNEL and DESTROY_INTER_DOMAIN_PATH. */
typedef struct _USB4HR_TUNNEL_STATUS_OUTPUT
{
    USB4HR_STATUS Status;
} USB4HR_TUNNEL_STATUS_OUTPUT, *PUSB4HR_TUNNEL_STATUS_OUTPUT;

/* Inter-domain packets, DP bandwidth, domain policy, reset *******************/

#define USB4HR_INTER_DOMAIN_MAX_PAYLOAD     248

/** Input of SEND_INTER_DOMAIN_REQUEST and SEND_INTER_DOMAIN_RESPONSE. */
typedef struct _USB4HR_INTER_DOMAIN_SEND_INPUT
{
    USB4HR_HANDLE AdapterHandle;
    ULONG64 PayloadLength;  /**< bytes, multiple of 4, at most 244 */
    PVOID Payload;
} USB4HR_INTER_DOMAIN_SEND_INPUT, *PUSB4HR_INTER_DOMAIN_SEND_INPUT;

/** Output of WAIT_INTER_DOMAIN_PACKET, 264 bytes. */
typedef struct _USB4HR_INTER_DOMAIN_PACKET_OUTPUT
{
    ULONG IsResponse;
    ULONG Reserved;
    ULONG64 PayloadLength;
    UCHAR Payload[USB4HR_INTER_DOMAIN_MAX_PAYLOAD];
} USB4HR_INTER_DOMAIN_PACKET_OUTPUT, *PUSB4HR_INTER_DOMAIN_PACKET_OUTPUT;

/** Input of ALLOCATE_DP_BANDWIDTH and RELEASE_DP_BANDWIDTH; allocation returns a USHORT. */
typedef struct _USB4HR_DP_BANDWIDTH_INPUT
{
    USB4HR_HANDLE DpInAdapterHandle;
    USHORT Bandwidth;       /**< 100 Mbps units */
} USB4HR_DP_BANDWIDTH_INPUT, *PUSB4HR_DP_BANDWIDTH_INPUT;

/** Input of REGISTER_DOMAIN_POLICY, 48 bytes; the callback prototypes are not decoded. */
typedef struct _USB4HR_DOMAIN_POLICY_INPUT
{
    PVOID Context;
    PVOID TmuConfigCallback;
    PVOID AsymmetricLinkCallback;
    PVOID DomainPowerDownCallback;
    PVOID CallbackContext;
    ULONG Flags;
} USB4HR_DOMAIN_POLICY_INPUT, *PUSB4HR_DOMAIN_POLICY_INPUT;

/** Input of REPORT_TMU_REQUIREMENT; the output is a ULONG domain policy. */
typedef struct _USB4HR_TMU_REQUIREMENT_INPUT
{
    USB4HR_HANDLE RegistrationHandle;
    ULONG RequestedMode;
} USB4HR_TMU_REQUIREMENT_INPUT, *PUSB4HR_TMU_REQUIREMENT_INPUT;

/** Input of HOST_ROUTER_RESET; the handle must be the depth 0 router. */
typedef struct _USB4HR_RESET_INPUT
{
    USB4HR_HANDLE RouterHandle;
} USB4HR_RESET_INPUT, *PUSB4HR_RESET_INPUT;

/* Parent interface (GUID_USB4HR_PARENT_INTERFACE) ****************************/

#define USB4HR_PARENT_INTERFACE_VERSION     1000

/* Domain flags */
#define USB4HR_DOMAIN_FLAG_NO_CLX           0x00000001ULL   /**< CL states disabled domain wide */
#define USB4HR_DOMAIN_FLAG_NO_XDOMAIN_E2E   0x00000002ULL   /**< no inter-domain end to end flow control */
#define USB4HR_DOMAIN_FLAG_LONG_DP_QUERY    0x00000004ULL   /**< extended DP resource query timeout */

/** Tunnel policy from the USB4 _OSC. */
typedef struct _USB4HR_TUNNEL_PERMISSIONS
{
    BOOLEAN PcieTunnelingDisabled;
    UCHAR HostInterfaceBufferLimit;
    UCHAR MaxUpstreamDpIn;
    UCHAR HostDpAdapters;
} USB4HR_TUNNEL_PERMISSIONS, *PUSB4HR_TUNNEL_PERMISSIONS;

C_ASSERT(sizeof(USB4HR_TUNNEL_PERMISSIONS) == 4);

/** Lane bonding or asymmetric link support of the link above the router. */
typedef BOOLEAN
(NTAPI USB4HR_PARENT_QUERY_LINK)(
    _In_ PVOID Context);
typedef USB4HR_PARENT_QUERY_LINK *PUSB4HR_PARENT_QUERY_LINK;

/** TRUE while the router connection the interface was handed for is still current. */
typedef BOOLEAN
(NTAPI USB4HR_PARENT_CONNECTION_LIVE)(
    _In_ PVOID Context);
typedef USB4HR_PARENT_CONNECTION_LIVE *PUSB4HR_PARENT_CONNECTION_LIVE;

/** Records the enumeration time a child asks for and returns the one in effect, in seconds. */
typedef ULONG
(NTAPI USB4HR_PARENT_AGREE_ENUM_DEADLINE)(
    _In_ PVOID Context,
    _In_ ULONG Seconds);
typedef USB4HR_PARENT_AGREE_ENUM_DEADLINE *PUSB4HR_PARENT_AGREE_ENUM_DEADLINE;

/** What a router learns from the device above it. 144 bytes, version 1000. */
typedef struct _USB4HR_PARENT_INTERFACE
{
    INTERFACE Header;
    ULONG DomainId;
    UCHAR DpBwModeCmId;
    UCHAR Reserved0[3];
    GUID DomainUuid;
    USB4HR_TOPOLOGY_ID TopologyId;
    ULONG Reserved1;
    PUSB4HR_PARENT_QUERY_LINK CanBondLanes;
    USB4HR_HANDLE RouterHandle;
    PDEVICE_OBJECT DomainRootPdo;
    USB4HR_TUNNEL_PERMISSIONS TunnelPermissions;
    ULONG Reserved2;
    ULONG64 DomainFlags;
    PUSB4HR_PARENT_CONNECTION_LIVE IsParentConnectionLive;
    UCHAR CmVersionLimit;     /**< 0x20 with USB4 version 2 enabled, else 0x10 */
    UCHAR HostInterfaceVersion;
    BOOLEAN EnhancedUniTmuSupported;
    UCHAR Reserved3[5];
    PUSB4HR_PARENT_QUERY_LINK IsAsymmetricSupported;
    PUSB4HR_PARENT_AGREE_ENUM_DEADLINE AgreeEnumerationDeadline;
} USB4HR_PARENT_INTERFACE, *PUSB4HR_PARENT_INTERFACE;

/* Hardware services interface (GUID_USB4HR_HARDWARE_SERVICES) ****************/

#define USB4HR_HARDWARE_SERVICES_VERSION    1000

/** Allocates the router handle for the router at TopologyId. */
typedef NTSTATUS
(NTAPI USB4HR_ALLOCATE_ROUTER_HANDLE)(
    _In_ PVOID Context,
    _In_ PUSB4HR_TOPOLOGY_ID TopologyId,
    _Out_ PUSB4HR_HANDLE RouterHandle);
typedef USB4HR_ALLOCATE_ROUTER_HANDLE *PUSB4HR_ALLOCATE_ROUTER_HANDLE;

/** Destroys a router handle; its event queues are purged. */
typedef NTSTATUS
(NTAPI USB4HR_DESTROY_ROUTER_HANDLE)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle);
typedef USB4HR_DESTROY_ROUTER_HANDLE *PUSB4HR_DESTROY_ROUTER_HANDLE;

/** Allocates handles for adapters 1 to Count (1 to 64); AdapterHandles[n] is adapter n + 1. */
typedef NTSTATUS
(NTAPI USB4HR_ISSUE_ADAPTER_HANDLES)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ UCHAR Count,
    _Out_writes_(Count) PUSB4HR_HANDLE AdapterHandles);
typedef USB4HR_ISSUE_ADAPTER_HANDLES *PUSB4HR_ISSUE_ADAPTER_HANDLES;

/** Destroys one adapter handle. */
typedef NTSTATUS
(NTAPI USB4HR_REVOKE_ADAPTER_HANDLE)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE AdapterHandle);
typedef USB4HR_REVOKE_ADAPTER_HANDLE *PUSB4HR_REVOKE_ADAPTER_HANDLE;

/** Records the router type and USB4 version byte for every node on the router's route. */
typedef NTSTATUS
(NTAPI USB4HR_REPORT_ROUTER_FAMILY)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ USB4HR_ROUTER_FAMILY Type,
    _In_ ULONG Usb4Version);
typedef USB4HR_REPORT_ROUTER_FAMILY *PUSB4HR_REPORT_ROUTER_FAMILY;

/** Completes parked WAIT requests of a node and stops its queues; the flags pick the queues. */
typedef VOID
(NTAPI USB4HR_PURGE_EVENT_QUEUES)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE Handle,
    _In_ BOOLEAN PurgeNotifications,
    _In_ BOOLEAN PurgeInterDomain);
typedef USB4HR_PURGE_EVENT_QUEUES *PUSB4HR_PURGE_EVENT_QUEUES;

/** Depth 0 router finished a D0 entry with D0Status; releases the power PDO waits. */
typedef VOID
(NTAPI USB4HR_ROOT_ROUTER_STARTED)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ NTSTATUS D0Status);
typedef USB4HR_ROOT_ROUTER_STARTED *PUSB4HR_ROOT_ROUTER_STARTED;

/** Reports the upstream link of a router; zeros on disconnect. */
typedef NTSTATUS
(NTAPI USB4HR_REPORT_UPSTREAM_LINK_BANDWIDTH)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ USHORT LinkBandwidthGbps,
    _In_ UCHAR LinkWidth,
    _In_ BOOLEAN Asymmetric);
typedef USB4HR_REPORT_UPSTREAM_LINK_BANDWIDTH *PUSB4HR_REPORT_UPSTREAM_LINK_BANDWIDTH;

/** DP tunnel bandwidth bookkeeping; the device router only logs the results. */
typedef NTSTATUS
(NTAPI USB4HR_READ_DP_BANDWIDTH_GRANT)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE TunnelHandle,
    _Out_ PUSHORT AllocatedBandwidth,
    _Out_ PULONG Detail,
    _Out_ PBOOLEAN Flag0,
    _Out_ PBOOLEAN Flag1);
typedef USB4HR_READ_DP_BANDWIDTH_GRANT *PUSB4HR_READ_DP_BANDWIDTH_GRANT;

/** Keeps the domain in D0 for a host DP IN adapter in alt mode behind a redriver. */
typedef NTSTATUS
(NTAPI USB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE)(
    _In_ PVOID Context,
    _In_ UCHAR DpInAdapter);
typedef USB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE *PUSB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE;

/** Drops the reference taken by USB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE. */
typedef VOID
(NTAPI USB4HR_UNPIN_DOMAIN_FOR_DP_ALT_MODE)(
    _In_ PVOID Context,
    _In_ UCHAR DpInAdapter);
typedef USB4HR_UNPIN_DOMAIN_FOR_DP_ALT_MODE *PUSB4HR_UNPIN_DOMAIN_FOR_DP_ALT_MODE;

/** Host router services every router driver of the domain calls directly. 120 bytes, version 1000. */
typedef struct _USB4HR_HARDWARE_SERVICES
{
    INTERFACE Header;
    PUSB4HR_ALLOCATE_ROUTER_HANDLE AllocateRouterHandle;
    PUSB4HR_DESTROY_ROUTER_HANDLE DestroyRouterHandle;
    PUSB4HR_ISSUE_ADAPTER_HANDLES IssueAdapterHandles;
    PUSB4HR_REVOKE_ADAPTER_HANDLE RevokeAdapterHandle;
    PUSB4HR_REPORT_ROUTER_FAMILY ReportRouterFamily;
    PUSB4HR_PURGE_EVENT_QUEUES PurgeEventQueues;
    PUSB4HR_ROOT_ROUTER_STARTED RootRouterStarted;
    PUSB4HR_REPORT_UPSTREAM_LINK_BANDWIDTH ReportUpstreamLinkBandwidth;
    PUSB4HR_READ_DP_BANDWIDTH_GRANT ReadDpBandwidthGrant;
    PUSB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE PinDomainForDpAltMode;
    PUSB4HR_UNPIN_DOMAIN_FOR_DP_ALT_MODE UnpinDomainForDpAltMode;
} USB4HR_HARDWARE_SERVICES, *PUSB4HR_HARDWARE_SERVICES;

/* x64 layouts ****************************************************************/

#ifdef _WIN64
C_ASSERT(sizeof(USB4HR_CONFIG_INPUT) == 40);
C_ASSERT(FIELD_OFFSET(USB4HR_CONFIG_INPUT, Space) == 8);
C_ASSERT(FIELD_OFFSET(USB4HR_CONFIG_INPUT, DwordOffset) == 16);
C_ASSERT(FIELD_OFFSET(USB4HR_CONFIG_INPUT, DwordCount) == 24);
C_ASSERT(FIELD_OFFSET(USB4HR_CONFIG_INPUT, Buffer) == 32);
C_ASSERT(sizeof(USB4HR_CONFIG_OUTPUT) == 4);
C_ASSERT(sizeof(USB4HR_CONFIG_EX_OUTPUT) == 8);
C_ASSERT(sizeof(USB4HR_WAIT_INPUT) == 8);
C_ASSERT(sizeof(USB4HR_WAIT_OUTPUT) == 8);

C_ASSERT(sizeof(USB4HR_TUNNEL_SEGMENT) == 24);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, CapabilityOffset) == 8);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, IngressHopId) == 10);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, OutputHopId) == 12);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, OutputAdapter) == 14);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, PathCredits) == 15);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, IngressFlowControl) == 16);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, EgressSharedBuffering) == 19);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, EgressMaxOutputHopId) == 20);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_SEGMENT, PmPacketSupport) == 22);

C_ASSERT(sizeof(USB4HR_TUNNEL_BUILD_REQUEST) == 472);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, Protocol.Inbound) == 8);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, Protocol.Outbound) == 56);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, Protocol.Usb3MaxLinkRate) == 104);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, Protocol.PcieExtendedEncapsulation) == 108);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.SinkDepth) == 12);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.DisconnectCallback) == 16);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.MainPath) == 32);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.DpInAuxPath) == 176);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.DpOutAuxPath) == 320);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.AllocatedBandwidth) == 464);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, DisplayPort.NrdMaxLaneCount) == 467);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, InterDomain.LinkDepth) == 32);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, InterDomain.EndToEndFlowControl) == 36);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, InterDomain.Inbound) == 40);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_REQUEST, InterDomain.Outbound) == 184);
C_ASSERT(sizeof(USB4HR_TUNNEL_BUILD_RESULT) == 24);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_RESULT, TunnelHandle) == 8);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_BUILD_RESULT, ModeEnabled) == 16);
C_ASSERT(sizeof(USB4HR_TUNNEL_TEARDOWN_REQUEST) == 8);
C_ASSERT(sizeof(USB4HR_TUNNEL_RESTORE_REQUEST) == 16);
C_ASSERT(FIELD_OFFSET(USB4HR_TUNNEL_RESTORE_REQUEST, NrdMaxLaneCount) == 9);
C_ASSERT(sizeof(USB4HR_TUNNEL_STATUS_OUTPUT) == 4);

C_ASSERT(sizeof(USB4HR_INTER_DOMAIN_SEND_INPUT) == 24);
C_ASSERT(sizeof(USB4HR_INTER_DOMAIN_PACKET_OUTPUT) == 264);
C_ASSERT(FIELD_OFFSET(USB4HR_INTER_DOMAIN_PACKET_OUTPUT, Payload) == 16);
C_ASSERT(sizeof(USB4HR_DP_BANDWIDTH_INPUT) == 16);
C_ASSERT(sizeof(USB4HR_DOMAIN_POLICY_INPUT) == 48);
C_ASSERT(sizeof(USB4HR_TMU_REQUIREMENT_INPUT) == 16);
C_ASSERT(sizeof(USB4HR_RESET_INPUT) == 8);

C_ASSERT(sizeof(USB4HR_PARENT_INTERFACE) == 0x90);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, DomainId) == 0x20);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, DpBwModeCmId) == 0x24);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, DomainUuid) == 0x28);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, TopologyId) == 0x38);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, CanBondLanes) == 0x48);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, RouterHandle) == 0x50);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, DomainRootPdo) == 0x58);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, TunnelPermissions) == 0x60);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, DomainFlags) == 0x68);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, IsParentConnectionLive) == 0x70);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, CmVersionLimit) == 0x78);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, HostInterfaceVersion) == 0x79);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, EnhancedUniTmuSupported) == 0x7A);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, IsAsymmetricSupported) == 0x80);
C_ASSERT(FIELD_OFFSET(USB4HR_PARENT_INTERFACE, AgreeEnumerationDeadline) == 0x88);

C_ASSERT(sizeof(USB4HR_HARDWARE_SERVICES) == 0x78);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, AllocateRouterHandle) == 0x20);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, DestroyRouterHandle) == 0x28);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, IssueAdapterHandles) == 0x30);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, RevokeAdapterHandle) == 0x38);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, ReportRouterFamily) == 0x40);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, PurgeEventQueues) == 0x48);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, RootRouterStarted) == 0x50);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, ReportUpstreamLinkBandwidth) == 0x58);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, ReadDpBandwidthGrant) == 0x60);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, PinDomainForDpAltMode) == 0x68);
C_ASSERT(FIELD_OFFSET(USB4HR_HARDWARE_SERVICES, UnpinDomainForDpAltMode) == 0x70);
#endif
