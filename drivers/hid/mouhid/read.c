/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Input report read loop and translation to MOUSE_INPUT_DATA
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouhid.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE MouHidReadComplete;

/* Indexed by button usage, only buttons 1 to 5 have flags */
static const USHORT MouHidButtonDown[] =
{
    0,
    MOUSE_LEFT_BUTTON_DOWN,
    MOUSE_RIGHT_BUTTON_DOWN,
    MOUSE_MIDDLE_BUTTON_DOWN,
    MOUSE_BUTTON_4_DOWN,
    MOUSE_BUTTON_5_DOWN
};

static const USHORT MouHidButtonUp[] =
{
    0,
    MOUSE_LEFT_BUTTON_UP,
    MOUSE_RIGHT_BUTTON_UP,
    MOUSE_MIDDLE_BUTTON_UP,
    MOUSE_BUTTON_4_UP,
    MOUSE_BUTTON_5_UP
};

static
LONG
NTAPI
MouHidSignExtend(
    _In_ ULONG Value,
    _In_ USHORT BitSize)
{
    ULONG SignBit = 1UL << (BitSize - 1);

    if (Value & SignBit)
        Value |= ~(SignBit - 1);

    return (LONG)Value;
}

/**
 * @brief
 * Reads one pointer axis from the current report.
 *
 * @remarks
 * A collection whose physical range hidparse rejects falls back to raw
 * logical values for the rest of its life.
 */
static
VOID
NTAPI
MouHidReadAxis(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ USAGE Usage,
    _In_ ULONG RawFlag,
    _In_ USHORT BitSize,
    _Inout_ PLONG Max,
    _Inout_ PLONG Position,
    _Inout_ PBOOLEAN LogProblem)
{
    PMOUHID_COLLECTION Collection = Extension->Collection;
    ULONG Value = 0;
    NTSTATUS Status;

    if (!(Extension->ProblemFlags & RawFlag))
    {
        Status = HidP_GetScaledUsageValue(HidP_Input,
                                          HID_USAGE_PAGE_GENERIC,
                                          0,
                                          Usage,
                                          Position,
                                          Collection->PreparsedData,
                                          Collection->Report,
                                          Collection->Caps.InputReportByteLength);
        if (Status == HIDP_STATUS_BAD_LOG_PHY_VALUES)
        {
            Extension->ProblemFlags |= RawFlag;
            *Max = (LONG)((1UL << (BitSize - 1)) - 1);
            *LogProblem = TRUE;
        }
    }

    if (Extension->ProblemFlags & RawFlag)
    {
        Status = HidP_GetUsageValue(HidP_Input,
                                    HID_USAGE_PAGE_GENERIC,
                                    0,
                                    Usage,
                                    &Value,
                                    Collection->PreparsedData,
                                    Collection->Report,
                                    Collection->Caps.InputReportByteLength);
        if (NT_SUCCESS(Status))
            *Position = MouHidSignExtend(Value, BitSize);
    }

    if (Collection->IsAbsolute && (*Max != 0))
        *Position = (LONG)(((LONGLONG)MOUHID_ABSOLUTE_RANGE * *Position) / *Max);
}

/**
 * @brief
 * Reads one wheel from the current report.
 *
 * @return
 * The signed number of wheel units, 0 when the wheel did not move.
 */
static
LONG
NTAPI
MouHidReadWheel(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ USAGE UsagePage,
    _In_ USAGE Usage,
    _In_ ULONG RawFlag,
    _In_ USHORT BitSize,
    _Inout_ PBOOLEAN Missing,
    _Inout_ PBOOLEAN LogProblem)
{
    PMOUHID_COLLECTION Collection = Extension->Collection;
    ULONG Value = 0;
    NTSTATUS Status;

    if (!(Extension->ProblemFlags & RawFlag))
    {
        Status = HidP_GetScaledUsageValue(HidP_Input,
                                          UsagePage,
                                          0,
                                          Usage,
                                          (PLONG)&Value,
                                          Collection->PreparsedData,
                                          Collection->Report,
                                          Collection->Caps.InputReportByteLength);
        if (Status == HIDP_STATUS_USAGE_NOT_FOUND)
        {
            *Missing = TRUE;
        }
        else if (Status == HIDP_STATUS_BAD_LOG_PHY_VALUES)
        {
            Extension->ProblemFlags |= RawFlag;
            *LogProblem = TRUE;
        }
    }

    if (Extension->ProblemFlags & RawFlag)
    {
        Status = HidP_GetUsageValue(HidP_Input,
                                    UsagePage,
                                    0,
                                    Usage,
                                    &Value,
                                    Collection->PreparsedData,
                                    Collection->Report,
                                    Collection->Caps.InputReportByteLength);
        if (NT_SUCCESS(Status))
            Value = (ULONG)MouHidSignExtend(Value, BitSize);
    }

    return (LONG)Value;
}

