/*
 * usb200.h
 *
 * This file is part of the ReactOS PSDK package.
 *
 * Contributors:
 *   Magnus Olsen.
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#pragma once

/* Helper macro to enable gcc's extension. */
#ifndef __GNU_EXTENSION
#ifdef __GNUC__
#define __GNU_EXTENSION __extension__
#else
#define __GNU_EXTENSION
#endif
#endif

#include "usb100.h"

#include <pshpack1.h>

typedef enum _USB_DEVICE_TYPE {
  Usb11Device = 0,
  Usb20Device
} USB_DEVICE_TYPE;

typedef enum _USB_DEVICE_SPEED {
  UsbLowSpeed = 0,
  UsbFullSpeed,
  UsbHighSpeed,
  UsbSuperSpeed
} USB_DEVICE_SPEED;

#define USB_PORT_STATUS_CONNECT                       0x0001
#define USB_PORT_STATUS_ENABLE                        0x0002
#define USB_PORT_STATUS_SUSPEND                       0x0004
#define USB_PORT_STATUS_OVER_CURRENT                  0x0008
#define USB_PORT_STATUS_RESET                         0x0010
#define USB_PORT_STATUS_POWER                         0x0100
#define USB_PORT_STATUS_LOW_SPEED                     0x0200
#define USB_PORT_STATUS_HIGH_SPEED                    0x0400


