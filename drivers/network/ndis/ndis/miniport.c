/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS library
 * FILE:        ndis/miniport.c
 * PURPOSE:     Routines used by NDIS miniport drivers
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 *              Vizzini (vizzini@plasmic.com)
 * REVISIONS:
 *   CSH 01/08-2000 Created
 *   20 Aug 2003 vizzini - DMA support
 *   3  Oct 2003 vizzini - SendPackets support
 */

#include "ndissys.h"

#include <ndisguid.h>

/*
 * Define to 1 to get a debugger breakpoint at the end of NdisInitializeWrapper
 * for each new miniport starting up
 */
#define BREAK_ON_MINIPORT_INIT 0

/*
 * This has to be big enough to hold the results of querying the Route value
 * from the Linkage key.  Please re-code me to determine this dynamically.
 */
#define ROUTE_DATA_SIZE 256

/* global list and lock of Miniports NDIS has registered */
LIST_ENTRY MiniportListHead;
KSPIN_LOCK MiniportListLock;

/* global list and lock of adapters NDIS has registered */
LIST_ENTRY AdapterListHead;
KSPIN_LOCK AdapterListLock;

#if DBG
VOID
MiniDisplayPacket(
    PNDIS_PACKET Packet,
    PCSTR Reason)
{
    ULONG i, Length;
    UCHAR Buffer[64];
    if ((DebugTraceLevel & DEBUG_PACKET) > 0) {
        Length = CopyPacketToBuffer(
            Buffer,
            Packet,
            0,
            64);

        DbgPrint("*** %s PACKET START (%p) ***\n", Reason, Packet);

        for (i = 0; i < Length; i++) {
            if (i % 16 == 0)
                DbgPrint("\n%04X ", i);
            DbgPrint("%02X ", Buffer[i]);
        }

        DbgPrint("\n*** %s PACKET STOP ***\n", Reason);
    }
}

#endif /* DBG */

PNDIS_MINIPORT_WORK_ITEM
MiniGetFirstWorkItem(
    PLOGICAL_ADAPTER Adapter,
    NDIS_WORK_ITEM_TYPE Type)
{
    PNDIS_MINIPORT_WORK_ITEM CurrentEntry = Adapter->WorkQueueHead;

    while (CurrentEntry)
    {
      if (CurrentEntry->WorkItemType == Type || Type == NdisMaxWorkItems)
          return CurrentEntry;

      CurrentEntry = (PNDIS_MINIPORT_WORK_ITEM)CurrentEntry->Link.Next;
    }

    return NULL;
}

BOOLEAN
MiniIsBusy(
    PLOGICAL_ADAPTER Adapter,
    NDIS_WORK_ITEM_TYPE Type)
{
    BOOLEAN Busy = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);

    if (MiniGetFirstWorkItem(Adapter, Type))
    {
        Busy = TRUE;
    }
    else if (Type == NdisWorkItemSend && Adapter->NdisMiniportBlock.FirstPendingPacket)
    {
       Busy = TRUE;
    }
    else if (Type == NdisWorkItemResetRequested &&
             Adapter->NdisMiniportBlock.ResetStatus == NDIS_STATUS_PENDING)
    {
       Busy = TRUE;
    }

    KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);

    return Busy;
}

VOID NTAPI
MiniResetComplete(
    IN  NDIS_HANDLE MiniportAdapterHandle,
    IN  NDIS_STATUS Status,
    IN  BOOLEAN     AddressingReset)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PLIST_ENTRY CurrentEntry;
    PADAPTER_BINDING AdapterBinding;
    KIRQL OldIrql;

    if (AddressingReset)
        MiniDoAddressingReset(Adapter);

    CoreIndicateStatusCode(Adapter, NDIS_STATUS_RESET_END);

    KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);

    if (Adapter->NdisMiniportBlock.ResetStatus != NDIS_STATUS_PENDING)
    {
        KeBugCheckEx(BUGCODE_ID_DRIVER,
                     (ULONG_PTR)MiniportAdapterHandle,
                     (ULONG_PTR)Status,
                     (ULONG_PTR)AddressingReset,
                     0);
    }

    Adapter->NdisMiniportBlock.ResetStatus = Status;

    CurrentEntry = Adapter->ProtocolListHead.Flink;

    while (CurrentEntry != &Adapter->ProtocolListHead)
    {
        AdapterBinding = CONTAINING_RECORD(CurrentEntry, ADAPTER_BINDING, AdapterListEntry);

        (*AdapterBinding->ProtocolBinding->Chars.ResetCompleteHandler)(
               AdapterBinding->NdisOpenBlock.ProtocolBindingContext,
               Status);

        CurrentEntry = CurrentEntry->Flink;
    }

    KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);
}

BOOLEAN
MiniAdapterHasAddress(
    PLOGICAL_ADAPTER Adapter,
    PNDIS_PACKET Packet)
/*
 * FUNCTION: Determines whether a packet has the same destination address as an adapter
 * ARGUMENTS:
 *     Adapter = Pointer to logical adapter object
 *     Packet  = Pointer to NDIS packet
 * RETURNS:
 *     TRUE if the destination address is that of the adapter, FALSE if not
 */
{
    UINT Length;
    PUCHAR PacketAddress;
    PUCHAR AdapterAddress;
    PNDIS_BUFFER NdisBuffer;
    UINT BufferLength;

    NDIS_DbgPrint(DEBUG_MINIPORT, ("Called.\n"));

#if DBG
    if(!Adapter)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Adapter object was null\n"));
        return FALSE;
    }

    if(!Packet)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Packet was null\n"));
        return FALSE;
    }
#endif

    NdisQueryPacket(Packet, NULL, NULL, &NdisBuffer, NULL);

    if (!NdisBuffer)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Packet contains no buffers.\n"));
        return FALSE;
    }

    NdisQueryBuffer(NdisBuffer, (PVOID)&PacketAddress, &BufferLength);

    /* FIXME: Should handle fragmented packets */

    switch (Adapter->NdisMiniportBlock.MediaType)
    {
        case NdisMedium802_3:
            Length = ETH_LENGTH_OF_ADDRESS;
            /* Destination address is the first field */
            break;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Adapter has unsupported media type (0x%X).\n", Adapter->NdisMiniportBlock.MediaType));
            return FALSE;
    }

    if (BufferLength < Length)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Buffer is too small.\n"));
        return FALSE;
    }

    AdapterAddress = (PUCHAR)&Adapter->Address;
    NDIS_DbgPrint(MAX_TRACE, ("packet address: %x:%x:%x:%x:%x:%x adapter address: %x:%x:%x:%x:%x:%x\n",
                              *(PacketAddress), *(PacketAddress+1), *(PacketAddress+2), *(PacketAddress+3), *(PacketAddress+4), *(PacketAddress+5),
                              *(AdapterAddress), *(AdapterAddress+1), *(AdapterAddress+2), *(AdapterAddress+3), *(AdapterAddress+4), *(AdapterAddress+5)));

    return (RtlCompareMemory(PacketAddress, AdapterAddress, Length) == Length);
}


PLOGICAL_ADAPTER
MiniLocateDevice(
    PNDIS_STRING AdapterName)
/*
 * FUNCTION: Finds an adapter object by name
 * ARGUMENTS:
 *     AdapterName = Pointer to name of adapter
 * RETURNS:
 *     Pointer to logical adapter object, or NULL if none was found.
 *     If found, the adapter is referenced for the caller. The caller
 *     is responsible for dereferencing after use
 */
{
    KIRQL OldIrql;
    PLIST_ENTRY CurrentEntry;
    PLOGICAL_ADAPTER Adapter = 0;

    ASSERT(AdapterName);

    NDIS_DbgPrint(DEBUG_MINIPORT, ("Called.\n"));

    if(IsListEmpty(&AdapterListHead))
    {
        NDIS_DbgPrint(MIN_TRACE, ("No registered miniports for protocol to bind to\n"));
        return NULL;
    }

    NDIS_DbgPrint(DEBUG_MINIPORT, ("AdapterName = %wZ\n", AdapterName));

    KeAcquireSpinLock(&AdapterListLock, &OldIrql);
    {
        CurrentEntry = AdapterListHead.Flink;

        while (CurrentEntry != &AdapterListHead)
        {
            Adapter = CONTAINING_RECORD(CurrentEntry, LOGICAL_ADAPTER, ListEntry);

            ASSERT(Adapter);

            NDIS_DbgPrint(DEBUG_MINIPORT, ("Examining adapter 0x%lx\n", Adapter));

            /* We're technically not allowed to call this above PASSIVE_LEVEL, but it doesn't break
             * right now and I'd rather use a working API than reimplement it here */
            if (RtlCompareUnicodeString(AdapterName, &Adapter->NdisMiniportBlock.MiniportName, TRUE) == 0)
            {
                break;
            }

            Adapter = NULL;
            CurrentEntry = CurrentEntry->Flink;
        }
    }
    KeReleaseSpinLock(&AdapterListLock, OldIrql);

    if(Adapter)
    {
        NDIS_DbgPrint(DEBUG_MINIPORT, ("Leaving. Adapter found at 0x%x\n", Adapter));
    }
    else
    {
        NDIS_DbgPrint(MIN_TRACE, ("Leaving (adapter not found for %wZ).\n", AdapterName));
    }

    return Adapter;
}

static IO_WORKITEM_ROUTINE MiniRestoreAddressing;

/* The miniport lost its addressing in a reset: give it back what NDIS cached */
static
VOID
NTAPI
MiniRestoreAddressing(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PLOGICAL_ADAPTER Adapter = DeviceObject->DeviceExtension;
    ULONG BytesRead;
    ULONG Value;

    IoFreeWorkItem((PIO_WORKITEM)Context);

    Value = Adapter->Core.CurrentLookahead;
    CoreSetInformation(Adapter, OID_GEN_CURRENT_LOOKAHEAD, &Value, sizeof(Value), &BytesRead);

    if (Adapter->Core.CurrentPacketFilter != 0)
    {
        Value = Adapter->Core.CurrentPacketFilter;
        CoreSetInformation(Adapter, OID_GEN_CURRENT_PACKET_FILTER, &Value, sizeof(Value), &BytesRead);
    }
}

VOID
NTAPI
MiniDoAddressingReset(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PIO_WORKITEM WorkItem;

    /* Resets complete at DISPATCH_LEVEL, and OID requests wait at PASSIVE_LEVEL */
    WorkItem = IoAllocateWorkItem(Adapter->NdisMiniportBlock.DeviceObject);
    if (WorkItem != NULL)
        IoQueueWorkItem(WorkItem, MiniRestoreAddressing, DelayedWorkQueue, WorkItem);
}