/**
 * @brief
 * Converts wheel units to a ButtonData delta using the current scale.
 */
static
USHORT
NTAPI
MouHidScaleWheel(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ LONG Units,
    _In_ BOOLEAN Horizontal)
{
    LONG Scale;
    LONG Divisor;
    LONG Delta;
    BOOLEAN Flip;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Extension->WheelLock, &OldIrql);

    if (Horizontal)
    {
        Scale = Extension->HScrollScale;
        Divisor = Extension->HResolutionMultiplier;
        Flip = Extension->FlipFlopHScroll;
    }
    else
    {
        Scale = Extension->WheelScale;
        Divisor = Extension->VResolutionMultiplier;
        Flip = Extension->FlipFlopWheel;
    }

    /* A multiplier with no physical range would divide by zero */
    if (Divisor == 0)
        Divisor = 1;

    Delta = (LONG)((ULONG)Scale * (ULONG)Units) / Divisor;
    if (Flip)
        Delta = -Delta;

    KeReleaseSpinLock(&Extension->WheelLock, OldIrql);

    return (USHORT)Delta;
}

static
VOID
NTAPI
MouHidReportInput(
    _In_ PMOUHID_EXTENSION Extension)
{
    PSERVICE_CALLBACK_ROUTINE ClassService;
    ULONG Consumed = 0;
    KIRQL OldIrql;

    ClassService = (PSERVICE_CALLBACK_ROUTINE)Extension->ConnectData.ClassService;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    ClassService(Extension->ConnectData.ClassDeviceObject,
                 &Extension->InputData,
                 &Extension->InputData + 1,
                 &Consumed);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Translates the report in the collection buffer and hands it to mouclass.
 */
static
VOID
NTAPI
MouHidProcessReport(
    _In_ PMOUHID_EXTENSION Extension)
{
    PMOUHID_COLLECTION Collection = Extension->Collection;
    PMOUSE_INPUT_DATA Data = &Extension->InputData;
    ULONG UsageCount = Collection->MaxUsageListLength;
    ULONG Index;
    USAGE Usage;
    PUSAGE Swap;
    LONG Units;
    BOOLEAN LogProblem = FALSE;
    NTSTATUS Status;

    Data->ButtonFlags = 0;
    Data->ButtonData = 0;
    Data->LastX = 0;
    Data->LastY = 0;

    Status = HidP_GetUsages(HidP_Input,
                            HID_USAGE_PAGE_BUTTON,
                            0,
                            Collection->CurrentUsages,
                            &UsageCount,
                            Collection->PreparsedData,
                            Collection->Report,
                            Collection->Caps.InputReportByteLength);
    if (NT_SUCCESS(Status))
    {
        Status = HidP_UsageListDifference(Collection->PreviousUsages,
                                          Collection->CurrentUsages,
                                          Collection->BreakUsages,
                                          Collection->MakeUsages,
                                          Collection->MaxUsageListLength);
    }

    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Collection->MaxUsageListLength; Index++)
        {
            Usage = Collection->MakeUsages[Index];
            if (Usage == 0)
                break;

            if (Usage < RTL_NUMBER_OF(MouHidButtonDown))
                Data->ButtonFlags |= MouHidButtonDown[Usage];
        }

        for (Index = 0; Index < Collection->MaxUsageListLength; Index++)
        {
            Usage = Collection->BreakUsages[Index];
            if (Usage == 0)
                break;

            if (Usage < RTL_NUMBER_OF(MouHidButtonUp))
                Data->ButtonFlags |= MouHidButtonUp[Usage];
        }

        Swap = Collection->PreviousUsages;
        Collection->PreviousUsages = Collection->CurrentUsages;
        Collection->CurrentUsages = Swap;
    }

    MouHidReadAxis(Extension,
                   HID_USAGE_GENERIC_X,
                   MOUHID_PROBLEM_RAW_X,
                   Collection->BitSizeX,
                   &Collection->MaxX,
                   &Data->LastX,
                   &LogProblem);

    MouHidReadAxis(Extension,
                   HID_USAGE_GENERIC_Y,
                   MOUHID_PROBLEM_RAW_Y,
                   Collection->BitSizeY,
                   &Collection->MaxY,
                   &Data->LastY,
                   &LogProblem);

    if (!Collection->WheelMissing)
    {
        Units = MouHidReadWheel(Extension,
                                Collection->WheelUsagePage,
                                Collection->WheelUsage,
                                MOUHID_PROBLEM_RAW_WHEEL,
                                Collection->BitSizeWheel,
                                &Collection->WheelMissing,
                                &LogProblem);
        if (Units != 0)
        {
            Data->ButtonData = MouHidScaleWheel(Extension, Units, FALSE);
            Data->ButtonFlags |= MOUSE_WHEEL;
        }
        else
        {
            Data->ButtonData = 0;
        }
    }

    if (!Collection->HWheelMissing)
    {
        Units = MouHidReadWheel(Extension,
                                Collection->HWheelUsagePage,
                                Collection->HWheelUsage,
                                MOUHID_PROBLEM_RAW_HWHEEL,
                                Collection->BitSizeHWheel,
                                &Collection->HWheelMissing,
                                &LogProblem);

        /* ButtonData holds one wheel, so both wheels moving takes two records */
        if ((Data->ButtonFlags & MOUSE_WHEEL) && (Units != 0))
        {
            MouHidReportInput(Extension);

            Data->ButtonFlags = 0;
            if (!Collection->IsAbsolute)
            {
                Data->LastX = 0;
                Data->LastY = 0;
            }
        }

        if (Units != 0)
        {
            Data->ButtonData = MouHidScaleWheel(Extension, Units, TRUE);
            Data->ButtonFlags |= MOUSE_HWHEEL;
        }
    }

    MouHidReportInput(Extension);

    if (LogProblem)
    {
        MouHidLogError(Extension->Self->DriverObject,
                       MOUHID_LOG_BAD_PHYSICAL_RANGE,
                       MOUHID_LOG_BAD_PHYSICAL_RANGE);
    }
}

