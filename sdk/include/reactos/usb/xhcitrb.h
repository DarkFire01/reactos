/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Resource definitions
 * COPYRIGHT:       Copyright 2017 Rama Teja Gampa <ramateja.g@gmail.com>
 *                  Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Transfer TRBs IDs ******************************************************************************/

#define NORMAL          1
#define SETUP_STAGE     2
#define DATA_STAGE      3
#define STATUS_STAGE    4
#define ISOCH           5
#define LINK            6  // BOTH TRANSFER AND COMMAND TRB TYPE
#define EVENT_DATA      7
#define NO_OP           8

/* Command TRB IDs ********************************************************************************/

#define ENABLE_SLOT_COMMAND             9
#define DISABLE_SLOT_COMMAND            10
#define ADDRESS_DEVICE_COMMAND          11
#define CONFIGURE_ENDPOINT_COMMAND      12
#define EVALUATE_CONTEXT_COMMAND        13
#define RESET_ENDPOINT_COMMAND          14
#define STOP_ENDPOINT_COMMAND           15
#define SET_TR_DEQUEUE_COMMAND          16
#define RESET_DEVICE_COMMAND            17
#define FORCE_EVENT_COMMMAND            18
#define NEGOTIATE_BANDWIDTH_COMMAND     19
#define SET_LATENCY_TOLERANCE_COMMAND   20
#define GET_PORT_BANDWIDTH_COMMAND      21
#define FORCE_HEADER_COMMAND            22
#define NO_OP_COMMAND                   23

/* Event TRB IDs **********************************************************************************/

#define TRANSFER_EVENT                  32
#define COMMAND_COMPLETION_EVENT        33
#define PORT_STATUS_CHANGE_EVENT        34
#define BANDWIDTH_RESET_REQUEST_EVENT   35
#define DOORBELL_EVENT                  36
#define HOST_CONTROLLER_EVENT           37
#define DEVICE_NOTIFICATION_EVENT       38
#define MF_INDEX_WARP_EVENT             39

/* TRB Completion Codes ***************************************************************************/

#define INVALID                     0
#define SUCCESS                     1
#define DATA_BUFFER_ERROR           2
#define BABBLLE_DETECTED_ERROR      3
#define USB_TRNASACTION_ERROR       4
#define TRB_ERROR                   5
#define STALL_ERROR                 6
#define RESOURCE_ERROR              7
#define BANDWIDTH_ERROR             8
#define NO_SLOTS_AVAILABLE_ERROR    9
#define INVALID_STREAM_TYPE_ERROR   10
#define SLOT_NOT_ENABLED_ERROR      11
#define ENDPOINT_NOT_ENABLED_ERROR  12
#define SHORT_PACKET                13
#define RING_UNDERRUN               14
#define RING_OVERRUN                15
#define VF_EVENT_RING_FULL_ERROR    16
#define PARAMETER_ERROR             17
#define BANDWIDTH_OVERRUN_ERROR     18
#define CONTEXT_STATE_ERROR         19
#define NO_PING_RESPONSE_ERROR      20
#define EVENT_RING_FULL_ERROR       21
#define INCOMPATIBLE_DEVICE_ERROR   22
#define MISSED_SERVICE_ERROR        23
#define COMMAND_RING_STOPPED        24
#define COMMAND_ABORTED             25
#define STOPPED                     26
#define STOPPED_LENGTH_INVALID      27
#define STOPPED_SHORT_PACKET        28
#define MAX_EXIT_LATENCY_ERROR      29
#define ISOCH_BUFFER_OVERRUN        31
#define EVENT_LOST_ERROR            32
#define UNDEFINED_ERROR             33
#define INVALID_STREAM_ID_ERROR     34
#define SECONDARY_BANDWIDTH_ERROR   35
#define SPLIT_TRNASACTION_ERROR     36

/* Transfer TRBs *************************************************************************************/

/* 6.4.1.1 - Normal Transfer TRB */
typedef struct _XHCI_NORMAL_TRANSFER_TRB
{
    struct
    {
        ULONG DataBufPtrLow                  : 32;
    };
    struct
    {
        ULONG DataBufPtrHigh                 : 32;
    };
    struct
    {
        ULONG TrbTransferLen                 : 17;
        ULONG TDSize                         : 5;
        ULONG InterruptTarget                : 10;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG ENT                            : 1;
        ULONG ISP                            : 1;
        ULONG NS                             : 1;
        ULONG CH                             : 1;
        ULONG IOC                            : 1;
        ULONG IDT                            : 1;
        ULONG RsvdZ1                         : 2;
        ULONG BEI                            : 1;
        ULONG TRBType                        : 6;
        ULONG RsvdZ2                         : 16;
    };
} XHCI_NORMAL_TRANSFER_TRB, *PXHCI_NORMAL_TRANSFER_TRB;
C_ASSERT(sizeof(XHCI_NORMAL_TRANSFER_TRB) == 16);

