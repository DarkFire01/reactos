/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         xHCI specification header
 * COPYRIGHT:       Copyright 2017 Rama Teja Gampa <ramateja.g@gmail.com>
 *                  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "xhcitrb.h"

extern USBPORT_REGISTRATION_PACKET RegPacket;

#define XHCI_MAX_ENDPOINTS 32;

/* Transfer TRBs IDs ******************************************************************************/

#define NORMAL                          1
#define SETUP_STAGE                     2
#define DATA_STAGE                      3
#define STATUS_STAGE                    4
#define ISOCH                           5
#define LINK                            6  /* BOTH TRNASFER AND COMMAND TRB TYPE */
#define EVENT_DATA                      7
#define NO_OP                           8

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

#define INVALID                         0
#define SUCCESS                         1
#define DATA_BUFFER_ERROR               2
#define BABBLLE_DETECTED_ERROR          3
#define USB_TRNASACTION_ERROR           4
#define TRB_ERROR                       5
#define STALL_ERROR                     6
#define RESOURCE_ERROR                  7
#define BANDWIDTH_ERROR                 8
#define NO_SLOTS_AVAILABLE_ERROR        9
#define INVALID_STREAM_TYPE_ERROR       10
#define SLOT_NOT_ENABLED_ERROR          11
#define ENDPOINT_NOT_ENABLED_ERROR      12
#define SHORT_PACKET                    13
#define RING_UNDERRUN                   14
#define RING_OVERRUN                    15
#define VF_EVENT_RING_FULL_ERROR        16
#define PARAMETER_ERROR                 17
#define BANDWIDTH_OVERRUN_ERROR         18
#define CONTEXT_STATE_ERROR             19
#define NO_PING_RESPONSE_ERROR          20
#define EVENT_RING_FULL_ERROR           21
#define INCOMPATIBLE_DEVICE_ERROR       22
#define MISSED_SERVICE_ERROR            23
#define COMMAND_RING_STOPPED            24
#define COMMAND_ABORTED                 25
#define STOPPED                         26
#define STOPPED_LENGTH_INVALID          27
#define STOPPED_SHORT_PACKET            28
#define MAX_EXIT_LATENCY_ERROR          29
#define ISOCH_BUFFER_OVERRUN            31
#define EVENT_LOST_ERROR                32
#define UNDEFINED_ERROR                 33
#define INVALID_STREAM_ID_ERROR         34
#define SECONDARY_BANDWIDTH_ERROR       35
#define SPLIT_TRNASACTION_ERROR         36

#define XHCI_FLAGS_CONTROLLER_SUSPEND   0x01

/* Structs ****************************************************************************************/

/* 
 * -> Table 6-1 For Data structs 
 * ->
 */


typedef struct  _XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY 
{
    PHYSICAL_ADDRESS ContextBaseAddr [256];
} XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY, *PXHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY;

/* TRBs - template is on figure 4-13 of the intel xHCI specification ******************************/
typedef struct _XHCI_GENERIC_TRB {
    ULONG Word0;
    ULONG Word1;
    ULONG Word2;
    ULONG Word3;
}XHCI_GENERIC_TRB, *PXHCI_GENERIC_TRB;
C_ASSERT(sizeof(XHCI_GENERIC_TRB) == 16);



/* Enable Slot TRB ********************************************************************************/

/* 6.4.3.2 */
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
} XHCI_ENABLE_SLOT_COMMAND_TRB, *PXHCI_ENABLE_SLOT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_ENABLE_SLOT_COMMAND_TRB) == 16);

/* Configure Endpoint Command TRB *****************************************************************/

/* 6.4.3.5 */
typedef struct _XHCI_CONTROL_ENDPOINT_TRB
{
    /* 00h - 03h */
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG InputContextPtrLow             : 27;
    };
    /* 04h - 07h */
    struct 
    {
        ULONG InputContextPtrHigh            : 32;
    };
    /* 08h - 0Bh */
    struct 
    {
        ULONG RsvdZ2                         : 32;
    };
    /* 0Ch - 0Fh */
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ3                         : 8;
        ULONG Deconfigure                    : 1;
        ULONG TRBType                        : 6;
        ULONG RsvdZ4                         : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_CONTROL_ENDPOINT_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_ENDPOINT_TRB) == 16);

/* Link TRB ***************************************************************************************/

