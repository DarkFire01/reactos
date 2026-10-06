/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DesignWare I2C PIO transfer engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * PIO only. The reference also drives the LPSS integrated DMA engine for large
 * transfers (the Dma* half of that driver, some twenty-five functions); a HID
 * peripheral moves tens of bytes at a time, so the FIFO path is the one that
 * matters and DMA is a throughput optimization rather than a prerequisite.
 *
 * How a DesignWare master works: every byte of a transfer is a write to
 * IC_DATA_CMD. For a write the byte itself goes in the low bits; for a read the
 * byte is a dummy and the READ bit asks the core to clock one in. Read data
 * comes back through the RX FIFO. The RESTART bit on the first command of a
 * transfer emits a repeated start, and STOP on the last command of the last
 * transfer releases the bus, which is exactly the write-then-read shape a HID
 * descriptor fetch uses.
 */

#define NDEBUG
#include "i2cpriv.h"

/* Everything that means the transfer is over, one way or the other */
#define I2C_INTR_ERRORS (DW_IC_INTR_TX_ABRT | DW_IC_INTR_RX_OVER | DW_IC_INTR_RX_UNDER)

static
VOID
I2cCompleteCurrentTransfer(
    _In_ PI2C_DEVICE Device,
    _In_ NTSTATUS Status,
    _In_ ULONG Information)
{
    SPBREQUEST Request = Device->CurrentRequest;

    I2cControllerDisableInterrupts(Device);
    (VOID)I2cRead(Device, DW_IC_CLR_INTR);

    Device->CurrentRequest = NULL;
    Device->CurrentTarget = NULL;
    Device->CurrentBuffer = NULL;
    Device->CurrentLength = 0;
    Device->BytesWritten = 0;
    Device->BytesRead = 0;

    if (Request != NULL)
    {
        WdfRequestSetInformation((WDFREQUEST)Request, Information);
        SpbRequestComplete(Request, Status);
    }
}

/*
 * The buffer behind a transfer. SpbCx captured and locked it, so the mapping is
 * safe to hold for the life of the request; MDL_MAPPED_TO_SYSTEM_VA means the
 * page is already there and this cannot fail.
 */
static
PUCHAR
I2cMapTransferBuffer(
    _In_ PMDL Mdl,
    _Out_ PULONG Length)
{
    PUCHAR Va;

    *Length = 0;
    if (Mdl == NULL)
    {
        return NULL;
    }

    Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    if (Va == NULL)
    {
        return NULL;
    }

    *Length = MmGetMdlByteCount(Mdl);
    return Va;
}

/*
 * Load the next transfer of the sequence into the engine. Returns FALSE when
 * there are none left, which is what ends the request.
 */
static
BOOLEAN
I2cStartNextTransfer(
    _In_ PI2C_DEVICE Device)
{
    SPB_TRANSFER_DESCRIPTOR Descriptor;
    PMDL Mdl = NULL;

    while (Device->TransferIndex < Device->TransferCount)
    {
        SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptor);
        SpbRequestGetTransferParameters(Device->CurrentRequest,
                                        Device->TransferIndex,
                                        &Descriptor,
                                        &Mdl);

        Device->CurrentBuffer = I2cMapTransferBuffer(Mdl, &Device->CurrentLength);
        Device->CurrentDirection = Descriptor.Direction;
        Device->BytesWritten = 0;
        Device->BytesRead = 0;

        /* A zero-length transfer puts nothing on the bus; skip to the next */
        if (Device->CurrentBuffer != NULL && Device->CurrentLength != 0)
        {
            return TRUE;
        }

        Device->TransferIndex++;
    }

    return FALSE;
}

/*
 * ControllerDoWrite / ControllerDoRead.
 *
 * Push as many commands as the transmit FIFO will take. RESTART goes on the
 * first command of any transfer after the first, and STOP on the very last
 * command of the request; between them the core holds the bus.
 */
static
VOID
I2cPushCommands(
    _In_ PI2C_DEVICE Device)
{
    ULONG Room;
    ULONG Command;

    Room = Device->TxFifoDepth - I2cRead(Device, DW_IC_TXFLR);

    while (Room != 0 && Device->BytesWritten < Device->CurrentLength)
    {
        if (Device->CurrentDirection == SpbTransferDirectionFromDevice)
        {
            Command = DW_IC_DATA_CMD_READ;
        }
        else
        {
            Command = Device->CurrentBuffer[Device->BytesWritten];
        }

        if (Device->BytesWritten == 0 && Device->TransferIndex != 0)
        {
            Command |= DW_IC_DATA_CMD_RESTART;
        }

        if ((Device->BytesWritten + 1) == Device->CurrentLength &&
            (Device->TransferIndex + 1) == Device->TransferCount)
        {
            Command |= DW_IC_DATA_CMD_STOP;
        }

        I2cWrite(Device, DW_IC_DATA_CMD, Command);

        Device->BytesWritten++;
        Room--;
    }
}

/* Drain whatever the core has clocked in */
static
VOID
I2cDrainReceiveFifo(
    _In_ PI2C_DEVICE Device)
{
    ULONG Available = I2cRead(Device, DW_IC_RXFLR);

    while (Available != 0 && Device->BytesRead < Device->CurrentLength)
    {
        Device->CurrentBuffer[Device->BytesRead] =
            (UCHAR)(I2cRead(Device, DW_IC_DATA_CMD) & 0xFF);

        Device->BytesRead++;
        Available--;
    }
}