static
NDIS_STATUS
MiniStartReset(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    KIRQL OldIrql;
    BOOLEAN AddressingReset;

    CoreIndicateStatusCode(Adapter, NDIS_STATUS_RESET_START);

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Status = MiniCallResetHandler(Adapter, &AddressingReset);

    KeAcquireSpinLockAtDpcLevel(&Adapter->NdisMiniportBlock.Lock);
    Adapter->NdisMiniportBlock.ResetStatus = Status;
    KeReleaseSpinLockFromDpcLevel(&Adapter->NdisMiniportBlock.Lock);

    KeLowerIrql(OldIrql);

    if (Status != NDIS_STATUS_PENDING)
    {
        if (AddressingReset)
            MiniDoAddressingReset(Adapter);

        CoreIndicateStatusCode(Adapter, NDIS_STATUS_RESET_END);

        MiniWorkItemComplete(Adapter, NdisWorkItemResetRequested);
    }

    return Status;
}

NDIS_STATUS
MiniReset(
    PLOGICAL_ADAPTER Adapter)
/*
 * FUNCTION: Resets the miniport
 * ARGUMENTS:
 *     Adapter = Pointer to the logical adapter object
 * RETURNS:
 *     Status of the operation
 */
{
   if (MiniIsBusy(Adapter, NdisWorkItemResetRequested)) {
       MiniQueueWorkItem(Adapter, NdisWorkItemResetRequested, NULL, FALSE);
       return NDIS_STATUS_PENDING;
   }

   return MiniStartReset(Adapter);
}

VOID NTAPI
MiniportHangDpc(
        PKDPC Dpc,
        PVOID DeferredContext,
        PVOID SystemArgument1,
        PVOID SystemArgument2)
{
  PLOGICAL_ADAPTER Adapter = DeferredContext;

  if (MiniCallCheckForHangHandler(Adapter)) {
      NDIS_DbgPrint(MIN_TRACE, ("Miniport detected adapter hang\n"));
      MiniReset(Adapter);
  }
}

VOID
MiniWorkItemComplete(
    PLOGICAL_ADAPTER     Adapter,
    NDIS_WORK_ITEM_TYPE  WorkItemType)
{
    PIO_WORKITEM IoWorkItem;

    /* Check if there's anything queued to run after this work item */
    if (!MiniIsBusy(Adapter, WorkItemType))
        return;

    /* There is, so fire the worker */
    IoWorkItem = IoAllocateWorkItem(Adapter->NdisMiniportBlock.DeviceObject);
    if (IoWorkItem)
        IoQueueWorkItem(IoWorkItem, MiniportWorker, DelayedWorkQueue, IoWorkItem);
}

VOID
FASTCALL
MiniQueueWorkItem(
    PLOGICAL_ADAPTER     Adapter,
    NDIS_WORK_ITEM_TYPE  WorkItemType,
    PVOID                WorkItemContext,
    BOOLEAN              Top)
/*
 * FUNCTION: Queues a work item for execution at a later time
 * ARGUMENTS:
 *     Adapter         = Pointer to the logical adapter object to queue work item on
 *     WorkItemType    = Type of work item to queue
 *     WorkItemContext = Pointer to context information for work item
 * RETURNS:
 *     Status of operation
 */
{
    PNDIS_MINIPORT_WORK_ITEM MiniportWorkItem;
    KIRQL OldIrql;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    ASSERT(Adapter);

    KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);
    if (Top)
    {
        if (WorkItemType == NdisWorkItemSend)
        {
            NDIS_DbgPrint(MIN_TRACE, ("Requeuing failed packet (%x).\n", WorkItemContext));
            Adapter->NdisMiniportBlock.FirstPendingPacket = WorkItemContext;
        }
        else
        {
            //This should never happen
            ASSERT(FALSE);
        }
    }
    else
    {
        MiniportWorkItem = ExAllocatePool(NonPagedPool, sizeof(NDIS_MINIPORT_WORK_ITEM));
        if (!MiniportWorkItem)
        {
            KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);
            NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
            return;
        }

        MiniportWorkItem->WorkItemType    = WorkItemType;
        MiniportWorkItem->WorkItemContext = WorkItemContext;

        /* safe due to adapter lock held */
        MiniportWorkItem->Link.Next = NULL;
        if (!Adapter->WorkQueueHead)
        {
            Adapter->WorkQueueHead = MiniportWorkItem;
            Adapter->WorkQueueTail = MiniportWorkItem;
        }
        else
        {
            Adapter->WorkQueueTail->Link.Next = (PSINGLE_LIST_ENTRY)MiniportWorkItem;
            Adapter->WorkQueueTail = MiniportWorkItem;
        }
    }

    KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);
}

NDIS_STATUS
FASTCALL
MiniDequeueWorkItem(
    PLOGICAL_ADAPTER    Adapter,
    NDIS_WORK_ITEM_TYPE *WorkItemType,
    PVOID               *WorkItemContext)
/*
 * FUNCTION: Dequeues a work item from the work queue of a logical adapter
 * ARGUMENTS:
 *     Adapter         = Pointer to the logical adapter object to dequeue work item from
 *     AdapterBinding  = Address of buffer for adapter binding for this request
 *     WorkItemType    = Address of buffer for work item type
 *     WorkItemContext = Address of buffer for pointer to context information
 * NOTES:
 *     Adapter lock must be held when called
 * RETURNS:
 *     Status of operation
 */
{
    PNDIS_MINIPORT_WORK_ITEM MiniportWorkItem;
    PNDIS_PACKET Packet;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    MiniportWorkItem = Adapter->WorkQueueHead;

    if ((Packet = Adapter->NdisMiniportBlock.FirstPendingPacket))
    {
        Adapter->NdisMiniportBlock.FirstPendingPacket = NULL;

        *WorkItemType = NdisWorkItemSend;
        *WorkItemContext = Packet;

        return NDIS_STATUS_SUCCESS;
    }
    else if (MiniportWorkItem)
    {
        /* safe due to adapter lock held */
        Adapter->WorkQueueHead = (PNDIS_MINIPORT_WORK_ITEM)MiniportWorkItem->Link.Next;

        if (MiniportWorkItem == Adapter->WorkQueueTail)
            Adapter->WorkQueueTail = NULL;

        *WorkItemType    = MiniportWorkItem->WorkItemType;
        *WorkItemContext = MiniportWorkItem->WorkItemContext;

        ExFreePool(MiniportWorkItem);

        return NDIS_STATUS_SUCCESS;
    }
    else
    {
        NDIS_DbgPrint(MIN_TRACE, ("No work item to dequeue\n"));

        return NDIS_STATUS_FAILURE;
    }
}

/*
 * @implemented
 */
#undef NdisMSetInformationComplete
VOID
EXPORT
NdisMSetInformationComplete(
    IN  NDIS_HANDLE MiniportAdapterHandle,
    IN  NDIS_STATUS Status)
{
  PLOGICAL_ADAPTER Adapter =
	(PLOGICAL_ADAPTER)MiniportAdapterHandle;
  KIRQL OldIrql;
  ASSERT(Adapter);
  KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
  if (Adapter->NdisMiniportBlock.SetCompleteHandler)
     (Adapter->NdisMiniportBlock.SetCompleteHandler)(MiniportAdapterHandle, Status);
  KeLowerIrql(OldIrql);
}

/*
 * @implemented
 */
#undef NdisMQueryInformationComplete
VOID
EXPORT
NdisMQueryInformationComplete(
    IN  NDIS_HANDLE MiniportAdapterHandle,
    IN  NDIS_STATUS Status)
{
    PLOGICAL_ADAPTER Adapter =
	(PLOGICAL_ADAPTER)MiniportAdapterHandle;
    KIRQL OldIrql;
    ASSERT(Adapter);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    if( Adapter->NdisMiniportBlock.QueryCompleteHandler )
	(Adapter->NdisMiniportBlock.QueryCompleteHandler)(MiniportAdapterHandle, Status);
    KeLowerIrql(OldIrql);
}

VOID
NTAPI
MiniportWorker(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
  PLOGICAL_ADAPTER Adapter = DeviceObject->DeviceExtension;
  KIRQL OldIrql;
  NDIS_STATUS NdisStatus;
  PVOID WorkItemContext;
  NDIS_WORK_ITEM_TYPE WorkItemType;

  IoFreeWorkItem((PIO_WORKITEM)Context);

  KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);

  NdisStatus =
      MiniDequeueWorkItem
      (Adapter, &WorkItemType, &WorkItemContext);

  KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);

  if (NdisStatus == NDIS_STATUS_SUCCESS)
    {
      switch (WorkItemType)
        {
          case NdisWorkItemSend:
            Mp5SendQueuedPacket(Adapter, (PNDIS_PACKET)WorkItemContext);
            break;

          case NdisWorkItemReturnPackets:
            break;

          case NdisWorkItemResetRequested:
            MiniStartReset(Adapter);
            break;

          case NdisWorkItemResetInProgress:
            break;

          case NdisWorkItemMiniportCallback:
            break;

          default:
            NDIS_DbgPrint(MIN_TRACE, ("Unknown NDIS work item type (%d).\n", WorkItemType));
            break;
        }
    }
}


/*
 * @implemented
 */
VOID
EXPORT
NdisMCloseLog(
    IN  NDIS_HANDLE LogHandle)
{
    PNDIS_LOG Log = (PNDIS_LOG)LogHandle;
    PNDIS_MINIPORT_BLOCK Miniport = Log->Miniport;
    KIRQL OldIrql;

    NDIS_DbgPrint(MAX_TRACE, ("called: LogHandle 0x%x\n", LogHandle));

    KeAcquireSpinLock(&(Miniport)->Lock, &OldIrql);
    Miniport->Log = NULL;
    KeReleaseSpinLock(&(Miniport)->Lock, OldIrql);

    ExFreePool(Log);
}

/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisMCreateLog(
    IN  NDIS_HANDLE     MiniportAdapterHandle,
    IN  UINT            Size,
    OUT PNDIS_HANDLE    LogHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PNDIS_LOG Log;
    KIRQL OldIrql;

    NDIS_DbgPrint(MAX_TRACE, ("called: MiniportAdapterHandle 0x%x, Size %ld\n", MiniportAdapterHandle, Size));

    KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);

    if (Adapter->NdisMiniportBlock.Log)
    {
        *LogHandle = NULL;
        return NDIS_STATUS_FAILURE;
    }

    Log = ExAllocatePool(NonPagedPool, Size + sizeof(NDIS_LOG));
    if (!Log)
    {
        *LogHandle = NULL;
        return NDIS_STATUS_RESOURCES;
    }

    Adapter->NdisMiniportBlock.Log = Log;

    KeInitializeSpinLock(&Log->LogLock);

    Log->Miniport = &Adapter->NdisMiniportBlock;
    Log->TotalSize = Size;
    Log->CurrentSize = 0;
    Log->OutPtr = 0;
    Log->InPtr = 0;
    Log->Irp = NULL;

    *LogHandle = Log;

    KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);

    return NDIS_STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMDeregisterAdapterShutdownHandler(
    IN  NDIS_HANDLE MiniportHandle)