/* 4.11.5.1 */
typedef struct _XHCI_LINK_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG RingSegmentPointerLo           : 28;
    };
    struct 
    {
        ULONG RingSegmentPointerHi           : 32;
    };
    struct 
    {
        ULONG RsvdZ2                         : 22;
        ULONG InterrupterTarget              : 10;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG ToggleCycle                    : 1;
        ULONG RsvdZ3                         : 2;
        ULONG ChainBit                       : 1;
        ULONG InterruptOnCompletion          : 1;
        ULONG RsvdZ4                         : 4;
        ULONG TRBType                        : 6;
        ULONG RsvdZ5                         : 16;
    };
} XHCI_LINK_TRB;
C_ASSERT(sizeof(XHCI_LINK_TRB) == 16);

/* Command TRB ************************************************************************************/

/* 4.11.4 */
typedef struct _XHCI_COMMAND_NO_OP_TRB 
{
    ULONG RsvdZ1;
    ULONG RsvdZ2;
    ULONG RsvdZ3;
    struct
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG TRBType                        : 6;
        ULONG RsvdZ5                         : 16;
    };
} XHCI_COMMAND_NO_OP_TRB;
C_ASSERT(sizeof(XHCI_COMMAND_NO_OP_TRB) == 16);

/* Stop Endpoint Commands TRB ************************************************************************************/

/* 6.4.3.8 */
typedef struct _XHCI_STOP_ENDPOINT_COMMAND_TRB
{
    ULONG RsvdZ1;
    ULONG RsvdZ2;
    ULONG RsvdZ3;
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

typedef union _XHCI_COMMAND_TRB 
{
    XHCI_STOP_ENDPOINT_COMMAND_TRB StopEndpoint;
    XHCI_ADDRESS_DEVICE_COMMAND_TRB AddressDevice;
    XHCI_ENABLE_SLOT_COMMAND_TRB SlotEnable;
    XHCI_COMMAND_NO_OP_TRB NoOperation;
}XHCI_COMMAND_TRB, *PXHCI_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_COMMAND_TRB) == 16);

/* Control Transfer Data TRBs ********************************************************************/

typedef struct _XHCI_CONTROL_SETUP_TRB 
{
    struct 
    {
        ULONG bmRequestType                  : 8;
        ULONG bRequest                       : 8;
        ULONG wValue                         : 16;
    };
    struct
    { 
        ULONG wIndex                         : 16;
        ULONG wLength                        : 16;
    };
    struct
    { 
        ULONG TRBTransferLength              : 17;
        ULONG RsvdZ                          : 5;
        ULONG InterrupterTarget              : 10;
    };
    struct
    { 
        ULONG CycleBit                       : 1;
        ULONG RsvdZ1                         : 4;
        ULONG InterruptOnCompletion          : 1;
        ULONG ImmediateData                  : 1;
        ULONG RsvdZ2                         : 3;
        ULONG TRBType                        : 6;
        ULONG TransferType                   : 2;
        ULONG RsvdZ3                         : 14;
    };   
} XHCI_CONTROL_SETUP_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_SETUP_TRB) == 16);

typedef struct _XHCI_CONTROL_DATA_TRB 
{
    struct 
    {
        ULONG DataBufferPointerLo            : 32;
    };
    struct 
    {
        ULONG DataBufferPointerHi            : 32;
    };
    struct 
    {
        ULONG TRBTransferLength              : 17;
        ULONG TDSize                         : 5;
        ULONG InterrupterTarget              : 10;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG EvaluateNextTRB                : 1;
        ULONG InterruptOnShortPacket         : 1;
        ULONG NoSnoop                        : 1;
        ULONG ChainBit                       : 1;
        ULONG InterruptOnCompletion          : 1;
        ULONG ImmediateData                  : 1;
        ULONG RsvdZ1                         : 2;
        ULONG TRBType                        : 6;
        ULONG Direction                      : 1;
        ULONG RsvdZ2                         : 15;
    };
} XHCI_CONTROL_DATA_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_DATA_TRB) == 16);

typedef struct _XHCI_CONTROL_STATUS_TRB 
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
        ULONG RsvdZ                          : 22;
        ULONG InterrupterTarget              : 10;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG EvaluateNextTRB                : 1;
        ULONG ChainBit                       : 2;
        ULONG InterruptOnCompletion          : 1;
        ULONG RsvdZ3                         : 4;
        ULONG TRBType                        : 6;
        ULONG Direction                      : 1;
        ULONG RsvdZ4                         : 15;
    };
} XHCI_CONTROL_STATUS_TRB;
C_ASSERT(sizeof(XHCI_CONTROL_STATUS_TRB) == 16);