/* 6.4.1.2.1 - Setup Stage TRB */
typedef struct _XHCI_CONTROL_SETUP_TRB 
{
    struct 
    {
        ULONG bmRequestType             : 8;
        ULONG bRequest                  : 8;
        ULONG wValue                    : 16;
    };
    struct 
    {
        ULONG wIndex                    : 16;
        ULONG wLength                   : 16;
    };
    struct 
    {
        ULONG TRBTransferLength         : 17;
        ULONG RsvdZ                     : 5;
        ULONG InterrupterTarget         : 10;
    };
    struct 
    {
        ULONG CycleBit                  : 1;
        ULONG RsvdZ1                    : 4;
        ULONG InterruptOnCompletion     : 1;
        ULONG ImmediateData             : 1;
        ULONG RsvdZ2                    : 3;
        ULONG TRBType                   : 6;
        ULONG TransferType              : 2;
        ULONG RsvdZ3                    : 14;
    };
} XHCI_CONTROL_SETUP_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_SETUP_TRB) == 16);

/* 6.4.1.2.2 - Data Stage TRB */
typedef struct _XHCI_CONTROL_DATA_TRB 
{
    struct 
    {
        ULONG DataBufferPointerLo       : 32;
    };
    struct 
    {
        ULONG DataBufferPointerHi       : 32;
    };
    struct 
    {
        ULONG TRBTransferLength         : 17;
        ULONG TDSize                    : 5;
        ULONG InterrupterTarget         : 10;
    };
    struct 
    {
        ULONG CycleBit                  : 1;
        ULONG EvaluateNextTRB           : 1;
        ULONG InterruptOnShortPacket    : 1;
        ULONG NoSnoop                   : 1;
        ULONG ChainBit                  : 1;
        ULONG InterruptOnCompletion     : 1;
        ULONG ImmediateData             : 1;
        ULONG RsvdZ1                    : 2;
        ULONG TRBType                   : 6;
        ULONG Direction                 : 1;
        ULONG RsvdZ2                    : 15;
    };
} XHCI_CONTROL_DATA_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_DATA_TRB) == 16);

/* 6.4.1.2.3 - Status Stage TRB */
typedef struct _XHCI_CONTROL_STATUS_TRB 
{
    struct 
    {
        ULONG RsvdZ1                    : 32;
    };
    struct 
    {
        ULONG RsvdZ2                    : 32;
    };
    struct 
    {
        ULONG RsvdZ                     : 22;
        ULONG InterrupterTarget         : 10;
    };
    struct 
    {
        ULONG CycleBit                  : 1;
        ULONG EvaluateNextTRB           : 1;
        ULONG ChainBit                  : 2;
        ULONG InterruptOnCompletion     : 1;
        ULONG RsvdZ3                    : 4;
        ULONG TRBType                   : 6;
        ULONG Direction                 : 1;
        ULONG RsvdZ4                    : 15;
    };
} XHCI_CONTROL_STATUS_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_STATUS_TRB) == 16);

/* Event TRBs ****************************************************************************************/

/* 6.4.2.1 - Transfer Event TRB */
typedef struct _XHCI_EVENT_TRANSFER_TRB
{
    struct
    {
        ULONG TRBPtrLo                  : 32;
    };
    struct
    {
        ULONG TRBPtrHi                  : 32;
    };
    struct
    {
        ULONG TrbTransferLen            : 24;
        ULONG CompleteionCode           : 8;
    };
    struct
    {
        ULONG CycleBit                  : 1;
        ULONG RsvdZ1                    : 1;
        ULONG EventData                 : 1;
        ULONG RsvdZ2                    : 7;
        ULONG TrbType                   : 6;
        ULONG EndpointID                : 6;
        ULONG RsvdZ3                    : 3;
        ULONG SlotID                    : 7;
    };
} XHCI_EVENT_TRANSFER_TRB;
C_ASSERT(sizeof(XHCI_EVENT_TRANSFER_TRB) == 16);

