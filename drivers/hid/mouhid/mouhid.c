/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/hid/mouhid/mouhid.c
 * PURPOSE:     Mouse HID Driver
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "mouhid.h"

static USHORT MouHid_ButtonUpFlags[] =
{
    0xFF, /* unused */
    MOUSE_LEFT_BUTTON_DOWN,
    MOUSE_RIGHT_BUTTON_DOWN,
    MOUSE_MIDDLE_BUTTON_DOWN,
    MOUSE_BUTTON_4_DOWN,
    MOUSE_BUTTON_5_DOWN
};

static USHORT MouHid_ButtonDownFlags[] =
{
    0xFF, /* unused */
    MOUSE_LEFT_BUTTON_UP,
    MOUSE_RIGHT_BUTTON_UP,
    MOUSE_MIDDLE_BUTTON_UP,
    MOUSE_BUTTON_4_UP,
    MOUSE_BUTTON_5_UP
};


/*
 * Report an axis this driver could not read, once per axis.
 *
 * GetButtonMove acts on HIDP_STATUS_BAD_LOG_PHY_VALUES and silently ignores
 * every other failure, leaving that axis at zero.  A device whose X resolves
 * and whose Y does not therefore moves only left and right, with nothing said
 * anywhere - which is a hard thing to find from the outside.  Say it once so
 * the status is on the record.
 */
static
VOID
MouHid_ReportAxisFailure(
    IN PCSTR Axis,
    IN NTSTATUS Status)
{
    static BOOLEAN Reported[2] = { FALSE, FALSE };
    ULONG Index = (Axis[0] == 'Y') ? 1 : 0;

    if (!Reported[Index])
    {
        Reported[Index] = TRUE;
        DPRINT1("[MOUHID] could not read %s: 0x%08lX - this axis reads as zero\n",
                Axis, Status);
    }
}


VOID
MouHid_GetButtonMove(
    IN PMOUHID_DEVICE_EXTENSION DeviceExtension,
    OUT PLONG LastX,
    OUT PLONG LastY)
{
    NTSTATUS Status;
    ULONG ValueX, ValueY;

    /* init result */
    *LastX = 0;
    *LastY = 0;

    if (!DeviceExtension->MouseAbsolute)
    {
        /* get scaled usage value x */
        Status =  HidP_GetScaledUsageValue(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       HIDP_LINK_COLLECTION_UNSPECIFIED,
                                       HID_USAGE_GENERIC_X,
                                       LastX,
                                       DeviceExtension->PreparsedData,
                                       DeviceExtension->Report,
                                       DeviceExtension->ReportLength);

        if (Status != HIDP_STATUS_SUCCESS)
        {
            MouHid_ReportAxisFailure("X", Status);

            /*
             * Do not re-decide the pointer's mode from a failed read - that is
             * settled at start from the caps.  Fall back to the unscaled value
             * for this report only.
             */
            if (Status == HIDP_STATUS_BAD_LOG_PHY_VALUES)
            {
                /* get unscaled value */
                Status = HidP_GetUsageValue(HidP_Input,
                                        HID_USAGE_PAGE_GENERIC,
                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                        HID_USAGE_GENERIC_X,
                                        &ValueX,
                                        DeviceExtension->PreparsedData,
                                        DeviceExtension->Report,
                                        DeviceExtension->ReportLength);

                /* FIXME handle error */
                ASSERT(Status == HIDP_STATUS_SUCCESS);

                /* absolute pointing devices values need be in range 0 - 0xffff */
                ASSERT(DeviceExtension->ValueCapsX.LogicalMax > 0);
                ASSERT(DeviceExtension->ValueCapsX.LogicalMax > DeviceExtension->ValueCapsX.LogicalMin);

                /* convert to logical range */
                *LastX = (ValueX * VIRTUAL_SCREEN_SIZE_X) / DeviceExtension->ValueCapsX.LogicalMax;
            }
        }
    }
    else
    {
        /* get unscaled value */
        Status = HidP_GetUsageValue(HidP_Input,
                                    HID_USAGE_PAGE_GENERIC,
                                    HIDP_LINK_COLLECTION_UNSPECIFIED,
                                    HID_USAGE_GENERIC_X,
                                    &ValueX,
                                    DeviceExtension->PreparsedData,
                                    DeviceExtension->Report,
                                    DeviceExtension->ReportLength);

        /* FIXME handle error */
        ASSERT(Status == HIDP_STATUS_SUCCESS);

        /* absolute pointing devices values need be in range 0 - 0xffff */
        ASSERT(DeviceExtension->ValueCapsX.LogicalMax > 0);
        ASSERT(DeviceExtension->ValueCapsX.LogicalMax > DeviceExtension->ValueCapsX.LogicalMin);

        /* convert to logical range */
        *LastX = (ValueX * VIRTUAL_SCREEN_SIZE_X) / DeviceExtension->ValueCapsX.LogicalMax;
    }

    if (!DeviceExtension->MouseAbsolute)
    {
        /* get scaled usage value y */
        Status =  HidP_GetScaledUsageValue(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       HIDP_LINK_COLLECTION_UNSPECIFIED,
                                       HID_USAGE_GENERIC_Y,
                                       LastY,
                                       DeviceExtension->PreparsedData,
                                       DeviceExtension->Report,
                                       DeviceExtension->ReportLength);

        if (Status != HIDP_STATUS_SUCCESS)
        {
            MouHid_ReportAxisFailure("Y", Status);

            /* As above: the mode is not re-decided here. */
            if (Status == HIDP_STATUS_BAD_LOG_PHY_VALUES)
            {
                // get unscaled value
                Status = HidP_GetUsageValue(HidP_Input,
                                        HID_USAGE_PAGE_GENERIC,
                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                        HID_USAGE_GENERIC_Y,
                                        &ValueY,
                                        DeviceExtension->PreparsedData,
                                        DeviceExtension->Report,
                                        DeviceExtension->ReportLength);

                /* FIXME handle error */
                ASSERT(Status == HIDP_STATUS_SUCCESS);

                /* absolute pointing devices values need be in range 0 - 0xffff */
                ASSERT(DeviceExtension->ValueCapsY.LogicalMax > 0);
                ASSERT(DeviceExtension->ValueCapsY.LogicalMax > DeviceExtension->ValueCapsY.LogicalMin);

                /* convert to logical range */
                *LastY = (ValueY * VIRTUAL_SCREEN_SIZE_Y) / DeviceExtension->ValueCapsY.LogicalMax;
            }
        }
    }
    else
    {
        // get unscaled value
        Status = HidP_GetUsageValue(HidP_Input,
                                HID_USAGE_PAGE_GENERIC,
                                HIDP_LINK_COLLECTION_UNSPECIFIED,
                                HID_USAGE_GENERIC_Y,
                                &ValueY,
                                DeviceExtension->PreparsedData,
                                DeviceExtension->Report,
                                DeviceExtension->ReportLength);

        /* FIXME handle error */
        ASSERT(Status == HIDP_STATUS_SUCCESS);

        /* absolute pointing devices values need be in range 0 - 0xffff */
        ASSERT(DeviceExtension->ValueCapsY.LogicalMax > 0);
        ASSERT(DeviceExtension->ValueCapsY.LogicalMax > DeviceExtension->ValueCapsY.LogicalMin);

        /* convert to logical range */
        *LastY = (ValueY * VIRTUAL_SCREEN_SIZE_Y) / DeviceExtension->ValueCapsY.LogicalMax;
    }
}

