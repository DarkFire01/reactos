/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper read loop and scan code translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdhid.h"

#define NDEBUG
#include <debug.h>

/* How a processed report decides the next read */
typedef enum _KBDHID_REPORT_RESULT
{
    KbdHidReportReadNext,
    KbdHidReportDeferred
} KBDHID_REPORT_RESULT;

BOOLEAN
NTAPI
KbdHidCancelAutoRepeat(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    if (KbdHidGlobals.DisableAutoRepeat)
        return FALSE;

    KeCancelTimer(&DeviceExtension->RepeatTimer);
    return TRUE;
}

static
BOOLEAN
KbdHidDumpKeyMatches(
    _In_ UCHAR DumpKey,
    _In_ UCHAR Code,
    _In_ KBDHID_SCAN_PREFIX Prefix)
{
    /* Bit 7 of the dump key stands for an E0 prefixed scan code */
    if (DumpKey & KBDHID_SCAN_BREAK)
        return (Prefix == KbdHidPrefixE0) && (((Code ^ DumpKey) & ~KBDHID_SCAN_BREAK) == 0);

    return (Prefix == KbdHidPrefixNone) && ((Code & ~KBDHID_SCAN_BREAK) == DumpKey);
}

/**
 * @brief
 * Follows the crash dump hot key. Holding exactly the configured modifiers
 * and releasing the dump key twice bugchecks the system.
 */
static
VOID
KbdHidTrackCrashDumpKeys(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR Code,
    _In_ KBDHID_SCAN_PREFIX Prefix)
{
    BOOLEAN IsRelease = (Code & KBDHID_SCAN_BREAK) != 0;
    ULONG Modifier = 0;
    BOOLEAN IsModifier = TRUE;

    switch (Code & ~KBDHID_SCAN_BREAK)
    {
        case KBDHID_SCAN_CTRL:
            if (Prefix == KbdHidPrefixNone)
                Modifier = KBDHID_DUMP_LEFT_CTRL;
            else if (Prefix == KbdHidPrefixE0)
                Modifier = KBDHID_DUMP_RIGHT_CTRL;
            break;

        case KBDHID_SCAN_ALT:
            if (Prefix == KbdHidPrefixNone)
                Modifier = KBDHID_DUMP_LEFT_ALT;
            else if (Prefix == KbdHidPrefixE0)
                Modifier = KBDHID_DUMP_RIGHT_ALT;
            break;

        case KBDHID_SCAN_LEFT_SHIFT:
            if (Prefix == KbdHidPrefixNone)
                Modifier = KBDHID_DUMP_LEFT_SHIFT;
            break;

        case KBDHID_SCAN_RIGHT_SHIFT:
            if (Prefix == KbdHidPrefixNone)
                Modifier = KBDHID_DUMP_RIGHT_SHIFT;
            break;

        default:
            IsModifier = FALSE;
            break;
    }

    if (IsModifier)
    {
        if (Modifier == 0)
            return;

        /* Letting go of a modifier also restarts the dump key count */
        if (IsRelease)
            DeviceExtension->CrashDumpState &= ~(Modifier | KBDHID_DUMP_KEY_RELEASES);
        else
            DeviceExtension->CrashDumpState |= Modifier;
        return;
    }

    if (!KbdHidDumpKeyMatches(KbdHidGlobals.DumpKey, Code, Prefix) &&
        !KbdHidDumpKeyMatches(KbdHidGlobals.DumpKeyAlt, Code, Prefix))
    {
        /* Any other key breaks the sequence */
        DeviceExtension->CrashDumpState = 0;
        return;
    }

    if (!IsRelease)
        return;

    if (DeviceExtension->CrashDumpState & KBDHID_DUMP_KEY_FIRST_RELEASE)
        DeviceExtension->CrashDumpState |= KBDHID_DUMP_KEY_SECOND_RELEASE;
    else
        DeviceExtension->CrashDumpState |= KBDHID_DUMP_KEY_FIRST_RELEASE;

    if (DeviceExtension->CrashDumpState == (KbdHidGlobals.DumpModifiers | KBDHID_DUMP_KEY_RELEASES))
    {
        DeviceExtension->CrashDumpState = 0;
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
    }
}