typedef union _BM_REQUEST_TYPE {
#ifdef __cplusplus
  struct {
#else
  struct _BM {
#endif
    UCHAR Recipient:2;
    UCHAR Reserved:3;
    UCHAR Type:2;
    UCHAR Dir:1;
  };
  UCHAR B;
} BM_REQUEST_TYPE, *PBM_REQUEST_TYPE;

typedef struct _USB_DEFAULT_PIPE_SETUP_PACKET {
  BM_REQUEST_TYPE bmRequestType;
  UCHAR bRequest;
  union _wValue {
    __GNU_EXTENSION struct {
      UCHAR LowByte;
      UCHAR HiByte;
    };
    USHORT W;
  } wValue;
  union _wIndex {
    __GNU_EXTENSION struct {
      UCHAR LowByte;
      UCHAR HiByte;
    };
    USHORT W;
  } wIndex;
  USHORT wLength;
} USB_DEFAULT_PIPE_SETUP_PACKET, *PUSB_DEFAULT_PIPE_SETUP_PACKET;

C_ASSERT(sizeof(USB_DEFAULT_PIPE_SETUP_PACKET) == 8);

#define USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE          0x06
#define USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE 0x07

typedef struct _USB_DEVICE_QUALIFIER_DESCRIPTOR {
  UCHAR bLength;
  UCHAR bDescriptorType;
  USHORT bcdUSB;
  UCHAR bDeviceClass;
  UCHAR bDeviceSubClass;
  UCHAR bDeviceProtocol;
  UCHAR bMaxPacketSize0;
  UCHAR bNumConfigurations;
  UCHAR bReserved;
} USB_DEVICE_QUALIFIER_DESCRIPTOR, *PUSB_DEVICE_QUALIFIER_DESCRIPTOR;

typedef union _USB_HIGH_SPEED_MAXPACKET {
  __GNU_EXTENSION struct _MP {
    USHORT MaxPacket:11;
    USHORT HSmux:2;
    USHORT Reserved:3;
  };
  USHORT us;
} USB_HIGH_SPEED_MAXPACKET, *PUSB_HIGH_SPEED_MAXPACKET;

#define USB_INTERFACE_ASSOCIATION_DESCRIPTOR_TYPE     0x0B

typedef struct _USB_INTERFACE_ASSOCIATION_DESCRIPTOR {
  UCHAR bLength;
  UCHAR bDescriptorType;
  UCHAR bFirstInterface;
  UCHAR bInterfaceCount;
  UCHAR bFunctionClass;
  UCHAR bFunctionSubClass;
  UCHAR bFunctionProtocol;
  UCHAR iFunction;
} USB_INTERFACE_ASSOCIATION_DESCRIPTOR, *PUSB_INTERFACE_ASSOCIATION_DESCRIPTOR;

typedef union _USB_20_PORT_STATUS {
  USHORT AsUshort16;
  struct {
    USHORT CurrentConnectStatus:1;
    USHORT PortEnabledDisabled:1;
    USHORT Suspend:1;
    USHORT OverCurrent:1;
    USHORT Reset:1;
    USHORT L1:1;
    USHORT Reserved0:2;
    USHORT PortPower:1;
    USHORT LowSpeedDeviceAttached:1;
    USHORT HighSpeedDeviceAttached:1;
    USHORT PortTestMode:1;
    USHORT PortIndicatorControl:1;
    USHORT Reserved1:3;
  };
} USB_20_PORT_STATUS, *PUSB_20_PORT_STATUS;

C_ASSERT(sizeof(USB_20_PORT_STATUS) == sizeof(USHORT));

#define USB_PORT_STATUS_CONNECT       0x0001
#define USB_PORT_STATUS_ENABLE        0x0002
#define USB_PORT_STATUS_SUSPEND       0x0004
#define USB_PORT_STATUS_OVER_CURRENT  0x0008
#define USB_PORT_STATUS_RESET         0x0010
#define USB_PORT_STATUS_POWER         0x0100
#define USB_PORT_STATUS_LOW_SPEED     0x0200
#define USB_PORT_STATUS_HIGH_SPEED    0x0400

typedef union _USB_20_PORT_CHANGE {
  USHORT AsUshort16;
  struct {
    USHORT ConnectStatusChange:1;
    USHORT PortEnableDisableChange:1;
    USHORT SuspendChange:1;
    USHORT OverCurrentIndicatorChange:1;
    USHORT ResetChange:1;
    USHORT Reserved2:11;
  };
} USB_20_PORT_CHANGE, *PUSB_20_PORT_CHANGE;

C_ASSERT(sizeof(USB_20_PORT_CHANGE) == sizeof(USHORT));

typedef union _USB_30_PORT_STATUS {
  USHORT AsUshort16;
  struct {
    USHORT CurrentConnectStatus:1;
    USHORT PortEnabledDisabled:1;
    USHORT Reserved0:1;
    USHORT OverCurrent:1;
    USHORT Reset:1;
    USHORT PortLinkState:4;
    USHORT PortPower:1;
    USHORT NegotiatedDeviceSpeed:3;
    USHORT Reserved1:3;
  };
} USB_30_PORT_STATUS, *PUSB_30_PORT_STATUS;

C_ASSERT(sizeof(USB_30_PORT_STATUS) == sizeof(USHORT));

#define PORT_LINK_STATE_U0               0
#define PORT_LINK_STATE_U1               1
#define PORT_LINK_STATE_U2               2
#define PORT_LINK_STATE_U3               3
#define PORT_LINK_STATE_DISABLED         4
#define PORT_LINK_STATE_RX_DETECT        5
#define PORT_LINK_STATE_INACTIVE         6
#define PORT_LINK_STATE_POLLING          7
#define PORT_LINK_STATE_RECOVERY         8
#define PORT_LINK_STATE_HOT_RESET        9
#define PORT_LINK_STATE_COMPLIANCE_MODE  10
#define PORT_LINK_STATE_LOOPBACK         11
#define PORT_LINK_STATE_TEST_MODE        11 // xHCI-specific, replacing LOOPBACK

typedef union _USB_30_PORT_CHANGE {
  USHORT AsUshort16;
  struct {
    USHORT ConnectStatusChange :1;
    USHORT Reserved2 :2;
    USHORT OverCurrentIndicatorChange :1;
    USHORT ResetChange :1;
    USHORT BHResetChange :1;
    USHORT PortLinkStateChange :1;
    USHORT PortConfigErrorChange :1;
    USHORT Reserved3 :8;
  };
} USB_30_PORT_CHANGE, *PUSB_30_PORT_CHANGE;

C_ASSERT(sizeof(USB_30_PORT_CHANGE) == sizeof(USHORT));

typedef union _USB_PORT_STATUS {
  USHORT AsUshort16;
  USB_20_PORT_STATUS Usb20PortStatus;
  USB_30_PORT_STATUS Usb30PortStatus;
} USB_PORT_STATUS, *PUSB_PORT_STATUS;

C_ASSERT(sizeof(USB_PORT_STATUS) == sizeof(USHORT));

typedef union _USB_PORT_CHANGE {
  USHORT AsUshort16;
  USB_20_PORT_CHANGE Usb20PortChange;
  USB_30_PORT_CHANGE Usb30PortChange;
} USB_PORT_CHANGE, *PUSB_PORT_CHANGE;

C_ASSERT(sizeof(USB_PORT_CHANGE) == sizeof(USHORT));

typedef union _USB_PORT_STATUS_AND_CHANGE {
  ULONG AsUlong32;
  struct {
    USB_PORT_STATUS PortStatus;
    USB_PORT_CHANGE PortChange;
  };
} USB_PORT_STATUS_AND_CHANGE, *PUSB_PORT_STATUS_AND_CHANGE;

C_ASSERT(sizeof(USB_PORT_STATUS_AND_CHANGE) == sizeof(ULONG));

typedef union _USB_HUB_STATUS {
  USHORT AsUshort16;
  struct {
    USHORT LocalPowerLost:1;
    USHORT OverCurrent:1;
    USHORT Reserved:14;
  };
} USB_HUB_STATUS, *PUSB_HUB_STATUS;

C_ASSERT(sizeof(USB_HUB_STATUS) == sizeof(USHORT));

typedef union _USB_HUB_CHANGE {
  USHORT AsUshort16;
  struct {
    USHORT LocalPowerChange:1;
    USHORT OverCurrentChange:1;
    USHORT Reserved:14;
  };
} USB_HUB_CHANGE, *PUSB_HUB_CHANGE;

C_ASSERT(sizeof(USB_HUB_CHANGE) == sizeof(USHORT));

typedef union _USB_HUB_STATUS_AND_CHANGE {
  ULONG AsUlong32;
  struct {
    USB_HUB_STATUS HubStatus;
    USB_HUB_CHANGE HubChange;
  };
} USB_HUB_STATUS_AND_CHANGE, *PUSB_HUB_STATUS_AND_CHANGE;

C_ASSERT(sizeof(USB_HUB_STATUS_AND_CHANGE) == sizeof(ULONG));

#define USB_20_HUB_DESCRIPTOR_TYPE  0x29
#define USB_30_HUB_DESCRIPTOR_TYPE  0x2A

#define USB_REQUEST_CLEAR_TT_BUFFER     0x08
#define USB_REQUEST_RESET_TT            0x09
#define USB_REQUEST_GET_TT_STATE        0x0A
#define USB_REQUEST_STOP_TT             0x0B

#define USB_REQUEST_SET_HUB_DEPTH       0x0C
#define USB_REQUEST_GET_PORT_ERR_COUNT  0x0D

#define USB_DEVICE_CLASS_RESERVED             0x00
#define USB_DEVICE_CLASS_AUDIO                0x01
#define USB_DEVICE_CLASS_COMMUNICATIONS       0x02
#define USB_DEVICE_CLASS_HUMAN_INTERFACE      0x03
#define USB_DEVICE_CLASS_MONITOR              0x04
#define USB_DEVICE_CLASS_PHYSICAL_INTERFACE   0x05
#define USB_DEVICE_CLASS_POWER                0x06
#define USB_DEVICE_CLASS_IMAGE                0x06
#define USB_DEVICE_CLASS_PRINTER              0x07
#define USB_DEVICE_CLASS_STORAGE              0x08
#define USB_DEVICE_CLASS_HUB                  0x09
#define USB_DEVICE_CLASS_CDC_DATA             0x0A
#define USB_DEVICE_CLASS_SMART_CARD           0x0B
#define USB_DEVICE_CLASS_CONTENT_SECURITY     0x0D
#define USB_DEVICE_CLASS_VIDEO                0x0E
#define USB_DEVICE_CLASS_PERSONAL_HEALTHCARE  0x0F
#define USB_DEVICE_CLASS_AUDIO_VIDEO          0x10
#define USB_DEVICE_CLASS_BILLBOARD            0x11
#define USB_DEVICE_CLASS_DIAGNOSTIC_DEVICE    0xDC
#define USB_DEVICE_CLASS_WIRELESS_CONTROLLER  0xE0
#define USB_DEVICE_CLASS_MISCELLANEOUS        0xEF
#define USB_DEVICE_CLASS_APPLICATION_SPECIFIC 0xFE
#define USB_DEVICE_CLASS_VENDOR_SPECIFIC      0xFF

/*
 * USB 3.x descriptors.  usb100.h and usb200.h stop at USB 2.0, so the
 * SuperSpeed additions - the BOS descriptor and the device capabilities it
 * carries, the endpoint companions, the 3.0 hub descriptor and the extended
 * port status - live here.  Layouts follow the USB 3.2 specification and are
 * byte-exact wire formats, so nothing here may be reordered or padded.
 */

#define USB_OTG_DESCRIPTOR_TYPE                                     0x09
#define USB_DEBUG_DESCRIPTOR_TYPE                                   0x0A
#define USB_BOS_DESCRIPTOR_TYPE                                     0x0F
#define USB_DEVICE_CAPABILITY_DESCRIPTOR_TYPE                       0x10
#define USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR_TYPE           0x30
#define USB_SUPERSPEEDPLUS_ISOCH_ENDPOINT_COMPANION_DESCRIPTOR_TYPE 0x31
#define USB_FEATURE_BATTERY_WAKE_MASK           0x28
#define USB_DEVICE_CAPABILITY_WIRELESS_USB                0x01
#define USB_DEVICE_CAPABILITY_USB20_EXTENSION             0x02
#define USB_DEVICE_CAPABILITY_SUPERSPEED_USB              0x03
#define USB_DEVICE_CAPABILITY_CONTAINER_ID                0x04
#define USB_DEVICE_CAPABILITY_PLATFORM                    0x05
#define USB_DEVICE_CAPABILITY_POWER_DELIVERY              0x06
#define USB_DEVICE_CAPABILITY_BATTERY_INFO                0x07
#define USB_DEVICE_CAPABILITY_PD_CONSUMER_PORT            0x08
#define USB_DEVICE_CAPABILITY_PD_PROVIDER_PORT            0x09
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_USB          0x0A
#define USB_DEVICE_CAPABILITY_PRECISION_TIME_MEASUREMENT  0x0B
#define USB_DEVICE_CAPABILITY_BILLBOARD                   0x0D
#define USB_DEVICE_CAPABILITY_FIRMWARE_STATUS             0x11
#define USB_DEVICE_CAPABILITY_USB20_EXTENSION_BMATTRIBUTES_RESERVED_MASK 0xFFFF00E1
#define USB_DEVICE_CAPABILITY_SUPERSPEED_BMATTRIBUTES_RESERVED_MASK 0xFD
#define USB_DEVICE_CAPABILITY_SUPERSPEED_BMATTRIBUTES_LTM_CAPABLE   0x02
#define USB_DEVICE_CAPABILITY_SUPERSPEED_SPEEDS_SUPPORTED_RESERVED_MASK  0xFFF0
#define USB_DEVICE_CAPABILITY_SUPERSPEED_SPEEDS_SUPPORTED_LOW            0x0001
#define USB_DEVICE_CAPABILITY_SUPERSPEED_SPEEDS_SUPPORTED_FULL           0x0002
#define USB_DEVICE_CAPABILITY_SUPERSPEED_SPEEDS_SUPPORTED_HIGH           0x0004
#define USB_DEVICE_CAPABILITY_SUPERSPEED_SPEEDS_SUPPORTED_SUPER          0x0008
#define USB_DEVICE_CAPABILITY_SUPERSPEED_U1_DEVICE_EXIT_MAX_VALUE 0x0A
#define USB_DEVICE_CAPABILITY_SUPERSPEED_U2_DEVICE_EXIT_MAX_VALUE 0x07FF
#define USB_DEVICE_CAPABILITY_MAX_U1_LATENCY    0x0A
#define USB_DEVICE_CAPABILITY_MAX_U2_LATENCY    0x07FF
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_LSE_BPS              0
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_LSE_KBPS             1
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_LSE_MBPS             2
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_LSE_GBPS             3
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_MODE_SYMMETRIC       0
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_MODE_ASYMMETRIC      1
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_DIR_RX               0
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_DIR_TX               1
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_PROTOCOL_SS          0
#define USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED_PROTOCOL_SSP         1
#define USB_ENDPOINT_ADDRESS_MASK                 0x0F
#define USB_ENDPOINT_TYPE_BULK_RESERVED_MASK      0xFC
#define USB_ENDPOINT_TYPE_CONTROL_RESERVED_MASK   0xFC
#define USB_20_ENDPOINT_TYPE_INTERRUPT_RESERVED_MASK   0xFC
#define USB_30_ENDPOINT_TYPE_INTERRUPT_RESERVED_MASK   0xCC
#define USB_ENDPOINT_TYPE_ISOCHRONOUS_RESERVED_MASK    0xC0
#define USB_30_ENDPOINT_TYPE_INTERRUPT_USAGE_MASK         0x30
#define USB_ENDPOINT_TYPE_ISOCHRONOUS_SYNCHRONIZATION_MASK               0x0C
#define USB_ENDPOINT_TYPE_ISOCHRONOUS_USAGE_MASK                            0x30
#define USB_ENDPOINT_SUPERSPEED_BULK_MAX_PACKET_SIZE       1024
#define USB_ENDPOINT_SUPERSPEED_CONTROL_MAX_PACKET_SIZE     512
#define USB_ENDPOINT_SUPERSPEED_ISO_MAX_PACKET_SIZE        1024
#define USB_ENDPOINT_SUPERSPEED_INTERRUPT_MAX_PACKET_SIZE  1024

typedef union _USB_DEVICE_STATUS {
    USHORT  AsUshort16;
    struct {
        USHORT  SelfPowered:1;
        USHORT  RemoteWakeup:1;
        USHORT  U1Enable:1;     // (USB 1.1, USB 2.0 Reserved)
        USHORT  U2Enable:1;     // (USB 1.1, USB 2.0 Reserved)
        USHORT  LtmEnable:1;    // (USB 1.1, USB 2.0 Reserved)
        USHORT  Reserved:11;
    };
} USB_DEVICE_STATUS, *PUSB_DEVICE_STATUS;

typedef struct _USB_BOS_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    USHORT  wTotalLength;
    UCHAR   bNumDeviceCaps;
} USB_BOS_DESCRIPTOR, *PUSB_BOS_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_USB20_EXTENSION_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    union {
        ULONG       AsUlong;
        struct {
            ULONG   Reserved:1;
            ULONG   LPMCapable:1;
            ULONG   BESLAndAlternateHIRDSupported:1;
            ULONG   BaselineBESLValid:1;
            ULONG   DeepBESLValid:1;
            ULONG   Reserved1:3;
            ULONG   BaselineBESL:4;
            ULONG   DeepBESL:4;
            ULONG   Reserved2:16;
        };
    }       bmAttributes;
} USB_DEVICE_CAPABILITY_USB20_EXTENSION_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_USB20_EXTENSION_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_SUPERSPEED_USB_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   bmAttributes;           // needs bitfield definitions
    USHORT  wSpeedsSupported;       // needs bitfield definitions
    UCHAR   bFunctionalitySupport;
    UCHAR   bU1DevExitLat;
    USHORT  wU2DevExitLat;
} USB_DEVICE_CAPABILITY_SUPERSPEED_USB_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_SUPERSPEED_USB_DESCRIPTOR;

