/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Priate management functions of xHCI
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "usbxhcip.h"
#include "hardware.h"
//NDEBUG
#include <debug.h>
//NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

XHCI_GLOBAL_SLOT_TRACKER SlotTracker;

/*
 *  Private API to handle PSC events.
 */
VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID)
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
        //TODO: Turn on USB2.0 SLOT
        //TODO: test xhci auto config.
        //PXHCI_AssignSlot(XhciExtension, PortID);
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
        /* Run de-escalation code */
        /*
         * -> Submit a disable slot command
         * -> clear transfer rings of all TDs associated with device post deattach
         */
    }
    //TODO: maybe some other handling would be cool yeah very poggy woggy.
}

/* @IMPLEMENTED - ``This works`` nvm lol */
VOID
NTAPI
PXHCI_AssignSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID)
{
    /* 4.3.2 of the Intel xHCI spec */
    PXHCI_HC_RESOURCES HcResourcesVA;
    ULONG SlotID, CheckCompletion;
    PXHCI_EXTENSION XhciExtension;
    PULONG DoorBellRegisterBase;
    PXHCI_TRB dequeue_pointer;
    XHCI_DOORBELL Doorbell_0;
    XHCI_EVENT_TRB eventTRB;
    XHCI_TRB Trb;

    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};

    SlotID = 0;
    CheckCompletion = INVALID;

    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    dequeue_pointer = HcResourcesVA-> EventRing.dequeue_pointer;
    eventTRB = (*dequeue_pointer).EventTRB;
    eventTRB.CommandCompletionTRB.SlotID = 0xFF;
    /* Send enable slot command properly */
    Trb.CommandTRB.SlotEnable.RsvdZ1 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ2 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ3 = 0;
    Trb.CommandTRB.SlotEnable.RsvdZ4 = 0;
    Trb.CommandTRB.SlotEnable.CycleBit = 1;
    Trb.CommandTRB.SlotEnable.RsvdZ5 = 0;
    Trb.CommandTRB.SlotEnable.SlotType = 0;
    Trb.CommandTRB.SlotEnable.TRBType = ENABLE_SLOT_COMMAND;
    /* Check for completion and grab the Slot ID */
    DPRINT1("PXHCI_AssignSlot: Assigning Slot.\n");
    XHCI_SendCommand(Trb,XhciExtension);
    XHCI_ProcessEvent(XhciExtension);

    HcResourcesVA -> CommandRing.firstSeg.XhciTrb[0] = Trb;
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
    eventTRB =  HcResourcesVA -> EventRing.firstSeg.XhciTrb[0].EventTRB;
    SlotID = eventTRB.CommandCompletionTRB.SlotID;
    CheckCompletion = eventTRB.CommandCompletionTRB.CompletionCode;
    DPRINT1("PXHCI_AssignSlot: The Slot ID assigned is %X\n", SlotID);
    DPRINT1("PXHCI_AssignSlot: The CheckCompletion assigned is %X\n", CheckCompletion);
    xhciExtension->DeviceContext[PortID].Enabled = TRUE;
    //PXHCI_InitSlot(xhciExtension, PortID, SlotID);
    __debugbreak();
}
PXHCI_HC_RESOURCES HcResourcesVA;
PXHCI_TRB dequeue_pointer;
VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID, ULONG SlotID)
{
    /* 4.3.3 of the Intel xHCI spec */
    PXHCI_OUTPUT_DEVICE_CONTEXT HcOutputDeviceContext;
    PXHCI_TRANSFER_RING HcTransferControlRing;
    PXHCI_INPUT_CONTEXT HcInputContext;
    PXHCI_SLOT_CONTEXT HcSlotContext;
    PXHCI_ENDPOINT_CONTEXT HcDefaultEndpoint;

    PXHCI_EXTENSION XhciExtension;
    PXHCI_EVENT_TRB eventTRB;
    PULONG OperationalRegs;
    ULONG CheckCompletion;
    ULONG_PTR TrDeqPtr;
    XHCI_TRB Trb;

    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    eventTRB = (PXHCI_EVENT_TRB)HcResourcesVA->EventRing.dequeue_pointer;

    OperationalRegs = xhciExtension->OperationalRegs;
    CheckCompletion = INVALID;

    HcOutputDeviceContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT), 'TVED');
    HcTransferControlRing = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_TRANSFER_RING), 'TVED');
    HcDefaultEndpoint = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_ENDPOINT_CONTEXT), 'TVED');
    HcInputContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_INPUT_CONTEXT), 'TVED');
    HcSlotContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_SLOT_CONTEXT), 'TVED');

    RtlZeroMemory((PVOID)HcOutputDeviceContext, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT));
    RtlZeroMemory((PVOID)HcTransferControlRing, sizeof(XHCI_TRANSFER_RING));
    RtlZeroMemory((PVOID)HcDefaultEndpoint, sizeof(XHCI_ENDPOINT_CONTEXT));
    RtlZeroMemory((PVOID)HcInputContext, sizeof(XHCI_INPUT_CONTEXT));
    RtlZeroMemory((PVOID)HcSlotContext, sizeof(XHCI_SLOT_CONTEXT));


    TrDeqPtr = (ULONG_PTR)HcTransferControlRing->firstSeg.XhciTrb;

    HcSlotContext->RouteString = 0;
    HcSlotContext->ParentPortNumber = PortID;
    HcSlotContext->ContextEntries = 1;
    HcSlotContext->ParentHubSlotID = SlotID;
    HcInputContext->InputContext.A0 = 1;
    HcInputContext->InputContext.A1 = 1;

    HcDefaultEndpoint->EPType = 4;
    HcDefaultEndpoint->MaxBurstSize = 0;
    HcDefaultEndpoint->TRDeqPtr = TrDeqPtr;
    HcDefaultEndpoint->DCS = 1;
    HcDefaultEndpoint->Interval = 0;
    HcDefaultEndpoint->MaxPStreams = 0;
    HcDefaultEndpoint->Mult = 0;
    HcDefaultEndpoint->CErr = 3;

    HcOutputDeviceContext->SlotContext = HcSlotContext;

    XHCI_Write64bitReg(OperationalRegs + XHCI_DCBAAP, (ULONG_PTR)HcOutputDeviceContext);

    /* 6.4.3.4 */
    Trb.CommandTRB.AddressDevice.InputContextPtrLow = (ULONG_PTR)HcInputContext->InputContext.RsvdZ1;
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = ((ULONG_PTR)HcInputContext->InputContext.RsvdZ1 + sizeof(XHCI_INPUT_CONTEXT));
    Trb.CommandTRB.AddressDevice.RsvdZ2 = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ3 = 0;
    Trb.CommandTRB.AddressDevice.CycleBit = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ4 = 0;
    Trb.CommandTRB.AddressDevice.TRBType = ADDRESS_DEVICE_COMMAND;
    Trb.CommandTRB.AddressDevice.SlotID = SlotID;
    XHCI_SendCommand(Trb,XhciExtension);


    while (!CheckCompletion)
    {
        SlotID = eventTRB->CommandCompletionTRB.SlotID;
        CheckCompletion = eventTRB->CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {
            KeStallExecutionProcessor(10);
            break;
        }
        else
        {
            DPRINT1("Task has failed with status %d\n",CheckCompletion);
        }
    }
    //  XHCI_ReadReg(OperationalRegs + XHCI_DCBAAP, (ULONG_PTR)HcOutputDeviceContext);
    DPRINT1("the USB address is %X\n", HcOutputDeviceContext->SlotContext->USBDeviceAddress);
}


