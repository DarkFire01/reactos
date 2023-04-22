/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Priate management functions of xHCI
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "usbxhcip.h"
//#define NDEBUG
#include <debug.h>
#define NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

/* Private Functions ******************************************************************************/

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

    __debugbreak();
    return MP_STATUS_SUCCESS;
}

/*
 *  Private API to handle PSC events.
 */
VOID
NTAPI
PXHCI_PortStatusChange(_Inout_ PXHCI_EXTENSION XhciExtension,
                       _In_ ULONG PortID)
{
    XHCI_PORT_STATUS_CONTROL PortStatus;
    PULONG OperationalRegs;

    OperationalRegs = XhciExtension->OperationalRegs;
    PortStatus.AsULONG = 0;

    /* 5.4.8 */
    PortStatus.AsULONG = READ_REGISTER_ULONG(OperationalRegs + (XHCI_PORTSC + (0x10 * (PortID - 1))));
    if(PortStatus.ConnectStatusChange == 1 &&
       PortStatus.CurrentConnectStatus == 1)
    {
        /* Attached:
         *  - CSC -> 1
         *  - CCS -> 1
         */
        DPRINT1("PXHCI_PortStatusChange: USB device has been inserted from port: %X\n", PortID);
        DPRINT1("PXHCI_PortStatusChange: Port speed is %d\n", PortStatus.PortSpeed);
        XhciExtension->DeviceContext[PortID].CurrentlyInserted = TRUE;
        //TODO: Turn on USB2.0 SLOT?
        //TODO: test xhci auto config?
    }
    else if(PortStatus.ConnectStatusChange == 1 &&
            PortStatus.CurrentConnectStatus == 0)
    {
        /* Detached:
         *    - CCS -> 0
         *    - CSC -> 1
         */
        DPRINT1("PXHCI_PortStatusChange: USB device has been removed from port: %X\n", PortID);
        XhciExtension->DeviceContext[PortID].CurrentlyInserted = FALSE;
        //TODO: This is technically a hack, we should run some code to clena up
        XhciExtension->DeviceContext[PortID].Enabled = FALSE;
        /* Run de-escalation code */
        /*
         * -> Submit a disable slot command
         * -> clear transfer rings of all TDs associated with device post deattach
         */
    }

    //TODO: maybe some other handling would be cool yeah very poggy woggy.
}