typedef union _USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED {
    ULONG   AsUlong32;
    struct {
        ULONG   SublinkSpeedAttrID:4;
        ULONG   LaneSpeedExponent:2;
        ULONG   SublinkTypeMode:1;
        ULONG   SublinkTypeDir:1;
        ULONG   Reserved:6;
        ULONG   LinkProtocol:2;
        ULONG   LaneSpeedMantissa:16;
    };
} USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED, *PUSB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED;

typedef struct _USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_USB_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   bReserved;
    union {
        ULONG        AsUlong;
        struct {
            ULONG    SublinkSpeedAttrCount:5;
            ULONG    SublinkSpeedIDCount:4;
            ULONG    Reserved:23;
        };
    }       bmAttributes;
    union {
        USHORT       AsUshort;
        struct {
            USHORT   SublinkSpeedAttrID:4;
            USHORT   Reserved:4;
            USHORT   MinRxLaneCount:4;
            USHORT   MinTxLaneCount:4;
        };
    }       wFunctionalitySupport;
    USHORT  wReserved;
    USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED   bmSublinkSpeedAttr[1];
} USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_USB_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_USB_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_CONTAINER_ID_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   bReserved;
    UCHAR   ContainerID[16];
} USB_DEVICE_CAPABILITY_CONTAINER_ID_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_CONTAINER_ID_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_BILLBOARD_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   iAddtionalInfoURL;
    UCHAR   bNumberOfAlternateModes;
    UCHAR   bPreferredAlternateMode;
    union {
        USHORT       AsUshort;
        struct {
            USHORT   VConnPowerNeededForFullFunctionality:3;
            USHORT   Reserved:12;
            USHORT   NoVconnPowerRequired:1;
        };
    } VconnPower;
    UCHAR   bmConfigured[32];
    ULONG   bReserved;
    struct {
        USHORT  wSVID;
        UCHAR   bAlternateMode;
        UCHAR   iAlternateModeSetting;
    } AlternateMode[1];
} USB_DEVICE_CAPABILITY_BILLBOARD_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_BILLBOARD_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
} USB_DEVICE_CAPABILITY_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_DESCRIPTOR;