/* Transfer type functions ************************************************************************/

MPSTATUS
NTAPI
PXHCI_InitTransferBulk(PXHCI_EXTENSION XhciExtension)
{
    __debugbreak();
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
PXHCI_InitTransferInterrupt(PXHCI_EXTENSION XhciExtension)
{
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_InitTransferIso(PXHCI_EXTENSION XhciExtension)
{
    UNIMPLEMENTED;
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
PXHCI_InitTransferControl(PXHCI_EXTENSION XhciExtension)
{
    __debugbreak();
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
XHCI_OpenIsoEndpoint(IN PXHCI_EXTENSION XhciExtension,
                     IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                     IN PXHCI_ENDPOINT  XhciEndpoint)
{
    DPRINT1("XHCI_OpenIsoEndpoint: function initiated\n");
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
XHCI_OpenControlEndpoint(IN PXHCI_EXTENSION XhciExtension,
                         IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                         IN PXHCI_ENDPOINT  XhciEndpoint)
{
   DPRINT1("XHCI_OpenControlEndpoint\n");
   PXHCI_HCD_TD TdVa = (PXHCI_HCD_TD)EndpointProperties->BufferVA;
   ULONG TdPA;
   ULONG TdCount;
   ULONG ix;

   DPRINT1("EndProperties.DeviceAddress is: %X\n",  EndpointProperties->DeviceAddress  );
   DPRINT1("EndProperties.EndpointAddress is: %X\n", EndpointProperties->EndpointAddress   );
   DPRINT1("EndProperties.TotalMaxPacketSize is: %X\n", EndpointProperties ); // TransactionPerMicroframe * MaxPacketSize
   DPRINT1("EndProperties.Period is: %X\n", EndpointProperties->Period);
   DPRINT1("EndProperties.DeviceSpeed is: %X\n", EndpointProperties->DeviceSpeed);
   DPRINT1("EndProperties.UsbBandwidth is: %X\n",  EndpointProperties->UsbBandwidth);
   DPRINT1("EndProperties.ScheduleOffset is: %X\n",  EndpointProperties->ScheduleOffset);
   DPRINT1("EndProperties.TransferType is: %X\n", EndpointProperties->TransferType);
   DPRINT1("EndProperties.Direction is: %X\n",EndpointProperties-> Direction );
   DPRINT1("EndProperties.BufferVA is: %X\n", EndpointProperties->BufferVA);
   DPRINT1("EndProperties.BufferPA is: %X\n", EndpointProperties->BufferPA);
   DPRINT1("EndProperties.BufferLength is: %X\n",EndpointProperties->BufferLength);
   DPRINT1("EndProperties.MaxTransferSize is: %X\n", EndpointProperties->MaxTransferSize);
   DPRINT1("EndProperties.HubAddr is: %X\n", EndpointProperties->HubAddr);
   DPRINT1("EndProperties.PortNumber is: %X\n", EndpointProperties->PortNumber);
   DPRINT1("EndProperties.InterruptScheduleMask is: %X\n", EndpointProperties->InterruptScheduleMask);
   DPRINT1("EndProperties.SplitCompletionMask is: %X\n", EndpointProperties->SplitCompletionMask);
   DPRINT1("EndProperties.TransactionPerMicroframe is: %X\n",EndpointProperties->TransactionPerMicroframe);


    DPRINT1("XHCI_OpenControlEndpoint: function initiated\n");
    InitializeListHead(&XhciEndpoint->ListTDs);

    XhciEndpoint->DmaBufferVA = (PVOID)EndpointProperties->BufferVA;
    XhciEndpoint->DmaBufferPA = EndpointProperties->BufferPA;
    XhciEndpoint->FirstTD = (PXHCI_HCD_TD)((ULONG_PTR)TdVa + sizeof(XHCI_HCD_TD));

    TdCount = (EndpointProperties->BufferLength - sizeof(XHCI_HCD_TD)) /
               sizeof(XHCI_HCD_TD);

    if (EndpointProperties->TransferType == USBPORT_TRANSFER_TYPE_CONTROL)
        XhciEndpoint->EndpointStatus |= USBPORT_ENDPOINT_CONTROL;

    XhciEndpoint->MaxTDs = TdCount;
    XhciEndpoint->RemainTDs = TdCount;



    TdPA = EndpointProperties->BufferPA + sizeof(XHCI_HCD_TD);

    for (ix = 0; ix < TdCount; ix++)
    {
        DPRINT1("XHCI_InitializeTDs: TdVA - %p, TdPA - %08X\n", TdVa, TdPA);

        RtlZeroMemory(TdVa, sizeof(XHCI_HCD_TD));
        //SETUP TRB HERE PLEASE FOR FUCKS SAKE

        TdVa++;
        TdPA += sizeof(XHCI_HCD_TD);
    }
    /* schdule */
    //__debugbreak();
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_OpenBulkEndpoint(IN PXHCI_EXTENSION XhciExtension,
                      IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                      IN PXHCI_ENDPOINT  XhciEndpoint)
{
    DPRINT1("XHCI_OpenBulkEndpoint: function initiated\n");
    return MP_STATUS_FAILURE ;
}

MPSTATUS
NTAPI
XHCI_OpenInterruptEndpoint(IN PXHCI_EXTENSION XhciExtension,
                           IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                           IN PXHCI_ENDPOINT  XhciEndpoint)
{
    DPRINT1("XHCI_OpenInterruptEndpoint: function initiated\n");
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
XHCI_SendTransfer(IN XHCI_TRB TransferTRB,
                  IN PXHCI_EXTENSION XhciExtension)
{
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL Doorbell_0;
    PXHCI_TRB enqueue_pointer;
    PXHCI_TRB enqueue_pointer_prev;
    PXHCI_TRB dequeue_pointer;
    XHCI_TRB CheckLink;
    PHYSICAL_ADDRESS LinkPointer;

    HcResourcesVA = XhciExtension->HcResourcesVA;
    HcResourcesPA = XhciExtension->HcResourcesPA;
    enqueue_pointer = HcResourcesVA->TransferRing.enqueue_pointer;
    dequeue_pointer = HcResourcesVA->TransferRing.dequeue_pointer;
    // check if ring is full
    if ((enqueue_pointer + 1) == dequeue_pointer)
    {
        DPRINT1 ("XHCI_SendCommand : Command ring is full \n");
        return MP_STATUS_FAILURE;
    }
    // check if the trb is link trb.
    CheckLink = *enqueue_pointer;
    if (CheckLink.LinkTRB.TRBType == LINK)
    {
        LinkPointer.QuadPart = CheckLink.GenericTRB.Word1;
        LinkPointer.QuadPart = LinkPointer.QuadPart << 32;
        LinkPointer.QuadPart = LinkPointer.QuadPart + CheckLink.GenericTRB.Word0;
        ASSERT(LinkPointer.QuadPart >= HcResourcesPA.QuadPart && LinkPointer.QuadPart < HcResourcesPA.QuadPart + sizeof(XHCI_HC_RESOURCES));
        enqueue_pointer_prev = enqueue_pointer;
        enqueue_pointer = (PXHCI_TRB)(HcResourcesVA + LinkPointer.QuadPart - HcResourcesPA.QuadPart);
        if ((enqueue_pointer == dequeue_pointer) || (enqueue_pointer == dequeue_pointer + 1))
        { // it can't move ahead break out of function
                DPRINT1 ("XHCI_SendCommand : Trabsfer ring is full \n");
                return MP_STATUS_FAILURE;
        }
        // now the link trb is valid. set its cycle state to Producer cycle state for the command ring to read
        CheckLink.LinkTRB.CycleBit = HcResourcesVA -> TransferRing.ProducerCycleState;
        // write the link trb back.
        *enqueue_pointer_prev = CheckLink;
        // now we can go ahead to the next pointer where we want to write the new trb. before that check and toggle if necessaary.
        if (CheckLink.LinkTRB.ToggleCycle == 1)
        {
            HcResourcesVA -> TransferRing.ProducerCycleState = ~ (HcResourcesVA -> TransferRing.ProducerCycleState); //update this when the xHC reaches link trb
        }
    }
    // place trb on the command ring
    *enqueue_pointer = TransferTRB;
    enqueue_pointer = enqueue_pointer + 1;
    HcResourcesVA->TransferRing.enqueue_pointer = enqueue_pointer;
    // ring doorbell
    DoorBellRegisterBase = XhciExtension->DoorBellRegisterBase;
    Doorbell_0.DoorBellTarget = 0;
    Doorbell_0.RsvdZ = 0;
    Doorbell_0.AsULONG = 0;
    WRITE_REGISTER_ULONG(DoorBellRegisterBase, Doorbell_0.AsULONG);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_TEST(PXHCI_EXTENSION XhciExtension, PXHCI_ENDPOINT XhciEndpoint)
{
    PXHCI_HC_RESOURCES HcResourcesVA = XhciExtension->HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA = XhciExtension->HcResourcesPA;
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL Doorbell_0;
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};
    XHCI_USB_STATUS Status;
    DPRINT1("beginning basic transfer\n");
    XHCI_COMMAND_RING_CONTROL CommandRingControlRegister;
    ULONGLONG CommandRingAddr;
    ULONGLONG EventRingAddr;
//       ULONGLONG TransferRingAddr;

    XHCI_EVENT_RING_TABLE_SIZE erstz;
    XHCI_EVENT_RING_TABLE_BASE_ADDR erstba;
    XHCI_TRB trb;
    XHCI_TRB eventtrb;
   /* Send enable slot command properly */
    trb.CommandTRB.SlotEnable.RsvdZ1 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ2 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ3 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ4 = 0;
    trb.CommandTRB.SlotEnable.CycleBit = 1;
    trb.CommandTRB.SlotEnable.RsvdZ5 = 0;
    trb.CommandTRB.SlotEnable.SlotType = 0;
    trb.CommandTRB.SlotEnable.TRBType = ENABLE_SLOT_COMMAND;

    XHCI_SendCommand(trb,XhciExtension);
    //XHCI_ProcessEvent(XhciExtension);

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

    DPRINT1("PXHCI_ControllerWorkTest: Slot ID   - %p\n", eventtrb.EventTRB.CommandCompletionTRB.SlotID);
    DPRINT1("PXHCI_ControllerWorkTest: Completeion code    - %p\n", eventtrb.EventTRB.CommandCompletionTRB.CompletionCode);
   // DPRINT1("PXHCI_ControllerWorkTest: eventtrb word2    - %p\n", eventtrb.GenericTRB.Word2);
   // DPRINT1("PXHCI_ControllerWorkTest: eventtrb word3    - %p\n", eventtrb.GenericTRB.Word3);
    // status check code
    Status.AsULONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_USBSTS);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCHalted    - %p\n", Status.HCHalted);
    DPRINT1("PXHCI_ControllerWorkTest: Status HostSystemError    - %p\n", Status.HostSystemError);
    DPRINT1("PXHCI_ControllerWorkTest: Status EventInterrupt    - %p\n", Status.EventInterrupt);
    DPRINT1("PXHCI_ControllerWorkTest: Status PortChangeDetect    - %p\n", Status.PortChangeDetect);
    DPRINT1("PXHCI_ControllerWorkTest: Status ControllerNotReady    - %p\n", Status.ControllerNotReady);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCError    - %p\n", Status.HCError);
    DPRINT1("PXHCI_ControllerWorkTest: Status     - %p\n", Status.AsULONG);
    CommandRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingAddr     - %x\n", CommandRingAddr);
    CommandRingControlRegister.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR+1) | READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR );
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister     - %x\n", CommandRingControlRegister.AsULONGLONG);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister1     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR ));
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister2     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR + 1 ));
    // event ring DPRINT1s
    EventRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentBaseAddr     - %x\n", HcResourcesVA -> EventRingSegTable.RingSegmentBaseAddr);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentSize     - %i\n", HcResourcesVA -> EventRingSegTable.RingSegmentSize);
    DPRINT1("PXHCI_ControllerWorkTest: event ring addr     - %x\n", EventRingAddr);
    //RunTimeRegisterBase + XHCI_ERSTSZ
    erstz.AsULONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ) ;
    DPRINT1("PXHCI_ControllerWorkTest: erstz     - %p\n", erstz.AsULONG);

    erstba.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRingSegTable);
    DPRINT1("PXHCI_ControllerWorkTest: erstba addr     - %x\n", erstba.AsULONGLONG);
    erstba.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1) | READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA );
    DPRINT1("PXHCI_ControllerWorkTest: erstba reg read     - %x\n", erstba.AsULONGLONG);

    DPRINT1("PXHCI_ControllerWorkTest: pointer crcr     - %p %p\n", XhciExtension->OperationalRegs + XHCI_CRCR+1 , XhciExtension->OperationalRegs + XHCI_CRCR);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstz     - %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstba     - %p %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1 , XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA);