/*
 * FUNCTION: de-registers a shutdown handler
 * ARGUMENTS:  MiniportHandle:  Handle passed into MiniportInitialize
 */
{
  PLOGICAL_ADAPTER  Adapter = (PLOGICAL_ADAPTER)MiniportHandle;

  NDIS_DbgPrint(DEBUG_MINIPORT, ("Called.\n"));

  if (Adapter->BugcheckContext != NULL && Adapter->BugcheckContext->ShutdownHandler) {
    KeDeregisterBugCheckCallback(Adapter->BugcheckContext->CallbackRecord);
    IoUnregisterShutdownNotification(Adapter->NdisMiniportBlock.DeviceObject);
  }
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMFlushLog(
    IN  NDIS_HANDLE LogHandle)
{
    PNDIS_LOG Log = (PNDIS_LOG) LogHandle;
    KIRQL OldIrql;

    NDIS_DbgPrint(MAX_TRACE, ("called: LogHandle 0x%x\n", LogHandle));

    /* Lock object */
    KeAcquireSpinLock(&Log->LogLock, &OldIrql);

    /* Set buffers size */
    Log->CurrentSize = 0;
    Log->OutPtr = 0;
    Log->InPtr = 0;

    /* Unlock object */
    KeReleaseSpinLock(&Log->LogLock, OldIrql);
}

/*
 * @implemented
 */
#undef NdisMIndicateStatus
VOID
EXPORT
NdisMIndicateStatus(
    IN  NDIS_HANDLE MiniportAdapterHandle,
    IN  NDIS_STATUS GeneralStatus,
    IN  PVOID       StatusBuffer,
    IN  UINT        StatusBufferSize)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;

    if (Adapter->NdisMiniportBlock.StatusHandler != NULL)
        Adapter->NdisMiniportBlock.StatusHandler(MiniportAdapterHandle, GeneralStatus, StatusBuffer, StatusBufferSize);
}

/*
 * @implemented
 */
#undef NdisMIndicateStatusComplete
VOID
EXPORT
NdisMIndicateStatusComplete(
    IN  NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;

    if (Adapter->NdisMiniportBlock.StatusCompleteHandler != NULL)
        Adapter->NdisMiniportBlock.StatusCompleteHandler(MiniportAdapterHandle);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisInitializeWrapper(
    OUT PNDIS_HANDLE    NdisWrapperHandle,
    IN  PVOID           SystemSpecific1,
    IN  PVOID           SystemSpecific2,
    IN  PVOID           SystemSpecific3)
/*
 * FUNCTION: Notifies the NDIS library that a new miniport is initializing
 * ARGUMENTS:
 *     NdisWrapperHandle = Address of buffer to place NDIS wrapper handle
 *     SystemSpecific1   = Pointer to the driver's driver object
 *     SystemSpecific2   = Pointer to the driver's registry path
 *     SystemSpecific3   = Always NULL
 * NOTES:
 *     - SystemSpecific2 goes invalid so we copy it
 */
{
  PNDIS_M_DRIVER_BLOCK Miniport;
  PUNICODE_STRING RegistryPath;
  WCHAR *RegistryBuffer;

  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

  ASSERT(NdisWrapperHandle);

  *NdisWrapperHandle = NULL;

#if BREAK_ON_MINIPORT_INIT
  DbgBreakPoint();
#endif

  Miniport = ExAllocatePool(NonPagedPool, sizeof(NDIS_M_DRIVER_BLOCK));

  if (!Miniport)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
      return;
    }

  RtlZeroMemory(Miniport, sizeof(NDIS_M_DRIVER_BLOCK));

  KeInitializeSpinLock(&Miniport->Lock);

  Miniport->DriverObject = (PDRIVER_OBJECT)SystemSpecific1;

  /* set the miniport's driver registry path */
  RegistryPath = ExAllocatePool(PagedPool, sizeof(UNICODE_STRING));
  if(!RegistryPath)
    {
      ExFreePool(Miniport);
      NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
      return;
    }

  RegistryPath->Length = ((PUNICODE_STRING)SystemSpecific2)->Length;
  RegistryPath->MaximumLength = RegistryPath->Length + sizeof(WCHAR);	/* room for 0-term */

  RegistryBuffer = ExAllocatePool(PagedPool, RegistryPath->MaximumLength);
  if(!RegistryBuffer)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
      ExFreePool(Miniport);
      ExFreePool(RegistryPath);
      return;
    }

  RtlCopyMemory(RegistryBuffer, ((PUNICODE_STRING)SystemSpecific2)->Buffer, RegistryPath->Length);
  RegistryBuffer[RegistryPath->Length/sizeof(WCHAR)] = 0;

  RegistryPath->Buffer = RegistryBuffer;
  Miniport->RegistryPath = RegistryPath;

  InitializeListHead(&Miniport->DeviceList);

  /* Put miniport in global miniport list */
  ExInterlockedInsertTailList(&MiniportListHead, &Miniport->ListEntry, &MiniportListLock);

  *NdisWrapperHandle = Miniport;
}

VOID NTAPI NdisIBugcheckCallback(
    IN PVOID   Buffer,
    IN ULONG   Length)
/*
 * FUNCTION:  Internal callback for handling bugchecks - calls adapter's shutdown handler
 * ARGUMENTS:
 *     Buffer:  Pointer to a bugcheck callback context
 *     Length:  Unused
 */
{
  PMINIPORT_BUGCHECK_CONTEXT Context = (PMINIPORT_BUGCHECK_CONTEXT)Buffer;
  ADAPTER_SHUTDOWN_HANDLER sh = (ADAPTER_SHUTDOWN_HANDLER)Context->ShutdownHandler;

   NDIS_DbgPrint(DEBUG_MINIPORT, ("Called.\n"));

  if(sh)
    sh(Context->DriverContext);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMRegisterAdapterShutdownHandler(
    IN  NDIS_HANDLE                 MiniportHandle,
    IN  PVOID                       ShutdownContext,
    IN  ADAPTER_SHUTDOWN_HANDLER    ShutdownHandler)
/*
 * FUNCTION:  Register a shutdown handler for an adapter
 * ARGUMENTS:
 *     MiniportHandle:  Handle originally passed into MiniportInitialize
 *     ShutdownContext:  Pre-initialized bugcheck context
 *     ShutdownHandler:  Function to call to handle the bugcheck
 * NOTES:
 *     - I'm not sure about ShutdownContext
 */
{
  PLOGICAL_ADAPTER            Adapter = (PLOGICAL_ADAPTER)MiniportHandle;
  PMINIPORT_BUGCHECK_CONTEXT  BugcheckContext;

  NDIS_DbgPrint(DEBUG_MINIPORT, ("Called.\n"));

  if (Adapter->BugcheckContext != NULL)
  {
      NDIS_DbgPrint(MIN_TRACE, ("Attempted to register again for a shutdown callback\n"));
      return;
  }

  BugcheckContext = ExAllocatePool(NonPagedPool, sizeof(MINIPORT_BUGCHECK_CONTEXT));
  if(!BugcheckContext)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
      return;
    }

  BugcheckContext->ShutdownHandler = ShutdownHandler;
  BugcheckContext->DriverContext = ShutdownContext;

  BugcheckContext->CallbackRecord = ExAllocatePool(NonPagedPool, sizeof(KBUGCHECK_CALLBACK_RECORD));
  if (!BugcheckContext->CallbackRecord) {
      ExFreePool(BugcheckContext);
      return;
  }

  Adapter->BugcheckContext = BugcheckContext;

  KeInitializeCallbackRecord(BugcheckContext->CallbackRecord);

  KeRegisterBugCheckCallback(BugcheckContext->CallbackRecord, NdisIBugcheckCallback,
      BugcheckContext, sizeof(*BugcheckContext), (PUCHAR)"Ndis Miniport");

  IoRegisterShutdownNotification(Adapter->NdisMiniportBlock.DeviceObject);
}

NTSTATUS
NTAPI
NdisIForwardIrpAndWaitCompletionRoutine(
    PDEVICE_OBJECT Fdo,
    PIRP Irp,
    PVOID Context)
{
  PKEVENT Event = Context;

  if (Irp->PendingReturned)
    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

  return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
NdisIForwardIrpAndWait(PLOGICAL_ADAPTER Adapter, PIRP Irp)
{
  KEVENT Event;
  NTSTATUS Status;

  KeInitializeEvent(&Event, NotificationEvent, FALSE);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, NdisIForwardIrpAndWaitCompletionRoutine, &Event,
                         TRUE, TRUE, TRUE);
  Status = IoCallDriver(Adapter->NdisMiniportBlock.NextDeviceObject, Irp);
  if (Status == STATUS_PENDING)
    {
      KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
      Status = Irp->IoStatus.Status;
    }
  return Status;
}