typedef struct _USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR {
    UCHAR       bLength;
    UCHAR       bDescriptorType;
    UCHAR       bMaxBurst;
    union {
        UCHAR       AsUchar;
        struct {
            UCHAR   MaxStreams:5;
            UCHAR   Reserved1:3;
        } Bulk;
        struct {
            UCHAR   Mult:2;
            UCHAR   Reserved2:5;
            UCHAR   SspCompanion:1;
        } Isochronous;
    }           bmAttributes;
    USHORT      wBytesPerInterval;
} USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR, *PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR;

typedef struct _USB_SUPERSPEEDPLUS_ISOCH_ENDPOINT_COMPANION_DESCRIPTOR {
    UCHAR       bLength;
    UCHAR       bDescriptorType;
    USHORT      wReserved;
    ULONG       dwBytesPerInterval;
} USB_SUPERSPEEDPLUS_ISOCH_ENDPOINT_COMPANION_DESCRIPTOR, *PUSB_SUPERSPEEDPLUS_ISOCH_ENDPOINT_COMPANION_DESCRIPTOR;

typedef struct _USB_30_HUB_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bNumberOfPorts;
    USHORT  wHubCharacteristics;
    UCHAR   bPowerOnToPowerGood;
    UCHAR   bHubControlCurrent;
    UCHAR   bHubHdrDecLat;
    USHORT  wHubDelay;
    USHORT  DeviceRemovable;
} USB_30_HUB_DESCRIPTOR, *PUSB_30_HUB_DESCRIPTOR;