VOID
MouHid_GetButtonFlags(
    IN PMOUHID_DEVICE_EXTENSION DeviceExtension,
    OUT PUSHORT ButtonFlags,
    OUT PUSHORT Flags)
{
    NTSTATUS Status;
    USAGE Usage;
    ULONG Index;
    PUSAGE TempList;
    ULONG CurrentUsageListLength;

    /* init flags */
    *ButtonFlags = 0;
    *Flags = 0;

    /* get usages */
    CurrentUsageListLength = DeviceExtension->UsageListLength;
    Status = HidP_GetUsages(HidP_Input,
                            HID_USAGE_PAGE_BUTTON,
                            HIDP_LINK_COLLECTION_UNSPECIFIED,
                            DeviceExtension->CurrentUsageList,
                            &CurrentUsageListLength,
                            DeviceExtension->PreparsedData,
                            DeviceExtension->Report,
                            DeviceExtension->ReportLength);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        DPRINT1("MouHid_GetButtonFlags failed to get usages with %x\n", Status);
        return;
    }

    /* extract usage list difference */
    Status = HidP_UsageListDifference(DeviceExtension->PreviousUsageList,
                                      DeviceExtension->CurrentUsageList,
                                      DeviceExtension->BreakUsageList,
                                      DeviceExtension->MakeUsageList,
                                      DeviceExtension->UsageListLength);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        DPRINT1("MouHid_GetButtonFlags failed to get usages with %x\n", Status);
        return;
    }

    if (DeviceExtension->UsageListLength)
    {
        Index = 0;
        do
        {
            /* get usage */
            Usage = DeviceExtension->BreakUsageList[Index];
            if (!Usage)
                break;

            if (Usage <= 5)
            {
                /* max 5 buttons supported */
                *ButtonFlags |= MouHid_ButtonDownFlags[Usage];
            }

            /* move to next index*/
            Index++;
        }while(Index < DeviceExtension->UsageListLength);
    }

    if (DeviceExtension->UsageListLength)
    {
        Index = 0;
        do
        {
            /* get usage */
            Usage = DeviceExtension->MakeUsageList[Index];
            if (!Usage)
                break;

            if (Usage <= 5)
            {
                /* max 5 buttons supported */
                *ButtonFlags |= MouHid_ButtonUpFlags[Usage];
            }

            /* move to next index*/
            Index++;
        }while(Index < DeviceExtension->UsageListLength);
    }

    /* now switch the previous list with current list */
    TempList = DeviceExtension->CurrentUsageList;
    DeviceExtension->CurrentUsageList = DeviceExtension->PreviousUsageList;
    DeviceExtension->PreviousUsageList = TempList;

    if (DeviceExtension->MouseAbsolute)
    {
        // mouse operates absolute
        *Flags |= MOUSE_MOVE_ABSOLUTE;
    }
}

VOID
MouHid_DispatchInputData(
    IN PMOUHID_DEVICE_EXTENSION DeviceExtension,
    IN PMOUSE_INPUT_DATA InputData)
{
    KIRQL OldIrql;
    ULONG InputDataConsumed;

    if (!DeviceExtension->ClassService)
        return;

    /* sanity check */
    ASSERT(DeviceExtension->ClassService);
    ASSERT(DeviceExtension->ClassDeviceObject);

    /* raise irql */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* dispatch input data */
    (*(PSERVICE_CALLBACK_ROUTINE)DeviceExtension->ClassService)(DeviceExtension->ClassDeviceObject, InputData, InputData + 1, &InputDataConsumed);

    /* lower irql to previous level */
    KeLowerIrql(OldIrql);
}

