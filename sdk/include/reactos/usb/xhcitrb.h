
#pragma once

#include <ntddk.h>
#include <windef.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <drivers/usbport/usbmport.h>

/* Address Device TRB ********************************************************************************/

/* 6.4.3.4 */
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