typedef union _USB_PORT_EXT_STATUS {
    ULONG   AsUlong32;
    struct {
        ULONG  RxSublinkSpeedID:4;
        ULONG  TxSublinkSpeedID:4;
        ULONG  RxLaneCount:4;
        ULONG  TxLaneCount:4;
        ULONG  Reserved:16;
    };
} USB_PORT_EXT_STATUS, *PUSB_PORT_EXT_STATUS;

typedef union _USB_PORT_EXT_STATUS_AND_CHANGE {
    ULONG64   AsUlong64;
    struct {
        USB_PORT_STATUS_AND_CHANGE  PortStatusChange; // 0-31
        USB_PORT_EXT_STATUS         PortExtStatus;    // 32-63
    };
} USB_PORT_EXT_STATUS_AND_CHANGE, *PUSB_PORT_EXT_STATUS_AND_CHANGE;

typedef union _USB_HUB_30_PORT_REMOTE_WAKE_MASK {
    UCHAR   AsUchar8;
    struct {
        UCHAR   ConnectRemoteWakeEnable:1;      // 0
        UCHAR   DisconnectRemoteWakeEnable:1;   // 1
        UCHAR   OverCurrentRemoteWakeEnable:1;  // 2
        UCHAR   Reserved0:5;                    // 3-7
    };
} USB_HUB_30_PORT_REMOTE_WAKE_MASK, *PUSB_HUB_30_PORT_REMOTE_WAKE_MASK;


