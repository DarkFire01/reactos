
#pragma once

#include <ntddk.h>
#include <windef.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <drivers/usbport/usbmport.h>

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

/* Event TRBs ****************************************************************************************/

/* 6.5 - Event Ring segment table */
typedef struct _XHCI_EVENT_RING_SEGMENT_TABLE
{
    ULONGLONG RingSegmentBaseAddr;
    struct 
    {
        ULONGLONG RingSegmentSize            :  16;
        ULONGLONG RsvdZ                      :  48;
    };
} XHCI_EVENT_RING_SEGMENT_TABLE;

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

/* Command TRBs **************************************************************************************/

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
} XHCI_ENABLE_SLOT_COMMAND_TRB, *PXHCI_ENABLE_SLOT_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_ENABLE_SLOT_COMMAND_TRB) == 16);

/* 4.11.4 - No Operation command */
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

/* 6.4.3.8 - Stop Endpoint Commands TRB  */
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

/* 6.4.3.4 - Address Device TRB */
typedef struct _XHCI_ADDRESS_DEVICE_COMMAND_TRB
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
} XHCI_ADDRESS_DEVICE_COMMAND_TRB, *PXHCI_ADDRESS_DEVICE_COMMAND_TRB;
C_ASSERT(sizeof(XHCI_ADDRESS_DEVICE_COMMAND_TRB) == 16);

/* Other TRBs ****************************************************************************************/
