/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C interrupt, input report reads and read completion
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidi2c.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * An empty report answers a reset. Ours releases the default queue, one the
 * device did on its own goes to whoever asked to hear about resets.
 */
static
VOID
NTAPI
HidI2cHandleResetReport(
    _In_ PHIDI2C_DEVICE Context)
{
    WDFREQUEST Request;

    if (Context->ResetPending)
    {
        Context->ResetPending = FALSE;

        /* A timer that already fired has released the queue itself */
        if (WdfTimerStop(Context->ResetTimer, FALSE))
            WdfIoQueueStart(Context->DefaultQueue);
        return;
    }

    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->ResetNotifyQueue, &Request)))
        WdfRequestComplete(Request, STATUS_SUCCESS);
}

/**
 * @brief
 * Reads the report the device signalled into the oldest pending read and
 * queues that read for the DPC to complete.
 *
 * Runs at passive level with the interrupt lock held.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
HidI2cEvtInterruptIsr(
    WDFINTERRUPT Interrupt,
    ULONG MessageId)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(WdfInterruptGetDevice(Interrupt));
    USHORT MaxLength = Context->Descriptor.MaxInputLength;
    PUCHAR Buffer = Context->InputBuffer;
    WDFREQUEST Request;
    WDFMEMORY Memory;
    ULONG ReportLength;
    USHORT Length;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(MessageId);

    if (Buffer == NULL)
        return TRUE;

    RtlZeroMemory(Buffer, MaxLength + sizeof(USHORT));

    Status = HidI2cReadInput(Context, Buffer, MaxLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: input read failed 0x%08lx\n", Status);
        return TRUE;
    }

    Length = (USHORT)(Buffer[0] | (Buffer[1] << 8));
    if (Length == 0)
    {
        HidI2cHandleResetReport(Context);
        return TRUE;
    }

    /* Nothing but the reset answer counts until the reset is done */
    if (Context->ResetPending)
        return TRUE;

    /* Devices that leave the length field out of the count still get their report through */
    if (Length <= sizeof(USHORT) || Length - sizeof(USHORT) > MaxLength)
    {
        DPRINT1("hidi2c: input report claims %u bytes\n", Length);
        return TRUE;
    }

    /* With no read outstanding the report is dropped */
    Status = WdfIoQueueRetrieveNextRequest(Context->ReadQueue, &Request);
    if (!NT_SUCCESS(Status))
        return TRUE;

    /* A read whose buffer cannot take the report still completes, just empty */
    ReportLength = Length - sizeof(USHORT);
    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory)) &&
        NT_SUCCESS(WdfMemoryCopyFromBuffer(Memory, 0, Buffer + sizeof(USHORT), ReportLength)))
    {
        WdfRequestSetInformation(Request, ReportLength);
    }

    Status = WdfRequestForwardToIoQueue(Request, Context->CompletionQueue);
    if (!NT_SUCCESS(Status))
        WdfRequestComplete(Request, Status);

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

/**
 * @brief
 * Completes every read the ISR has filled. A DPC that finds another one
 * already draining leaves it a note to go around again.
 */
_Use_decl_annotations_
VOID
NTAPI
HidI2cEvtInterruptDpc(
    WDFINTERRUPT Interrupt,
    WDFOBJECT AssociatedObject)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(WdfInterruptGetDevice(Interrupt));
    WDFREQUEST Request;
    BOOLEAN Again;

    UNREFERENCED_PARAMETER(AssociatedObject);

    WdfSpinLockAcquire(Context->CompletionLock);
    if (Context->CompletionRunning)
    {
        Context->CompletionRerun = TRUE;
        WdfSpinLockRelease(Context->CompletionLock);
        return;
    }
    Context->CompletionRunning = TRUE;
    Context->CompletionRerun = FALSE;
    WdfSpinLockRelease(Context->CompletionLock);

    do
    {
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->CompletionQueue, &Request)))
            WdfRequestComplete(Request, STATUS_SUCCESS);

        WdfSpinLockAcquire(Context->CompletionLock);
        Again = Context->CompletionRerun;
        if (Again)
            Context->CompletionRerun = FALSE;
        else
            Context->CompletionRunning = FALSE;
        WdfSpinLockRelease(Context->CompletionLock);
    } while (Again);
}