NTSTATUS
NTAPI
NdisICreateClose(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NdisIPnPStartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
/*
 * FUNCTION: Handle the PnP start device event
 * ARGUMENTS:
 *     DeviceObject = Functional Device Object
 *     Irp          = IRP_MN_START_DEVICE I/O request packet
 * RETURNS:
 *     Status of operation
 */
{
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)DeviceObject->DeviceExtension;
  NDIS_WRAPPER_CONTEXT WrapperContext;
  NDIS_STATUS NdisStatus;
  NTSTATUS Status;
  ULONG ResourceCount;
  ULONG ResourceListSize;
  UNICODE_STRING ParamName;
  PNDIS_CONFIGURATION_PARAMETER ConfigParam;
  NDIS_HANDLE ConfigHandle;
  ULONG Size;
  LARGE_INTEGER Timeout;
  PLIST_ENTRY CurrentEntry;
  PPROTOCOL_BINDING ProtocolBinding;

  /*
   * Prepare wrapper context used by HW and configuration routines.
   */

  NDIS_DbgPrint(DEBUG_MINIPORT, ("Start Device %wZ\n", &Adapter->NdisMiniportBlock.MiniportName));

  NDIS_DbgPrint(MAX_TRACE, ("Inserting adapter 0x%x into adapter list\n", Adapter));

  /* Put adapter in global adapter list */
  ExInterlockedInsertTailList(&AdapterListHead, &Adapter->ListEntry, &AdapterListLock);

  Status = IoOpenDeviceRegistryKey(
    Adapter->NdisMiniportBlock.PhysicalDeviceObject, PLUGPLAY_REGKEY_DRIVER,
    KEY_ALL_ACCESS, &WrapperContext.RegistryHandle);
  if (!NT_SUCCESS(Status))
    {
      NDIS_DbgPrint(MIN_TRACE,("failed to open adapter-specific reg key\n"));
      ExInterlockedRemoveEntryList( &Adapter->ListEntry, &AdapterListLock );
      return Status;
    }

  NDIS_DbgPrint(MAX_TRACE, ("opened device reg key\n"));

  WrapperContext.DeviceObject = Adapter->NdisMiniportBlock.DeviceObject;

  /*
   * Store the adapter resources used by HW routines such as
   * NdisMQueryAdapterResources.
   */

  if (Stack->Parameters.StartDevice.AllocatedResources != NULL)
    {
      ResourceCount = Stack->Parameters.StartDevice.AllocatedResources->List[0].
                      PartialResourceList.Count;
      ResourceListSize =
        FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.
                     PartialDescriptors[ResourceCount]);

      Adapter->NdisMiniportBlock.AllocatedResources =
        ExAllocatePool(PagedPool, ResourceListSize);
      if (Adapter->NdisMiniportBlock.AllocatedResources == NULL)
        {
          NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources\n"));
	  ExInterlockedRemoveEntryList( &Adapter->ListEntry, &AdapterListLock );
          return STATUS_INSUFFICIENT_RESOURCES;
        }

      Adapter->NdisMiniportBlock.Resources =
        ExAllocatePool(PagedPool, ResourceListSize);
      if (!Adapter->NdisMiniportBlock.Resources)
      {
          NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources\n"));
          ExFreePool(Adapter->NdisMiniportBlock.AllocatedResources);
          ExInterlockedRemoveEntryList(&Adapter->ListEntry, &AdapterListLock);
          return STATUS_INSUFFICIENT_RESOURCES;
      }

      RtlCopyMemory(Adapter->NdisMiniportBlock.Resources,
                    Stack->Parameters.StartDevice.AllocatedResources,
                    ResourceListSize);

      RtlCopyMemory(Adapter->NdisMiniportBlock.AllocatedResources,
                    Stack->Parameters.StartDevice.AllocatedResources,
                    ResourceListSize);
    }

  if (Stack->Parameters.StartDevice.AllocatedResourcesTranslated != NULL)
    {
      ResourceCount = Stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0].
                      PartialResourceList.Count;
      ResourceListSize =
        FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.
                     PartialDescriptors[ResourceCount]);

      Adapter->NdisMiniportBlock.AllocatedResourcesTranslated =
        ExAllocatePool(PagedPool, ResourceListSize);
      if (Adapter->NdisMiniportBlock.AllocatedResourcesTranslated == NULL)
        {
          NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources\n"));
	  ExInterlockedRemoveEntryList( &Adapter->ListEntry, &AdapterListLock );
          return STATUS_INSUFFICIENT_RESOURCES;
        }

      RtlCopyMemory(Adapter->NdisMiniportBlock.AllocatedResourcesTranslated,
                    Stack->Parameters.StartDevice.AllocatedResourcesTranslated,
                    ResourceListSize);
   }

  /*
   * Store the Bus Type, Bus Number and Slot information. It's used by
   * the hardware routines then.
   */

  NdisOpenConfiguration(&NdisStatus, &ConfigHandle, (NDIS_HANDLE)&WrapperContext);
  if (NdisStatus != NDIS_STATUS_SUCCESS)
  {
      NDIS_DbgPrint(MIN_TRACE, ("Failed to open configuration key\n"));
      ExInterlockedRemoveEntryList( &Adapter->ListEntry, &AdapterListLock );
      return NdisStatus;
  }

  Size = sizeof(ULONG);
  Status = IoGetDeviceProperty(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                               DevicePropertyLegacyBusType, Size,
                               &Adapter->NdisMiniportBlock.BusType, &Size);
  if (!NT_SUCCESS(Status) || (INTERFACE_TYPE)Adapter->NdisMiniportBlock.BusType == InterfaceTypeUndefined)
    {
      NdisInitUnicodeString(&ParamName, L"BusType");
      NdisReadConfiguration(&NdisStatus, &ConfigParam, ConfigHandle,
                            &ParamName, NdisParameterInteger);
      if (NdisStatus == NDIS_STATUS_SUCCESS)
        Adapter->NdisMiniportBlock.BusType = ConfigParam->ParameterData.IntegerData;
      else
        Adapter->NdisMiniportBlock.BusType = NdisInterfaceIsa;
    }

  Status = IoGetDeviceProperty(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                               DevicePropertyBusNumber, Size,
                               &Adapter->NdisMiniportBlock.BusNumber, &Size);
  if (!NT_SUCCESS(Status) || Adapter->NdisMiniportBlock.BusNumber == 0xFFFFFFF0)
    {
      NdisInitUnicodeString(&ParamName, L"BusNumber");
      NdisReadConfiguration(&NdisStatus, &ConfigParam, ConfigHandle,
                            &ParamName, NdisParameterInteger);
      if (NdisStatus == NDIS_STATUS_SUCCESS)
        Adapter->NdisMiniportBlock.BusNumber = ConfigParam->ParameterData.IntegerData;
      else
        Adapter->NdisMiniportBlock.BusNumber = 0;
    }
  WrapperContext.BusNumber = Adapter->NdisMiniportBlock.BusNumber;

  Status = IoGetDeviceProperty(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                               DevicePropertyAddress, Size,
                               &Adapter->NdisMiniportBlock.SlotNumber, &Size);
  if (!NT_SUCCESS(Status) || Adapter->NdisMiniportBlock.SlotNumber == (NDIS_INTERFACE_TYPE)-1)
    {
      NdisInitUnicodeString(&ParamName, L"SlotNumber");
      NdisReadConfiguration(&NdisStatus, &ConfigParam, ConfigHandle,
                            &ParamName, NdisParameterInteger);
      if (NdisStatus == NDIS_STATUS_SUCCESS)
        Adapter->NdisMiniportBlock.SlotNumber = ConfigParam->ParameterData.IntegerData;
      else
        Adapter->NdisMiniportBlock.SlotNumber = 0;
    }
  else
    {
        /* Convert slotnumber to PCI_SLOT_NUMBER */
        ULONG PciSlotNumber = Adapter->NdisMiniportBlock.SlotNumber;
        PCI_SLOT_NUMBER SlotNumber;

        SlotNumber.u.AsULONG = 0;
        SlotNumber.u.bits.DeviceNumber = (PciSlotNumber >> 16) & 0xFFFF;
        SlotNumber.u.bits.FunctionNumber = PciSlotNumber & 0xFFFF;

        Adapter->NdisMiniportBlock.SlotNumber = SlotNumber.u.AsULONG;
    }
  WrapperContext.SlotNumber = Adapter->NdisMiniportBlock.SlotNumber;

  if (Adapter->NdisMiniportBlock.BusType == NdisInterfacePci)
    {
      /* Without it NdisMGetBusData reads nothing, which drivers take as a missing adapter */
      Status = NdisQueryPciBusInterface(Adapter);
      if (!NT_SUCCESS(Status))
        NDIS_DbgPrint(MIN_TRACE, ("No PCI bus interface for %wZ (0x%lx)\n",
                                  &Adapter->NdisMiniportBlock.MiniportName, Status));
    }

  NdisCloseConfiguration(ConfigHandle);

  /* Both kinds of miniport come up through the core */
  NdisStatus = CoreInitializeAdapter(Adapter, &WrapperContext);

  ZwClose(WrapperContext.RegistryHandle);

  if (NdisStatus != NDIS_STATUS_SUCCESS)
    {
      NDIS_DbgPrint(MIN_TRACE, ("MiniportInitialize() failed for an adapter (%lx).\n", NdisStatus));
      ExInterlockedRemoveEntryList( &Adapter->ListEntry, &AdapterListLock );
      return NdisStatus;
    }

  /* NDIS 5 miniports register for shutdown themselves */
  if (MINIPORT_IS_NDIS6(Adapter))
    IoRegisterShutdownNotification(Adapter->NdisMiniportBlock.DeviceObject);

  /* Check for a hang every two seconds if it wasn't set in MiniportInitialize */
  if (Adapter->NdisMiniportBlock.CheckForHangSeconds == 0)
      Adapter->NdisMiniportBlock.CheckForHangSeconds = 2;

  Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
  Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStarted;

  IoSetDeviceInterfaceState(&Adapter->NdisMiniportBlock.SymbolicLinkName, TRUE);

  Timeout.QuadPart = Int32x32To64(Adapter->NdisMiniportBlock.CheckForHangSeconds, -1000000);
  KeSetTimerEx(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Timer, Timeout,
               Adapter->NdisMiniportBlock.CheckForHangSeconds * 1000,
               &Adapter->NdisMiniportBlock.WakeUpDpcTimer.Dpc);

  /* Put adapter in adapter list for this miniport */
  ExInterlockedInsertTailList(&Adapter->NdisMiniportBlock.DriverHandle->DeviceList, &Adapter->MiniportListEntry, &Adapter->NdisMiniportBlock.DriverHandle->Lock);

  /* Refresh bindings for all protocols */
  CurrentEntry = ProtocolListHead.Flink;
  while (CurrentEntry != &ProtocolListHead)
  {
      ProtocolBinding = CONTAINING_RECORD(CurrentEntry, PROTOCOL_BINDING, ListEntry);

      ndisBindMiniportsToProtocol(&NdisStatus, ProtocolBinding);

      CurrentEntry = CurrentEntry->Flink;
  }

  return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NdisIPnPStopDevice(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
/*
 * FUNCTION: Handle the PnP stop device event
 * ARGUMENTS:
 *     DeviceObject = Functional Device Object
 *     Irp          = IRP_MN_STOP_DEVICE I/O request packet
 * RETURNS:
 *     Status of operation
 */
{
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)DeviceObject->DeviceExtension;

  /* Remove adapter from adapter list for this miniport */
  ExInterlockedRemoveEntryList(&Adapter->MiniportListEntry, &Adapter->NdisMiniportBlock.DriverHandle->Lock);

  /* Remove adapter from global adapter list */
  ExInterlockedRemoveEntryList(&Adapter->ListEntry, &AdapterListLock);

  KeCancelTimer(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Timer);

  /* Set this here so MiniportISR will be forced to run for interrupts generated in MiniportHalt */
  Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
  Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStopped;

  if (MINIPORT_IS_NDIS6(Adapter))
    IoUnregisterShutdownNotification(DeviceObject);

  CoreHaltAdapter(Adapter, NdisHaltDeviceStopped);

  IoSetDeviceInterfaceState(&Adapter->NdisMiniportBlock.SymbolicLinkName, FALSE);

  if (Adapter->NdisMiniportBlock.AllocatedResources)
    {
      ExFreePool(Adapter->NdisMiniportBlock.AllocatedResources);
      Adapter->NdisMiniportBlock.AllocatedResources = NULL;
    }
  if (Adapter->NdisMiniportBlock.AllocatedResourcesTranslated)
    {
      ExFreePool(Adapter->NdisMiniportBlock.AllocatedResourcesTranslated);
      Adapter->NdisMiniportBlock.AllocatedResourcesTranslated = NULL;
    }

    if (Adapter->BusInterface.SetBusData != NULL)
    {
      if (Adapter->BusInterface.InterfaceDereference)
        Adapter->BusInterface.InterfaceDereference(Adapter->BusInterface.Context);

      Adapter->BusInterface.SetBusData = NULL;
    }

  if (Adapter->NdisMiniportBlock.Resources)
    {
      ExFreePool(Adapter->NdisMiniportBlock.Resources);
      Adapter->NdisMiniportBlock.Resources = NULL;
    }

  if (Adapter->NdisMiniportBlock.EthDB)
    {
      EthDeleteFilter(Adapter->NdisMiniportBlock.EthDB);
      Adapter->NdisMiniportBlock.EthDB = NULL;
    }

  return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NdisIShutdown(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
  PLOGICAL_ADAPTER Adapter = DeviceObject->DeviceExtension;

  CoreShutdownAdapter(Adapter, NdisShutdownPowerOff);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NdisIDeviceIoControl(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)DeviceObject->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NDIS_STATUS Status = STATUS_NOT_SUPPORTED;
  ULONG ControlCode;
  ULONG Written;

  Irp->IoStatus.Information = 0;

  ASSERT(Adapter);

  ControlCode = Stack->Parameters.DeviceIoControl.IoControlCode;
  switch (ControlCode)
  {
    case IOCTL_NDIS_QUERY_GLOBAL_STATS:
      Status = CoreQueryInformation(Adapter,
                                    *(PNDIS_OID)Irp->AssociatedIrp.SystemBuffer,
                                    MmGetSystemAddressForMdl(Irp->MdlAddress),
                                    Stack->Parameters.DeviceIoControl.OutputBufferLength,
                                    &Written);
      Irp->IoStatus.Information = Written;
      break;

    case IOCTL_NDIS_RESERVED7:
      NDIS_DbgPrint(MIN_TRACE, ("NdisIDeviceIoControl: IOCTL_NDIS_RESERVED7 UNIMPLEMENTED (CORE-13831)\n"));
      Status = STATUS_NOT_IMPLEMENTED;
      break;

    default:
      NDIS_DbgPrint(MIN_TRACE, ("NdisIDeviceIoControl: unsupported control code 0x%lx\n", ControlCode));
      break;
  }

  if (Status != NDIS_STATUS_PENDING)
  {
      Irp->IoStatus.Status = Status;
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
  }
  else
      IoMarkIrpPending(Irp);

  return Status;
}

static
NTSTATUS
NdisIPnPRemoveDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    NTSTATUS Status;
    PLOGICAL_ADAPTER Adapter = DeviceObject->DeviceExtension;
    
    if (Adapter->NdisMiniportBlock.SymbolicLinkName.Buffer)
    {
        IoSetDeviceInterfaceState(&Adapter->NdisMiniportBlock.SymbolicLinkName, FALSE);
        RtlFreeUnicodeString(&Adapter->NdisMiniportBlock.SymbolicLinkName);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Adapter->NdisMiniportBlock.NextDeviceObject, Irp);

    IoDetachDevice(Adapter->NdisMiniportBlock.NextDeviceObject);
        
    RtlFreeUnicodeString(&Adapter->NdisMiniportBlock.MiniportName);

    if (Adapter->NdisMiniportBlock.PnPDeviceState == NdisPnPDeviceStarted)
    {
        /* Remove adapter from adapter list for this miniport */
        ExInterlockedRemoveEntryList(&Adapter->MiniportListEntry, &Adapter->NdisMiniportBlock.DriverHandle->Lock);

        /* Remove adapter from global adapter list */
        ExInterlockedRemoveEntryList(&Adapter->ListEntry, &AdapterListLock);

        KeCancelTimer(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Timer);

        if (MINIPORT_IS_NDIS6(Adapter))
            IoUnregisterShutdownNotification(DeviceObject);

        CoreHaltAdapter(Adapter, NdisHaltDeviceDisabled);
    }

    if (Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportRemoveDeviceHandler != NULL)
    {
        Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportRemoveDeviceHandler(
            Adapter->Core.AddDeviceContext);
    }

    if (Adapter->NdisMiniportBlock.EthDB)
    {
        EthDeleteFilter(Adapter->NdisMiniportBlock.EthDB);
        Adapter->NdisMiniportBlock.EthDB = NULL;
    }

    if (Adapter->NdisMiniportBlock.Resources)
    {
        ExFreePool(Adapter->NdisMiniportBlock.Resources);
        Adapter->NdisMiniportBlock.Resources = NULL;
    }

    if (Adapter->NdisMiniportBlock.AllocatedResources)
    {
        ExFreePool(Adapter->NdisMiniportBlock.AllocatedResources);
        Adapter->NdisMiniportBlock.AllocatedResources = NULL;
    }

    if (Adapter->NdisMiniportBlock.AllocatedResourcesTranslated)
    {
        ExFreePool(Adapter->NdisMiniportBlock.AllocatedResourcesTranslated);
        Adapter->NdisMiniportBlock.AllocatedResourcesTranslated = NULL;
    }

    IoDeleteDevice(DeviceObject);

    return Status;
}