/* Has the transfer in flight finished putting its bytes across? */
static
BOOLEAN
I2cTransferComplete(
    _In_ PI2C_DEVICE Device)
{
    if (Device->CurrentDirection == SpbTransferDirectionFromDevice)
    {
        return (Device->BytesRead >= Device->CurrentLength);
    }

    /*
     * A write is not done when the last byte is queued, only when the FIFO has
     * drained, otherwise the next transfer's RESTART would be programmed while
     * bytes were still on the wire.
     */
    return (Device->BytesWritten >= Device->CurrentLength) &&
           ((I2cRead(Device, DW_IC_STATUS) & DW_IC_STATUS_TFE) != 0);
}

/*
 * IC_INTR_STAT is already masked by IC_INTR_MASK, so a non-zero read is ours.
 * The abort source has to be read here, before the clear below wipes it.
 */
BOOLEAN
NTAPI
I2cEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId)
{
    PI2C_DEVICE Device = I2cGetDeviceContext(WdfInterruptGetDevice(Interrupt));
    ULONG Status;

    UNREFERENCED_PARAMETER(MessageId);

    Status = I2cRead(Device, DW_IC_INTR_STAT);
    if (Status == 0)
    {
        return FALSE;
    }

    if ((Status & DW_IC_INTR_TX_ABRT) != 0)
    {
        Device->AbortSource = I2cRead(Device, DW_IC_TX_ABRT_SOURCE);
    }

    Device->InterruptStatus |= Status;

    /*
     * Mask everything rather than clear selectively: the DPC decides what to do
     * next and re-arms, so the line cannot re-assert underneath it. TX_ABRT is
     * only cleared by its own register, which the clear-all below covers.
     */
    I2cControllerDisableInterrupts(Device);
    (VOID)I2cRead(Device, DW_IC_CLR_INTR);

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

/*
 * OnInterruptDpc / ControllerProcessInterrupts.
 *
 * All the work happens here, at DISPATCH_LEVEL, so the FIFO handling can touch
 * the mapped buffers.
 */
VOID
NTAPI
I2cEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PI2C_DEVICE Device = I2cGetDeviceContext(WdfInterruptGetDevice(Interrupt));
    ULONG Status;
    ULONG Abort;

    UNREFERENCED_PARAMETER(AssociatedObject);

    Status = InterlockedExchange((volatile LONG *)&Device->InterruptStatus, 0);
    Abort = Device->AbortSource;
    Device->AbortSource = 0;

    if (Device->CurrentRequest == NULL)
    {
        return;
    }

    if ((Status & I2C_INTR_ERRORS) != 0)
    {
        /*
         * ABRT_7B_ADDR_NOACK is bit 0 and means nothing answered, which is the
         * ordinary "no device at this address" answer rather than a fault.
         */
        DPRINT1("i2c: transfer aborted, IC_TX_ABRT_SOURCE 0x%08lX\n", Abort);

        I2cCompleteCurrentTransfer(Device,
                                   ((Abort & 1) != 0) ? STATUS_DEVICE_DOES_NOT_EXIST
                                                      : STATUS_IO_DEVICE_ERROR,
                                   0);
        return;
    }

    if (Device->CurrentDirection == SpbTransferDirectionFromDevice)
    {
        I2cDrainReceiveFifo(Device);
    }

    if (!I2cTransferComplete(Device))
    {
        I2cPushCommands(Device);
        I2cControllerEnableInterrupts(Device,
                                      DW_IC_INTR_DEFAULT_MASK | DW_IC_INTR_TX_EMPTY);
        return;
    }

    Device->TransferIndex++;

    if (I2cStartNextTransfer(Device))
    {
        I2cPushCommands(Device);
        I2cControllerEnableInterrupts(Device,
                                      DW_IC_INTR_DEFAULT_MASK | DW_IC_INTR_TX_EMPTY);
        return;
    }

    I2cCompleteCurrentTransfer(Device, STATUS_SUCCESS, Device->CurrentLength);
}

/*
 * Common entry for everything SpbCx routes here. A read or a write is a
 * one-transfer sequence, which is why they share the engine.
 */
static
VOID
I2cBeginRequest(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST Request,
    _In_ ULONG TransferCount)
{
    PI2C_DEVICE Device = I2cGetDeviceContext(Controller);
    PI2C_TARGET Target;
    NTSTATUS Status;

    Target = I2cGetTargetContext(SpbTargetGetFileObject(SpbTarget));
    if (Target == NULL)
    {
        SpbRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    Device->CurrentTarget = SpbTarget;
    Device->CurrentRequest = Request;
    Device->TransferIndex = 0;
    Device->TransferCount = TransferCount;
    Device->InterruptStatus = 0;
    Device->AbortSource = 0;

    Status = I2cControllerConfigureForTransfer(Device, Target);
    if (!NT_SUCCESS(Status))
    {
        I2cCompleteCurrentTransfer(Device, Status, 0);
        return;
    }

    if (!I2cStartNextTransfer(Device))
    {
        /* Nothing to move; a request of only empty transfers is still a success */
        I2cCompleteCurrentTransfer(Device, STATUS_SUCCESS, 0);
        return;
    }

    I2cPushCommands(Device);
    I2cControllerEnableInterrupts(Device,
                                  DW_IC_INTR_DEFAULT_MASK | DW_IC_INTR_TX_EMPTY);
}

VOID
NTAPI
I2cEvtIoSequence(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST Request,
    _In_ ULONG TransferCount)
{
    I2cBeginRequest(Controller, SpbTarget, Request, TransferCount);
}

VOID
NTAPI
I2cEvtIoRead(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST Request,
    _In_ size_t Length)
{
    UNREFERENCED_PARAMETER(Length);

    I2cBeginRequest(Controller, SpbTarget, Request, 1);
}

VOID
NTAPI
I2cEvtIoWrite(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST Request,
    _In_ size_t Length)
{
    UNREFERENCED_PARAMETER(Length);

    I2cBeginRequest(Controller, SpbTarget, Request, 1);
}