typedef union _XHCI_CONTROL_TRB 
{
    XHCI_CONTROL_SETUP_TRB  SetupTRB;
    XHCI_CONTROL_DATA_TRB   DataTRB;
    XHCI_CONTROL_STATUS_TRB StatusTRB;
    XHCI_GENERIC_TRB    GenericTRB;
} XHCI_CONTROL_TRB, *PXHCI_CONTROL_TRB;  
C_ASSERT(sizeof(XHCI_CONTROL_TRB) == 16);

typedef struct _XHCI_EVENT_COMMAND_COMPLETION_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 4;
        ULONG CommandTRBPointerLo            : 28;
    };
    ULONG CommandTRBPointerHi;
    struct 
    {
        ULONG CommandCompletionParam         : 24;
        ULONG CompletionCode                 : 8;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ2                         : 9;
        ULONG TRBType                        : 6;
        ULONG VFID                           : 8;
        ULONG SlotID                         : 8;
    };
} XHCI_EVENT_COMMAND_COMPLETION_TRB;
C_ASSERT(sizeof(XHCI_EVENT_COMMAND_COMPLETION_TRB) == 16);

typedef struct _XHCI_EVENT_PORT_STATUS_CHANGE_TRB
{
    struct 
    {
        ULONG RsvdZ1                         : 24;
        ULONG PortID                         : 8;
    };
    ULONG RsvdZ2;
    struct 
    {
        ULONG RsvdZ3                         : 24;
        ULONG CompletionCode                 : 8;
    };
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ4                         : 9;
        ULONG TRBType                        : 6;
        ULONG RsvdZ5                         : 16;
    };
} XHCI_EVENT_PORT_STATUS_CHANGE_TRB;
C_ASSERT(sizeof(XHCI_EVENT_PORT_STATUS_CHANGE_TRB) == 16);

typedef struct _XHCI_EVENT_GENERIC_TRB
{
    ULONG Word0;
    ULONG Word1;
    ULONG Word2;
    struct 
    {
        ULONG CycleBit                       : 1;
        ULONG RsvdZ1                         : 9;
        ULONG TRBType                        : 6;
        ULONG RsvdZ2                         : 8;
        ULONG SlotID                         : 8;
    };
}XHCI_EVENT_GENERIC_TRB;
C_ASSERT(sizeof(XHCI_EVENT_GENERIC_TRB) == 16);

typedef union _XHCI_EVENT_TRB 
{
    XHCI_EVENT_COMMAND_COMPLETION_TRB   CommandCompletionTRB;
    XHCI_EVENT_PORT_STATUS_CHANGE_TRB   PortStatusChangeTRB;
    XHCI_EVENT_GENERIC_TRB              EventGenericTRB;
}XHCI_EVENT_TRB, *PXHCI_EVENT_TRB;
C_ASSERT(sizeof(XHCI_EVENT_TRB) == 16);

/* Main Structs ***********************************************************************************/

typedef union _XHCI_LINK_ADDR
{
    struct 
    {
        ULONGLONG RsvdZ1                     : 4;
        ULONGLONG RingSegmentPointerLo       : 28;
        ULONGLONG RingSegmentPointerHi       : 32;
    };
    ULONGLONG AsULONGLONG;
} XHCI_LINK_ADDR;

// 6.5
typedef struct _XHCI_EVENT_RING_SEGMENT_TABLE
{
    ULONGLONG RingSegmentBaseAddr;
    struct 
    {
        ULONGLONG RingSegmentSize            :  16;
        ULONGLONG RsvdZ                      :  48;
    };
} XHCI_EVENT_RING_SEGMENT_TABLE;

typedef union _XHCI_TRB 
{
    XHCI_COMMAND_TRB    CommandTRB;
    XHCI_LINK_TRB       LinkTRB;
    XHCI_CONTROL_TRB    ControlTRB;
    XHCI_EVENT_TRB      EventTRB;
    XHCI_GENERIC_TRB    GenericTRB;
} XHCI_TRB, *PXHCI_TRB;
C_ASSERT(sizeof(XHCI_TRB) == 16);

typedef struct _XHCI_SEGMENT 
{
    XHCI_TRB XhciTrb[256];
    PVOID nextSegment;
}XHCI_SEGMENT, *PXHCI_SEGMENT;

typedef struct _XHCI_RING 
{
    XHCI_SEGMENT firstSeg;
    PXHCI_TRB dequeue_pointer;
    PXHCI_TRB enqueue_pointer;
    PXHCI_SEGMENT enqueue_segment;
    PXHCI_SEGMENT dequeue_segment;
    struct 
    {
        UCHAR ProducerCycleState             : 1;
        UCHAR ConsumerCycleState             : 1;
    };
} XHCI_RING, *PXHCI_RING;