/*
 * The USB 3.x request codes, feature selectors and status selectors that go
 * with the descriptors above.
 */

#define USB_REQUEST_GET_FIRMWARE_STATUS 0x1A
#define USB_REQUEST_SET_FIRMWARE_STATUS 0x1B
#define USB_REQUEST_SET_SEL             0x30
#define USB_REQUEST_ISOCH_DELAY         0x31
#define USB_FEATURE_TEST_MODE                   0x02
#define USB_FEATURE_FUNCTION_SUSPEND            0x00
#define USB_FEATURE_U1_ENABLE                   0x30
#define USB_FEATURE_U2_ENABLE                   0x31
#define USB_FEATURE_LTM_ENABLE                  0x32
#define USB_FEATURE_LDM_ENABLE                  0x35
#define USB_FEATURE_OS_IS_PD_AWARE              0x29
#define USB_FEATURE_POLICY_MODE                 0x2A
#define USB_FEATURE_CHARGING_POLICY             0x36
#define USB_CHARGING_POLICY_DEFAULT             0x00
#define USB_CHARGING_POLICY_ICCHPF              0x01
#define USB_CHARGING_POLICY_ICCLPF              0x02
#define USB_CHARGING_POLICY_NO_POWER            0x03
#define USB_STATUS_PORT_STATUS                  0x00
#define USB_STATUS_PD_STATUS                    0x01
#define USB_STATUS_EXT_PORT_STATUS              0x02
#define USB_SUPERSPEEDPLUS_ISOCHRONOUS_MIN_BYTESPERINTERVAL 0xC001
#define USB_SUPERSPEEDPLUS_ISOCHRONOUS_MAX_BYTESPERINTERVAL 0xFFFFFF
#define USB_REQUEST_GET_STATE           0x02