NTSTATUS
NTAPI
MouHid_ReadCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    USHORT ButtonFlags;
    LONG UsageValue;
    NTSTATUS Status;
    LONG LastX, LastY;
    MOUSE_INPUT_DATA MouseInputData;
    USHORT Flags;

    /* get device extension */
    DeviceExtension = Context;

    if (Irp->IoStatus.Status == STATUS_PRIVILEGE_NOT_HELD ||
        Irp->IoStatus.Status == STATUS_DEVICE_NOT_CONNECTED ||
        Irp->IoStatus.Status == STATUS_CANCELLED ||
        DeviceExtension->StopReadReport)
    {
        /* failed to read or should be stopped*/
        DPRINT1("[MOUHID] ReadCompletion terminating read Status %x\n", Irp->IoStatus.Status);

        /* report no longer active */
        DeviceExtension->ReadReportActive = FALSE;

        /* request stopping of the report cycle */
        DeviceExtension->StopReadReport = FALSE;

        /* signal completion event */
        KeSetEvent(&DeviceExtension->ReadCompletionEvent, 0, 0);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /*
     * A completion that failed, or that carries no report, has nothing in it
     * to parse.
     *
     * Only three statuses were treated as terminal, so every other failure fell
     * straight through into the parser, which read whatever the previous report
     * had left in the buffer and delivered it as real input - and then re-armed.
     * A device erroring in a loop therefore produced a stream of phantom input
     * and a tight resubmit loop at DISPATCH_LEVEL.
     *
     * Re-arm without parsing, and stop if it never recovers rather than
     * spinning forever.
     */
    if (!NT_SUCCESS(Irp->IoStatus.Status) || Irp->IoStatus.Information == 0)
    {
        if (++DeviceExtension->ReadErrorCount > MOUHID_MAX_READ_ERRORS)
        {
            DPRINT1("[MOUHID] giving up after %lu bad reads, last Status %x\n",
                    DeviceExtension->ReadErrorCount, Irp->IoStatus.Status);
            DeviceExtension->ReadReportActive = FALSE;
            DeviceExtension->StopReadReport = FALSE;
            KeSetEvent(&DeviceExtension->ReadCompletionEvent, 0, 0);
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        MouHid_InitiateRead(DeviceExtension);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    DeviceExtension->ReadErrorCount = 0;

    /* get mouse change */
    MouHid_GetButtonMove(DeviceExtension, &LastX, &LastY);

    /* get mouse change flags */
    MouHid_GetButtonFlags(DeviceExtension, &ButtonFlags, &Flags);

    /* init input data */
    RtlZeroMemory(&MouseInputData, sizeof(MOUSE_INPUT_DATA));

    /* init input data */
    MouseInputData.ButtonFlags = ButtonFlags;
    MouseInputData.Flags = Flags;
    MouseInputData.LastX = LastX;
    MouseInputData.LastY = LastY;

    /* detect mouse wheel change */
    if (DeviceExtension->MouseIdentifier == WHEELMOUSE_HID_HARDWARE)
    {
        /* get usage */
        UsageValue = 0;
        Status = HidP_GetScaledUsageValue(HidP_Input,
                                          HID_USAGE_PAGE_GENERIC,
                                          HIDP_LINK_COLLECTION_UNSPECIFIED,
                                          HID_USAGE_GENERIC_WHEEL,
                                          &UsageValue,
                                          DeviceExtension->PreparsedData,
                                          DeviceExtension->Report,
                                          DeviceExtension->ReportLength);
        if (Status == HIDP_STATUS_SUCCESS && UsageValue != 0)
        {
            /* store wheel status */
            MouseInputData.ButtonFlags |= MOUSE_WHEEL;
            MouseInputData.ButtonData = (USHORT)(UsageValue * WHEEL_DELTA);
        }
        else
        {
            DPRINT("[MOUHID] failed to get wheel status with %x\n", Status);
        }
    }

    /*
     * The first few reports, raw and decoded, so a misbehaving axis can be
     * read straight off the log instead of guessed at.
     *
     * Bounded two ways: a handful of reports, because on a multiprocessor
     * kernel every debug print freezes every other processor; and by
     * ReportLength, because the dump this replaces named seven bytes
     * unconditionally - more than a boot mouse report actually holds.
     */
    if (DeviceExtension->ReportsTraced < 8)
    {
        CHAR  Hex[3 * 24 + 1];
        ULONG Count = DeviceExtension->ReportLength;
        ULONG i;

        DeviceExtension->ReportsTraced++;

        /*
         * Every byte the report holds, not a fixed eight.
         *
         * The fixed count was wrong in both directions: it named more bytes
         * than a boot mouse report has, and fewer than this one does. A nine
         * byte report was dumped as eight, and the byte it left out was the
         * high half of the very field that was reading back wrong.
         */
        if (Count > 24)
        {
            Count = 24;
        }

        for (i = 0; i < Count; i++)
        {
            CHAR *p = &Hex[i * 3];
            UCHAR b = (UCHAR)DeviceExtension->Report[i];

            p[0] = "0123456789abcdef"[b >> 4];
            p[1] = "0123456789abcdef"[b & 0xF];
            p[2] = ' ';
        }
        Hex[Count * 3] = '\0';

        /* Information is how many bytes the read actually produced. A report
           that comes up short leaves the tail of the buffer holding whatever
           was there before, which for a 16-bit field split across the end
           means an axis that can only ever express its low byte */
        DPRINT1("[MOUHID] len %lu got %lu  %s -> X %ld Y %ld btn 0x%x %s\n",
                DeviceExtension->ReportLength,
                (ULONG)Irp->IoStatus.Information, Hex,
                LastX, LastY, ButtonFlags,
                DeviceExtension->MouseAbsolute ? "ABSOLUTE" : "relative");
    }

    DPRINT("[MOUHID] LastX %ld LastY %ld Flags %x ButtonFlags %x ButtonData %x\n", MouseInputData.LastX, MouseInputData.LastY, MouseInputData.Flags, MouseInputData.ButtonFlags, MouseInputData.ButtonData);

    /* dispatch mouse action */
    MouHid_DispatchInputData(DeviceExtension, &MouseInputData);

    /* re-init read */
    MouHid_InitiateRead(DeviceExtension);

    /* stop completion */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
MouHid_InitiateRead(
    IN PMOUHID_DEVICE_EXTENSION DeviceExtension)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    /*
     * Do not re-enter on the caller's stack.
     *
     * MouHid_ReadCompletion re-arms by calling straight back in here, and the
     * request below can complete inline - hidclass refuses a malformed read
     * without ever pending it.  The completion routine then runs on this very
     * stack and calls in again, and the whole thing recurses until the kernel
     * stack is gone.  That is a double fault, seen as 0x7F(8) after a dozen
     * rejected reads.
     *
     * Count the submissions instead: the first caller owns the loop below and
     * a caller nested inside it only leaves its request behind and returns.
     * HidClassFDO_SubmitRead solves the identical problem the identical way.
     */
    if (InterlockedIncrement(&DeviceExtension->ReadSubmitCount) > 1)
    {
        return STATUS_PENDING;
    }

    do
    {
    /* re-use irp */
    IoReuseIrp(DeviceExtension->Irp, STATUS_SUCCESS);

    /* init irp */
    DeviceExtension->Irp->MdlAddress = DeviceExtension->ReportMDL;

    /* get next stack location */
    IoStack = IoGetNextIrpStackLocation(DeviceExtension->Irp);

    /* init stack location */
    IoStack->Parameters.Read.Length = DeviceExtension->ReportLength;
    IoStack->Parameters.Read.Key = 0;
    IoStack->Parameters.Read.ByteOffset.QuadPart = 0LL;
    IoStack->MajorFunction = IRP_MJ_READ;
    IoStack->FileObject = DeviceExtension->FileObject;

    /* set completion routine */
    IoSetCompletionRoutine(DeviceExtension->Irp, MouHid_ReadCompletion, DeviceExtension, TRUE, TRUE, TRUE);

    /* read is active */
    DeviceExtension->ReadReportActive = TRUE;

    /* start the read */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, DeviceExtension->Irp);

    } while (InterlockedDecrement(&DeviceExtension->ReadSubmitCount) > 0);

    /* done */
    return Status;
}

NTSTATUS
NTAPI
MouHid_CreateCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    KeSetEvent(Context, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
NTAPI
MouHid_Create(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    KEVENT Event;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    DPRINT("MOUHID: IRP_MJ_CREATE\n");

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* get stack location */
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    /* copy stack location to next */
    IoCopyCurrentIrpStackLocationToNext(Irp);

    /* init event */
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    /* prepare irp */
    IoSetCompletionRoutine(Irp, MouHid_CreateCompletion, &Event, TRUE, TRUE, TRUE);

    /* call lower driver */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        /* request pending */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        /*
         * Take the result out of the irp.
         *
         * The completion routine returns STATUS_MORE_PROCESSING_REQUIRED, so
         * the irp is still ours and its IoStatus holds what actually happened.
         * Status is still STATUS_PENDING at this point, and
         * NT_SUCCESS(STATUS_PENDING) is TRUE, so the check below used to pass
         * for a create the stack had refused - and this then stored the file
         * object and started the read cycle on a device that never opened,
         * leaving ReadReportActive set on a stack that is not going to deliver
         * anything.  Every close and remove path keys off that flag.
         */
        Status = Irp->IoStatus.Status;
    }

    /* check for success */
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    /* is the driver already in use */
    if (DeviceExtension->FileObject == NULL)
    {
         /* did the caller specify correct attributes */
         ASSERT(IoStack->Parameters.Create.SecurityContext);
         if (IoStack->Parameters.Create.SecurityContext->DesiredAccess)
         {
             /* store file object */
             DeviceExtension->FileObject = IoStack->FileObject;

             /* reset event */
             KeClearEvent(&DeviceExtension->ReadCompletionEvent);

             /* initiating read */
             Status = MouHid_InitiateRead(DeviceExtension);
             DPRINT("[MOUHID] MouHid_InitiateRead: status %x\n", Status);
             if (Status == STATUS_PENDING)
             {
                 /* report irp is pending */
                 Status = STATUS_SUCCESS;
             }
         }
    }

    /* complete request */
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}


NTSTATUS
NTAPI
MouHid_Close(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION IoStack;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    DPRINT("[MOUHID] IRP_MJ_CLOSE ReadReportActive %x\n", DeviceExtension->ReadReportActive);

    /*
     * Only the handle that started the report cycle may stop it.
     *
     * MouHid_Create hands ownership to the first opener and starts reading on
     * its behalf; a later opener is let in but starts nothing.  Tearing the
     * cycle down for any close at all meant that when something opened the
     * collection briefly and closed it again - the power manager does exactly
     * that to every HID collection, to ask for its system button capabilities -
     * the read that mouclass owned was cancelled and never restarted.  The
     * device then stopped responding with the whole stack still loaded and
     * looking perfectly healthy.
     *
     * This was previously hidden by the deadlock in this function: such a close
     * never reached the teardown at all.
     */
    if (IoStack->FileObject != DeviceExtension->FileObject)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    }

    if (DeviceExtension->ReadReportActive)
    {
        LARGE_INTEGER Timeout;

        /* request stopping of the report cycle */
        DeviceExtension->StopReadReport = TRUE;

        /*
         * Cancel first, then wait.  This used to be the other way round, and
         * it could not work.
         *
         * StopReadReport is only ever looked at by MouHid_ReadCompletion, which
         * runs when the outstanding IOCTL_HID_READ_REPORT completes - and that
         * only happens when the device actually has something to report.  A
         * mouse nobody is touching never completes one, so the wait was
         * waiting for something that only the cancel underneath it could
         * produce.
         *
         * It deadlocked the whole boot: the power manager opens and closes
         * every HID collection to ask for its button capabilities
         * (PopAddPolicyDevice -> PopGetPolicyDeviceObject -> NtClose), so
         * Phase 1 initialisation came through here and stopped for ever.  The
         * machine then sits with every processor idle and never reaches smss.
         *
         * Cancelling completes the request with STATUS_CANCELLED, which is one
         * of the statuses the completion routine treats as terminal, so it
         * signals the event and this wait ends.
         */
        IoCancelIrp(DeviceExtension->Irp);

        /*
         * Bounded, deliberately - the reference waits without a limit.  The
         * cancel above should always produce a completion, but a lower driver
         * that does not honour it would otherwise take the machine with it,
         * and losing a boot to a silent wait is exactly what this is fixing.
         */
        Timeout.QuadPart = -50000000LL;   /* 5 seconds */
        if (KeWaitForSingleObject(&DeviceExtension->ReadCompletionEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  &Timeout) == STATUS_TIMEOUT)
        {
            DPRINT1("[MOUHID] read cycle did not stop after cancel\n");
        }
    }

    DPRINT("[MOUHID] IRP_MJ_CLOSE ReadReportActive %x\n", DeviceExtension->ReadReportActive);

    /* remove file object */
    DeviceExtension->FileObject = NULL;

    /* skip location */
    IoSkipCurrentIrpStackLocation(Irp);

    /* pass irp to down the stack */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
MouHid_InternalDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PMOUSE_ATTRIBUTES Attributes;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    PCONNECT_DATA Data;

    /* get current stack location */
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    DPRINT("[MOUHID] InternalDeviceControl %x\n", IoStack->Parameters.DeviceIoControl.IoControlCode);

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* handle requests */
    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_MOUSE_QUERY_ATTRIBUTES:
         /* verify output buffer length */
         if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_ATTRIBUTES))
         {
             /* invalid request */
             DPRINT1("[MOUHID] IOCTL_MOUSE_QUERY_ATTRIBUTES Buffer too small\n");
             Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
             IoCompleteRequest(Irp, IO_NO_INCREMENT);
             return STATUS_BUFFER_TOO_SMALL;
         }

         /* get output buffer */
         Attributes = Irp->AssociatedIrp.SystemBuffer;

         /* type of mouse */
         Attributes->MouseIdentifier = DeviceExtension->MouseIdentifier;

         /* number of buttons */
         Attributes->NumberOfButtons = DeviceExtension->UsageListLength;

         /* sample rate not used for usb */
         Attributes->SampleRate = 0;

         /* queue length */
         Attributes->InputDataQueueLength = 2;

         DPRINT("[MOUHID] MouseIdentifier %x\n", Attributes->MouseIdentifier);
         DPRINT("[MOUHID] NumberOfButtons %x\n", Attributes->NumberOfButtons);
         DPRINT("[MOUHID] SampleRate %x\n", Attributes->SampleRate);
         DPRINT("[MOUHID] InputDataQueueLength %x\n", Attributes->InputDataQueueLength);

         /* complete request */
         Irp->IoStatus.Information = sizeof(MOUSE_ATTRIBUTES);
         Irp->IoStatus.Status = STATUS_SUCCESS;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_SUCCESS;

    case IOCTL_INTERNAL_MOUSE_CONNECT:
         /* verify input buffer length */
         if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA))
         {
             /* invalid request */
             Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
             IoCompleteRequest(Irp, IO_NO_INCREMENT);
             return STATUS_INVALID_PARAMETER;
         }

         /* is it already connected */
         if (DeviceExtension->ClassService)
         {
             /* already connected */
             Irp->IoStatus.Status = STATUS_SHARING_VIOLATION;
             IoCompleteRequest(Irp, IO_NO_INCREMENT);
             return STATUS_SHARING_VIOLATION;
         }

         /* get connect data */
         Data = IoStack->Parameters.DeviceIoControl.Type3InputBuffer;

         /* store connect details */
         DeviceExtension->ClassDeviceObject = Data->ClassDeviceObject;
         DeviceExtension->ClassService = Data->ClassService;

         /* completed successfully */
         Irp->IoStatus.Status = STATUS_SUCCESS;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_SUCCESS;

    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        /* not supported */
        Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_IMPLEMENTED;

    case IOCTL_INTERNAL_MOUSE_ENABLE:
        /* not supported */
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;

    case IOCTL_INTERNAL_MOUSE_DISABLE:
        /* not supported */
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    DPRINT1("[MOUHID] Unknown DeviceControl %x\n", IoStack->Parameters.DeviceIoControl.IoControlCode);
    /* unknown request not supported */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