/* The status a miniport's MiniportFilterResourceRequirements completes the IRP with */
static
NTSTATUS
MiniFilterStatusToNtStatus(
    _In_ NDIS_STATUS Status)
{
    switch (Status)
    {
        case NDIS_STATUS_SUCCESS:
        case NDIS_STATUS_PENDING:
        case NDIS_STATUS_BUFFER_OVERFLOW:
        case NDIS_STATUS_FAILURE:
        case NDIS_STATUS_RESOURCES:
        case NDIS_STATUS_NOT_SUPPORTED:
            return Status;

        case NDIS_STATUS_BUFFER_TOO_SHORT:
            return STATUS_BUFFER_TOO_SMALL;

        case NDIS_STATUS_INVALID_LENGTH:
            return STATUS_INVALID_BUFFER_SIZE;

        case NDIS_STATUS_INVALID_DATA:
            return STATUS_INVALID_PARAMETER;

        default:
            return STATUS_UNSUCCESSFUL;
    }
}

/* The bus answers first, then the miniport may change the requirements */
static
NTSTATUS
MiniFilterResourceRequirements(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PIRP Irp)
{
    MINIPORT_FILTER_RESOURCE_REQUIREMENTS_HANDLER Handler =
        Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportFilterResourceRequirementsHandler;
    NTSTATUS Status;

    Status = NdisIForwardIrpAndWait(Adapter, Irp);
    if (NT_SUCCESS(Status))
        Status = MiniFilterStatusToNtStatus(Handler(Adapter->Core.AddDeviceContext, Irp));

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
NdisIDispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)DeviceObject->DeviceExtension;
  NTSTATUS Status;

  switch (Stack->MinorFunction)
    {
      case IRP_MN_START_DEVICE:
        /* A miniport with a start handler sees the IRP before the bus does */
        if (Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportStartDeviceHandler != NULL &&
            Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportStartDeviceHandler(
                Adapter->Core.AddDeviceContext, Irp) != NDIS_STATUS_SUCCESS)
        {
            Status = STATUS_UNSUCCESSFUL;
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }

        Status = NdisIForwardIrpAndWait(Adapter, Irp);
        if (NT_SUCCESS(Status) && NT_SUCCESS(Irp->IoStatus.Status))
          {
	      Status = NdisIPnPStartDevice(DeviceObject, Irp);
          }
          else
              NDIS_DbgPrint(MIN_TRACE, ("Lower driver failed device start\n"));
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;

      case IRP_MN_STOP_DEVICE:
        Status = NdisIPnPStopDevice(DeviceObject, Irp);
        if (!NT_SUCCESS(Status))
            NDIS_DbgPrint(MIN_TRACE, ("WARNING: Ignoring halt device failure! Passing the IRP down anyway\n"));
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

      case IRP_MN_QUERY_REMOVE_DEVICE:
      case IRP_MN_QUERY_STOP_DEVICE:
        Status = NdisIPnPQueryStopDevice(DeviceObject, Irp);
        Irp->IoStatus.Status = Status;
        if (Status != STATUS_SUCCESS)
        {
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            NDIS_DbgPrint(MIN_TRACE, ("Failing miniport halt request\n"));
            return Status;
        }
        break;

      case IRP_MN_CANCEL_REMOVE_DEVICE:
      case IRP_MN_CANCEL_STOP_DEVICE:
        Status = NdisIForwardIrpAndWait(Adapter, Irp);
        if (NT_SUCCESS(Status) && NT_SUCCESS(Irp->IoStatus.Status))
        {
            Status = NdisIPnPCancelStopDevice(DeviceObject, Irp);
        }
        else
        {
            NDIS_DbgPrint(MIN_TRACE, ("Lower driver failed cancel stop/remove request\n"));
        }
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;

      case IRP_MN_QUERY_PNP_DEVICE_STATE:
        Status = NDIS_STATUS_SUCCESS;
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information |= Adapter->NdisMiniportBlock.PnPFlags;
        break;

      case IRP_MN_REMOVE_DEVICE:
        return NdisIPnPRemoveDevice(DeviceObject, Irp);

      case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
        if (Adapter->NdisMiniportBlock.DriverHandle->PnpCharacteristics.MiniportFilterResourceRequirementsHandler != NULL)
            return MiniFilterResourceRequirements(Adapter, Irp);
        break;

      default:
        NDIS_DbgPrint(MIN_TRACE, ("Unhandled minor function: 0x%X\n", Stack->MinorFunction));
        break;
    }

  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(Adapter->NdisMiniportBlock.NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
NdisIPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
  PLOGICAL_ADAPTER Adapter = DeviceObject->DeviceExtension;

  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(Adapter->NdisMiniportBlock.NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
NdisIAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject)
/*
 * FUNCTION: Create a device for an adapter found using PnP
 * ARGUMENTS:
 *     DriverObject         = Pointer to the miniport driver object
 *     PhysicalDeviceObject = Pointer to the PDO for our adapter
 */
{
  static const WCHAR ClassKeyName[] = {'C','l','a','s','s','\\'};
  static const WCHAR LinkageKeyName[] = {'\\','L','i','n','k','a','g','e',0};
  PNDIS_M_DRIVER_BLOCK Miniport;
  PNDIS_M_DRIVER_BLOCK *MiniportPtr;
  WCHAR *LinkageKeyBuffer;
  ULONG DriverKeyLength;
  RTL_QUERY_REGISTRY_TABLE QueryTable[2];
  UNICODE_STRING ExportName;
  PDEVICE_OBJECT DeviceObject;
  PLOGICAL_ADAPTER Adapter;
  NTSTATUS Status;

  /*
   * Gain the access to the miniport data structure first.
   */

  MiniportPtr = IoGetDriverObjectExtension(DriverObject, (PVOID)'NMID');
  if (MiniportPtr == NULL)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't get driver object extension.\n"));
      return NDIS_STATUS_FAILURE;
    }
  Miniport = *MiniportPtr;

  /*
   * Get name of the Linkage registry key for our adapter. It's located under
   * the driver key for our driver and so we have basicly two ways to do it.
   * Either we can use IoOpenDriverRegistryKey or compose it using information
   * gathered by IoGetDeviceProperty. I chose the second because
   * IoOpenDriverRegistryKey wasn't implemented at the time of writing.
   */

  Status = IoGetDeviceProperty(PhysicalDeviceObject, DevicePropertyDriverKeyName,
                               0, NULL, &DriverKeyLength);
  if (Status != STATUS_BUFFER_TOO_SMALL && Status != STATUS_BUFFER_OVERFLOW && Status != STATUS_SUCCESS)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't get miniport driver key length.\n"));
      return Status;
    }

  LinkageKeyBuffer = ExAllocatePool(PagedPool, DriverKeyLength +
                                    sizeof(ClassKeyName) + sizeof(LinkageKeyName));
  if (LinkageKeyBuffer == NULL)
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't allocate memory for driver key name.\n"));
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  Status = IoGetDeviceProperty(PhysicalDeviceObject, DevicePropertyDriverKeyName,
                               DriverKeyLength, LinkageKeyBuffer +
                               (sizeof(ClassKeyName) / sizeof(WCHAR)),
                               &DriverKeyLength);
  if (!NT_SUCCESS(Status))
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't get miniport driver key.\n"));
      ExFreePool(LinkageKeyBuffer);
      return Status;
    }

  /* Compose the linkage key name. */
  RtlCopyMemory(LinkageKeyBuffer, ClassKeyName, sizeof(ClassKeyName));
  RtlCopyMemory(LinkageKeyBuffer + ((sizeof(ClassKeyName) + DriverKeyLength) /
                sizeof(WCHAR)) - 1, LinkageKeyName, sizeof(LinkageKeyName));

  NDIS_DbgPrint(DEBUG_MINIPORT, ("LinkageKey: %S.\n", LinkageKeyBuffer));

  /*
   * Now open the linkage key and read the "Export" and "RootDevice" values
   * which contains device name and root service respectively.
   */

  RtlZeroMemory(QueryTable, sizeof(QueryTable));
  RtlInitUnicodeString(&ExportName, NULL);
  QueryTable[0].Flags = RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_DIRECT;
  QueryTable[0].Name = L"Export";
  QueryTable[0].EntryContext = &ExportName;

  Status = RtlQueryRegistryValues(RTL_REGISTRY_CONTROL, LinkageKeyBuffer,
                                  QueryTable, NULL, NULL);
  ExFreePool(LinkageKeyBuffer);
  if (!NT_SUCCESS(Status))
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't get miniport device name. (%x)\n", Status));
      return Status;
    }

  /*
   * Create the device object.
   */

  NDIS_DbgPrint(MAX_TRACE, ("creating device %wZ\n", &ExportName));

  Status = IoCreateDevice(Miniport->DriverObject, sizeof(LOGICAL_ADAPTER),
    &ExportName, FILE_DEVICE_PHYSICAL_NETCARD,
    0, FALSE, &DeviceObject);
  if (!NT_SUCCESS(Status))
    {
      NDIS_DbgPrint(MIN_TRACE, ("Could not create device object.\n"));
      RtlFreeUnicodeString(&ExportName);
      return Status;
    }

  /*
   * Initialize the adapter structure.
   */

  Adapter = (PLOGICAL_ADAPTER)DeviceObject->DeviceExtension;
  KeInitializeSpinLock(&Adapter->NdisMiniportBlock.Lock);
  InitializeListHead(&Adapter->ProtocolListHead);
  CoreInitializeAdapterBlock(Adapter);

  Status = IoRegisterDeviceInterface(PhysicalDeviceObject,
                                     &GUID_DEVINTERFACE_NET,
                                     NULL,
                                     &Adapter->NdisMiniportBlock.SymbolicLinkName);

  if (!NT_SUCCESS(Status))
  {
      NDIS_DbgPrint(MIN_TRACE, ("Could not create device interface.\n"));
      IoDeleteDevice(DeviceObject);
      RtlFreeUnicodeString(&ExportName);
      return Status;
  }

  Adapter->NdisMiniportBlock.DriverHandle = Miniport;
  Adapter->NdisMiniportBlock.MiniportName = ExportName;
  Adapter->NdisMiniportBlock.DeviceObject = DeviceObject;
  Adapter->NdisMiniportBlock.PhysicalDeviceObject = PhysicalDeviceObject;
  Adapter->NdisMiniportBlock.NextDeviceObject =
    IoAttachDeviceToDeviceStack(Adapter->NdisMiniportBlock.DeviceObject,
                                PhysicalDeviceObject);

  Adapter->NdisMiniportBlock.OldPnPDeviceState = 0;
  Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceAdded;

  KeInitializeTimer(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Timer);
  KeInitializeDpc(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Dpc, MiniportHangDpc, Adapter);

  if (Miniport->PnpCharacteristics.MiniportAddDeviceHandler != NULL)
    {
      Status = Miniport->PnpCharacteristics.MiniportAddDeviceHandler(Adapter, Miniport->MiniportDriverContext);
      if (Status != NDIS_STATUS_SUCCESS)
        {
          NDIS_DbgPrint(MIN_TRACE, ("MiniportAddDevice failed (0x%x).\n", Status));
          IoDetachDevice(Adapter->NdisMiniportBlock.NextDeviceObject);
          RtlFreeUnicodeString(&Adapter->NdisMiniportBlock.SymbolicLinkName);
          RtlFreeUnicodeString(&ExportName);
          IoDeleteDevice(DeviceObject);
          return Status;
        }
    }

  DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NdisGenericIrpHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    /* Use the characteristics to classify the device */
    if (DeviceObject->DeviceType == FILE_DEVICE_PHYSICAL_NETCARD)
    {
        if ((IrpSp->MajorFunction == IRP_MJ_CREATE) ||
            (IrpSp->MajorFunction == IRP_MJ_CLOSE) ||
            (IrpSp->MajorFunction == IRP_MJ_CLEANUP))
        {
            return NdisICreateClose(DeviceObject, Irp);
        }
        else if (IrpSp->MajorFunction == IRP_MJ_PNP)
        {
            return NdisIDispatchPnp(DeviceObject, Irp);
        }
        else if (IrpSp->MajorFunction == IRP_MJ_SHUTDOWN)
        {
            return NdisIShutdown(DeviceObject, Irp);
        }
        else if (IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL)
        {
            return NdisIDeviceIoControl(DeviceObject, Irp);
        }
        else if (IrpSp->MajorFunction == IRP_MJ_POWER)
        {
            return NdisIPower(DeviceObject, Irp);
        }
        NDIS_DbgPrint(MIN_TRACE, ("Unexpected IRP MajorFunction 0x%x\n", IrpSp->MajorFunction));
        ASSERT(FALSE);
    }
    else if (DeviceObject->DeviceType == FILE_DEVICE_NETWORK)
    {
        PNDIS_M_DEVICE_BLOCK DeviceBlock = DeviceObject->DeviceExtension;

        ASSERT(DeviceBlock->DeviceObject == DeviceObject);

        if (DeviceBlock->MajorFunction[IrpSp->MajorFunction] != NULL)
        {
            return DeviceBlock->MajorFunction[IrpSp->MajorFunction](DeviceObject, Irp);
        }
    }
    else
    {
        ASSERT(FALSE);
    }

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_INVALID_DEVICE_REQUEST;
}