typedef struct _XHCI_HC_RESOURCES 
{
    XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY DCBAA;
    DECLSPEC_ALIGN(16) XHCI_RING         EventRing ;
    DECLSPEC_ALIGN(64) XHCI_RING         CommandRing ;
    DECLSPEC_ALIGN(64) XHCI_EVENT_RING_SEGMENT_TABLE EventRingSegTable;
} XHCI_HC_RESOURCES, *PXHCI_HC_RESOURCES;
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,EventRing)% 16 == 0); 
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,CommandRing)% 64 == 0); 
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,EventRingSegTable)% 64 == 0);

typedef struct _XHCI_EXTENSION 
{
    ULONG Reserved;
    ULONG Flags;
    PULONG BaseIoAdress;
    PULONG OperationalRegs;
    PULONG RunTimeRegisterBase;
    PULONG DoorBellRegisterBase;
    UCHAR FrameLengthAdjustment;
    BOOLEAN IsStarted;
    USHORT HcSystemErrors;
    ULONG PortRoutingControl;
    USHORT NumberOfPorts; // HCSPARAMS1 => N_PORTS 
    USHORT PortPowerControl; // HCSPARAMS => Port Power Control (PPC)
    USHORT PageSize;
    USHORT MaxScratchPadBuffers;
    PMDL ScratchPadArrayMDL;
    PMDL ScratchPadBufferMDL;
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
} XHCI_EXTENSION, *PXHCI_EXTENSION;

typedef struct _XHCI_TRANSFER_RING
{
    XHCI_SEGMENT firstSeg;
    PXHCI_TRB dequeue_pointer;
    PXHCI_TRB enqueue_pointer;
    PXHCI_SEGMENT enqueue_segment;
    PXHCI_SEGMENT dequeue_segment;
    struct 
    {
        UCHAR ProducerCycleState             : 1;
        UCHAR ConsumerCycleState             : 1;
    };
} XHCI_TRANSFER_RING, *PXHCI_TRANSFER_RING;

// 6.6
typedef union _XHCI_SCRATCHPAD_BUFFER_ARRAY
{
    struct 
    {
        ULONGLONG RsvdZ1                     :  12;
        ULONGLONG bufferBaseAddr             :  52;
    };
    ULONGLONG AsULONGLONG;
} XHCI_SCRATCHPAD_BUFFER_ARRAY, *PXHCI_SCRATCHPAD_BUFFER_ARRAY;
C_ASSERT(sizeof(XHCI_SCRATCHPAD_BUFFER_ARRAY) == 8);

/* 5.4.8 */
typedef struct _XHCI_PORT_STATUS_REGISTER
{
    struct 
    {
        ULONG WPR                            : 1;
        ULONG DR                             : 1;
        ULONG RsvdZ1                         : 2;
        ULONG WOE                            : 1;
        ULONG WDE                            : 1;
        ULONG WCE                            : 1;
        ULONG CAS                            : 1;
        ULONG CEC                            : 1;
        ULONG PLC                            : 1;
        ULONG PRC                            : 1;
        ULONG OCC                            : 1;
        ULONG WRC                            : 1;
        ULONG PEC                            : 1;
        ULONG CSC                            : 1;
        ULONG LWS                            : 1;
        ULONG PIC                            : 2;
        ULONG PortSpeed                      : 4;
        ULONG PP                             : 1;
        ULONG PLS                            : 4;
        ULONG PR                             : 1;
        ULONG OCA                            : 1;
        ULONG RsvdZ2                         : 1;
        ULONG PED                            : 1;
        ULONG CCS                            : 1;
        
    };
} XHCI_PORT_STATUS_REGISTER, *PXHCI_PORT_STATUS_REGISTER;

/* Device Context *********************************************************************************/

// 6.2.1
typedef struct _XHCI_DEVICE_CONTEXT
{
    struct 
    {
        ULONG RsvdZ1                         : 6;
        ULONGLONG DeviceContextBA            : 58;
    };
    struct 
    {
        ULONG RsvdZ2                         : 6;
        ULONGLONG ScratchPadBufferBA         : 58;
    };
} XHCI_DEVICE_CONTEXT, *PXHCI_DEVICE_CONTEXT;

/* Endpoint Context *******************************************************************************/