MouHid_DeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* skip stack location */
    IoSkipCurrentIrpStackLocation(Irp);

    /* pass and forget */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
MouHid_Power(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    DeviceExtension = DeviceObject->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
MouHid_SystemControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    DeviceExtension = DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
MouHid_SubmitRequest(
    PDEVICE_OBJECT DeviceObject,
    ULONG IoControlCode,
    ULONG InputBufferSize,
    PVOID InputBuffer,
    ULONG OutputBufferSize,
    PVOID OutputBuffer)
{
    KEVENT Event;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    PIRP Irp;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* init event */
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    /* build request */
    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceExtension->NextDeviceObject,
                                        InputBuffer,
                                        InputBufferSize,
                                        OutputBuffer,
                                        OutputBufferSize,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* send request */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        /* wait for request to complete */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    /* done */
    return Status;
}

NTSTATUS
NTAPI
MouHid_StartDevice(
    IN PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status;
    ULONG Buttons;
    HID_COLLECTION_INFORMATION Information;
    PVOID PreparsedData;
    HIDP_CAPS Capabilities;
    USHORT ValueCapsLength;
    HIDP_VALUE_CAPS ValueCaps;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    PUSAGE Buffer;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* query collection information */
    Status = MouHid_SubmitRequest(DeviceObject,
                                  IOCTL_HID_GET_COLLECTION_INFORMATION,
                                  0,
                                  NULL,
                                  sizeof(HID_COLLECTION_INFORMATION),
                                  &Information);
    if (!NT_SUCCESS(Status))
    {
        /* failed to query collection information */
        DPRINT1("[MOUHID] failed to obtain collection information with %x\n", Status);
        return Status;
    }

    /* lets allocate space for preparsed data */
    PreparsedData = ExAllocatePoolWithTag(NonPagedPool, Information.DescriptorSize, MOUHID_TAG);
    if (!PreparsedData)
    {
        /* no memory */
        DPRINT1("[MOUHID] no memory size %u\n", Information.DescriptorSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* now obtain the preparsed data */
    Status = MouHid_SubmitRequest(DeviceObject,
                                  IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                  0,
                                  NULL,
                                  Information.DescriptorSize,
                                  PreparsedData);
    if (!NT_SUCCESS(Status))
    {
        /* failed to get preparsed data */
        DPRINT1("[MOUHID] failed to obtain collection information with %x\n", Status);
        ExFreePoolWithTag(PreparsedData, MOUHID_TAG);
        return Status;
    }

    /* lets get the caps */
    Status = HidP_GetCaps(PreparsedData, &Capabilities);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        /* failed to get capabilities */
        DPRINT1("[MOUHID] failed to obtain caps with %x\n", Status);
        ExFreePoolWithTag(PreparsedData, MOUHID_TAG);
        return Status;
    }

    DPRINT("[MOUHID] Usage %x UsagePage %x InputReportLength %lu\n", Capabilities.Usage, Capabilities.UsagePage, Capabilities.InputReportByteLength);

    /* verify capabilities */
    if ((Capabilities.Usage != HID_USAGE_GENERIC_POINTER && Capabilities.Usage != HID_USAGE_GENERIC_MOUSE) || Capabilities.UsagePage != HID_USAGE_PAGE_GENERIC)
    {
        /* not supported */
        ExFreePoolWithTag(PreparsedData, MOUHID_TAG);
        return STATUS_UNSUCCESSFUL;
    }

    /* init input report */
    DeviceExtension->ReportLength = Capabilities.InputReportByteLength;
    ASSERT(DeviceExtension->ReportLength);
    DeviceExtension->Report = ExAllocatePoolWithTag(NonPagedPool, DeviceExtension->ReportLength, MOUHID_TAG);
    ASSERT(DeviceExtension->Report);
    RtlZeroMemory(DeviceExtension->Report, DeviceExtension->ReportLength);

    /* build mdl */
    DeviceExtension->ReportMDL = IoAllocateMdl(DeviceExtension->Report,
                                               DeviceExtension->ReportLength,
                                               FALSE,
                                               FALSE,
                                               NULL);
    ASSERT(DeviceExtension->ReportMDL);

    /* init mdl */
    MmBuildMdlForNonPagedPool(DeviceExtension->ReportMDL);

    /* get max number of buttons */
    Buttons = HidP_MaxUsageListLength(HidP_Input,
                                      HID_USAGE_PAGE_BUTTON,
                                      PreparsedData);
    DPRINT("[MOUHID] Buttons %lu\n", Buttons);
    ASSERT(Buttons > 0);

    /* now allocate an array for those buttons */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, sizeof(USAGE) * 4 * Buttons, MOUHID_TAG);
    if (!Buffer)
    {
        /* no memory */
        ExFreePoolWithTag(PreparsedData, MOUHID_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DeviceExtension->UsageListBuffer = Buffer;

    /* init usage lists */
    RtlZeroMemory(Buffer, sizeof(USAGE) * 4 * Buttons);
    DeviceExtension->CurrentUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->PreviousUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->MakeUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->BreakUsageList = Buffer;

    /* store number of buttons */
    DeviceExtension->UsageListLength = (USHORT)Buttons;

    /* store preparsed data */
    DeviceExtension->PreparsedData = PreparsedData;

    /*
     * Whether this is a relative or an absolute pointer is a property of the
     * device, and the descriptor states it: HIDP_VALUE_CAPS.IsAbsolute on the
     * X axis.  Read it once, here, and believe it.
     *
     * MouHid_GetButtonMove used to infer it instead, by treating
     * HIDP_STATUS_BAD_LOG_PHY_VALUES from HidP_GetScaledUsageValue as "this
     * must be an absolute device" and latching MouseAbsolute for good.  That
     * inference is not in the reference and it is wrong here, because this
     * tree's parser returns that status for any axis whose logical range is
     * not negative - which plenty of ordinary relative mice have.  A relative
     * mouse so misdetected then had every delta run through
     * (Value * VIRTUAL_SCREEN_SIZE_X) / LogicalMax, turning one count of
     * movement into most of a screen: the pointer jumps to the edge and stays
     * there.
     *
     * The reference does exactly this and nothing more - one
     * HidP_GetSpecificValueCaps for usage 0x30 on the Generic Desktop page,
     * then "if (ValueCaps.IsAbsolute)".
     */
    ValueCapsLength = 1;
    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       HIDP_LINK_COLLECTION_UNSPECIFIED,
                                       HID_USAGE_GENERIC_X,
                                       &DeviceExtension->ValueCapsX,
                                       &ValueCapsLength,
                                       PreparsedData);
    if (Status == HIDP_STATUS_SUCCESS)
    {
        DeviceExtension->MouseAbsolute = DeviceExtension->ValueCapsX.IsAbsolute
                                             ? TRUE : FALSE;
    }

    ValueCapsLength = 1;
    HidP_GetSpecificValueCaps(HidP_Input,
                              HID_USAGE_PAGE_GENERIC,
                              HIDP_LINK_COLLECTION_UNSPECIFIED,
                              HID_USAGE_GENERIC_Y,
                              &DeviceExtension->ValueCapsY,
                              &ValueCapsLength,
                              PreparsedData);

    /*
     * Both axes, once, at a level that survives NDEBUG.
     *
     * BitSize is the field that decides how the value is read back: the parser
     * sign-extends through a mask built from it, so an axis matched against a
     * wider item than it occupies comes back unsigned. Two axes of the same
     * device disagreeing here is exactly the shape of a pointer that moves on
     * one axis and runs to the edge on the other, and none of it was visible.
     */
    DPRINT1("[MOUHID] X caps: bits %u abs %u logical %ld..%ld | "
            "Y caps: bits %u abs %u logical %ld..%ld -> %s pointer\n",
            DeviceExtension->ValueCapsX.BitSize,
            DeviceExtension->ValueCapsX.IsAbsolute,
            DeviceExtension->ValueCapsX.LogicalMin,
            DeviceExtension->ValueCapsX.LogicalMax,
            DeviceExtension->ValueCapsY.BitSize,
            DeviceExtension->ValueCapsY.IsAbsolute,
            DeviceExtension->ValueCapsY.LogicalMin,
            DeviceExtension->ValueCapsY.LogicalMax,
            DeviceExtension->MouseAbsolute ? "absolute" : "relative");

    /* now check for wheel mouse support */
    ValueCapsLength = 1;
    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       HIDP_LINK_COLLECTION_UNSPECIFIED,
                                       HID_USAGE_GENERIC_WHEEL,
                                       &ValueCaps,
                                       &ValueCapsLength,
                                       PreparsedData);
    if (Status == HIDP_STATUS_SUCCESS )
    {
        /* mouse has wheel support */
        DeviceExtension->MouseIdentifier = WHEELMOUSE_HID_HARDWARE;
        DeviceExtension->WheelUsagePage = ValueCaps.UsagePage;
        DPRINT("[MOUHID] mouse wheel support detected\n", Status);
    }
    else
    {
        /* check if the mouse has z-axis */
        ValueCapsLength = 1;
        Status = HidP_GetSpecificValueCaps(HidP_Input,
                                           HID_USAGE_PAGE_GENERIC,
                                           HIDP_LINK_COLLECTION_UNSPECIFIED,
                                           HID_USAGE_GENERIC_Z,
                                           &ValueCaps,
                                           &ValueCapsLength,
                                           PreparsedData);
        if (Status == HIDP_STATUS_SUCCESS && ValueCapsLength == 1)
        {
            /* wheel support */
            DeviceExtension->MouseIdentifier = WHEELMOUSE_HID_HARDWARE;
            DeviceExtension->WheelUsagePage = ValueCaps.UsagePage;
            DPRINT("[MOUHID] mouse wheel support detected with z-axis\n", Status);
        }
    }

    /* check if mice is absolute */
    if (DeviceExtension->ValueCapsY.IsAbsolute &&
        DeviceExtension->ValueCapsX.IsAbsolute)
    {
        /* mice is absolute */
        DeviceExtension->MouseAbsolute = TRUE;
    }

    /* completed successfully */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MouHid_StartDeviceCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    KeSetEvent(Context, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
MouHid_FreeResources(
    IN PDEVICE_OBJECT DeviceObject)
{
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* free resources */
    if (DeviceExtension->PreparsedData)
    {
        ExFreePoolWithTag(DeviceExtension->PreparsedData, MOUHID_TAG);
        DeviceExtension->PreparsedData = NULL;
    }

    if (DeviceExtension->UsageListBuffer)
    {
        ExFreePoolWithTag(DeviceExtension->UsageListBuffer, MOUHID_TAG);
        DeviceExtension->UsageListBuffer = NULL;
        DeviceExtension->CurrentUsageList = NULL;
        DeviceExtension->PreviousUsageList = NULL;
        DeviceExtension->MakeUsageList = NULL;
        DeviceExtension->BreakUsageList = NULL;
    }

    if (DeviceExtension->ReportMDL)
    {
        IoFreeMdl(DeviceExtension->ReportMDL);
        DeviceExtension->ReportMDL = NULL;
    }

    if (DeviceExtension->Report)
    {
        ExFreePoolWithTag(DeviceExtension->Report, MOUHID_TAG);
        DeviceExtension->Report = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MouHid_Flush(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* skip current stack location */
    IoSkipCurrentIrpStackLocation(Irp);

    /* get next stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);

    /* change request to hid flush queue request */
    IoStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_FLUSH_QUEUE;

    /* call device */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
MouHid_Pnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    KEVENT Event;
    NTSTATUS Status;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* get current irp stack */
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    DPRINT("[MOUHID] IRP_MJ_PNP Request: %x\n", IoStack->MinorFunction);

    switch (IoStack->MinorFunction)
    {
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        /* free resources */
        MouHid_FreeResources(DeviceObject);
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        /* indicate success */
        Irp->IoStatus.Status = STATUS_SUCCESS;

        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE:
        /* FIXME synchronization */

        /* request stop */
        DeviceExtension->StopReadReport = TRUE;

        /* cancel irp */
        IoCancelIrp(DeviceExtension->Irp);

        /*
         * Let the read cycle actually stop before anything it is still using
         * is freed.
         *
         * Only if there is one to stop: ReadCompletionEvent is a manual reset
         * event that starts unsignalled and is only ever signalled by a read
         * completion, so a device that was never opened - or whose cycle has
         * already stopped - would wait here for something that is never going
         * to happen.  Bounded as well, because the cancel above is the only
         * thing that can produce the completion and a lower driver that
         * ignores it would otherwise take PnP down with it.
         */
        if (DeviceExtension->ReadReportActive)
        {
            LARGE_INTEGER Timeout;

            Timeout.QuadPart = -50000000LL;   /* 5 seconds */
            if (KeWaitForSingleObject(&DeviceExtension->ReadCompletionEvent,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      &Timeout) == STATUS_TIMEOUT)
            {
                DPRINT1("[MOUHID] read cycle did not stop before remove\n");
            }
        }

        /*
         * Only now free what the read was using.  This used to happen straight
         * after the cancel and before the wait, so the report buffer and its
         * MDL were released while a cancelled read could still be completing
         * into them.
         */
        MouHid_FreeResources(DeviceObject);

        /* indicate success */
        Irp->IoStatus.Status = STATUS_SUCCESS;

        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

        /* free irp */
        IoFreeIrp(DeviceExtension->Irp);

        /* detach device */
        IoDetachDevice(DeviceExtension->NextDeviceObject);

        /* delete device */
        IoDeleteDevice(DeviceObject);

        /* done */
        return Status;

    case IRP_MN_START_DEVICE:
        /* init event */
        KeInitializeEvent(&Event, NotificationEvent, FALSE);

        /* copy stack location */
        IoCopyCurrentIrpStackLocationToNext (Irp);

        /* set completion routine */
        IoSetCompletionRoutine(Irp, MouHid_StartDeviceCompletion, &Event, TRUE, TRUE, TRUE);
        Irp->IoStatus.Status = 0;

        /* pass request */
        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = Irp->IoStatus.Status;
        }

        if (!NT_SUCCESS(Status))
        {
            /* failed */
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }

        /* lets start the device */
        Status = MouHid_StartDevice(DeviceObject);
        DPRINT("MouHid_StartDevice %x\n", Status);

        /* complete request */
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        /* done */
        return Status;

    default:
        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    }
}

NTSTATUS
NTAPI
MouHid_AddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject, NextDeviceObject;
    PMOUHID_DEVICE_EXTENSION DeviceExtension;
    POWER_STATE State;

    /* create device object */
    Status = IoCreateDevice(DriverObject,
                            sizeof(MOUHID_DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_MOUSE,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* failed to create device object */
        return Status;
    }

    /* now attach it */
    NextDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!NextDeviceObject)
    {
        /* failed to attach */
        IoDeleteDevice(DeviceObject);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* zero extension */
    RtlZeroMemory(DeviceExtension, sizeof(MOUHID_DEVICE_EXTENSION));

    /* init device extension */
    DeviceExtension->MouseIdentifier = MOUSE_HID_HARDWARE;
    DeviceExtension->WheelUsagePage = 0;
    DeviceExtension->NextDeviceObject = NextDeviceObject;
    KeInitializeEvent(&DeviceExtension->ReadCompletionEvent, NotificationEvent, FALSE);
    DeviceExtension->Irp = IoAllocateIrp(NextDeviceObject->StackSize, FALSE);

    /* FIXME handle allocation error */
    ASSERT(DeviceExtension->Irp);

    /* FIXME query parameter 'FlipFlopWheel', 'WheelScalingFactor' */

    /* set power state to D0 */
    State.DeviceState =  PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, State);

    /* init device object */
    DeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    DeviceObject->Flags  &= ~DO_DEVICE_INITIALIZING;

    /* completed successfully */
    return STATUS_SUCCESS;
}

VOID
NTAPI
MouHid_Unload(
    IN PDRIVER_OBJECT DriverObject)
{
    UNIMPLEMENTED;
}


NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegPath)
{
    /* FIXME check for parameters 'UseOnlyMice', 'TreatAbsoluteAsRelative', 'TreatAbsolutePointerAsAbsolute' */

    /* initialize driver object */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = MouHid_Create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MouHid_Close;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = MouHid_Flush;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MouHid_DeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = MouHid_InternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_POWER] = MouHid_Power;
    DriverObject->MajorFunction[IRP_MJ_PNP] = MouHid_Pnp;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = MouHid_SystemControl;
    DriverObject->DriverUnload = MouHid_Unload;
    DriverObject->DriverExtension->AddDevice = MouHid_AddDevice;

    /* done */
    return STATUS_SUCCESS;
}
