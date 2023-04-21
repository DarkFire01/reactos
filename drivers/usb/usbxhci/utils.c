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
/* Ooga booga random util file go brr */

/*
 * In the XHCI spec the most basic hardware transfer unit of measurement is
 * a Trb, And these can include commands, events but this function  is for things that go
 * on the transfer ring
 */
MPSTATUS
NTAPI
PXHCI_GenerateTransferTrb(_In_ PXHCI_EXTENSION XhciExtension,
                          _In_ PXHCI_ENDPOINT  XhciEndpoint)
{
    /* Acquire device context */
    return MP_STATUS_SUCCESS;
}
#if 1
UINT8
NTAPI
PXHCI_FindTheHeckingSlotIDFromDevAddr(_In_  PXHCI_EXTENSION XhciExtension,
                                      _In_  UINT8           BusDevAddr)
{
  UINT8  Index;

  for (Index = 0; Index < 255; Index++) {
    if (XhciExtension->DeviceContext[Index + 1].Enabled &&
        (XhciExtension->DeviceContext[Index + 1].SlotId != 0) &&
        (XhciExtension->DeviceContext[Index + 1].BusDevAddr == BusDevAddr))
    {
      break;
    }
  }

  if (Index == 255) {
    return 0;
  }

  return XhciExtension->DeviceContext[Index + 1].SlotId;
}
#endif

/* ooga booga check trb status */
MPSTATUS
NTAPI
PXHCI_CheckTrbResult(_In_    PXHCI_EXTENSION XhciExtension,
                     _Inout_ PXHCI_EVENT_TRB eventTRB)
{
    PXHCI_HC_RESOURCES HcResourcesVA;
    ULONG CheckCompletion;
    PXHCI_TRB dequeue_pointer;
    /* We are going to write to the EventTRB here */
    /* Assume failure */
    CheckCompletion = INVALID;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    /* Grab the Dequeue pointer */
    dequeue_pointer = HcResourcesVA-> EventRing.dequeue_pointer;
    /* This TRB is equal to the last latest eventTRB */
    //TODO: This code doesn't handle synchronized events, that's really bad :D
    *eventTRB = (*dequeue_pointer).EventTRB;

    while (!CheckCompletion)
    {
        CheckCompletion = eventTRB->CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {

            break;
        }
    }

    return MP_STATUS_SUCCESS;
}

/* ooga booga ring xhci doorbell to trigger any active trbs */
MPSTATUS
NTAPI
PXHCI_DingDongMotherfucker(_In_ PXHCI_EXTENSION XhciExtension,
                           _In_ UINT32 SlotId,
                           _In_ UINT32 Dci)
{
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL Doorbell_0;
    DoorBellRegisterBase = XhciExtension->DoorBellRegisterBase;
    if (SlotId == 0) {
        Doorbell_0.DoorBellTarget = 0;
        Doorbell_0.RsvdZ = 0;
        Doorbell_0.AsULONG = 0;
        WRITE_REGISTER_ULONG(DoorBellRegisterBase, Doorbell_0.AsULONG);
    } else {
        DPRINT1("Commiting le ding dong on slot ID specific device is UNIMPLEMENTED\n");
    }
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
PXHCI_ExecTransfer (_In_  PXHCI_EXTENSION XhciExtension,
                 _In_  BOOLEAN            CmdTransfer,
                 _In_  XHCI_ENDPOINT YourLocalEndpoint)
{
    XHCI_EVENT_TRB eventTRB = {0};
    if (CmdTransfer)
    {
        PXHCI_DingDongMotherfucker( XhciExtension, 0, 0);
    }

    PXHCI_CheckTrbResult(XhciExtension,
                          & eventTRB);

}

/**
  Create a command transfer TRB to support XHCI command interfaces.

  @param  Xhc       The XHCI Instance.
  @param  CmdTrb    The cmd TRB to be executed.

  @return Created URB or NULL.

**/
XHCI_ENDPOINT
NTAPI
PXHCI_CreateCmdTrb (_In_ PXHCI_EXTENSION XhciExtension,
                    _In_ PXHCI_TRB       CmdTrb
  )
{
  /* Technically creating an entire Transferdescriptor */
  XHCI_ENDPOINT FakeEndPoint = {0};

  FakeEndPoint.TransferRing = &XhciExtension->HcResourcesVA->CommandRing;
  PXHCI_SyncTrsRing (XhciExtension, &XhciExtension->HcResourcesVA->CommandRing);
  FakeEndPoint.RemainTDs = 1;
  FakeEndPoint.MaxTDs = 2;
  
  FakeEndPoint.RemainTDs = 1;
  FakeEndPoint.FirstTrb = FakeEndPoint.TransferRing->enqueue_pointer;
  RtlCopyMemory (FakeEndPoint.DmaBufferVA, &CmdTrb->GenericTRB, sizeof (XHCI_GENERIC_TRB));
  FakeEndPoint.FirstTrb->CommandTRB.NoOperation.CycleBit = FakeEndPoint.TransferRing->ProducerCycleState & BIT0;
  FakeEndPoint.LastTrb            = FakeEndPoint.FirstTrb;

  return FakeEndPoint;
}

/**
  Synchronize the specified transfer ring to update the enqueue and dequeue pointer.

  @param  XhciExtension        The XHCI Instance.
  @param  TrsRing     The transfer ring to sync.

  @retval EFI_SUCCESS The transfer ring is synchronized successfully.

**/
MPSTATUS
NTAPI
PXHCI_SyncTrsRing (_In_ PXHCI_EXTENSION XhciExtension,
                   _In_ PXHCI_TRANSFER_RING      TrsRing)
{
  UINT32         Index;
  XHCI_TRB  *TrsTrb;

  ASSERT (TrsRing != NULL);
  //
  // Calculate the latest RingEnqueue and RingPCS
  //
  TrsTrb = TrsRing->enqueue_pointer;
  ASSERT (TrsTrb != NULL);

  for (Index = 0; Index < TrsRing->TrbNumber; Index++) {
    if (TrsTrb->ControlTRB.StatusTRB.CycleBit != (TrsRing->ProducerCycleState & BIT0)) {
      break;
    }

    TrsTrb++;
    if ((UINT8)TrsTrb->LinkTRB.TRBType == TRB_TYPE_LINK) {
      ASSERT (((XHCI_LINK_TRB*)TrsTrb)->ToggleCycle != 0);
      //
      // set cycle bit in Link TRB as normal
      //
      ((XHCI_LINK_TRB*)TrsTrb)->CycleBit = TrsRing->ProducerCycleState & BIT0;
      //
      // Toggle PCS maintained by software
      //
      TrsRing->ProducerCycleState = (TrsRing->ProducerCycleState & BIT0) ? 0 : 1;
      TrsTrb           = TrsRing->firstSeg.XhciTrb; // Use host address
    }
  }

  ASSERT (Index != TrsRing->TrbNumber);

  if (TrsTrb != TrsRing->enqueue_pointer) {
    TrsRing->enqueue_pointer = TrsTrb;
  }

  //
  // Clear the Trb context for enqueue, but reserve the PCS bit
  //
  TrsTrb->GenericTRB.Word0 = 0;
  TrsTrb->GenericTRB.Word0 = 0;
  return 0;
}