#if 0
(drivers\usb\usbport\debug.c:214) SetupPacket->bmRequestType.B - a3
(drivers\usb\usbport\debug.c:215) SetupPacket->bRequest        - 0
(drivers\usb\usbport\debug.c:216) SetupPacket->wValue.LowByte  - 0
(drivers\usb\usbport\debug.c:217) SetupPacket->wValue.HiByte   - 0
(drivers\usb\usbport\debug.c:218) SetupPacket->wIndex.W        - 1
(drivers\usb\usbport\debug.c:219) SetupPacket->wLength         - 4

    trb.CommandTRB.NoOperation.RsvdZ1 = 0;
    trb.CommandTRB.NoOperation.RsvdZ2 = 0;
    trb.CommandTRB.NoOperation.RsvdZ3 = 0;
    trb.CommandTRB.NoOperation.CycleBit = 0;
    trb.CommandTRB.NoOperation.RsvdZ4 = 0;
    trb.CommandTRB.NoOperation.TRBType = 0;
    trb.CommandTRB.NoOperation.RsvdZ5 = 0;
    trb.ControlTRB.SetupTRB.bmRequestType = 0xA3;
    trb.ControlTRB.SetupTRB.bRequest = 0;
    trb.ControlTRB.SetupTRB.CycleBit = 1;
    trb.ControlTRB.SetupTRB.ImmediateData = 0;
    trb.ControlTRB.SetupTRB.InterrupterTarget = 0;
    trb.ControlTRB.SetupTRB.TRBTransferLength = 8;
    trb.ControlTRB.SetupTRB.InterruptOnCompletion = 1;
    trb.ControlTRB.SetupTRB.TransferType = TRB_TYPE_SETUP_STAGE;
    trb.ControlTRB.SetupTRB.wValue = 0;
    trb.ControlTRB.SetupTRB.wIndex = 1;
    trb.ControlTRB.SetupTRB.wLength = 4;
    XHCI_SendTransfer(trb,XhciExtension);
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

  eventtrb =  HcResourcesVA -> EventRing.firstSeg.XhciTrb[0];
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word0    - %p\n", eventtrb.GenericTRB.Word0);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word1    - %p\n", eventtrb.GenericTRB.Word1);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word2    - %p\n", eventtrb.GenericTRB.Word2);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word3    - %p\n", eventtrb.GenericTRB.Word3);
    // status check code
    Status.AsULONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_USBSTS);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCHalted    - %p\n", Status.HCHalted);
    DPRINT1("PXHCI_ControllerWorkTest: Status HostSystemError    - %p\n", Status.HostSystemError);
    DPRINT1("PXHCI_ControllerWorkTest: Status EventInterrupt    - %p\n", Status.EventInterrupt);
    DPRINT1("PXHCI_ControllerWorkTest: Status PortChangeDetect    - %p\n", Status.PortChangeDetect);
    DPRINT1("PXHCI_ControllerWorkTest: Status ControllerNotReady    - %p\n", Status.ControllerNotReady);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCError    - %p\n", Status.HCError);
    DPRINT1("PXHCI_ControllerWorkTest: Status     - %p\n", Status.AsULONG);
    CommandRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingAddr     - %x\n", CommandRingAddr);
    CommandRingControlRegister.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR+1) | READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR );
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister     - %x\n", CommandRingControlRegister.AsULONGLONG);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister1     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR ));
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister2     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR + 1 ));
    // event ring DPRINT1s
    EventRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentBaseAddr     - %x\n", HcResourcesVA -> EventRingSegTable.RingSegmentBaseAddr);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentSize     - %i\n", HcResourcesVA -> EventRingSegTable.RingSegmentSize);
    DPRINT1("PXHCI_ControllerWorkTest: event ring addr     - %x\n", EventRingAddr);
    //RunTimeRegisterBase + XHCI_ERSTSZ
    erstz.AsULONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ) ;
    DPRINT1("PXHCI_ControllerWorkTest: erstz     - %p\n", erstz.AsULONG);

    erstba.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRingSegTable);
    DPRINT1("PXHCI_ControllerWorkTest: erstba addr     - %x\n", erstba.AsULONGLONG);
    erstba.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1) | READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA );
    DPRINT1("PXHCI_ControllerWorkTest: erstba reg read     - %x\n", erstba.AsULONGLONG);

    DPRINT1("PXHCI_ControllerWorkTest: pointer crcr     - %p %p\n", XhciExtension->OperationalRegs + XHCI_CRCR+1 , XhciExtension->OperationalRegs + XHCI_CRCR);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstz     - %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstba     - %p %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1 , XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA);
