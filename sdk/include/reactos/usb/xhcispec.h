/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         xHCI specification header
 * COPYRIGHT:       Copyright 2017 Rama Teja Gampa <ramateja.g@gmail.com>
 *                  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <windef.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <drivers/usbport/usbmport.h>
#include "xhcitrb.h"

#define XHCI_MAX_ENDPOINTS 32;
#define XHCI_FLAGS_CONTROLLER_SUSPEND 0x01

/* Generic TRBs ***********************************************************************************/

typedef struct _XHCI_GENERIC_TRB {
    ULONG Word0;
    ULONG Word1;
    ULONG Word2;
    ULONG Word3;
}XHCI_GENERIC_TRB, *PXHCI_GENERIC_TRB;
C_ASSERT(sizeof(XHCI_GENERIC_TRB) == 16);

typedef struct _XHCI_EVENT_GENERIC_TRB
{
    ULONG Word0;
    ULONG Word1;
    ULONG Word2;
    struct 
    {
        ULONG CycleBit          : 1;
        ULONG RsvdZ1            : 9;
        ULONG TRBType           : 6;
        ULONG RsvdZ2            : 8;
        ULONG SlotID            : 8;
    };
}XHCI_EVENT_GENERIC_TRB;
C_ASSERT(sizeof(XHCI_EVENT_GENERIC_TRB) == 16);

typedef union _XHCI_COMMAND_TRB 
{
    XHCI_COMMAND_NO_OP_TRB NoOperation;
    XHCI_ENABLE_SLOT_COMMAND_TRB SlotEnable;
    XHCI_ADDRESS_DEVICE_COMMAND_TRB AddressDevice;
}XHCI_COMMAND_TRB, *PXHCI_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_COMMAND_TRB) == 16);

typedef union _XHCI_CONTROL_TRB 
{
    XHCI_CONTROL_SETUP_TRB  SetupTRB;
    XHCI_CONTROL_DATA_TRB   DataTRB;
    XHCI_CONTROL_STATUS_TRB StatusTRB;
    XHCI_GENERIC_TRB    GenericTRB;
} XHCI_CONTROL_TRB, *PXHCI_CONTROL_TRB;  
C_ASSERT(sizeof(XHCI_CONTROL_TRB) == 16);

typedef union _XHCI_EVENT_TRB 
{
    XHCI_EVENT_COMMAND_COMPLETION_TRB   CommandCompletionTRB;
    XHCI_EVENT_PORT_STATUS_CHANGE_TRB   PortStatusChangeTRB;
    XHCI_EVENT_GENERIC_TRB              EventGenericTRB;
} XHCI_EVENT_TRB, *PXHCI_EVENT_TRB;
C_ASSERT(sizeof(XHCI_EVENT_TRB) == 16);

typedef union _XHCI_TRB 
{
    XHCI_COMMAND_TRB    CommandTRB;
    XHCI_LINK_TRB       LinkTRB;
    XHCI_CONTROL_TRB    ControlTRB;
    XHCI_EVENT_TRB      EventTRB;
    XHCI_GENERIC_TRB    GenericTRB;
} XHCI_TRB, *PXHCI_TRB;
C_ASSERT(sizeof(XHCI_TRB) == 16);

extern USBPORT_REGISTRATION_PACKET RegPacket;

typedef struct  _XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY 
{
    PHYSICAL_ADDRESS ContextBaseAddr [256];
} XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY, *PXHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY;

typedef struct _XHCI_SEGMENT 
{
    XHCI_TRB XhciTrb[256];
    PVOID nextSegment;
} XHCI_SEGMENT, *PXHCI_SEGMENT;

typedef struct _XHCI_RING 
{
    XHCI_SEGMENT firstSeg;
    PXHCI_TRB dequeue_pointer;
    PXHCI_TRB enqueue_pointer;
    PXHCI_SEGMENT enqueue_segment;
    PXHCI_SEGMENT dequeue_segment;
    struct 
    {
        UCHAR ProducerCycleState : 1;
        UCHAR ConsumerCycleState : 1;
    };
} XHCI_RING, *PXHCI_RING;

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


/* 6.5 */
typedef struct _XHCI_EVENT_RING_SEGMENT_TABLE
{
    ULONGLONG RingSegmentBaseAddr;
    struct 
    {
        ULONGLONG RingSegmentSize :  16;
        ULONGLONG RsvdZ           :  48;
    };
} XHCI_EVENT_RING_SEGMENT_TABLE;


typedef struct _XHCI_HC_RESOURCES 
{
    XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY DCBAA;
    DECLSPEC_ALIGN(16) XHCI_RING         EventRing ;
    DECLSPEC_ALIGN(64) XHCI_RING         CommandRing ;
    DECLSPEC_ALIGN(64) XHCI_RING         TransferRing ;
    DECLSPEC_ALIGN(64) XHCI_EVENT_RING_SEGMENT_TABLE EventRingSegTable;
} XHCI_HC_RESOURCES, *PXHCI_HC_RESOURCES;
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,EventRing)% 16 == 0);
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,CommandRing)% 64 == 0);
C_ASSERT (FIELD_OFFSET(XHCI_HC_RESOURCES,TransferRing)% 64 == 0);
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

/* 6.6 */
typedef union _XHCI_SCRATCHPAD_BUFFER_ARRAY
{
    struct 
    {
        ULONGLONG RsvdZ1              :  12;
        ULONGLONG bufferBaseAddr      :  52;
    };
    ULONGLONG AsULONGLONG;
} XHCI_SCRATCHPAD_BUFFER_ARRAY, *PXHCI_SCRATCHPAD_BUFFER_ARRAY;
C_ASSERT(sizeof(XHCI_SCRATCHPAD_BUFFER_ARRAY) == 8);

/* Endpoint Context *******************************************************************************/

/* 6.2.3 */
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

/* 6.2.5.1 */
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

/* Device Context *********************************************************************************/

/* 6.2.1 */
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

/* Input Context **********************************************************************************/

/* 6.2.5 */
typedef struct _XHCI_INPUT_CONTEXT
{
    XHCI_INPUT_CONTROL_CONTEXT InputContext;
    XHCI_SLOT_CONTEXT SlotContext;
    XHCI_ENDPOINT EndpointList[31]; /* Hard value in spec */
} XHCI_INPUT_CONTEXT, *PXHCI_INPUT_CONTEXT;

/* Device Context *********************************************************************************/

/* 6.2.5 */
typedef struct _XHCI_OUTPUT_DEVICE_CONTEXT
{
    XHCI_SLOT_CONTEXT SlotContext;
    XHCI_ENDPOINT EndpointList[31]; /* Hard value in spec */
} XHCI_OUTPUT_DEVICE_CONTEXT, *PXHCI_OUTPUT_DEVICE_CONTEXT;
