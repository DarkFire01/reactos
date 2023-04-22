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

    //__debugbreak();
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

VOID
NTAPI
PXHCI_AssignSlot(_Inout_ PXHCI_EXTENSION xhciExtension, _In_ ULONG PortID)
{
    /* 4.3.2 of the Intel xHCI spec */
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};
    XHCI_TRB trb;
    XHCI_TRB eventtrb;

    ULONG SlotID, CheckCompletion;
    DPRINT("PXHCI_AssignSlot: Initiated.\n");
    trb.CommandTRB.SlotEnable.RsvdZ1 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ2 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ3 = 0;
    trb.CommandTRB.SlotEnable.RsvdZ5 = 0;
    trb.CommandTRB.SlotEnable.CycleBit = 1;
    trb.CommandTRB.SlotEnable.RsvdZ4 = 0;
    trb.CommandTRB.SlotEnable.TRBType = ENABLE_SLOT_COMMAND;
    trb.CommandTRB.SlotEnable.SlotType = 1;
    XHCI_SendCommand(trb,xhciExtension);
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

    XHCI_ProcessEvent(xhciExtension);
    eventtrb =  xhciExtension->HcResourcesVA -> EventRing.firstSeg.XhciTrb[0];
    SlotID = eventtrb.EventTRB.CommandCompletionTRB.SlotID;
    CheckCompletion = eventtrb.EventTRB.CommandCompletionTRB.CompletionCode;
    DPRINT1("PXHCI_AssignSlot: The Slot ID assigned is %X\n", SlotID);
    DPRINT1("PXHCI_AssignSlot: The CheckCompletion assigned is %X\n", CheckCompletion);
    xhciExtension->DeviceContext[PortID].Enabled = TRUE;
    PXHCI_InitSlot(xhciExtension, PortID, SlotID);
}

MPSTATUS
NTAPI
PXHCI_InitSlot2(IN PXHCI_EXTENSION XhciExtension, ULONG PortID, ULONG SlotID);
/*
 *  Okay so we are now querying the endpoint, and we have a device being setup for the first time
 *  let's make sure the xHC knows what the heckin heckers is going on.
 */
VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID, ULONG SlotID)
{
    PXHCI_InitSlot2(xhciExtension, PortID, SlotID);

    __debugbreak();
    #if 0
    __debugbreak();
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};

    /* 4.3.3 of the Intel xHCI spec */
    PXHCI_OUTPUT_DEVICE_CONTEXT HcOutputDeviceContext;
    PXHCI_TRANSFER_RING HcTransferControlRing;
    PXHCI_INPUT_CONTEXT HcInputContext;
    PXHCI_SLOT_CONTEXT HcSlotContext;
    PXHCI_ENDPOINT_CONTEXT HcDefaultEndpoint;
    PXHCI_HC_RESOURCES HcResourcesVA;
    PXHCI_EXTENSION XhciExtension;

    PHYSICAL_ADDRESS Zero, Max, BaseAddress;
    XHCI_TRB eventTRB;
    PULONG OperationalRegs;
    ULONG CheckCompletion;
    ULONG_PTR TrDeqPtr;
    XHCI_TRB Trb;
    ULONG_PTR  BufferArrayPointer;
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;

    OperationalRegs = xhciExtension->OperationalRegs;
    CheckCompletion = INVALID;

    Zero.QuadPart = 0;
    Max.QuadPart = -1;

    BufferArrayPointer = MmAllocateContiguousMemory(sizeof(XHCI_OUTPUT_DEVICE_CONTEXT), Max);
    if (BufferArrayPointer == NULL)
    {
        DPRINT("XHCI_InitializeResources  : Scratch pad array ContiguousMemory allcoation fail NULL\n");
        return MP_STATUS_FAILURE;
    }

    BaseAddress.QuadPart =  BufferArrayPointer << PAGE_SHIFT;

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

    /* ah yes, let's just open the fucking control endpoint to get the USB to addressing mode*/
    HcDefaultEndpoint->EPType = 4;
    HcDefaultEndpoint->MaxBurstSize = 0;
    HcDefaultEndpoint->TRDeqPtr = TrDeqPtr; /* Fuck you USB 3.0 */
    HcDefaultEndpoint->DCS = 1;
    HcDefaultEndpoint->Interval = 0;
    HcDefaultEndpoint->MaxPStreams = 0;
    HcDefaultEndpoint->Mult = 0;
    HcDefaultEndpoint->CErr = 3;

    HcOutputDeviceContext->SlotContext = HcSlotContext;

    XHCI_Write64bitReg(OperationalRegs + XHCI_DCBAAP, (ULONG_PTR)HcOutputDeviceContext);

    /* 6.4.3.4 */
    Trb.CommandTRB.AddressDevice.RsvdZ1 = 0;
    Trb.CommandTRB.AddressDevice.InputContextPtrLow = (ULONG)(&HcInputContext->InputContext);