/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisMRegisterMiniport(
    IN  NDIS_HANDLE                     NdisWrapperHandle,
    IN  PNDIS_MINIPORT_CHARACTERISTICS  MiniportCharacteristics,
    IN  UINT                            CharacteristicsLength)
/*
 * FUNCTION: Registers a miniport's MiniportXxx entry points with the NDIS library
 * ARGUMENTS:
 *     NdisWrapperHandle       = Pointer to handle returned by NdisMInitializeWrapper
 *     MiniportCharacteristics = Pointer to a buffer with miniport characteristics
 *     CharacteristicsLength   = Number of bytes in characteristics buffer
 * RETURNS:
 *     Status of operation
 */
{
  UINT MinSize;
  PNDIS_M_DRIVER_BLOCK Miniport = GET_MINIPORT_DRIVER(NdisWrapperHandle);
  PNDIS_M_DRIVER_BLOCK *MiniportPtr;
  NTSTATUS Status;
  ULONG i;

  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

  switch (MiniportCharacteristics->MajorNdisVersion)
    {
      case 0x03:
        MinSize = sizeof(NDIS30_MINIPORT_CHARACTERISTICS);
        break;

      case 0x04:
        MinSize = sizeof(NDIS40_MINIPORT_CHARACTERISTICS);
        break;

      case 0x05:
        switch (MiniportCharacteristics->MinorNdisVersion)
        {
            case 0x00:
                MinSize = sizeof(NDIS50_MINIPORT_CHARACTERISTICS);
                break;

            case 0x01:
                MinSize = sizeof(NDIS51_MINIPORT_CHARACTERISTICS);
                break;

            default:
                NDIS_DbgPrint(MIN_TRACE, ("Bad 5.x minor characteristics version.\n"));
                return NDIS_STATUS_BAD_VERSION;
        }
        break;

      default:
        NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics version.\n"));
        return NDIS_STATUS_BAD_VERSION;
    }

   NDIS_DbgPrint(MID_TRACE, ("Initializing an NDIS %u.%u miniport\n",
                              MiniportCharacteristics->MajorNdisVersion,
                              MiniportCharacteristics->MinorNdisVersion));

  if (CharacteristicsLength < MinSize)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics length.\n"));
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

  /* Check if mandatory MiniportXxx functions are specified */
  if ((!MiniportCharacteristics->HaltHandler) ||
       (!MiniportCharacteristics->InitializeHandler)||
       (!MiniportCharacteristics->ResetHandler))
    {
      NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics.\n"));
      return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

  if (MiniportCharacteristics->MajorNdisVersion < 0x05)
  {
      if ((!MiniportCharacteristics->QueryInformationHandler) ||
          (!MiniportCharacteristics->SetInformationHandler))
      {
           NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics. (Set/Query)\n"));
           return NDIS_STATUS_BAD_CHARACTERISTICS;
      }
  }
  else
  {
      if (((!MiniportCharacteristics->QueryInformationHandler) ||
           (!MiniportCharacteristics->SetInformationHandler)) &&
           (!MiniportCharacteristics->CoRequestHandler))
      {
           NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics. (Set/Query)\n"));
           return NDIS_STATUS_BAD_CHARACTERISTICS;
      }
  }

  if (MiniportCharacteristics->MajorNdisVersion == 0x03)
    {
      if (!MiniportCharacteristics->SendHandler)
        {
          NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics. (NDIS 3.0)\n"));
          return NDIS_STATUS_BAD_CHARACTERISTICS;
        }
    }
  else if (MiniportCharacteristics->MajorNdisVersion == 0x04)
    {
      /* NDIS 4.0 */
      if ((!MiniportCharacteristics->SendHandler) &&
          (!MiniportCharacteristics->SendPacketsHandler))
        {
          NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics. (NDIS 4.0)\n"));
          return NDIS_STATUS_BAD_CHARACTERISTICS;
        }
    }
  else if (MiniportCharacteristics->MajorNdisVersion == 0x05)
    {
      /* TODO: Add more checks here */

      if ((!MiniportCharacteristics->SendHandler) &&
          (!MiniportCharacteristics->SendPacketsHandler) &&
          (!MiniportCharacteristics->CoSendPacketsHandler))
        {
          NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics. (NDIS 5.0)\n"));
          return NDIS_STATUS_BAD_CHARACTERISTICS;
        }
    }

  RtlCopyMemory(&Miniport->MiniportCharacteristics, MiniportCharacteristics, MinSize);

  /*
   * NOTE: This is VERY unoptimal! Should we store the NDIS_M_DRIVER_BLOCK
   * structure in the driver extension or what?
   */

  Status = IoAllocateDriverObjectExtension(Miniport->DriverObject, (PVOID)'NMID',
                                           sizeof(PNDIS_M_DRIVER_BLOCK), (PVOID*)&MiniportPtr);
  if (!NT_SUCCESS(Status))
    {
      NDIS_DbgPrint(MIN_TRACE, ("Can't allocate driver object extension.\n"));
      return NDIS_STATUS_RESOURCES;
    }

  *MiniportPtr = Miniport;

  /* We have to register for all of these so handler registered in NdisMRegisterDevice work */
  for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
  {
       Miniport->DriverObject->MajorFunction[i] = NdisGenericIrpHandler;
  }

  Miniport->DriverObject->DriverExtension->AddDevice = NdisIAddDevice;

  return NDIS_STATUS_SUCCESS;
}

/*
 * @implemented
 */
#undef NdisMResetComplete
VOID
EXPORT
NdisMResetComplete(
    IN NDIS_HANDLE MiniportAdapterHandle,
    IN NDIS_STATUS Status,
    IN BOOLEAN     AddressingReset)
{
  MiniResetComplete(MiniportAdapterHandle, Status, AddressingReset);
}

/*
 * @implemented
 */
#undef NdisMSendComplete
VOID
EXPORT
NdisMSendComplete(
    IN  NDIS_HANDLE     MiniportAdapterHandle,
    IN  PNDIS_PACKET    Packet,
    IN  NDIS_STATUS     Status)
/*
 * FUNCTION: Forwards a message to the initiating protocol saying
 *           that a packet was handled
 * ARGUMENTS:
 *     NdisAdapterHandle = Handle input to MiniportInitialize
 *     Packet            = Pointer to NDIS packet that was sent
 *     Status            = Status of send operation
 */
{
  MiniSendComplete(MiniportAdapterHandle, Packet, Status);
}

/*
 * @implemented
 */
#undef NdisMSendResourcesAvailable
VOID
EXPORT
NdisMSendResourcesAvailable(
    IN  NDIS_HANDLE MiniportAdapterHandle)
{
  MiniWorkItemComplete((PLOGICAL_ADAPTER)MiniportAdapterHandle, NdisWorkItemSend);
}

/*
 * @implemented
 */
#undef NdisMTransferDataComplete
VOID
EXPORT
NdisMTransferDataComplete(
    IN  NDIS_HANDLE     MiniportAdapterHandle,
    IN  PNDIS_PACKET    Packet,
    IN  NDIS_STATUS     Status,
    IN  UINT            BytesTransferred)
{
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;

  if (Adapter->NdisMiniportBlock.TDCompleteHandler != NULL)
    Adapter->NdisMiniportBlock.TDCompleteHandler(MiniportAdapterHandle, Packet, Status, BytesTransferred);
}

/*
 * @implemented
 */
#undef NdisMSetAttributes
VOID
EXPORT
NdisMSetAttributes(
    IN  NDIS_HANDLE         MiniportAdapterHandle,
    IN  NDIS_HANDLE         MiniportAdapterContext,
    IN  BOOLEAN             BusMaster,
    IN  NDIS_INTERFACE_TYPE AdapterType)
/*
 * FUNCTION: Informs the NDIS library of significant features of the caller's NIC
 * ARGUMENTS:
 *     MiniportAdapterHandle  = Handle input to MiniportInitialize
 *     MiniportAdapterContext = Pointer to context information
 *     BusMaster              = Specifies TRUE if the caller's NIC is a busmaster DMA device
 *     AdapterType            = Specifies the I/O bus interface of the caller's NIC
 */
{
  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));
  NdisMSetAttributesEx(MiniportAdapterHandle, MiniportAdapterContext, 0,
                       BusMaster ? NDIS_ATTRIBUTE_BUS_MASTER : 0,
                       AdapterType);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMSetAttributesEx(
    IN  NDIS_HANDLE         MiniportAdapterHandle,
    IN  NDIS_HANDLE         MiniportAdapterContext,
    IN  UINT                CheckForHangTimeInSeconds   OPTIONAL,
    IN  ULONG               AttributeFlags,
    IN  NDIS_INTERFACE_TYPE	AdapterType)
