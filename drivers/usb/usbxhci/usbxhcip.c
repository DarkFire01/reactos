/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Priate management functions of xHCI
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "usbxhcip.h"
#include "hardware.h"
//#define NDEBUG
#include <debug.h>
#define NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID)
{
    PXHCI_PORT_STATUS_REGISTER PortStatusReg;
    BOOLEAN DeviceInsertedEvent = TRUE;
    PULONG OperationalRegs;
    PXHCI_PORT_STATUS_REGISTER RegValue;
    OperationalRegs = XhciExtension->OperationalRegs;

    /* 5.4.8 */
    RegValue = (PXHCI_PORT_STATUS_REGISTER)READ_REGISTER_ULONG(OperationalRegs + (XHCI_PORTSC + (0x10 * (PortID - 1))));
    PortStatusReg = (PXHCI_PORT_STATUS_REGISTER)RegValue;
    DPRINT1("The Value of is %X\n", RegValue->WPR);
    DPRINT1("The Value of is %X\n", RegValue->DR);
    DPRINT1("The Value of is %X\n", RegValue->RsvdZ1);
    DPRINT1("The Value of is %X\n", RegValue->WOE);
    DPRINT1("The Value of is %X\n", RegValue->WDE);
    DPRINT1("The Value of is %X\n", RegValue->WCE);
    DPRINT1("The Value of is %X\n", RegValue->CAS);
    DPRINT1("The Value of is %X\n", RegValue->CEC);
    DPRINT1("The Value of is %X\n", RegValue->PLC);
    DPRINT1("The Value of is %X\n", RegValue->PRC);
    DPRINT1("The Value of is %X\n", RegValue->OCC);
    DPRINT1("The Value of is %X\n", RegValue->WRC);
    DPRINT1("The Value of is %X\n", RegValue->PEC);
    DPRINT1("The Value of is %X\n", RegValue->CSC);
    DPRINT1("The Value of is %X\n", RegValue->LWS);
    DPRINT1("The Value of is %X\n", RegValue->PIC);
    DPRINT1("The Value of is %X\n", RegValue->PortSpeed);
    DPRINT1("The Value of is %X\n", RegValue->PP);
    DPRINT1("The Value of is %X\n", RegValue->PLS);
    DPRINT1("The Value of is %X\n", RegValue->PR);
    DPRINT1("The Value of is %X\n", RegValue->OCA);   
    DPRINT1("The Value of is %X\n", RegValue->RsvdZ2);
    DPRINT1("The Value of is %X\n", RegValue->PED);
    DPRINT1("The Value of is %X\n", RegValue->CCS);
    if(DeviceInsertedEvent == TRUE)
    {
        /* Attached: */
        DPRINT("PXHCI_PortStatusChange: USB device has been inserted from port: %X\n", PortID);
        PXHCI_AssignSlot(XhciExtension, PortID);
    }
    else
    {
        /* Detached: 
         *    - CCS -> 0 
         *    - CSC -> 1 
         */
        DPRINT("PXHCI_PortStatusChange: USB device has been removed from port: %X\n", PortID);
        /* Run de-escalation code */
        /*
         * -> Submit a disable slot command
         * -> clear transfer rings of all TDs associated with device post deattach
         */
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

    RtlZeroMemory((PVOID)HcOutputDeviceContext, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT));
    RtlZeroMemory((PVOID)HcTransferControlRing, sizeof(XHCI_TRANSFER_RING));
    RtlZeroMemory((PVOID)HcInputContext, sizeof(XHCI_INPUT_CONTEXT));
    RtlZeroMemory((PVOID)HcSlotContext, sizeof(XHCI_SLOT_CONTEXT));

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
}