#ifdef _M_AMD64
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = (ULONG)(&HcInputContext->InputContext) >> 32;//(HcInputContext->InputContext >> 32);
#else
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = 0;
#endif
    Trb.CommandTRB.AddressDevice.RsvdZ2 = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ3 = 0;
    Trb.CommandTRB.AddressDevice.CycleBit = 1;
    Trb.CommandTRB.AddressDevice.RsvdZ4 = 0;
    Trb.CommandTRB.AddressDevice.BSR = 0;
    Trb.CommandTRB.AddressDevice.TRBType = ADDRESS_DEVICE_COMMAND;
    Trb.CommandTRB.AddressDevice.SlotID = SlotID;
    XHCI_SendCommand(Trb,XhciExtension);
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
    XHCI_ProcessEvent(XhciExtension);
    // check for event completion trb
    eventTRB =  HcResourcesVA -> EventRing.firstSeg.XhciTrb[0];
    SlotID = eventTRB.EventTRB.CommandCompletionTRB.SlotID;
    CheckCompletion = eventTRB.EventTRB.CommandCompletionTRB.CompletionCode;
    DPRINT1("PXHCI_AssignSlot: The Slot ID assigned is %X\n", SlotID);
    DPRINT1("PXHCI_AssignSlot: The CheckCompletion assigned is %X\n", CheckCompletion);
    DPRINT1("PXHCI_AssignSlot: the othe rvalue is  %X\n", eventTRB.EventTRB.CommandCompletionTRB.CommandTRBPointerHi);
    DPRINT1("the USB address is %X\n", HcOutputDeviceContext->SlotContext->USBDeviceAddress);
    xhciExtension->DeviceContext[PortID].Enabled = TRUE;
    __debugbreak();
    #endif
}



/* ******************************************************************************************************************/