// 6.2.3
typedef struct _XHCI_ENDPOINT
{
    /* Offset 00h */
    struct 
    {
        ULONG EPState                        : 3;
        ULONG RsvdZ1                         : 5;
        ULONG Mult                           : 2;
        ULONG MaxPStreams                    : 5;
        ULONG LSA                            : 1;
        ULONG Interval                       : 8;
        ULONG MaxESITHigh                    : 8;
    };
    /* Offset 04h */
    struct 
    {
        ULONG RsvdZ2                         : 1;
        ULONG CErr                           : 2;
        ULONG EPType                         : 3;
        ULONG RsvdZ3                         : 1;
        ULONG HID                            : 1;
        ULONG MaxBurstSize                   : 8;
        ULONG MaxPacketSize                  : 16;

    };
    /* Offset 08h */
    struct 
    {
        ULONG DCS                            : 1;
        ULONG Rsvdz4                         : 3;
        ULONGLONG TRDeqPtr                   : 60;
    };
    
    /* Offset 10h */
    struct 
    {
        ULONG AverageTRBLength               : 16;
        ULONG MaxESITPayload                 : 16;
    };
} XHCI_ENDPOINT, *PXHCI_ENDPOINT;

/* Slot Context ***********************************************************************************/

/* 6.2.2 */
typedef struct _XHCI_SLOT_CONTEXT
{
    struct
    {
        ULONG RouteString                    : 20;
        ULONG Speed                          : 4;   /* Deprecated in latest spec */
        ULONG RsvdZ1                         : 1;
        ULONG MultiTT                        : 1;
        ULONG Hub                            : 1;
        ULONG ContextEntries                 : 5;
    };
    struct
    {
        ULONG MaxExitLatency                 : 16;
        ULONG RootHubPortNumber              : 8;
        ULONG NumberOfPorts                  : 8;
    };
    struct
    {
        ULONG ParentHubSlotID                : 8;
        ULONG ParentPortNumber               : 8;
        ULONG TTThinkTime                    : 2;
        ULONG RsvdZ2                         : 4;
        ULONG InterrupterTarget              : 10;
    };
    struct
    {
        ULONG USBDeviceAddress               : 8;
        ULONG RsvdZ3                         : 19;
        ULONG SlotState                      : 5;
    };
    struct
    {
        ULONG RsvdZ4                         : 32;
    };
    struct
    {
        ULONG RsvdZ5                         : 32;
    };
    struct
    {
        ULONG RsvdZ6                         : 32;
    };
    struct
    {
        ULONG RsvdZ7                         : 32;
    };
} XHCI_SLOT_CONTEXT, *PXHCI_SLOT_CONTEXT;

/* Input Control Context **************************************************************************/

//6.2.5.1
typedef struct _XHCI_INPUT_CONTROL_CONTEXT
{
    //Offset 00h
    struct
    {
        ULONG RsvdZ1                         : 2;
        ULONG DropContextFlags               : 30;
    };
    // Offset 04h
    struct
    {
        ULONG A0                             : 1;
        ULONG A1                             : 1;
        ULONG AddContextFlags                : 30;
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
        ULONG RsvdZ4                         : 32;
    };
      struct
    {
        ULONG RsvdZ5                         : 32;
    };
      struct
    {
        ULONG RsvdZ6                         : 32;
    };
    // Offset 1Ch
    struct
    {
        ULONG ConfigVal                      : 8;
        ULONG InterfaceNum                   : 8;
        ULONG AltSetting                     : 8;
        ULONG RsvdZ7                         : 8;
    };
} XHCI_INPUT_CONTROL_CONTEXT, *PXHCI_INPUT_CONTROL_CONTEXT;

typedef struct _XHCI_ENDPOINT_CONTEXT {
    struct
    {
        ULONG dwendpoint0                    : 32;
    };
    struct
    {
        ULONG dwendpoint1                    : 32;
    };
    struct
    {
        ULONG dwendpoint2                    : 32;
    };
    struct
    {
        ULONG dwendpoint3                    : 32;
    };
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
} XHCI_ENDPOINT_CONTEXT, *PXHCI_ENDPOINT_CONTEXT;


/* Input Context **********************************************************************************/

/* 6.2.5 */
typedef struct _XHCI_INPUT_CONTEXT
{
    XHCI_INPUT_CONTROL_CONTEXT InputContext;
    XHCI_SLOT_CONTEXT SlotContext;
    XHCI_ENDPOINT_CONTEXT EndpointList[31]; /* Hard value in spec */
} XHCI_INPUT_CONTEXT, *PXHCI_INPUT_CONTEXT;

/* Device Context *********************************************************************************/

/* 6.2.5 */
typedef struct _XHCI_OUTPUT_DEVICE_CONTEXT
{
    XHCI_SLOT_CONTEXT SlotContext;
    XHCI_ENDPOINT_CONTEXT EndpointList[31]; /* Hard value in spec */
} XHCI_OUTPUT_DEVICE_CONTEXT, *PXHCI_OUTPUT_DEVICE_CONTEXT;
