/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable processor related routines
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

KAFFINITY KeActiveProcessors = 0;

/* Number of processors */
CCHAR KeNumberProcessors = 0;

#ifdef CONFIG_SMP

/* Theoretical maximum number of processors that can be handled.
 * Set once at run-time. Returned by KeQueryMaximumProcessorCount(). */
ULONG KeMaximumProcessors = MAXIMUM_PROCESSORS;

/* Maximum number of logical processors that can be started
 * (including dynamically) at run-time. If 0: do not perform checks. */
ULONG KeNumprocSpecified = 0;

/* Maximum number of logical processors that can be started
 * at boot-time. If 0: do not perform checks. */
ULONG KeBootprocSpecified = 0;

#endif // CONFIG_SMP

/* FUNCTIONS *****************************************************************/

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    return KeActiveProcessors;
}

/**
 * Retrieves the number of the current processor.
 *
 * \param ProcessorNumber Pointer to a PROCESSOR_NUMBER structure that receives the processor number.
 *
 * \return NTSTATUS The status of the operation.
 */
NTSTATUS
NTAPI
NtGetCurrentProcessorNumberEx(
    _Out_ PPROCESSOR_NUMBER ProcessorNumber)
{
    _SEH2_TRY
    {
        ProbeForWrite(ProcessorNumber, sizeof(PROCESSOR_NUMBER), __alignof(PROCESSOR_NUMBER));
        ProcessorNumber->Group = 0; // TODO: Support processor groups
        ProcessorNumber->Number = (UCHAR)KeGetCurrentProcessorNumber();
        ProcessorNumber->Reserved = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        return _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

/*
 * The kernel is compiled for an older target than the drivers that call this, where
 * KE_PROCESSOR_CHANGE_NOTIFY_CONTEXT still ended at Status. A caller reads the later shape, so
 * the notification is built in that one and the extra field is always there to be read.
 */
typedef struct _KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT
{
    KE_PROCESSOR_CHANGE_NOTIFY_STATE State;
    ULONG NtNumber;
    NTSTATUS Status;
    PROCESSOR_NUMBER ProcNumber;
} KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT;

typedef struct _KI_PROCESSOR_CHANGE_CALLBACK
{
    LIST_ENTRY ListEntry;
    PPROCESSOR_CALLBACK_FUNCTION CallbackFunction;
    PVOID CallbackContext;
} KI_PROCESSOR_CHANGE_CALLBACK, *PKI_PROCESSOR_CHANGE_CALLBACK;

#define TAG_PROCESSOR_CALLBACK 'cPeK'

/* Self referencing so that no separate initialisation is needed before the first caller. */
static LIST_ENTRY KiProcessorChangeCallbackList =
{
    &KiProcessorChangeCallbackList,
    &KiProcessorChangeCallbackList
};

static KSPIN_LOCK KiProcessorChangeCallbackLock;

/* Tell one callback about one processor, and report back what it made of it. */
static
NTSTATUS
KiNotifyProcessorChange(
    _In_ PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
    _In_opt_ PVOID CallbackContext,
    _In_ KE_PROCESSOR_CHANGE_NOTIFY_STATE State,
    _In_ ULONG Number)
{
    KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT ChangeContext;
    NTSTATUS OperationStatus = STATUS_SUCCESS;

    ChangeContext.State = State;
    ChangeContext.NtNumber = Number;
    ChangeContext.Status = STATUS_SUCCESS;
    ChangeContext.ProcNumber.Group = 0;
    ChangeContext.ProcNumber.Number = (UCHAR)Number;
    ChangeContext.ProcNumber.Reserved = 0;

    CallbackFunction(CallbackContext,
                     (PKE_PROCESSOR_CHANGE_NOTIFY_CONTEXT)&ChangeContext,
                     &OperationStatus);

    return OperationStatus;
}

/**
 * rief Registers a callback to be told when a processor is added to the system.
 *
 * \param CallbackFunction The routine to call.
 * \param CallbackContext Passed back to the routine untouched.
 * \param Flags KE_PROCESSOR_CHANGE_ADD_EXISTING to also be told about the processors that are
 *        already running, each of which is reported as an add that has completed.
 *
 * eturn An opaque handle for KeDeregisterProcessorChangeCallback, or NULL.
 *
 * emarks No processor is ever added after boot here, so a registration that survives this
 *          call will not be called again. The list is still kept so that the handle stays
 *          valid and deregistration behaves.
 */
PVOID
NTAPI
KeRegisterProcessorChangeCallback(
    _In_ PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
    _In_opt_ PVOID CallbackContext,
    _In_ ULONG Flags)
{
    PKI_PROCESSOR_CHANGE_CALLBACK Callback;
    NTSTATUS Status;
    ULONG Number;
    ULONG Undo;

    if (CallbackFunction == NULL)
        return NULL;

    Callback = ExAllocatePoolWithTag(NonPagedPool,
                                     sizeof(*Callback),
                                     TAG_PROCESSOR_CALLBACK);
    if (Callback == NULL)
        return NULL;

    Callback->CallbackFunction = CallbackFunction;
    Callback->CallbackContext = CallbackContext;

    if (Flags & KE_PROCESSOR_CHANGE_ADD_EXISTING)
    {
        for (Number = 0; Number < (ULONG)KeNumberProcessors; Number++)
        {
            Status = KiNotifyProcessorChange(CallbackFunction,
                                             CallbackContext,
                                             KeProcessorAddCompleteNotify,
                                             Number);
            if (NT_SUCCESS(Status))
                continue;

            /* It refused one, so take back the ones it has already been told about. */
            for (Undo = 0; Undo < Number; Undo++)
            {
                KiNotifyProcessorChange(CallbackFunction,
                                        CallbackContext,
                                        KeProcessorAddFailureNotify,
                                        Undo);
            }

            ExFreePoolWithTag(Callback, TAG_PROCESSOR_CALLBACK);
            return NULL;
        }
    }

    ExInterlockedInsertTailList(&KiProcessorChangeCallbackList,
                                &Callback->ListEntry,
                                &KiProcessorChangeCallbackLock);

    return Callback;
}

/**
 * rief Drops a registration made by KeRegisterProcessorChangeCallback.
 *
 * \param CallbackHandle The handle that registration returned.
 */
VOID
NTAPI
KeDeregisterProcessorChangeCallback(
    _In_ PVOID CallbackHandle)
{
    PKI_PROCESSOR_CHANGE_CALLBACK Callback = CallbackHandle;
    KIRQL OldIrql;

    if (Callback == NULL)
        return;

    KeAcquireSpinLock(&KiProcessorChangeCallbackLock, &OldIrql);
    RemoveEntryList(&Callback->ListEntry);
    KeReleaseSpinLock(&KiProcessorChangeCallbackLock, OldIrql);

    ExFreePoolWithTag(Callback, TAG_PROCESSOR_CALLBACK);
}

/**
 * rief Returns the highest NUMA node number in the system.
 *
 * eturn The highest node number, which is one less than the count of nodes.
 */
USHORT
NTAPI
KeQueryHighestNodeNumber(VOID)
{
    return (USHORT)(KeNumberNodes - 1);
}

/**
 * rief Converts a reading of the auxiliary counter into the performance counter timebase.
 *
 * \param AuxiliaryCounterValue The reading to convert.
 * \param PerformanceCounterValue Receives the same instant on the performance counter.
 * \param ConversionError Receives how far the conversion may be out, in performance
 *        counter ticks. Optional.
 *
 * eturn STATUS_NOT_SUPPORTED when the platform has no auxiliary counter, otherwise
 *         whatever the timer reports.
 */
NTSTATUS
NTAPI
KeConvertAuxiliaryCounterToPerformanceCounter(
    _In_ ULONG64 AuxiliaryCounterValue,
    _Out_ PULONG64 PerformanceCounterValue,
    _Out_opt_ PULONG64 ConversionError)
{
    if (PerformanceCounterValue == NULL)
        return STATUS_INVALID_PARAMETER;

    /* The counter is a platform feature, so a HAL without it has nothing to convert. */
    if (HalTimerConvertAuxiliaryCounterToPerformanceCounter == NULL)
    {
        *PerformanceCounterValue = 0;
        if (ConversionError != NULL)
            *ConversionError = 0;

        return STATUS_NOT_SUPPORTED;
    }

    return HalTimerConvertAuxiliaryCounterToPerformanceCounter(AuxiliaryCounterValue,
                                                               PerformanceCounterValue,
                                                               ConversionError);
}
