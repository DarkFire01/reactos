/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RDDM Miniport IOCTRL Handlers
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <dxgkrnl.h>
//#define NDEBUG
#include <debug.h>

extern PDXGKRNL_PRIVATE_EXTENSION DxgkpExtension;


/*
 * *Sigh* interrupts.
 * The problems start fast, without MSI interrupts we're limited to this file. Which is essentially a bit of a hack
 * to try to get interrupts in our dxgkrnl working as close to vista as possible.
 *
 * Vista DXGKRNL synchs many many interrupts across multiple MSI IDs
 *
 */


BOOLEAN
APIENTRY
DxgkCbQueueDpc(_In_ HANDLE DeviceHandle)
{
    return KeInsertQueueDpc(
               DxgkpExtension->DpcObject,
               NULL,
               NULL);
}

NTSTATUS
APIENTRY
DxgkCbSynchronizeExecution(_In_ HANDLE DeviceHandle,
                           _In_ PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                           _In_ PVOID Context,
                           _In_ ULONG MessageNumber,
                           _Out_ PBOOLEAN ReturnValue)
{
   // *ReturnValue = KeSynchronizeExecution(DxgkpExtension->InterruptObject, SynchronizeRoutine, Context);
    return STATUS_SUCCESS;
}


BOOLEAN NTAPI
DxgkpInterruptRoutine(
   IN struct _KINTERRUPT *Interrupt,
   IN PVOID ServiceContext)
{
    return DxgkpExtension->DxgkDdiInterruptRoutine(DxgkpExtension->MiniportContext, 0);
}


BOOLEAN NTAPI
DxgkrnlSetupInterrupt()
{
    NTSTATUS Status;
    ULONG InterruptVector;
    KIRQL Irql;
    KAFFINITY Affinity;

    InterruptVector = HalGetInterruptVector(DxgkpExtension->AdapterInterfaceType,
                                            DxgkpExtension->SystemIoBusNumber,
                                            0,
                                            0,
                                              &Irql,
                                         &Affinity);

      if (InterruptVector == 0)
      {
         DPRINT1("HalGetInterruptVector failed\n");
         return FALSE;
      }

      KeInitializeSpinLock(&DxgkpExtension->InterruptSpinLock);
      Status = IoConnectInterrupt(
         &DxgkpExtension->InterruptObject,
         DxgkpInterruptRoutine,
         DxgkpExtension->MiniportContext,
         &DxgkpExtension->InterruptSpinLock,
         InterruptVector,
         Irql,
         Irql,
         Latched,
         FALSE,
         Affinity,
         FALSE);

      if (!NT_SUCCESS(Status))
      {
        DPRINT1("IoConnectInterrupt failed with status 0x%08x\n", Status);
         return FALSE;
      }
   return TRUE;
}

/* Video Scheduler callbacks */
VOID
APIENTRY
CALLBACK
DxgkCbNotifyInterrupt(IN_CONST_HANDLE hAdapter,
                      IN_CONST_PDXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyInterruptData)
{
    switch (NotifyInterruptData->InterruptType)
    {
        case DXGK_INTERRUPT_DMA_COMPLETED:
            DPRINT("WARNING!!! Scheduler is UNIMPLEMENTED: Event DXGK_INTERRUPT_DMA_COMPLETED detected\n");
            break;
        case DXGK_INTERRUPT_DMA_PREEMPTED:
            DPRINT("WARNING!!! Scheduler is UNIMPLEMENTED: Event DXGK_INTERRUPT_DMA_PREEMPTED detected\n");
            break;
        case DXGK_INTERRUPT_CRTC_VSYNC:
            DPRINT("WARNING!!! Scheduler is UNIMPLEMENTED: Event DXGK_INTERRUPT_CRTC_VSYNC detected\n");
            break;
        case DXGK_INTERRUPT_DMA_FAULTED:
            DPRINT("WARNING!!! Scheduler is UNIMPLEMENTED: Event DXGK_INTERRUPT_DMA_FAULTED detected\n");
            break;
    }
}

VOID
APIENTRY
CALLBACK
DxgkCbNotifyDpc(IN_CONST_HANDLE hAdapter)
{
    DPRINT("WARNING!!! Scheduler is UNIMPLEMENTED: Event DxgkCbNotifyDpc detected\n");
}