#endif
    __debugbreak();
    return 0;
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
    DPRINT1("XHCI_ControllerWorkTest: Initiated.\n");
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
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word0    - %p\n", eventtrb.GenericTRB.Word0);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word1    - %p\n", eventtrb.GenericTRB.Word1);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word2    - %p\n", eventtrb.GenericTRB.Word2);
    DPRINT1("PXHCI_ControllerWorkTest: eventtrb word3    - %p\n", eventtrb.GenericTRB.Word3);
    // status check code
    Status.AsULONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_USBSTS);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCHalted    - %p\n", Status.HCHalted);
    DPRINT1("PXHCI_ControllerWorkTest: Status HostSystemError    - %p\n", Status.HostSystemError);
    DPRINT1("PXHCI_ControllerWorkTest: Status EventInterrupt    - %p\n", Status.EventInterrupt);
    DPRINT1("PXHCI_ControllerWorkTest: Status PortChangeDetect    - %p\n", Status.PortChangeDetect);
    DPRINT1("PXHCI_ControllerWorkTest: Status ControllerNotReady    - %p\n", Status.ControllerNotReady);
    DPRINT1("PXHCI_ControllerWorkTest: Status HCError    - %p\n", Status.HCError);
    DPRINT1("PXHCI_ControllerWorkTest: Status     - %p\n", Status.AsULONG);
    // command ring check
    HcResourcesPA.QuadPart = (ULONG_PTR)resourcesStartPA;
    CommandRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingAddr     - %x\n", CommandRingAddr);
    CommandRingControlRegister.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR+1) | READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR );
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister     - %x\n", CommandRingControlRegister.AsULONGLONG);
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister1     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR ));
    DPRINT1("PXHCI_ControllerWorkTest: CommandRingControlRegister2     - %p\n", READ_REGISTER_ULONG(XhciExtension->OperationalRegs + XHCI_CRCR + 1 ));
    // event ring DPRINT1s
    EventRingAddr = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentBaseAddr     - %x\n", HcResourcesVA -> EventRingSegTable.RingSegmentBaseAddr);
    DPRINT1("PXHCI_ControllerWorkTest: EventRingSegTable.RingSegmentSize     - %i\n", HcResourcesVA -> EventRingSegTable.RingSegmentSize);
    DPRINT1("PXHCI_ControllerWorkTest: event ring addr     - %x\n", EventRingAddr);
    //RunTimeRegisterBase + XHCI_ERSTSZ
    erstz.AsULONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ) ;
    DPRINT1("PXHCI_ControllerWorkTest: erstz     - %p\n", erstz.AsULONG);

    erstba.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRingSegTable);
    DPRINT1("PXHCI_ControllerWorkTest: erstba addr     - %x\n", erstba.AsULONGLONG);
    erstba.AsULONGLONG = READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1) | READ_REGISTER_ULONG(XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA );
    DPRINT1("PXHCI_ControllerWorkTest: erstba reg read     - %x\n", erstba.AsULONGLONG);

    DPRINT1("PXHCI_ControllerWorkTest: pointer crcr     - %p %p\n", XhciExtension->OperationalRegs + XHCI_CRCR+1 , XhciExtension->OperationalRegs + XHCI_CRCR);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstz     - %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTSZ);
    DPRINT1("PXHCI_ControllerWorkTest: pointer erstba     - %p %p\n", XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA+1 , XhciExtension->RunTimeRegisterBase + XHCI_ERSTBA);

    return MP_STATUS_SUCCESS;
}