/*
 * FUNCTION: Informs the NDIS library of significant features of the caller's NIC
 * ARGUMENTS:
 *     MiniportAdapterHandle     = Handle input to MiniportInitialize
 *     MiniportAdapterContext    = Pointer to context information
 *     CheckForHangTimeInSeconds = Specifies interval in seconds at which
 *                                 MiniportCheckForHang should be called
 *     AttributeFlags            = Bitmask that indicates specific attributes
 *     AdapterType               = Specifies the I/O bus interface of the caller's NIC
 */
{
  PLOGICAL_ADAPTER Adapter = GET_LOGICAL_ADAPTER(MiniportAdapterHandle);

  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

  Adapter->NdisMiniportBlock.MiniportAdapterContext = MiniportAdapterContext;
  Adapter->NdisMiniportBlock.Flags = AttributeFlags;
  Adapter->NdisMiniportBlock.AdapterType = AdapterType;
  if (CheckForHangTimeInSeconds > 0)
      Adapter->NdisMiniportBlock.CheckForHangSeconds = CheckForHangTimeInSeconds;
  if (AttributeFlags & NDIS_ATTRIBUTE_INTERMEDIATE_DRIVER)
    NDIS_DbgPrint(MIN_TRACE, ("Intermediate drivers not supported yet.\n"));

  NDIS_DbgPrint(MID_TRACE, ("Miniport attribute flags: 0x%x\n", AttributeFlags));

  if (Adapter->NdisMiniportBlock.DriverHandle->MiniportCharacteristics.AdapterShutdownHandler)
  {
      NDIS_DbgPrint(MAX_TRACE, ("Miniport set AdapterShutdownHandler in MiniportCharacteristics\n"));
      NdisMRegisterAdapterShutdownHandler(Adapter,
                      Adapter->NdisMiniportBlock.MiniportAdapterContext,
                      Adapter->NdisMiniportBlock.DriverHandle->MiniportCharacteristics.AdapterShutdownHandler);
  }
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMSleep(
    IN  ULONG   MicrosecondsToSleep)
/*
 * FUNCTION: delay the thread's execution for MicrosecondsToSleep
 * ARGUMENTS:
 *     MicrosecondsToSleep: duh...
 * NOTES:
 *     - Because this is a blocking call, current IRQL must be < DISPATCH_LEVEL
 */
{
  KTIMER Timer;
  LARGE_INTEGER DueTime;

  PAGED_CODE();

  DueTime.QuadPart = (-1) * 10 * MicrosecondsToSleep;

  KeInitializeTimer(&Timer);
  KeSetTimer(&Timer, DueTime, 0);
  KeWaitForSingleObject(&Timer, Executive, KernelMode, FALSE, 0);
}

/*
 * @implemented
 */
BOOLEAN
EXPORT
NdisMSynchronizeWithInterrupt(
    IN  PNDIS_MINIPORT_INTERRUPT    Interrupt,
    IN  PVOID                       SynchronizeFunction,
    IN  PVOID                       SynchronizeContext)
{
  return(KeSynchronizeExecution(Interrupt->InterruptObject,
				(PKSYNCHRONIZE_ROUTINE)SynchronizeFunction,
				SynchronizeContext));
}

/*
 * @unimplemented
 */
NDIS_STATUS
EXPORT
NdisMWriteLogData(
    IN  NDIS_HANDLE LogHandle,
    IN  PVOID       LogBuffer,
    IN  UINT        LogBufferSize)
{
    PUCHAR Buffer = LogBuffer;
    UINT i, j, idx;

    UNIMPLEMENTED;
    for (i = 0; i < LogBufferSize; i += 16)
    {
        DbgPrint("%08x |", i);
        for (j = 0; j < 16; j++)
        {
            idx = i + j;
            if (idx < LogBufferSize)
                DbgPrint(" %02x", Buffer[idx]);
            else
                DbgPrint("   ");
        }
        DbgPrint(" | ");
        for (j = 0; j < 16; j++)
        {
            idx = i + j;
            if (idx == LogBufferSize)
                break;
            if (Buffer[idx] >= ' ') /* FIXME: not portable! replace by if (isprint(Buffer[idx])) ? */
                DbgPrint("%c", Buffer[idx]);
            else
                DbgPrint(".");
        }
        DbgPrint("\n");
    }

    return NDIS_STATUS_FAILURE;
}

/*
 * @implemented
 */
VOID
EXPORT
NdisTerminateWrapper(
    IN  NDIS_HANDLE NdisWrapperHandle,
    IN  PVOID       SystemSpecific)
/*
 * FUNCTION: Releases resources allocated by a call to NdisInitializeWrapper
 * ARGUMENTS:
 *     NdisWrapperHandle = Handle returned by NdisInitializeWrapper (NDIS_M_DRIVER_BLOCK)
 *     SystemSpecific    = Always NULL
 */
{
  PNDIS_M_DRIVER_BLOCK Miniport = GET_MINIPORT_DRIVER(NdisWrapperHandle);

  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

  ExFreePool(Miniport->RegistryPath->Buffer);
  ExFreePool(Miniport->RegistryPath);
  ExInterlockedRemoveEntryList(&Miniport->ListEntry, &MiniportListLock);
  ExFreePool(Miniport);
}


/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisMQueryAdapterInstanceName(
    OUT PNDIS_STRING    AdapterInstanceName,
    IN  NDIS_HANDLE     MiniportAdapterHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    UNICODE_STRING AdapterName;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    AdapterName.Length = 0;
    AdapterName.MaximumLength = Adapter->NdisMiniportBlock.MiniportName.MaximumLength;
    AdapterName.Buffer = ExAllocatePool(PagedPool, AdapterName.MaximumLength);
    if (!AdapterName.Buffer) {
        NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources\n"));
        return NDIS_STATUS_RESOURCES;
    }

    RtlCopyUnicodeString(&AdapterName, &Adapter->NdisMiniportBlock.MiniportName);

    *AdapterInstanceName = AdapterName;

    return NDIS_STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
EXPORT
NdisDeregisterAdapterShutdownHandler(
    IN  NDIS_HANDLE NdisAdapterHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 4.0
 */
{
    NdisMDeregisterAdapterShutdownHandler(NdisAdapterHandle);
}


/*
 * @implemented
 */
VOID
EXPORT
NdisRegisterAdapterShutdownHandler(
    IN  NDIS_HANDLE                 NdisAdapterHandle,
    IN  PVOID                       ShutdownContext,
    IN  ADAPTER_SHUTDOWN_HANDLER    ShutdownHandler)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 4.0
 */
{
    NdisMRegisterAdapterShutdownHandler(NdisAdapterHandle,
                                        ShutdownContext,
                                        ShutdownHandler);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMGetDeviceProperty(
    IN      NDIS_HANDLE         MiniportAdapterHandle,
    IN OUT  PDEVICE_OBJECT      *PhysicalDeviceObject           OPTIONAL,
    IN OUT  PDEVICE_OBJECT      *FunctionalDeviceObject         OPTIONAL,
    IN OUT  PDEVICE_OBJECT      *NextDeviceObject               OPTIONAL,
    IN OUT  PCM_RESOURCE_LIST   *AllocatedResources             OPTIONAL,
    IN OUT  PCM_RESOURCE_LIST   *AllocatedResourcesTranslated   OPTIONAL)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    NDIS_DbgPrint(MAX_TRACE, ("Called\n"));

    if (PhysicalDeviceObject != NULL)
        *PhysicalDeviceObject = Adapter->NdisMiniportBlock.PhysicalDeviceObject;

    if (FunctionalDeviceObject != NULL)
        *FunctionalDeviceObject = Adapter->NdisMiniportBlock.DeviceObject;

    if (NextDeviceObject != NULL)
        *NextDeviceObject = Adapter->NdisMiniportBlock.NextDeviceObject;

    if (AllocatedResources != NULL)
        *AllocatedResources = Adapter->NdisMiniportBlock.AllocatedResources;

    if (AllocatedResourcesTranslated != NULL)
        *AllocatedResourcesTranslated = Adapter->NdisMiniportBlock.AllocatedResourcesTranslated;
}

/*
 * @implemented
 */
VOID
EXPORT
NdisMRegisterUnloadHandler(
    IN  NDIS_HANDLE     NdisWrapperHandle,
    IN  PDRIVER_UNLOAD  UnloadHandler)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PNDIS_M_DRIVER_BLOCK DriverBlock = NdisWrapperHandle;

    NDIS_DbgPrint(MAX_TRACE, ("Miniport registered unload handler\n"));

    DriverBlock->DriverObject->DriverUnload = UnloadHandler;
}

/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisMRegisterDevice(
    IN  NDIS_HANDLE         NdisWrapperHandle,
    IN  PNDIS_STRING        DeviceName,
    IN  PNDIS_STRING        SymbolicName,
    IN  PDRIVER_DISPATCH    MajorFunctions[],
    OUT PDEVICE_OBJECT      *pDeviceObject,
    OUT NDIS_HANDLE         *NdisDeviceHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PNDIS_M_DRIVER_BLOCK DriverBlock = NdisWrapperHandle;
    PNDIS_M_DEVICE_BLOCK DeviceBlock;
    PDEVICE_OBJECT DeviceObject;
    NDIS_STATUS Status;
    UINT i;

    NDIS_DbgPrint(MAX_TRACE, ("Called\n"));

    Status = IoCreateDevice(DriverBlock->DriverObject,
                            sizeof(NDIS_M_DEVICE_BLOCK),
                            DeviceName,
                            FILE_DEVICE_NETWORK,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("IoCreateDevice failed (%x)\n", Status));
        return Status;
    }

    Status = IoCreateSymbolicLink(SymbolicName, DeviceName);

    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("IoCreateSymbolicLink failed (%x)\n", Status));
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    DeviceBlock = DeviceObject->DeviceExtension;

    if (!DeviceBlock)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources\n"));
        IoDeleteDevice(DeviceObject);
        IoDeleteSymbolicLink(SymbolicName);
        return NDIS_STATUS_RESOURCES;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
         DeviceBlock->MajorFunction[i] = MajorFunctions[i];

    DeviceBlock->DeviceObject = DeviceObject;
    DeviceBlock->SymbolicName = SymbolicName;

    *pDeviceObject = DeviceObject;
    *NdisDeviceHandle = DeviceBlock;

    return NDIS_STATUS_SUCCESS;
}