/* 6.4.2.2 - Command Completion Event TRB */
typedef struct _XHCI_EVENT_COMMAND_COMPLETION_TRB
{
    struct 
    {
        ULONG RsvdZ1                : 4;
        ULONG CommandTRBPointerLo   : 28;
    };
    ULONG CommandTRBPointerHi;
    struct 
    {
        ULONG CommandCompletionParam     : 24;
        ULONG CompletionCode             : 8;
    };
    struct 
    {
        ULONG CycleBit          : 1;
        ULONG RsvdZ2            : 9;
        ULONG TRBType           : 6;
        ULONG VFID              : 8;
        ULONG SlotID            : 8;
    };
} XHCI_EVENT_COMMAND_COMPLETION_TRB;
C_ASSERT(sizeof(XHCI_EVENT_COMMAND_COMPLETION_TRB) == 16);

/* 6.4.2.3 - Port Status Change Event TRB */
typedef struct _XHCI_EVENT_PORT_STATUS_CHANGE_TRB
{
    struct 
    {
        ULONG RsvdZ1                : 24;
        ULONG PortID                : 8;
    };
    struct 
    {
        ULONG RsvdZ2                : 32;
    };
    struct 
    {
        ULONG RsvdZ3                     : 24;
        ULONG CompletionCode             : 8;
    };
    struct 
    {
        ULONG CycleBit          : 1;
        ULONG RsvdZ4            : 9;
        ULONG TRBType           : 6;
        ULONG RsvdZ5            : 16;
    };
} XHCI_EVENT_PORT_STATUS_CHANGE_TRB;
C_ASSERT(sizeof(XHCI_EVENT_PORT_STATUS_CHANGE_TRB) == 16);

/* 6.4.2.4 - Bandwith Request Event TRB */
typedef struct _XHCI_BANDWITH_REQUEST_TRB 
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 24;
        ULONG CompletetionCode               : 8;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG TRBType                        : 6;
        ULONG RsvdZ5                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_BANDWITH_REQUEST_TRB;
C_ASSERT(sizeof(XHCI_BANDWITH_REQUEST_TRB) == 16);

/* 6.4.2.5 - Doorbell Request Event TRB */
typedef struct _XHCI_EVENT_DOORBELL_REQUEST_TRB
{
    struct
    {
        ULONG RsvdZ1                    : 32;
    };
    struct
    {
        ULONG RsvdZ2                    : 32;
    };
    struct
    {
        ULONG RsvdZ3                    : 32;
    };
    struct
    {
        ULONG RsvdZ4                    : 32;
    };
} XHCI_EVENT_DOORBELL_REQUEST_TRB;
C_ASSERT(sizeof(XHCI_EVENT_DOORBELL_REQUEST_TRB) == 16);

/* 6.4.2.6 - Host Controller Event Event TRB - UNIMPLEMENTED */
typedef struct _XHCI_EVENT_HOST_CONTROLLER_TRB
{
    struct
    {
        ULONG RsvdZ1                    : 32;
    };
    struct
    {
        ULONG RsvdZ2                    : 32;
    };
    struct
    {
        ULONG RsvdZ3                    : 32;
    };
    struct
    {
        ULONG RsvdZ4                    : 32;
    };
} XHCI_EVENT_HOST_CONTROLLER_TRB;
C_ASSERT(sizeof(XHCI_EVENT_HOST_CONTROLLER_TRB) == 16);

/* 6.4.2.7 - Device Notification Event TRB - UNIMPLEMENTED */
typedef struct _XHCI_EVENT_DEVICE_NOTIFICATION_TRB
{
    struct
    {
        ULONG RsvdZ1                    : 32;
    };
    struct
    {
        ULONG RsvdZ2                    : 32;
    };
    struct
    {
        ULONG RsvdZ3                    : 32;
    };
    struct
    {
        ULONG RsvdZ4                    : 32;
    };
} XHCI_EVENT_DEVICE_NOTIFICATION_TRB;
C_ASSERT(sizeof(XHCI_EVENT_DEVICE_NOTIFICATION_TRB) == 16);

