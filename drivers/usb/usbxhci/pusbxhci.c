/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Priate management functions of xHCI
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "pusbxhci.h"
//#define NDEBUG
#include <debug.h>
#define NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION xhciExtension, IN ULONG PortID)
{
    /* half implemented */
    //XHCI_PORT_STATUS_REGISTER *PortStatusReg;
    BOOLEAN DeviceInsertedEvent = TRUE;
    //PULONG OperationalRegs;
    //LONG RegValue;
    //OperationalRegs = xhciExtension->OperationalRegs;
    /* 5.4.8 */
    //RegValue = READ_REGISTER_ULONG(OperationalRegs + (XHCI_PORTSC + (0x10 * (PortID - 1)))) ;
    //PortStatusReg = (PXHCI_PORT_STATUS_REGISTER)RegValue;

    if(DeviceInsertedEvent == TRUE)
    {
        DPRINT("PXHCI_PortStatusChange: USB device has been inserted from port: %X\n", PortID);
        PXHCI_AssignSlot(xhciExtension, PortID);
    }
    else
    {
        DPRINT("PXHCI_PortStatusChange: USB device has been removed from port: %X\n", PortID);
        /* Run de-escalation code */
    }
}

VOID
NTAPI
PXHCI_AssignSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID)
{
    /* 4.3.2 of the Intel xHCI spec */
    PXHCI_HC_RESOURCES HcResourcesVA;
    ULONG SlotID, CheckCompletion;
    PXHCI_EXTENSION XhciExtension;
    PXHCI_TRB dequeue_pointer;
    XHCI_EVENT_TRB eventTRB;
    XHCI_TRB Trb;

    SlotID = 0;
    CheckCompletion = INVALID;

    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    dequeue_pointer = HcResourcesVA-> EventRing.dequeue_pointer;
    eventTRB = (*dequeue_pointer).EventTRB;

    /* Send enable slot command properly */
    Trb.CommandTRB.SlotEnable.RsvdZ1 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ2 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ3 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ4 = 0;
    Trb.CommandTRB.SlotEnable.CycleBit = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ5 = 0;
    Trb.CommandTRB.SlotEnable.SlotType = 0;
    Trb.CommandTRB.SlotEnable.TRBType = ENABLE_SLOT_COMMAND;
    XHCI_SendCommand(Trb,XhciExtension);

    /* Check for completion and grab the Slot ID */
    DPRINT("PXHCI_AssignSlot: Assigning Slot.\n");
    while (!CheckCompletion)
    {
        SlotID = eventTRB.CommandCompletionTRB.SlotID;
        CheckCompletion = eventTRB.CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {
            KeStallExecutionProcessor(10);
            break;
        }
    }

    DPRINT("PXHCI_AssignSlot: The Slot ID assigned is %X\n", SlotID);
    PXHCI_InitSlot(xhciExtension, PortID, SlotID);
}

XHCI_ENDPOINT HcDefaultEndpoint;
PXHCI_HC_RESOURCES HcResourcesVA;
VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID, ULONG SlotID)
{
    /* 4.3.3 of the Intel xHCI spec */
    PXHCI_OUTPUT_DEVICE_CONTEXT HcOutputDeviceContext;
    PXHCI_TRANSFER_RING HcTransferControlRing;
    PXHCI_INPUT_CONTEXT HcInputContext;
    PXHCI_SLOT_CONTEXT HcSlotContext;

    PXHCI_EXTENSION XhciExtension;
    PXHCI_TRB dequeue_pointer;
    XHCI_EVENT_TRB eventTRB;
    PULONG OperationalRegs;
    ULONG CheckCompletion;
    PHYSICAL_ADDRESS max;
    ULONG_PTR TrDeqPtr;
    XHCI_TRB Trb;
    DPRINT1("Test");
__debugbreak();
    OperationalRegs = xhciExtension->OperationalRegs;
    CheckCompletion = INVALID;
    max.QuadPart = -1;

    dequeue_pointer = HcResourcesVA-> EventRing.dequeue_pointer;
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    eventTRB = (*dequeue_pointer).EventTRB;

    HcOutputDeviceContext = MmAllocateContiguousMemory(sizeof(XHCI_OUTPUT_DEVICE_CONTEXT),max);
    HcTransferControlRing = MmAllocateContiguousMemory(sizeof(XHCI_TRANSFER_RING),max);
    HcInputContext = MmAllocateContiguousMemory(sizeof(XHCI_INPUT_CONTEXT),max);
    HcSlotContext = MmAllocateContiguousMemory(sizeof(XHCI_SLOT_CONTEXT),max);
    DPRINT1("memory alloc");
__debugbreak();
    RtlZeroMemory((PVOID)HcOutputDeviceContext, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT));
    RtlZeroMemory((PVOID)HcTransferControlRing, sizeof(XHCI_TRANSFER_RING));
    RtlZeroMemory((PVOID)HcInputContext, sizeof(XHCI_INPUT_CONTEXT));
    RtlZeroMemory((PVOID)HcSlotContext, sizeof(XHCI_SLOT_CONTEXT));
    DPRINT1("Memory zero");
__debugbreak();
    TrDeqPtr = (ULONG_PTR)HcTransferControlRing->firstSeg.XhciTrb;

    HcInputContext->InputContext.A0 = 1;
    HcInputContext->InputContext.A1 = 1;

    HcSlotContext->RouteString = 0;
    HcSlotContext->ParentPortNumber = PortID;
    HcSlotContext->ContextEntries = 1;
    HcSlotContext->ParentHubSlotID = SlotID;

    HcDefaultEndpoint.EPType = 4;
    HcDefaultEndpoint.MaxBurstSize = 0;
    HcDefaultEndpoint.TRDeqPtr = TrDeqPtr;
    HcDefaultEndpoint.DCS = 1;
    HcDefaultEndpoint.Interval = 0;
    HcDefaultEndpoint.MaxPStreams = 0;
    HcDefaultEndpoint.Mult = 0;
    HcDefaultEndpoint.CErr = 3;

    XHCI_Write64bitReg(OperationalRegs + XHCI_DCBAAP, (ULONG_PTR)HcOutputDeviceContext);

    Trb.CommandTRB.AddressDevice.InputContextPtrLow = (ULONG_PTR)HcInputContext->InputContext.RsvdZ1;
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = ((ULONG_PTR)HcInputContext->InputContext.RsvdZ1 + sizeof(XHCI_INPUT_CONTEXT));
    Trb.CommandTRB.AddressDevice.RsvdZ2 = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ3 = 0;
    Trb.CommandTRB.AddressDevice.CycleBit = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ4 = 0;
    Trb.CommandTRB.AddressDevice.TRBType = ADDRESS_DEVICE_COMMAND;

    XHCI_SendCommand(Trb,XhciExtension);

    while (!CheckCompletion)
    {
        SlotID = eventTRB.CommandCompletionTRB.SlotID;
        CheckCompletion = eventTRB.CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {
            KeStallExecutionProcessor(10);
            break;
        }
    }

    DPRINT1("Device is online");
    __debugbreak();
}

/* Transfer type functions ************************************************************************/

MPSTATUS
NTAPI
PXHCI_InitTransferBulk(PXHCI_EXTENSION XhciExtension)
{
    __debugbreak();
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_InitTransferInterrupt(PXHCI_EXTENSION XhciExtension)
{
    __debugbreak();
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_InitTransferIso(PXHCI_EXTENSION XhciExtension)
{
    UNIMPLEMENTED;
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_InitTransferControl(PXHCI_EXTENSION XhciExtension)
{
    __debugbreak();
    return MP_STATUS_SUCCESS;
} 