/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisMDeregisterDevice(
    IN  NDIS_HANDLE NdisDeviceHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PNDIS_M_DEVICE_BLOCK DeviceBlock = NdisDeviceHandle;

    IoDeleteDevice(DeviceBlock->DeviceObject);

    IoDeleteSymbolicLink(DeviceBlock->SymbolicName);

    return NDIS_STATUS_SUCCESS;
}

/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisQueryAdapterInstanceName(
    OUT PNDIS_STRING    AdapterInstanceName,
    IN  NDIS_HANDLE     NdisBindingHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PADAPTER_BINDING AdapterBinding = NdisBindingHandle;
    PLOGICAL_ADAPTER Adapter = AdapterBinding->Adapter;

    return NdisMQueryAdapterInstanceName(AdapterInstanceName,
                                         Adapter);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisCompletePnPEvent(
    IN  NDIS_STATUS     Status,
    IN  NDIS_HANDLE     NdisBindingHandle,
    IN  PNET_PNP_EVENT  NetPnPEvent)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
  PIRP Irp = (PIRP)NetPnPEvent->NdisReserved[0];
  PLIST_ENTRY CurrentEntry = (PLIST_ENTRY)NetPnPEvent->NdisReserved[1];
  PADAPTER_BINDING AdapterBinding = NdisBindingHandle;
  PLOGICAL_ADAPTER Adapter = AdapterBinding->Adapter;
  NDIS_STATUS NdisStatus;

  /* A miniport initiated event has no IRP, just a waiter in NdisMNetPnPEvent */
  if (Irp == NULL && NetPnPEvent->NdisReserved[2] != 0)
  {
      NetPnPEvent->NdisReserved[3] = (ULONG_PTR)Status;
      KeSetEvent((PKEVENT)NetPnPEvent->NdisReserved[2], IO_NO_INCREMENT, FALSE);
      return;
  }

  if (Status != NDIS_STATUS_SUCCESS)
  {
      if (NetPnPEvent->Buffer) ExFreePool(NetPnPEvent->Buffer);
      ExFreePool(NetPnPEvent);
      Irp->IoStatus.Status = Status;
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      return;
  }

  while (CurrentEntry != &Adapter->ProtocolListHead)
  {
     AdapterBinding = CONTAINING_RECORD(CurrentEntry, ADAPTER_BINDING, AdapterListEntry);

     NdisStatus = (*AdapterBinding->ProtocolBinding->Chars.PnPEventHandler)(
      AdapterBinding->NdisOpenBlock.ProtocolBindingContext,
      NetPnPEvent);

     if (NdisStatus == NDIS_STATUS_PENDING)
     {
         NetPnPEvent->NdisReserved[1] = (ULONG_PTR)CurrentEntry->Flink;
         return;
     }
     else if (NdisStatus != NDIS_STATUS_SUCCESS)
     {
         if (NetPnPEvent->Buffer) ExFreePool(NetPnPEvent->Buffer);
         ExFreePool(NetPnPEvent);
         Irp->IoStatus.Status = NdisStatus;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return;
     }

     CurrentEntry = CurrentEntry->Flink;
  }

  if (NetPnPEvent->Buffer) ExFreePool(NetPnPEvent->Buffer);
  ExFreePool(NetPnPEvent);

  Irp->IoStatus.Status = NDIS_STATUS_SUCCESS;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/*
 * @implemented
 */
VOID
EXPORT
NdisCancelSendPackets(
    IN NDIS_HANDLE  NdisBindingHandle,
    IN PVOID  CancelId)
{
    PADAPTER_BINDING AdapterBinding = NdisBindingHandle;
    PLOGICAL_ADAPTER Adapter = AdapterBinding->Adapter;

    NDIS_DbgPrint(MAX_TRACE, ("Called for ID %x.\n", CancelId));

    if (Adapter->Core.State != CoreMiniportHalted && Adapter->Core.Dispatch != NULL)
        Adapter->Core.Dispatch->CancelSendHandler(CORE_DISPATCH_CONTEXT(Adapter), CancelId);
}


/*
 * @implemented
 */
NDIS_HANDLE
EXPORT
NdisIMGetBindingContext(
    IN  NDIS_HANDLE NdisBindingHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PADAPTER_BINDING AdapterBinding = NdisBindingHandle;
    PLOGICAL_ADAPTER Adapter = AdapterBinding->Adapter;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return Adapter->NdisMiniportBlock.DeviceContext;
}


/*
 * @implemented
 */
NDIS_HANDLE
EXPORT
NdisIMGetDeviceContext(
    IN  NDIS_HANDLE MiniportAdapterHandle)
/*
 * FUNCTION:
 * ARGUMENTS:
 * NOTES:
 *    NDIS 5.0
 */
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return Adapter->NdisMiniportBlock.DeviceContext;
}

/* EOF */
