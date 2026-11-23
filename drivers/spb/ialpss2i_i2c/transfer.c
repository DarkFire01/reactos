/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DesignWare I2C PIO transfer engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * Every byte is one IC_DATA_CMD write: the data for a write, a READ command
 * for a read. The first command of each transfer carries RESTART and the last
 * command of the request carries STOP, which completes on STOP_DET.
 */

#include "ialpss2i_i2c.h"

#define NDEBUG
#include <debug.h>

#define LI2C_READ_MASK  (DW_INTR_RX_FULL | DW_INTR_TX_ABRT | DW_INTR_STOP_DET)
#define LI2C_WRITE_MASK (DW_INTR_TX_EMPTY | DW_INTR_TX_ABRT | DW_INTR_STOP_DET)
#define LI2C_STOP_MASK  (DW_INTR_TX_ABRT | DW_INTR_STOP_DET)

static EVT_WDF_REQUEST_CANCEL Li2cEvtRequestCancel;

/**
 * @brief
 * Changes the interrupt mask without racing the ISR.
 */
static
VOID
NTAPI
Li2cArm(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Mask)
{
    WdfInterruptAcquireLock(Device->Interrupt);
    Li2cSetInterruptMask(Device, Mask);
    WdfInterruptReleaseLock(Device->Interrupt);
}

/**
 * @brief
 * Hands a finished request back to SpbCx, unless the cancel routine owns it.
 */
static
VOID
NTAPI
Li2cCompleteRequest(
    _In_ SPBREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ SIZE_T Information)
{
    if (WdfRequestUnmarkCancelable((WDFREQUEST)Request) == STATUS_CANCELLED)
        return;

    WdfRequestSetInformation((WDFREQUEST)Request, Information);
    SpbRequestComplete(Request, Status);
}

/**
 * @brief
 * Detaches the request from the engine. Called with the lock held.
 *
 * @return
 * The request, for the caller to complete once the lock is dropped.
 */
static
SPBREQUEST
NTAPI
Li2cDetachRequest(
    _In_ PLI2C_DEVICE Device)
{
    SPBREQUEST Request = Device->Transfer.Request;

    Li2cArm(Device, 0);
    RtlZeroMemory(&Device->Transfer, sizeof(Device->Transfer));
    return Request;
}

/**
 * @brief
 * Whether no transfer after the current one moves any data, so this one ends with STOP.
 */
static
BOOLEAN
NTAPI
Li2cIsFinalTransfer(
    _In_ PLI2C_DEVICE Device)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    SPB_TRANSFER_DESCRIPTOR Descriptor;
    ULONG Index;

    for (Index = Transfer->Index + 1; Index < Transfer->Count; Index++)
    {
        SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptor);
        SpbRequestGetTransferParameters(Transfer->Request, Index, &Descriptor, NULL);
        if (Descriptor.TransferLength != 0)
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Pushes as many READ commands as both FIFOs leave room for.
 */
static
VOID
NTAPI
Li2cQueueReads(
    _In_ PLI2C_DEVICE Device)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    BOOLEAN Final = Li2cIsFinalTransfer(Device);
    ULONG RxRoom = (DW_FIFO_DEPTH - 1) - Li2cRead(Device, DW_RXFLR);
    ULONG TxRoom = DW_FIFO_DEPTH - Li2cRead(Device, DW_TXFLR);
    ULONG Room = min(RxRoom, TxRoom);
    ULONG Pushed = 0;
    ULONG Command;

    while (Room != 0 && Transfer->Queued < Transfer->Length)
    {
        Command = DW_CMD_READ;
        if (Transfer->Queued == 0)
            Command |= DW_CMD_RESTART;
        if (Final && Transfer->Queued + 1 == Transfer->Length)
            Command |= DW_CMD_STOP;

        Li2cWrite(Device, DW_DATA_CMD, Command);
        Transfer->Queued++;
        Pushed++;
        Room--;
    }

    /* RX_FULL once every byte just asked for has arrived */
    if (Pushed != 0)
        Li2cWrite(Device, DW_RX_TL, Pushed - 1);
}

/**
 * @brief
 * Pushes as many data bytes as the TX FIFO has room for.
 */