typedef union _USB_INTERFACE_STATUS {
    USHORT  AsUshort16;
    struct {
        USHORT  RemoteWakeupCapable:1;
        USHORT  RemoteWakeupEnabled:1;
        USHORT  Reserved:14;
    };
} USB_INTERFACE_STATUS, *PUSB_INTERFACE_STATUS;

typedef struct _USB_DEVICE_CAPABILITY_POWER_DELIVERY_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   bReserved;
    union {
        ULONG       AsUlong;
        struct {
            ULONG   Reserved1:1;
            ULONG   BatteryCharging:1;
            ULONG   USBPowerDelivery:1;
            ULONG   Provider:1;
            ULONG   Consumer:1;
            ULONG   ChargingPolicy:1;
            ULONG   TypeCCurrent:1;
            ULONG   Reserved2:1;
            ULONG   ACSupply:1;
            ULONG   Battery:1;
            ULONG   Other:1;
            ULONG   NumBatteries:3;
            ULONG   UsesVbus:1;
            ULONG   Reserved3:17;
        };
    }        bmAttributes;
    USHORT   bmProviderPorts;
    USHORT   bmConsumerPorts;
    USHORT   bcdBCVersion;
    USHORT   bcdPDVersion;
    USHORT   bcdUSBTypeCVersion;
} USB_DEVICE_CAPABILITY_POWER_DELIVERY_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_POWER_DELIVERY_DESCRIPTOR;

typedef struct _USB_DEVICE_CAPABILITY_PLATFORM_DESCRIPTOR {
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    UCHAR   bDevCapabilityType;
    UCHAR   bReserved;
    GUID    PlatformCapabilityUuid;
    UCHAR   CapabililityData[1];
} USB_DEVICE_CAPABILITY_PLATFORM_DESCRIPTOR, *PUSB_DEVICE_CAPABILITY_PLATFORM_DESCRIPTOR;


/* Names the BOS platform capability that carries an MS OS 2.0 descriptor set. */

DEFINE_GUID(GUID_USB_MSOS20_PLATFORM_CAPABILITY_ID,
0xD8DD60DF, 0x4589, 0x4CC7, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F);

#include <poppack.h>