MPSTATUS
NTAPI
PXHCI_ControllerWorkTest(IN PXHCI_EXTENSION XhciExtension,
                         IN PXHCI_HC_RESOURCES HcResourcesVA,
                         IN PVOID resourcesStartPA)
{
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL Doorbell_0;
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};
    XHCI_USB_STATUS Status;
    PHYSICAL_ADDRESS HcResourcesPA;
    DPRINT1("Starting work test");
    XHCI_COMMAND_RING_CONTROL CommandRingControlRegister;
    ULONGLONG CommandRingAddr;
    ULONGLONG EventRingAddr;
    XHCI_EVENT_RING_TABLE_SIZE erstz;
    XHCI_EVENT_RING_TABLE_BASE_ADDR erstba;
    // place a no op command trb on the command ring
    XHCI_TRB trb;
    XHCI_TRB eventtrb;
    DPRINT("XHCI_ControllerWorkTest: Initiated.\n");
    trb.CommandTRB.NoOperation.RsvdZ1 = 0;
    trb.CommandTRB.NoOperation.RsvdZ2 = 0;
    trb.CommandTRB.NoOperation.RsvdZ3 = 0;
    trb.CommandTRB.NoOperation.CycleBit = 1;
    trb.CommandTRB.NoOperation.RsvdZ4 = 0;
    trb.CommandTRB.NoOperation.TRBType = NO_OP_COMMAND;
    trb.CommandTRB.NoOperation.RsvdZ5 = 0;
    XHCI_SendCommand(trb,XhciExtension);
    XHCI_ProcessEvent(XhciExtension);

    HcResourcesVA -> CommandRing.firstSeg.XhciTrb[0] = trb;
    // ring the commmand ring door bell register
    DoorBellRegisterBase = XhciExtension->DoorBellRegisterBase;
    Doorbell_0.DoorBellTarget = 0;
    Doorbell_0.RsvdZ = 0;
    Doorbell_0.AsULONG = 0;
    WRITE_REGISTER_ULONG(DoorBellRegisterBase, Doorbell_0.AsULONG);
    // wait for some time.
    KeQuerySystemTime(&CurrentTime);
    CurrentTime.QuadPart += 100 * 100; // 100 msec
    while(TRUE)
    {
        KeQuerySystemTime(&LastTime);
        if (LastTime.QuadPart >= CurrentTime.QuadPart)
        {
            break;
        }
    }
    
    // check for event completion trb
    eventtrb =  HcResourcesVA -> EventRing.firstSeg.XhciTrb[0];
    DPRINT("PXHCI_ControllerWorkTest: eventtrb word0    - %p\n", eventtrb.GenericTRB.Word0);
    DPRINT("PXHCI_ControllerWorkTest: eventtrb word1    - %p\n", eventtrb.GenericTRB.Word1);
    DPRINT("PXHCI_ControllerWorkTest: eventtrb word2    - %p\n", eventtrb.GenericTRB.Word2);
    DPRINT("PXHCI_ControllerWorkTest: eventtrb word3    - %p\n", eventtrb.GenericTRB.Word3);
    // status check code
    Status.AsULONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_USBSTS);
    DPRINT("PXHCI_ControllerWorkTest: Status HCHalted    - %p\n", Status.HCHalted);
    DPRINT("PXHCI_ControllerWorkTest: Status HostSystemError    - %p\n", Status.HostSystemError);
    DPRINT("PXHCI_ControllerWorkTest: Status EventInterrupt    - %p\n", Status.EventInterrupt);
    DPRINT("PXHCI_ControllerWorkTest: Status PortChangeDetect    - %p\n", Status.PortChangeDetect);
    DPRINT("PXHCI_ControllerWorkTest: Status ControllerNotReady    - %p\n", Status.ControllerNotReady);
    DPRINT("PXHCI_ControllerWorkTest: Status HCError    - %p\n", Status.HCError);
    DPRINT("PXHCI_ControllerWorkTest: Status     - %p\n", Status.AsULONG);
    // command ring check
    HcResourcesPA.QuadPart = (ULONG_PTR)resourcesStartPA;
    CommandRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]);
    DPRINT("PXHCI_ControllerWorkTest: CommandRingAddr     - %x\n", CommandRingAddr);
    CommandRingControlRegister.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR+1) | READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR );
    DPRINT("PXHCI_ControllerWorkTest: CommandRingControlRegister     - %x\n", CommandRingControlRegister.AsULONGLONG);
    DPRINT("PXHCI_ControllerWorkTest: CommandRingControlRegister1     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR ));
    DPRINT("PXHCI_ControllerWorkTest: CommandRingControlRegister2     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR + 1 ));
    // event ring dprints
    EventRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    DPRINT("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentBaseAddr     - %x\n", HcResourcesVA -> EventRingSegTable.RingSegmentBaseAddr);
    DPRINT("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentSize     - %i\n", HcResourcesVA -> EventRingSegTable.RingSegmentSize);
    DPRINT("PXHCI_ControllerWorkTest: event ring addr     - %x\n", EventRingAddr);
    //RunTimeRegisterBase + XHCI_ERSTSZ
    erstz.AsULONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ) ;
    DPRINT("PXHCI_ControllerWorkTest: erstz     - %p\n", erstz.AsULONG);
    
    erstba.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRingSegTable);
    DPRINT("PXHCI_ControllerWorkTest: erstba addr     - %x\n", erstba.AsULONGLONG);
    erstba.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1) | READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA );
    DPRINT("PXHCI_ControllerWorkTest: erstba reg read     - %x\n", erstba.AsULONGLONG);
    
    DPRINT("PXHCI_ControllerWorkTest: pointer crcr     - %p %p\n", XhciExtension->OperationalRegs + XHCI_CRCR+1 , XhciExtension->OperationalRegs + XHCI_CRCR);
    DPRINT("PXHCI_ControllerWorkTest: pointer erstz     - %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ);
    DPRINT("PXHCI_ControllerWorkTest: pointer erstba     - %p %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1 , XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA);

    //__debugbreak();
    return MP_STATUS_SUCCESS;
}