static
VOID
NTAPI
Li2cQueueWrites(
    _In_ PLI2C_DEVICE Device)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    BOOLEAN Final = Li2cIsFinalTransfer(Device);
    ULONG Room = DW_FIFO_DEPTH - Li2cRead(Device, DW_TXFLR);
    ULONG Command;

    while (Room != 0 && Transfer->Queued < Transfer->Length)
    {
        Command = Transfer->Buffer[Transfer->Queued];
        if (Transfer->Queued == 0)
            Command |= DW_CMD_RESTART;
        if (Final && Transfer->Queued + 1 == Transfer->Length)
            Command |= DW_CMD_STOP;

        Li2cWrite(Device, DW_DATA_CMD, Command);
        Transfer->Queued++;
        Room--;
    }
}

static
VOID
NTAPI
Li2cDrainReads(
    _In_ PLI2C_DEVICE Device)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    ULONG Available = Li2cRead(Device, DW_RXFLR);

    while (Available != 0 && Transfer->Received < Transfer->Length)
    {
        Transfer->Buffer[Transfer->Received] = (UCHAR)Li2cRead(Device, DW_DATA_CMD);
        Transfer->Received++;
        Available--;
    }
}

/**
 * @brief
 * Fills the FIFO for the loaded transfer and arms the interrupts it waits on.
 */
static
VOID
NTAPI
Li2cKickTransfer(
    _In_ PLI2C_DEVICE Device)
{
    if (Device->Transfer.Direction == SpbTransferDirectionFromDevice)
    {
        Li2cQueueReads(Device);
        Li2cArm(Device, LI2C_READ_MASK);
    }
    else
    {
        Li2cQueueWrites(Device);
        Li2cArm(Device, LI2C_WRITE_MASK);
    }
}

/**
 * @brief
 * Loads the next transfer that moves data and starts it, after its delay if it has one.
 *
 * @return
 * STATUS_MORE_PROCESSING_REQUIRED while the request is in flight, otherwise the
 * status to complete it with.
 */