static
NTSTATUS
NTAPI
MouHidReadComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PMOUHID_EXTENSION Extension = Context;
    NTSTATUS Status = Irp->IoStatus.Status;
    BOOLEAN Rearm;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* A submitter still inside IoCallDriver picks the result up itself */
    Rearm = (InterlockedCompareExchange(&Extension->ReadState,
                                        MOUHID_READ_INLINE_DONE,
                                        MOUHID_READ_SUBMITTING) != MOUHID_READ_SUBMITTING);

    if ((Extension->EnableCount == 0) ||
        (Status == STATUS_DELETE_PENDING) ||
        (Status == STATUS_PRIVILEGE_NOT_HELD) ||
        (Status == STATUS_DEVICE_NOT_CONNECTED) ||
        (Status == STATUS_CANCELLED))
    {
        DPRINT("MouHid: read loop stopping 0x%08lx\n", Status);
        if (Rearm)
        {
            KeSetEvent(&Extension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
            IoReleaseRemoveLock(&Extension->RemoveLock, Extension->ReadIrp);
        }

        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (Status == STATUS_SUCCESS)
        MouHidProcessReport(Extension);

    if (Rearm)
        MouHidStartRead(Extension);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Submits the read IRP and keeps resubmitting it while reads complete inline.
 *
 * @remarks
 * The caller holds a remove lock reference tagged with the read IRP. It is
 * dropped once the loop stops.
 */
NTSTATUS
NTAPI
MouHidStartRead(
    _In_ PMOUHID_EXTENSION Extension)
{
    PIRP Irp = Extension->ReadIrp;
    PIO_STACK_LOCATION NextStack;
    NTSTATUS Status = Irp->IoStatus.Status;

    if (InterlockedExchange(&Extension->ReadState, MOUHID_READ_SUBMITTING) != MOUHID_READ_REARM)
        return Status;

    while (NT_SUCCESS(Status))
    {
        IoReuseIrp(Irp, STATUS_SUCCESS);
        Irp->MdlAddress = Extension->Collection->ReportMdl;

        NextStack = IoGetNextIrpStackLocation(Irp);
        NextStack->MajorFunction = IRP_MJ_READ;
        NextStack->FileObject = Extension->ReadFile;
        NextStack->Parameters.Read.Length = Extension->Collection->Caps.InputReportByteLength;
        NextStack->Parameters.Read.Key = 0;
        NextStack->Parameters.Read.ByteOffset.QuadPart = 0;
        IoSetCompletionRoutine(Irp, MouHidReadComplete, Extension, TRUE, TRUE, TRUE);

        KeClearEvent(&Extension->ReadSentEvent);

        if ((Extension->EnableCount == 0) || Extension->Removed)
        {
            IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
            Status = Extension->Removed ? STATUS_DELETE_PENDING : STATUS_UNSUCCESSFUL;
            KeSetEvent(&Extension->ReadSentEvent, IO_NO_INCREMENT, FALSE);
            KeSetEvent(&Extension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
            return Status;
        }

        Status = IoCallDriver(Extension->TopOfStack, Irp);
        KeSetEvent(&Extension->ReadSentEvent, IO_NO_INCREMENT, FALSE);

        /* Still pending, the completion routine resubmits */
        if (InterlockedExchange(&Extension->ReadState, MOUHID_READ_REARM) != MOUHID_READ_INLINE_DONE)
            return Status;

        if (InterlockedExchange(&Extension->ReadState, MOUHID_READ_SUBMITTING) != MOUHID_READ_REARM)
            return Status;
    }

    DPRINT1("MouHid: read failed 0x%08lx\n", Status);
    KeSetEvent(&Extension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}