/**
 * @brief
 * Scan code sink for the HID parser. Prefix bytes are folded into the flags
 * of the following code and every complete code goes to kbdclass.
 */
static
BOOLEAN
NTAPI
KbdHidInsertScanCodes(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Length) PCHAR NewScanCodes,
    _In_ ULONG Length)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = Context;
    PKEYBOARD_INPUT_DATA InputData = &DeviceExtension->InputData;
    PSERVICE_CALLBACK_ROUTINE ClassService;
    ULONG InputDataConsumed = 0;
    ULONG Index;
    UCHAR Code;
    KIRQL OldIrql;

    for (Index = 0; Index < Length; Index++)
    {
        Code = (UCHAR)NewScanCodes[Index];

        if (Code == KEYBOARD_OVERRUN_MAKE_CODE)
        {
            DPRINT("Keyboard overrun\n");
            InputData->MakeCode = KEYBOARD_OVERRUN_MAKE_CODE;
            InputData->Flags = 0;
            DeviceExtension->ScanPrefix = KbdHidPrefixNone;
            continue;
        }

        if (DeviceExtension->ScanPrefix == KbdHidPrefixNone)
        {
            if (Code == KBDHID_SCAN_EXTENDED0)
            {
                InputData->Flags |= KEY_E0;
                DeviceExtension->ScanPrefix = KbdHidPrefixE0;
                continue;
            }

            if (Code == KBDHID_SCAN_EXTENDED1)
            {
                InputData->Flags |= KEY_E1;
                DeviceExtension->ScanPrefix = KbdHidPrefixE1;
                continue;
            }
        }

        if (KbdHidGlobals.DumpModifiers != 0)
            KbdHidTrackCrashDumpKeys(DeviceExtension, Code, DeviceExtension->ScanPrefix);

        if (Code & KBDHID_SCAN_BREAK)
        {
            InputData->Flags |= KEY_BREAK;
            Code &= ~KBDHID_SCAN_BREAK;
        }

        InputData->MakeCode = Code;
        DeviceExtension->ScanPrefix = KbdHidPrefixNone;

        if (DeviceExtension->OpenCount != 0)
        {
            ClassService = (PSERVICE_CALLBACK_ROUTINE)DeviceExtension->ConnectData.ClassService;

            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            ClassService(DeviceExtension->ConnectData.ClassDeviceObject,
                         InputData,
                         InputData + 1,
                         &InputDataConsumed);
            KeLowerIrql(OldIrql);
        }

        InputData->Flags = 0;
    }

    return TRUE;
}

/**
 * @brief
 * Applies the UsageMappings remap list to a keyboard page report.
 *
 * @return
 * FALSE when the report reports a rollover error and must be dropped.
 */
static
BOOLEAN
KbdHidRemapUsages(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ PKBDHID_HID_DATA HidData)
{
    PUSAGE_AND_PAGE Usages = HidData->CurrentUsages;
    PKBDHID_USAGE_MAPPING Entry;
    BOOLEAN IsValid = TRUE;
    ULONG Index;
    USAGE Usage;
    KIRQL OldIrql;

    /* Only a report led by a key page usage is remapped */
    if (Usages[0].UsagePage != HID_USAGE_PAGE_KEYBOARD)
        return TRUE;

    KeAcquireSpinLock(&DeviceExtension->MappingLock, &OldIrql);

    for (Index = 0; Index < HidData->MaxUsages; Index++)
    {
        Usage = Usages[Index].Usage;
        for (Entry = DeviceExtension->Mappings; Entry != NULL; Entry = Entry->Next)
        {
            if (Entry->From == Usage)
            {
                Usage = Entry->To;
                break;
            }
        }
        Usages[Index].Usage = Usage;

        if (Usage == HID_USAGE_KEYBOARD_ROLLOVER)
        {
            IsValid = FALSE;
            break;
        }

        if (Usage == 0)
            break;
    }

    KeReleaseSpinLock(&DeviceExtension->MappingLock, OldIrql);
    return IsValid;
}