static
NTSTATUS
NTAPI
Li2cStartNextTransfer(
    _In_ PLI2C_DEVICE Device)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    SPB_TRANSFER_DESCRIPTOR Descriptor;
    PMDL Mdl;

    for (; Transfer->Index < Transfer->Count; Transfer->Index++)
    {
        Mdl = NULL;
        SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptor);
        SpbRequestGetTransferParameters(Transfer->Request, Transfer->Index, &Descriptor, &Mdl);

        if (Descriptor.TransferLength == 0 || Mdl == NULL)
            continue;

        /* SpbCx keeps the pages locked for the life of the request */
        Transfer->Buffer = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
        if (Transfer->Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Transfer->Length = Descriptor.TransferLength;
        Transfer->Direction = Descriptor.Direction;
        Transfer->Queued = 0;
        Transfer->Received = 0;

        if (Descriptor.DelayInUs != 0)
        {
            WdfTimerStart(Device->DelayTimer, WDF_REL_TIMEOUT_IN_US(Descriptor.DelayInUs));
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        Li2cKickTransfer(Device);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * The completion status for an abort, from IC_TX_ABRT_SOURCE.
 *
 * A missing device fails the request. A NACK on data ends it early but
 * successfully, with what the LPSS block counted as sent.
 */
static
NTSTATUS
NTAPI
Li2cAbortStatus(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Source,
    _Out_ PSIZE_T Information)
{
    *Information = 0;

    if ((Source & DW_ABRT_ADDRESS_NACK) != 0)
        return STATUS_NO_SUCH_DEVICE;

    if ((Source & DW_ABRT_DATA_NACK) != 0)
    {
        *Information = Li2cReadPrivate(Device, LPSS_TX_COUNT) & 0xFFFFFF;
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/**
 * @brief
 * Moves the transfer in flight along after an interrupt. Called with the lock held.
 *
 * @return
 * STATUS_MORE_PROCESSING_REQUIRED while the request goes on, otherwise its status.
 */
static
NTSTATUS
NTAPI
Li2cAdvance(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Status,
    _Out_ PSIZE_T Information)
{
    PLI2C_TRANSFER Transfer = &Device->Transfer;
    BOOLEAN Final = Li2cIsFinalTransfer(Device);
    BOOLEAN Moved;

    *Information = Transfer->Done;

    if (Transfer->Direction == SpbTransferDirectionFromDevice)
    {
        Li2cDrainReads(Device);
        Moved = (Transfer->Received == Transfer->Length);

        if (!Moved && Transfer->Queued < Transfer->Length)
        {
            Li2cQueueReads(Device);
            Li2cArm(Device, LI2C_READ_MASK);
            return STATUS_MORE_PROCESSING_REQUIRED;
        }
    }
    else
    {
        if (Transfer->Queued < Transfer->Length)
        {
            Li2cQueueWrites(Device);
            Li2cArm(Device, LI2C_WRITE_MASK);
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        /* Everything queued; it has moved once the FIFO drained or the bus stopped */
        Moved = (Status & DW_INTR_STOP_DET) != 0 || Li2cRead(Device, DW_TXFLR) == 0;
    }

    if ((Status & DW_INTR_STOP_DET) != 0)
    {
        if (!Moved || !Final)
        {
            DPRINT1("iaLPSS2 I2C: bus stopped in transfer %lu\n", Transfer->Index);
            *Information = Transfer->Done + Transfer->Received;
            return STATUS_UNSUCCESSFUL;
        }

        *Information = Transfer->Done + Transfer->Length;
        return STATUS_SUCCESS;
    }

    if (!Moved)
    {
        /* Reads still in flight, or writes still in the FIFO */
        if (Transfer->Direction == SpbTransferDirectionFromDevice)
        {
            Li2cWrite(Device, DW_RX_TL, (ULONG)(Transfer->Length - Transfer->Received - 1));
            Li2cArm(Device, LI2C_READ_MASK);
        }
        else
        {
            Li2cArm(Device, LI2C_WRITE_MASK);
        }
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (Final)
    {
        Li2cArm(Device, LI2C_STOP_MASK);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    Transfer->Done += Transfer->Length;
    Transfer->Index++;
    return Li2cStartNextTransfer(Device);
}

_Use_decl_annotations_
BOOLEAN
NTAPI
Li2cEvtInterruptIsr(
    WDFINTERRUPT Interrupt,
    ULONG MessageId)
{
    PLI2C_DEVICE Device = Li2cGetDevice(WdfInterruptGetDevice(Interrupt));
    ULONG Status;

    UNREFERENCED_PARAMETER(MessageId);

    Status = Li2cRead(Device, DW_RAW_INTR_STAT) & Device->InterruptMask;
    if (Status == 0)
        return FALSE;

    /* Clearing the abort wipes its source, so take it first */
    if ((Status & DW_INTR_TX_ABRT) != 0)
    {
        Device->AbortSource = Li2cRead(Device, DW_TX_ABRT_SOURCE);
        (VOID)Li2cRead(Device, DW_CLR_TX_ABRT);
    }

    /* Only what was seen is cleared, a STOP_DET arriving now stays latched */
    if ((Status & DW_INTR_STOP_DET) != 0)
        (VOID)Li2cRead(Device, DW_CLR_STOP_DET);
    if ((Status & DW_INTR_RX_OVER) != 0)
        (VOID)Li2cRead(Device, DW_CLR_RX_OVER);

    InterlockedOr((volatile LONG *)&Device->PendingStatus, (LONG)Status);

    /* The DPC re-arms what it waits for */
    Li2cSetInterruptMask(Device, 0);

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

_Use_decl_annotations_
VOID
NTAPI
Li2cEvtInterruptDpc(
    WDFINTERRUPT Interrupt,
    WDFOBJECT AssociatedObject)
{
    PLI2C_DEVICE Device = Li2cGetDevice(WdfInterruptGetDevice(Interrupt));
    SPBREQUEST Finished = NULL;
    SIZE_T Information = 0;
    NTSTATUS Result;
    ULONG Status;

    UNREFERENCED_PARAMETER(AssociatedObject);

    WdfSpinLockAcquire(Device->Lock);

    Status = (ULONG)InterlockedExchange((volatile LONG *)&Device->PendingStatus, 0);
    if (Device->Transfer.Request == NULL)
    {
        WdfSpinLockRelease(Device->Lock);
        return;
    }

    if ((Status & DW_INTR_TX_ABRT) != 0)
    {
        DPRINT1("iaLPSS2 I2C: transfer aborted, source 0x%08lx\n", Device->AbortSource);
        Result = Li2cAbortStatus(Device, Device->AbortSource, &Information);
    }
    else
    {
        Result = Li2cAdvance(Device, Status, &Information);
    }

    if (Result != STATUS_MORE_PROCESSING_REQUIRED)
        Finished = Li2cDetachRequest(Device);

    WdfSpinLockRelease(Device->Lock);

    if (Finished != NULL)
        Li2cCompleteRequest(Finished, Result, Information);
}

_Use_decl_annotations_
VOID
NTAPI
Li2cEvtDelayTimer(
    WDFTIMER Timer)
{
    PLI2C_DEVICE Device = Li2cGetDevice(WdfTimerGetParentObject(Timer));

    WdfSpinLockAcquire(Device->Lock);
    if (Device->Transfer.Request != NULL)
        Li2cKickTransfer(Device);
    WdfSpinLockRelease(Device->Lock);
}

static
VOID
NTAPI
Li2cEvtRequestCancel(
    _In_ WDFREQUEST FxRequest)
{
    PLI2C_DEVICE Device = Li2cGetDevice(WdfIoQueueGetDevice(WdfRequestGetIoQueue(FxRequest)));

    WdfSpinLockAcquire(Device->Lock);
    if ((WDFREQUEST)Device->Transfer.Request == FxRequest)
    {
        WdfTimerStop(Device->DelayTimer, FALSE);
        WdfInterruptAcquireLock(Device->Interrupt);
        Li2cAbortTransfer(Device);
        WdfInterruptReleaseLock(Device->Interrupt);
        (VOID)Li2cDetachRequest(Device);
    }
    WdfSpinLockRelease(Device->Lock);

    WdfRequestSetInformation(FxRequest, 0);
    SpbRequestComplete((SPBREQUEST)FxRequest, STATUS_CANCELLED);
}

/**
 * @brief
 * Starts a request of TransferCount transfers on the target.
 */
static
VOID
NTAPI
Li2cStartRequest(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST Request,
    _In_ ULONG TransferCount)
{
    PLI2C_DEVICE Device = Li2cGetDevice(Controller);
    PLI2C_TARGET Target = Li2cGetTarget(SpbTargetGetFileObject(SpbTarget));
    SPBREQUEST Finished = NULL;
    NTSTATUS Status;

    Status = WdfRequestMarkCancelableEx((WDFREQUEST)Request, Li2cEvtRequestCancel);
    if (!NT_SUCCESS(Status))
    {
        SpbRequestComplete(Request, Status);
        return;
    }

    WdfSpinLockAcquire(Device->Lock);

    RtlZeroMemory(&Device->Transfer, sizeof(Device->Transfer));
    Device->Transfer.Request = Request;
    Device->Transfer.Count = TransferCount;
    Device->PendingStatus = 0;
    Device->AbortSource = 0;

    Status = Li2cConfigureForTarget(Device, Target);
    if (NT_SUCCESS(Status))
        Status = Li2cStartNextTransfer(Device);

    if (Status != STATUS_MORE_PROCESSING_REQUIRED)
        Finished = Li2cDetachRequest(Device);

    WdfSpinLockRelease(Device->Lock);

    /* A request of only empty transfers still succeeds */
    if (Finished != NULL)
        Li2cCompleteRequest(Finished, Status, 0);
}

_Use_decl_annotations_
VOID
NTAPI
Li2cEvtIoSequence(
    WDFDEVICE Controller,
    SPBTARGET Target,
    SPBREQUEST Request,
    ULONG TransferCount)
{
    Li2cStartRequest(Controller, Target, Request, TransferCount);
}

_Use_decl_annotations_
VOID
NTAPI
Li2cEvtIoRead(
    WDFDEVICE Controller,
    SPBTARGET Target,
    SPBREQUEST Request,
    size_t Length)
{
    UNREFERENCED_PARAMETER(Length);

    Li2cStartRequest(Controller, Target, Request, 1);
}

_Use_decl_annotations_
VOID
NTAPI
Li2cEvtIoWrite(
    WDFDEVICE Controller,
    SPBTARGET Target,
    SPBREQUEST Request,
    size_t Length)
{
    UNREFERENCED_PARAMETER(Length);

    Li2cStartRequest(Controller, Target, Request, 1);
}