/* 6.4.2.8 - MFINDEX Wrap Event TRB - UNIMPLEMENTED */
typedef struct _XHCI_EVENT_MFINDEX_WRAP_TRB
{
    struct
    {
        ULONG RsvdZ1                    : 32;
    };
    struct
    {
        ULONG RsvdZ2                    : 32;
    };
    struct
    {
        ULONG RsvdZ3                    : 32;
    };
    struct
    {
        ULONG RsvdZ4                    : 32;
    };
} XHCI_EVENT_MFINDEX_WRAP_TRB;
C_ASSERT(sizeof(XHCI_EVENT_MFINDEX_WRAP_TRB) == 16);

/* Command TRBs **************************************************************************************/

/* 6.4.3.1 - No Operation Command TRB */
typedef struct _XHCI_COMMAND_NO_OP_TRB 
{
    ULONG RsvdZ1;
    ULONG RsvdZ2;
    ULONG RsvdZ3;
    struct
    {
        ULONG CycleBit                  : 1;
        ULONG RsvdZ4                    : 9;
        ULONG TRBType                   : 6;
        ULONG RsvdZ5                    : 16;
    };
} XHCI_COMMAND_NO_OP_TRB;
C_ASSERT(sizeof(XHCI_COMMAND_NO_OP_TRB) == 16);

/* 6.4.3.2 - Enable Slot TRB */
typedef struct _XHCI_ENABLE_SLOT_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 10;
        ULONG TRBType                        : 6;
        ULONG SlotType                       : 5;
        ULONG RsvdZ5                         : 10;
    };
} XHCI_ENABLE_SLOT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_ENABLE_SLOT_COMMAND_TRB) == 16);

/* 6.4.3.3 - Disable Slot TRB */
typedef struct _XHCI_DISABLE_SLOT_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG TRBType                        : 6;
        ULONG RsvdZ5                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_DISABLE_SLOT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_DISABLE_SLOT_COMMAND_TRB) == 16);

/* 6.4.3.4 - Address Device TRB */
typedef struct _XHCI_ADDRESS_DEVICE_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG InputContextPtrLow             : 27;
    };
    struct 
    {
        ULONG InputContextPtrHigh            : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ3                         : 8;
        ULONG Deconfigure                    : 1;
        ULONG TRBType                        : 6;
        ULONG RsvdZ4                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_ADDRESS_DEVICE_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_ADDRESS_DEVICE_COMMAND_TRB) == 16);

/* 6.4.3.5 - Configure Endpoint Command TRB */
typedef struct _XHCI_CONFIGURE_ENDPOINT_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG InputContextPtrLow             : 27;
    };
    struct 
    {
        ULONG InputContextPtrHigh            : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ3                         : 8;
        ULONG Deconfigure                    : 1;
        ULONG TRBType                        : 6;
        ULONG RsvdZ4                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_CONFIGURE_ENDPOINT_TRB;
C_ASSERT(sizeof(XHCI_CONFIGURE_ENDPOINT_TRB) == 16);

/* 6.4.3.6 - Evaluate Context Command TRB */
typedef struct _XHCI_EVALUATE_CONTEXT_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG InputContextPtrLow             : 27;
    };
    struct 
    {
        ULONG InputContextPtrHigh            : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ3                         : 8;
        ULONG BSR                            : 1;
        ULONG TRBType                        : 6;
        ULONG RsvdZ4                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_EVALUATE_CONTEXT_TRB;
C_ASSERT(sizeof(XHCI_EVALUATE_CONTEXT_TRB) == 16);

/* 6.4.3.7 - Reset Endpoint Command TRB */
typedef struct _XHCI_RESET_ENDPOINT_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 8;
        ULONG TransferStatePreserve          : 1;
        ULONG TRBType                        : 6;
        ULONG EndpointID                     : 5;
        ULONG RsvdZ5                         : 3;
        ULONG SlotID                         : 8;
    };
} XHCI_RESET_ENDPOINT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_RESET_ENDPOINT_COMMAND_TRB) == 16);

/* 6.4.3.8 - Stop Endpoint Command TRB  */
typedef struct _XHCI_STOP_ENDPOINT_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 32;
    };
    struct
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG TRBType                        : 6;
        ULONG EndpointID                     : 5;
        ULONG RsvdZ5                         : 2;
        ULONG Suspend                        : 1;
        ULONG SlotID                         : 8;
    };
} XHCI_STOP_ENDPOINT_COMMAND_TRB, *PXHCI_STOP_ENDPOINT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_STOP_ENDPOINT_COMMAND_TRB) == 16);