static
VOID
KbdHidReportChatter(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PIO_ERROR_LOG_PACKET Entry;

    if (DeviceExtension->ChatterReported)
        return;

    DeviceExtension->ChatterReported = TRUE;
    DeviceExtension->ProblemFlags |= KBDHID_PROBLEM_CHATTERY;
    DPRINT1("Keyboard keeps sending reports that change nothing\n");

    Entry = IoAllocateErrorLogEntry(DeviceExtension->Self->DriverObject, sizeof(*Entry));
    if (Entry == NULL)
        return;

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->ErrorCode = KBDHID_LOG_CHATTERY_DEVICE;
    Entry->FinalStatus = KBDHID_LOG_CHATTERY_DEVICE;
    IoWriteErrorLogEntry(Entry);
}

static
BOOLEAN
KbdHidIsListEmpty(
    _In_ PUSAGE_AND_PAGE List)
{
    return (List[0].Usage == 0) && (List[0].UsagePage == 0);
}

/**
 * @brief
 * Turns a fresh input report into make and break scan codes.
 *
 * @param[in] IsAsynchronous
 * TRUE when the read completed after KbdHidStartRead let go of the IRP.
 */
static
KBDHID_REPORT_RESULT
KbdHidProcessReport(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN IsAsynchronous)
{
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    ULONG MaxUsages = HidData->MaxUsages;
    ULONG UsageCount = MaxUsages;
    SIZE_T ListLength = MaxUsages * sizeof(*HidData->CurrentUsages);
    PUSAGE_AND_PAGE Recycled;
    KBDHID_REPORT_RESULT Result = KbdHidReportReadNext;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (MaxUsages == 0)
        return KbdHidReportReadNext;

    Status = HidP_GetUsagesEx(HidP_Input,
                              0,
                              HidData->CurrentUsages,
                              &UsageCount,
                              HidData->PreparsedData,
                              HidData->InputReport,
                              HidData->Caps.InputReportByteLength);
    if (!NT_SUCCESS(Status))
        return KbdHidReportReadNext;

    /* Print Screen alone breaks into an attached debugger */
    if ((UsageCount == 1) &&
        (HidData->CurrentUsages[0].UsagePage == HID_USAGE_PAGE_KEYBOARD) &&
        (HidData->CurrentUsages[0].Usage == HID_USAGE_KEYBOARD_PRINT_SCREEN) &&
        KbdHidGlobals.BreakOnSysRq &&
        KD_DEBUGGER_ENABLED &&
        !KdRefreshDebuggerNotPresent())
    {
        DbgBreakPointWithStatus(DBG_STATUS_SYSRQ);
    }

    if (!KbdHidRemapUsages(DeviceExtension, HidData))
        return KbdHidReportReadNext;

    KeAcquireSpinLock(&HidData->Lock, &OldIrql);

    Status = HidP_UsageAndPageListDifference(HidData->PreviousUsages,
                                             HidData->CurrentUsages,
                                             HidData->ScratchBreak,
                                             HidData->ScratchMake,
                                             MaxUsages);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Same keys as last time, nothing to send */
    if (KbdHidIsListEmpty(HidData->ScratchMake) && KbdHidIsListEmpty(HidData->ScratchBreak))
        goto Done;

    /* Keep the old repeat list, the new keys replace it below */
    RtlCopyMemory(HidData->ScratchMake, HidData->RepeatUsages, ListLength);

    Status = HidP_UsageAndPageListDifference(HidData->PreviousUsages,
                                             HidData->CurrentUsages,
                                             HidData->BreakUsages,
                                             HidData->RepeatUsages,
                                             MaxUsages);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = HidP_TranslateUsageAndPagesToI8042ScanCodes(HidData->BreakUsages,
                                                         MaxUsages,
                                                         HidP_Keyboard_Break,
                                                         &HidData->ModifierState,
                                                         KbdHidInsertScanCodes,
                                                         DeviceExtension);
    if (!NT_SUCCESS(Status))
        goto Done;

    if (KbdHidIsListEmpty(HidData->CurrentUsages) || !KbdHidIsListEmpty(HidData->RepeatUsages))
    {
        Status = HidP_TranslateUsageAndPagesToI8042ScanCodes(HidData->RepeatUsages,
                                                             MaxUsages,
                                                             HidP_Keyboard_Make,
                                                             &HidData->ModifierState,
                                                             KbdHidInsertScanCodes,
                                                             DeviceExtension);
    }
    else
    {
        /* Only releases, the keys still held from the old repeat list keep repeating */
        Status = HidP_UsageAndPageListDifference(HidData->BreakUsages,
                                                 HidData->ScratchMake,
                                                 HidData->ScratchBreak,
                                                 HidData->RepeatUsages,
                                                 MaxUsages);
    }
    if (!NT_SUCCESS(Status))
        goto Done;

    Recycled = HidData->PreviousUsages;
    HidData->PreviousUsages = HidData->CurrentUsages;
    RtlZeroMemory(Recycled, ListLength);
    HidData->CurrentUsages = Recycled;

    if (UsageCount == 0)
    {
        KbdHidCancelAutoRepeat(DeviceExtension);
        goto Done;
    }

    if ((HidData->BreakUsages[0].Usage != 0) || (HidData->RepeatUsages[0].Usage != 0))
    {
        if (!KbdHidGlobals.DisableAutoRepeat)
        {
            KeSetTimerEx(&DeviceExtension->RepeatTimer,
                         DeviceExtension->RepeatDelay,
                         DeviceExtension->RepeatPeriod,
                         &DeviceExtension->RepeatDpc);
        }
        goto Done;
    }

    KbdHidReportChatter(DeviceExtension);

    if (IsAsynchronous)
    {
        /* Hold off the next read, the retry DPC picks the loop up again */
        if (!DeviceExtension->Removed)
        {
            KeSetTimerEx(&DeviceExtension->RetryTimer,
                         DeviceExtension->RetryDelay,
                         0,
                         &DeviceExtension->RetryDpc);
        }
        else
        {
            KeSetEvent(&DeviceExtension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
        }

        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, DeviceExtension->ReadIrp);
        Result = KbdHidReportDeferred;
    }

    KbdHidCancelAutoRepeat(DeviceExtension);

Done:
    KeReleaseSpinLock(&HidData->Lock, OldIrql);
    return Result;
}