MPSTATUS
NTAPI  /* 4.3.3 */
PXHCI_InitSlot2(IN PXHCI_EXTENSION XhciExtension, ULONG PortID, ULONG SlotID)
{

    PHYSICAL_ADDRESS Max, BaseAddress;
    PXHCI_TRANSFER_RING FirstTransferRing;
    PULONG OperationalRegs;
    XHCI_TRB Trb;
    ULONG i;

    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};

   //Zero.QuadPart = 0;
    Max.QuadPart = -1;
    OperationalRegs = XhciExtension->OperationalRegs;

    /* This part is stupid ill try to explain it. */

    /* Allocate a InputDeviceContext and reset the entire struct to zero */
    PXHCI_INPUT_DEVICE_CONTEXT HcInputDeviceContext = MmAllocateContiguousMemory(sizeof(XHCI_INPUT_DEVICE_CONTEXT), Max);
    RtlZeroMemory((PVOID)HcInputDeviceContext, sizeof(XHCI_INPUT_DEVICE_CONTEXT));
    if (HcInputDeviceContext == NULL)
    {
        DPRINT("PXHCI_InitSlot  : XHCI_INPUT_DEVICE_CONTEXT ContiguousMemory allcoation fail NULL\n");
        return MP_STATUS_FAILURE;
    }

    /* allocate and zero a Output Device context */
    PXHCI_OUTPUT_DEVICE_CONTEXT HcOutputDeviceContext = MmAllocateContiguousMemory(sizeof(XHCI_OUTPUT_DEVICE_CONTEXT), Max);
    RtlZeroMemory((PVOID)HcOutputDeviceContext, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT));
    if (HcOutputDeviceContext == NULL)
    {
        DPRINT("PXHCI_InitSlot  : XHCI_OUTPUT_DEVICE_CONTEXT ContiguousMemory allcoation fail NULL\n");
        return MP_STATUS_FAILURE;
    }

    /* Slot context and endpoint 0 context are affected by this interfacing  */
    HcInputDeviceContext->InputContext.A0 = 1;
    HcInputDeviceContext->InputContext.A1 = 1;

    HcInputDeviceContext->SlotContext.RootHubPortNumber =  PortID; /* Given by system software */
    HcInputDeviceContext->SlotContext.RouteString = 0;  /* This is a hack at best */
    HcInputDeviceContext->SlotContext.ContextEntries = 1; /* there's now one context entry whoa uwu */

    /* TRANSFER RING SETUP */
    /* Okay so since we are technically opening an endpoint let's create a transfer ring now so we got that going for us */
    XhciExtension->DeviceContext[PortID].EndpointTransferRing[0] = MmAllocateContiguousMemory(sizeof(XHCI_TRANSFER_RING), Max);
    if (XhciExtension->DeviceContext[PortID].EndpointTransferRing[0] == NULL)
    {
        DPRINT("PXHCI_InitSlot  : transfer ring ContiguousMemory allcoation fail NULL\n");
        return MP_STATUS_FAILURE;
    }
    FirstTransferRing = (PXHCI_TRANSFER_RING)(XhciExtension->DeviceContext[PortID].EndpointTransferRing[0]);
    FirstTransferRing->enqueue_pointer = &(FirstTransferRing->firstSeg.XhciTrb[0]);
    FirstTransferRing->dequeue_pointer = &(FirstTransferRing->firstSeg.XhciTrb[0]);
    for (i=0; i<256; i++)
    {
        FirstTransferRing->firstSeg.XhciTrb[i].GenericTRB.Word0 = 0;
        FirstTransferRing->firstSeg.XhciTrb[i].GenericTRB.Word1 = 0;
        FirstTransferRing->firstSeg.XhciTrb[i].GenericTRB.Word2 = 0;
        FirstTransferRing->firstSeg.XhciTrb[i].GenericTRB.Word3 = 0;
    }
    FirstTransferRing->ProducerCycleState = 1;
    FirstTransferRing->ConsumerCycleState = 1;
    /* END OF TRANSFER RING SETUP */

    /* ah yes, let's just open the fucking control endpoint to get the USB to addressing mode*/
    HcInputDeviceContext->EndpointList[0].EPType = 4;
    HcInputDeviceContext->EndpointList[0].MaxBurstSize = 1; /* what */
    HcInputDeviceContext->EndpointList[0].TRDeqPtr = (ULONG_PTR)FirstTransferRing->dequeue_pointer; /* Fuck you USB 3.0 */
    HcInputDeviceContext->EndpointList[0].DCS = 1;
    HcInputDeviceContext->EndpointList[0].Interval = 0;
    HcInputDeviceContext->EndpointList[0].MaxPStreams = 0;
    HcInputDeviceContext->EndpointList[0].Mult = 0;
    HcInputDeviceContext->EndpointList[0].CErr = 3; /* maybe im blind but why does every single endpoint neeed this value */
    /* What does it mean, does it mean anyting? why are we here why am i doing this, when can i see my family again i miss my kids*/


    HcOutputDeviceContext->SlotContext = HcInputDeviceContext->SlotContext;
    XHCI_Write64bitReg(OperationalRegs + XHCI_DCBAAP, (ULONG_PTR)HcOutputDeviceContext);

    BaseAddress.QuadPart = (ULONG_PTR)HcInputDeviceContext << PAGE_SHIFT;
    /* 6.4.3.4 */
    Trb.CommandTRB.AddressDevice.RsvdZ1 = 0;
    Trb.CommandTRB.AddressDevice.InputContextPtrLow = (ULONG)BaseAddress.LowPart;
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = (ULONG)BaseAddress.HighPart;
    Trb.CommandTRB.AddressDevice.RsvdZ2 = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ3 = 0;
    Trb.CommandTRB.AddressDevice.CycleBit = 1;
    Trb.CommandTRB.AddressDevice.RsvdZ4 = 0;
    Trb.CommandTRB.AddressDevice.BSR = 0;
    Trb.CommandTRB.AddressDevice.TRBType = ADDRESS_DEVICE_COMMAND;
    Trb.CommandTRB.AddressDevice.SlotID = 1;
    XHCI_SendCommand(Trb,XhciExtension);
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
    XHCI_ProcessEvent(XhciExtension);
    /* All of this is literally just to setup the MINIMAL ENVIRONMENT to start talking to the usb device
     * thanks intel
     */
    return 0;
}