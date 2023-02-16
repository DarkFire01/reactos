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

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID)
{
    BOOLEAN DeviceInsertedEvent = TRUE;
    #if 0
    PXHCI_PORT_STATUS_REGISTER PortStatusReg;
    BOOLEAN DeviceInsertedEvent = TRUE;
    PULONG OperationalRegs;
    PXHCI_PORT_STATUS_REGISTER RegValue;
    OperationalRegs = XhciExtension->OperationalRegs;

    /* 5.4.8 */
    (ULONG)RegValue = READ_REGISTER_ULONG(OperationalRegs + (XHCI_PORTSC + (0x10 * (PortID - 1))));
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
    #endif
    if(DeviceInsertedEvent == TRUE)
    {
        /* Attached: */
        DPRINT1("PXHCI_PortStatusChange: USB device has been inserted from port: %X\n", PortID);
        PXHCI_AssignSlot(XhciExtension, PortID);
    }
    else
    {
        /* Detached:
         *    - CCS -> 0
         *    - CSC -> 1
         */
        DPRINT1("PXHCI_PortStatusChange: USB device has been removed from port: %X\n", PortID);
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
    DPRINT1("PXHCI_AssignSlot: Assigning Slot.\n");
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

    DPRINT1("PXHCI_AssignSlot: The Slot ID assigned is %X\n", SlotID);
    PXHCI_InitSlot(xhciExtension, PortID, SlotID);
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
        SlotID = eventTRB->CommandCompletionTRB.SlotID;
        CheckCompletion = eventTRB->CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {
            KeStallExecutionProcessor(10);
            break;
        }
    }
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

    #if 1
 //   PEHCI_HCD_QH QH;
  // ULONG QhPA;
   PXHCI_HCD_TD TdVA;
    // PEHCI_HCD_TD TD;
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

  //  RtlZeroMemory(XhciEndpoint->DmaBufferVA, sizeof(XHCI_HCD_TD));
#if 0
    QH = (PEHCI_HCD_QH)EhciEndpoint->DmaBufferVA + 1;
    QhPA = EhciEndpoint->DmaBufferPA + sizeof(EHCI_HCD_TD);

    EhciEndpoint->FirstTD = (PEHCI_HCD_TD)(QH + 1);
#endif
    TdCount = (EndpointProperties->BufferLength -
               (sizeof(XHCI_HCD_TD))) /
               sizeof(XHCI_HCD_TD);

    if (EndpointProperties->TransferType == USBPORT_TRANSFER_TYPE_CONTROL)
        XhciEndpoint->EndpointStatus |= USBPORT_ENDPOINT_CONTROL;

    XhciEndpoint->MaxTDs = TdCount;
    XhciEndpoint->RemainTDs = TdCount;
    TdVA = (PXHCI_HCD_TD)XhciEndpoint->FirstTD;
    //TdPA = QhPA + sizeof(XHCI_HCD_QH);

    for (ix = 0; ix < TdCount; ix++)
    {

      //  RtlZeroMemory(TdVA, sizeof(XHCI_HCD_TD));

       // ASSERT((TdPA & ~LINK_POINTER_MASK) == 0);

      //  TdVA->PhysicalAddress = TdPA;
       // TdVA->XhciEndpoint = XhciEndpoint;
       // TdVA->XhciTransfer = NULL;

       // TdPA += sizeof(XHCI_HCD_TD);
       // TdVA += 1;
    }

    __debugbreak();
#endif
  #if 0
    EhciEndpoint->QH = EHCI_InitializeQH(EhciExtension,
                                         EhciEndpoint,
                                         QH,
                                         QhPA);

    if (IsControl)
    {
        QH->sqh.HwQH.EndpointParams.DataToggleControl = 1;
        EhciEndpoint->HcdHeadP = NULL;
    }
    else
    {
        QH->sqh.HwQH.EndpointParams.DataToggleControl = 0;
    }

    TD = EHCI_AllocTd(EhciExtension, EhciEndpoint);

    if (!TD)
        return MP_STATUS_NO_RESOURCES;

    TD->TdFlags |= EHCI_HCD_TD_FLAG_DUMMY;
    TD->HwTD.Token.Status &= (UCHAR)~EHCI_TOKEN_STATUS_ACTIVE;

    TD->HwTD.NextTD = TERMINATE_POINTER;
    TD->HwTD.AlternateNextTD = TERMINATE_POINTER;

    TD->NextHcdTD = NULL;
    TD->AltNextHcdTD = NULL;

    EhciEndpoint->HcdTailP = TD;
    EhciEndpoint->HcdHeadP = TD;

    QH->sqh.HwQH.CurrentTD = TD->PhysicalAddress;
    QH->sqh.HwQH.NextTD = TERMINATE_POINTER;
    QH->sqh.HwQH.AlternateNextTD = TERMINATE_POINTER;

    QH->sqh.HwQH.Token.Status &= (UCHAR)~EHCI_TOKEN_STATUS_ACTIVE;
    QH->sqh.HwQH.Token.TransferBytes = 0;
#endif
DPRINT1("Exiting open endpoint \n");
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