static
BOOLEAN
KbdHidIsReadLoopFatal(
    _In_ NTSTATUS Status)
{
    return (Status == STATUS_DELETE_PENDING) ||
           (Status == STATUS_PRIVILEGE_NOT_HELD) ||
           (Status == STATUS_DEVICE_NOT_CONNECTED) ||
           (Status == STATUS_CANCELLED);
}

/**
 * @brief
 * Read completion. Processes the report and, unless KbdHidStartRead is still
 * waiting on IoCallDriver for this IRP, sends the next read itself.
 */
static
NTSTATUS
NTAPI
KbdHidReadComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = Context;
    NTSTATUS Status = Irp->IoStatus.Status;
    BOOLEAN IsAsynchronous;

    UNREFERENCED_PARAMETER(DeviceObject);

    IsAsynchronous = (InterlockedCompareExchange(&DeviceExtension->ReadState,
                                               KbdHidReadCompletedInline,
                                               KbdHidReadIssuing) != KbdHidReadIssuing);

    if ((DeviceExtension->OpenCount == 0) || KbdHidIsReadLoopFatal(Status))
    {
        DPRINT("Read loop ends, status 0x%lx\n", Status);

        if (IsAsynchronous)
        {
            KeSetEvent(&DeviceExtension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
            IoReleaseRemoveLock(&DeviceExtension->RemoveLock, DeviceExtension->ReadIrp);
        }
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /* Any other failure stops the loop in KbdHidStartRead */
    if ((Status == STATUS_SUCCESS) &&
        (KbdHidProcessReport(DeviceExtension, IsAsynchronous) == KbdHidReportDeferred))
    {
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (IsAsynchronous)
        KbdHidStartRead(DeviceExtension);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Sends reads to the collection until one stays pending or fails. Reads
 * that complete inline are looped here instead of recursing.
 *
 * @return
 * STATUS_PENDING while the loop runs, otherwise the status that ended it.
 */
NTSTATUS
NTAPI
KbdHidStartRead(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PIRP Irp = DeviceExtension->ReadIrp;
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    PIO_STACK_LOCATION NextStack;
    NTSTATUS Status = Irp->IoStatus.Status;
    LONG PreviousState;

    if (InterlockedExchange(&DeviceExtension->ReadState, KbdHidReadIssuing) != KbdHidReadIdle)
        return Status;

    for (;;)
    {
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Keyboard read failed 0x%lx, input stops until reopened\n", Status);
            KeSetEvent(&DeviceExtension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
            IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
            break;
        }

        IoReuseIrp(Irp, STATUS_SUCCESS);
        Irp->MdlAddress = HidData->InputMdl;

        NextStack = IoGetNextIrpStackLocation(Irp);
        NextStack->MajorFunction = IRP_MJ_READ;
        NextStack->Parameters.Read.Length = HidData->Caps.InputReportByteLength;
        NextStack->Parameters.Read.Key = 0;
        NextStack->Parameters.Read.ByteOffset.QuadPart = 0;
        NextStack->FileObject = DeviceExtension->ReadFileObject;
        IoSetCompletionRoutine(Irp, KbdHidReadComplete, DeviceExtension, TRUE, TRUE, TRUE);

        KeClearEvent(&DeviceExtension->ReadIssueDoneEvent);

        if ((DeviceExtension->OpenCount == 0) || DeviceExtension->Removed)
        {
            Status = DeviceExtension->Removed ? STATUS_DELETE_PENDING : STATUS_UNSUCCESSFUL;
            IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
            KeSetEvent(&DeviceExtension->ReadIssueDoneEvent, IO_NO_INCREMENT, FALSE);
            KeSetEvent(&DeviceExtension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
            break;
        }

        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
        KeSetEvent(&DeviceExtension->ReadIssueDoneEvent, IO_NO_INCREMENT, FALSE);

        PreviousState = InterlockedExchange(&DeviceExtension->ReadState, KbdHidReadIdle);
        if (PreviousState != KbdHidReadCompletedInline)
            break;

        /* The completion ran inside IoCallDriver and left the next read to us */
        InterlockedExchange(&DeviceExtension->ReadState, KbdHidReadIssuing);
    }

    return Status;
}

VOID
NTAPI
KbdHidRetryReadDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (!NT_SUCCESS(IoAcquireRemoveLock(&DeviceExtension->RemoveLock, DeviceExtension->ReadIrp)))
    {
        KeSetEvent(&DeviceExtension->ReadStoppedEvent, IO_NO_INCREMENT, FALSE);
        return;
    }

    KbdHidStartRead(DeviceExtension);
}

/**
 * @brief
 * Typematic timer DPC, sends the make codes of the repeating keys again.
 */
VOID
NTAPI
KbdHidAutoRepeatDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeferredContext;
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* Skip this tick when a report is being processed */
    if (!KeTryToAcquireSpinLockAtDpcLevel(&HidData->Lock))
        return;

    if (HidData->RepeatUsages[0].Usage != 0)
    {
        HidP_TranslateUsageAndPagesToI8042ScanCodes(HidData->RepeatUsages,
                                                    HidData->MaxUsages,
                                                    HidP_Keyboard_Make,
                                                    &HidData->ModifierState,
                                                    KbdHidInsertScanCodes,
                                                    DeviceExtension);
    }

    KeReleaseSpinLockFromDpcLevel(&HidData->Lock);
}