/* 6.4.3.9 - Set TR Dequeue Pointer Command TRB - UNIMPLEMENTED */
typedef struct _XHCI_SET_DEQUEUE_PTR_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG InputContextPtrHigh            : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG CycleBit                       : 32;
    };
} XHCI_SET_DEQUEUE_PTR_TRB;
C_ASSERT(sizeof(XHCI_SET_DEQUEUE_PTR_TRB) == 16);

/* 6.4.3.10 - Reset Device Command TRB */
typedef struct _XHCI_RESET_DEVICE_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct 
    {
        ULONG RsvdZ3                         : 32;
    };
    struct
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG RsvdZ5                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_RESET_DEVICE_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_RESET_DEVICE_COMMAND_TRB) == 16);

/* 6.4.3.11 - Force Event Command TRB - UNIMPLEMENTED - OPTIONAL*/
/* 6.4.3.12 - Negotiate Bandwith Command TRB - UNIMPLEMENTED - OPTIONAL */
/* 6.4.3.13 - Set Latency Tolerance Command TRB - UNIMPLEMENTED - OPTIONAL */

/* 6.4.3.14 - Get Port Bandwidth Command TRB */
typedef struct _XHCI_GET_PORT_BANDWIDTH_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG PortBandwidthContextPtrLo      : 28;
    };
    struct 
    {
        ULONG PortBandwidthContextPtrHi      : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    struct
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ3                         : 8;
        ULONG TrbType                        : 6;
        ULONG DevSpeed                       : 4;
        ULONG RsvdZ4                         : 4;
        ULONG HubSlotID                      : 8;
    };
} XHCI_GET_PORT_BANDWIDTH_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_GET_PORT_BANDWIDTH_COMMAND_TRB) == 16);

/* 6.4.3.15 - Force Header Command TRB */
typedef struct _XHCI_FORCE_HEADER_COMMAND_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 5;
        ULONG HeaderInfoLo                   : 27;
    };
    struct 
    {
        ULONG HeaderInfoMid                  : 32;
    };
    struct 
    {
        ULONG HeaderInfoHi                  : 32;
    };
    struct
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ2                         : 8;
        ULONG TrbType                        : 6;
        ULONG RsvdZ3                         : 8;
        ULONG RootHubPortNum                 : 8;
    };
} XHCI_FORCE_HEADER_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_FORCE_HEADER_COMMAND_TRB) == 16);

/* 6.4.3.16 - Get Extended Property Command TRB - UNIMPLEMENTED - OPTIONAL */
/* 6.4.3.17 - Set Extended Property Command TRB - UNIMPLEMENTED - OPTIONAL */

/* Other TRBs ****************************************************************************************/

/* 6.4.4.1 - Link TRB */
typedef struct _XHCI_LINK_TRB
{
    struct 
    {
        ULONG RsvdZ1                     : 4;
        ULONG RingSegmentPointerLo       : 28;
    };
    struct 
    {
        ULONG RingSegmentPointerHi       : 32;
    };
    struct 
    {
        ULONG RsvdZ2                     : 22;
        ULONG InterrupterTarget          : 10;
    };
    struct 
    {
        ULONG CycleBit                   : 1;
        ULONG ToggleCycle                : 1;
        ULONG RsvdZ3                     : 2;
        ULONG ChainBit                   : 1;
        ULONG InterruptOnCompletion      : 1;
        ULONG RsvdZ4                     : 4;
        ULONG TRBType                    : 6;
        ULONG RsvdZ5                     : 16;
    };
} XHCI_LINK_TRB;
C_ASSERT(sizeof(XHCI_LINK_TRB) == 16);

/* 6.4.4.2 - Event Data TRB */
typedef struct _XHCI_EVENT_DATA_TRB
{
    struct 
    {
        ULONG EventDataLo                : 32;
    };
    struct 
    {
        ULONG EventDataHi                : 32;
    };
    struct 
    {
        ULONG RsvdZ1                     : 22;
        ULONG InterrupterTarget          : 10;
    };
    struct 
    {
        ULONG CycleBit                   : 1;
        ULONG EvaluateNextTrb            : 1;
        ULONG RsvdZ2                     : 2;
        ULONG ChainBit                   : 1;
        ULONG InterruptOnCompletion      : 1;
        ULONG RsvdZ3                     : 3;
        ULONG BlockEventInterrupt        : 4;
        ULONG TRBType                    : 6;
        ULONG RsvdZ4                     : 11;
    };
} XHCI_EVENT_DATA_TRB;
C_ASSERT(sizeof(XHCI_EVENT_DATA_TRB) == 16);
