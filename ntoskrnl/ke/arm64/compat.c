/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm64/compat.c
 * PURPOSE:         ARM64 compatibility helpers for bringup
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef RtlFillMemory
#undef RtlFillMemory
#endif
#ifdef RtlMoveMemory
#undef RtlMoveMemory
#endif
#ifdef RtlZeroMemory
#undef RtlZeroMemory
#endif
#ifdef RtlFillMemoryUlong
#undef RtlFillMemoryUlong
#endif
ULONG KiDmaIoCoherency = 0;
ULONG KeLargestCacheLine = 64;

/*
 * KiInterruptDispatch - Kernel-side IRQ dispatcher for ARM64.
 *
 * Called from the KiInterruptException assembly stub with SavedSp pointing
 * to the 192-byte register-save frame on the kernel stack.
 *
 * The actual dispatch logic lives in the HAL (gic.c: HalpDispatchIrq) because
 * the HAL owns the GIC MMIO addresses and IRQL tables.  The HAL registers its
 * dispatch function in Pcr->HalReserved[14] during HalpInitPhase0.
 *
 * Using HalReserved[] avoids any cross-module symbol dependency: ntoskrnl
 * calls this stub (which is in ntoskrnl), and the HAL populates the slot via
 * its normal access to the PCR -- no new HAL exports required.
 */
VOID
NTAPI
KiInterruptDispatch(IN ULONG_PTR SavedSp)
{
    PKPCR Pcr = KeGetPcr();
    VOID (NTAPI *HalIrqDispatch)(ULONG_PTR) =
        (VOID (NTAPI *)(ULONG_PTR))Pcr->HalReserved[14];

    if (HalIrqDispatch)
        HalIrqDispatch(SavedSp);
}

VOID
NTAPI
RtlFillMemory(OUT VOID *Destination,
              IN SIZE_T Length,
              IN UCHAR Fill);

VOID
NTAPI
RtlZeroMemory(OUT VOID *Destination,
              IN SIZE_T Length);

VOID
NTAPI
RtlCaptureContext(OUT PCONTEXT ContextRecord)
{
    RtlZeroMemory(ContextRecord, sizeof(*ContextRecord));
    ContextRecord->ContextFlags = CONTEXT_FULL;
}

SIZE_T
NTAPI
RtlCompareMemory(IN const VOID *Source1,
                 IN const VOID *Source2,
                 IN SIZE_T Length)
{
    const UCHAR *A = (const UCHAR *)Source1;
    const UCHAR *B = (const UCHAR *)Source2;
    SIZE_T Index;

    for (Index = 0; Index < Length; ++Index)
    {
        if (A[Index] != B[Index]) break;
    }

    return Index;
}

SIZE_T
NTAPI
RtlCompareMemoryUlong(IN PVOID Source,
                      IN SIZE_T Length,
                      IN ULONG Pattern)
{
    ULONG *Data = (ULONG *)Source;
    SIZE_T Count = Length / sizeof(ULONG);
    SIZE_T Index = 0;

    while ((Index < Count) && (Data[Index] == Pattern))
    {
        ++Index;
    }

    return Index * sizeof(ULONG);
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPopEntrySList(IN OUT PSLIST_HEADER ListHead)
{
    PSLIST_ENTRY Entry;

    Entry = (PSLIST_ENTRY)(ULONG_PTR)ListHead->Region;
    if (!Entry) return NULL;
    ListHead->Region = (ULONGLONG)(ULONG_PTR)Entry->Next;

    return Entry;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPushEntrySList(IN OUT PSLIST_HEADER ListHead,
                             IN OUT PSLIST_ENTRY ListEntry)
{
    PSLIST_ENTRY OldHead;

    OldHead = (PSLIST_ENTRY)(ULONG_PTR)ListHead->Region;
    ListEntry->Next = OldHead;
    ListHead->Region = (ULONGLONG)(ULONG_PTR)ListEntry;

    return OldHead;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedFlushSList(IN OUT PSLIST_HEADER ListHead)
{
    PSLIST_ENTRY OldHead = (PSLIST_ENTRY)(ULONG_PTR)ListHead->Region;
    ListHead->Region = 0;
    return OldHead;
}

PSLIST_ENTRY
NTAPI
ExpInterlockedPopEntrySList(IN OUT PSLIST_HEADER SListHead)
{
    return RtlInterlockedPopEntrySList(SListHead);
}

PSLIST_ENTRY
NTAPI
ExpInterlockedPushEntrySList(IN OUT PSLIST_HEADER SListHead,
                             IN OUT PSLIST_ENTRY SListEntry)
{
    return RtlInterlockedPushEntrySList(SListHead, SListEntry);
}

PSLIST_ENTRY
NTAPI
ExpInterlockedFlushSList(IN OUT PSLIST_HEADER SListHead)
{
    return RtlInterlockedFlushSList(SListHead);
}

ULONG
NTAPI
KeGetCurrentProcessorNumber(VOID)
{
    return (ULONG)KeGetCurrentPrcb()->Number;
}

ULONG
NTAPI
KeGetCurrentProcessorIndex(VOID)
{
    return KeGetCurrentProcessorNumber();
}

#undef KeGetCurrentPrcb
PKPRCB
NTAPI
KeGetCurrentPrcb(VOID)
{
    return (&((PKIPCR)KeGetPcr())->Prcb);
}
#define KeGetCurrentPrcb() (&((PKIPCR)KeGetPcr())->Prcb)

VOID
NTAPI
KiFlushSingleTb(IN BOOLEAN Invalid,
                IN PVOID Virtual)
{
    UNREFERENCED_PARAMETER(Invalid);
    KeArm64InvalidateTlbEntry(Virtual);
}

VOID
KeFlushTb(VOID)
{
    KeArm64InvalidateAllTlb();
}

VOID
NTAPI
KeFlushCurrentTb(VOID)
{
    KeFlushTb();
}

BOOLEAN
NTAPI
KeInvalidateAllCaches(VOID)
{
    KeArm64CleanDataCache();
    KeArm64InvalidateICache();
    return TRUE;
}

ULONG
NTAPI
KeGetRecommendedSharedDataAlignment(VOID)
{
    return KeLargestCacheLine;
}

VOID
NTAPI
KeFlushEntireTb(IN BOOLEAN Invalid,
                IN BOOLEAN AllProcessors)
{
    UNREFERENCED_PARAMETER(Invalid);
    UNREFERENCED_PARAMETER(AllProcessors);
    KeFlushCurrentTb();
}

VOID
NTAPI
KeSetDmaIoCoherency(IN ULONG Coherency)
{
    KiDmaIoCoherency = Coherency;
}

VOID
__cdecl
KeSaveStateForHibernate(IN PKPROCESSOR_STATE State)
{
    RtlCaptureContext(&State->ContextFrame);
}

NTSTATUS
NTAPI
NtVdmControl(IN ULONG ControlCode,
             IN PVOID ControlData)
{
    UNREFERENCED_PARAMETER(ControlCode);
    UNREFERENCED_PARAMETER(ControlData);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NtSetLdtEntries(IN ULONG Selector1,
                IN LDT_ENTRY LdtEntry1,
                IN ULONG Selector2,
                IN LDT_ENTRY LdtEntry2)
{
    UNREFERENCED_PARAMETER(Selector1);
    UNREFERENCED_PARAMETER(LdtEntry1);
    UNREFERENCED_PARAMETER(Selector2);
    UNREFERENCED_PARAMETER(LdtEntry2);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NtCallbackReturn(IN PVOID Result,
                 IN ULONG ResultLength,
                 IN NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(Result);
    UNREFERENCED_PARAMETER(ResultLength);
    return Status;
}

VOID
NTAPI
KeInitializeInterrupt(IN PKINTERRUPT Interrupt,
                      IN PKSERVICE_ROUTINE ServiceRoutine,
                      IN PVOID ServiceContext,
                      IN PKSPIN_LOCK SpinLock,
                      IN ULONG Vector,
                      IN KIRQL Irql,
                      IN KIRQL SynchronizeIrql,
                      IN KINTERRUPT_MODE InterruptMode,
                      IN BOOLEAN ShareVector,
                      IN CHAR ProcessorNumber,
                      IN BOOLEAN FloatingSave)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(ServiceRoutine);
    UNREFERENCED_PARAMETER(ServiceContext);
    UNREFERENCED_PARAMETER(SpinLock);
    UNREFERENCED_PARAMETER(Vector);
    UNREFERENCED_PARAMETER(Irql);
    UNREFERENCED_PARAMETER(SynchronizeIrql);
    UNREFERENCED_PARAMETER(InterruptMode);
    UNREFERENCED_PARAMETER(ShareVector);
    UNREFERENCED_PARAMETER(ProcessorNumber);
    UNREFERENCED_PARAMETER(FloatingSave);
}

BOOLEAN
NTAPI
KeConnectInterrupt(IN PKINTERRUPT Interrupt)
{
    UNREFERENCED_PARAMETER(Interrupt);
    return FALSE;
}

BOOLEAN
NTAPI
KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
{
    UNREFERENCED_PARAMETER(Interrupt);
    return FALSE;
}

BOOLEAN
NTAPI
KeSynchronizeExecution(IN OUT PKINTERRUPT Interrupt,
                       IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                       IN PVOID SynchronizeContext)
{
    UNREFERENCED_PARAMETER(Interrupt);
    return SynchronizeRoutine(SynchronizeContext);
}

KIRQL
FASTCALL
KeAcquireQueuedSpinLock(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    UNREFERENCED_PARAMETER(LockNumber);
    return KfRaiseIrql(DISPATCH_LEVEL);
}

VOID
FASTCALL
KeReleaseQueuedSpinLock(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber,
                        IN KIRQL OldIrql)
{
    UNREFERENCED_PARAMETER(LockNumber);
    KfLowerIrql(OldIrql);
}

VOID
FASTCALL
KeAcquireInStackQueuedSpinLock(IN PKSPIN_LOCK SpinLock,
                               IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    UNREFERENCED_PARAMETER(SpinLock);
    LockHandle->OldIrql = KfRaiseIrql(DISPATCH_LEVEL);
}

VOID
FASTCALL
KeReleaseInStackQueuedSpinLock(IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    KfLowerIrql(LockHandle->OldIrql);
}

ULONG
NTAPI
KxSwitchKdProcessor(IN ULONG ProcessorIndex)
{
    UNREFERENCED_PARAMETER(ProcessorIndex);
    return KeGetCurrentProcessorNumber();
}

EXCEPTION_DISPOSITION
NTAPI
__C_specific_handler(IN struct _EXCEPTION_RECORD *ExceptionRecord,
                     IN PVOID EstablisherFrame,
                     IN struct _CONTEXT *ContextRecord,
                     IN struct _DISPATCHER_CONTEXT *DispatcherContext)
{
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(EstablisherFrame);
    UNREFERENCED_PARAMETER(ContextRecord);
    UNREFERENCED_PARAMETER(DispatcherContext);
    return ExceptionContinueSearch;
}

VOID
__cdecl
_local_unwind(VOID)
{
}

VOID
NTAPI
RtlFillMemory(OUT VOID *Destination,
              IN SIZE_T Length,
              IN UCHAR Fill)
{
    UCHAR *Bytes = (UCHAR *)Destination;
    SIZE_T Index;

    for (Index = 0; Index < Length; ++Index)
    {
        Bytes[Index] = Fill;
    }
}

VOID
NTAPI
RtlMoveMemory(OUT VOID *Destination,
              IN const VOID *Source,
              IN SIZE_T Length)
{
    UCHAR *Dst = (UCHAR *)Destination;
    const UCHAR *Src = (const UCHAR *)Source;
    SIZE_T Index;

    if ((Dst > Src) && (Dst < Src + Length))
    {
        for (Index = Length; Index != 0; --Index)
        {
            Dst[Index - 1] = Src[Index - 1];
        }
    }
    else
    {
        for (Index = 0; Index < Length; ++Index)
        {
            Dst[Index] = Src[Index];
        }
    }
}

VOID
NTAPI
RtlZeroMemory(OUT VOID *Destination,
              IN SIZE_T Length)
{
    RtlFillMemory(Destination, Length, 0);
}

VOID
NTAPI
RtlGetCallersAddress(OUT PVOID *CallersAddress,
                     OUT PVOID *CallersCaller)
{
    *CallersAddress = NULL;
    *CallersCaller = NULL;
}

VOID
NTAPI
_RtlFillMemoryUlong(OUT VOID *Destination,
                    IN SIZE_T Length,
                    IN ULONG Pattern)
{
    PULONG Ptr = (PULONG)Destination;
    SIZE_T Count = Length / sizeof(ULONG);
    SIZE_T Index;

    for (Index = 0; Index < Count; ++Index)
    {
        Ptr[Index] = Pattern;
    }
}

VOID
NTAPI
RtlFillMemoryUlong(OUT VOID *Destination,
                   IN SIZE_T Length,
                   IN ULONG Pattern)
{
    _RtlFillMemoryUlong(Destination, Length, Pattern);
}

VOID
NTAPI
RtlFillMemoryUlonglong(OUT VOID *Destination,
                       IN SIZE_T Length,
                       IN ULONGLONG Pattern)
{
    PULONGLONG Ptr = (PULONGLONG)Destination;
    SIZE_T Count = Length / sizeof(ULONGLONG);
    SIZE_T Index;

    for (Index = 0; Index < Count; ++Index)
    {
        Ptr[Index] = Pattern;
    }
}

NTSTATUS
NTAPI
KeRaiseUserException(IN NTSTATUS ExceptionCode)
{
    ExRaiseStatus(ExceptionCode);
    return ExceptionCode;
}

NTSTATUS
NTAPI
KeUserModeCallback(IN ULONG RoutineIndex,
                   IN PVOID Argument,
                   IN ULONG ArgumentLength,
                   OUT PVOID *Result,
                   OUT PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(RoutineIndex);
    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(ArgumentLength);

    if (Result) *Result = NULL;
    if (ResultLength) *ResultLength = 0;

    return STATUS_NOT_IMPLEMENTED;
}

VOID
NTAPI
RtlUnwind(IN PVOID TargetFrame,
          IN PVOID TargetIp,
          IN PEXCEPTION_RECORD ExceptionRecord,
          IN PVOID ReturnValue)
{
    UNREFERENCED_PARAMETER(TargetFrame);
    UNREFERENCED_PARAMETER(TargetIp);
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(ReturnValue);
    ExRaiseStatus(STATUS_UNWIND);
}

NTSTATUS
NTAPI
KiCallUserMode(IN PVOID *OutputBuffer,
               IN PULONG OutputLength)
{
    if (OutputBuffer) *OutputBuffer = NULL;
    if (OutputLength) *OutputLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

VOID
NTAPI
KiRestoreProcessorControlState(IN PKPROCESSOR_STATE ProcessorState)
{
    UNREFERENCED_PARAMETER(ProcessorState);
}

VOID
NTAPI
KiSaveProcessorControlState(OUT PKPROCESSOR_STATE ProcessorState)
{
    RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));
}

VOID
NTAPI
KiInitializeUserApc(IN PKEXCEPTION_FRAME Reserved,
                    IN PKTRAP_FRAME TrapFrame,
                    IN PKNORMAL_ROUTINE NormalRoutine,
                    IN PVOID NormalContext,
                    IN PVOID SystemArgument1,
                    IN PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
}

BOOLEAN
FASTCALL
KiSwapContext(IN KIRQL WaitIrql,
              IN PKTHREAD CurrentThread)
{
    UNREFERENCED_PARAMETER(WaitIrql);
    UNREFERENCED_PARAMETER(CurrentThread);
    return FALSE;
}

DECLSPEC_NORETURN
VOID
NTAPI
KiExceptionExit(IN PKTRAP_FRAME TrapFrame,
                IN PKEXCEPTION_FRAME ExceptionFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);

    /*
     * ARM64 bringup stub: a real implementation must restore processor state
     * and return from exception in assembly. Avoid recursive debug breaks.
     */
    KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED,
                 0,
                 (ULONG_PTR)TrapFrame,
                 (ULONG_PTR)ExceptionFrame,
                 0);
}

VOID
NTAPI
KeContextToTrapFrame(IN PCONTEXT Context,
                     IN PKEXCEPTION_FRAME ExeptionFrame,
                     IN PKTRAP_FRAME TrapFrame,
                     IN ULONG ContextFlags,
                     IN KPROCESSOR_MODE PreviousMode)
{
    UNREFERENCED_PARAMETER(PreviousMode);

    if (ContextFlags & CONTEXT_INTEGER)
    {
        TrapFrame->X0 = Context->X0;
        TrapFrame->X1 = Context->X1;
        TrapFrame->X2 = Context->X2;
        TrapFrame->X3 = Context->X3;
        TrapFrame->X4 = Context->X4;
        TrapFrame->X5 = Context->X5;
        TrapFrame->X6 = Context->X6;
        TrapFrame->X7 = Context->X7;
        TrapFrame->X8 = Context->X8;
        TrapFrame->X9 = Context->X9;
        TrapFrame->X10 = Context->X10;
        TrapFrame->X11 = Context->X11;
        TrapFrame->X12 = Context->X12;
        TrapFrame->X13 = Context->X13;
        TrapFrame->X14 = Context->X14;
        TrapFrame->X15 = Context->X15;
        TrapFrame->X16 = Context->X16;
        TrapFrame->X17 = Context->X17;
        TrapFrame->X18 = Context->X18;

        if (ExeptionFrame)
        {
            ExeptionFrame->X19 = Context->X19;
            ExeptionFrame->X20 = Context->X20;
            ExeptionFrame->X21 = Context->X21;
            ExeptionFrame->X22 = Context->X22;
            ExeptionFrame->X23 = Context->X23;
            ExeptionFrame->X24 = Context->X24;
            ExeptionFrame->X25 = Context->X25;
            ExeptionFrame->X26 = Context->X26;
            ExeptionFrame->X27 = Context->X27;
            ExeptionFrame->X28 = Context->X28;
        }
    }

    if (ContextFlags & CONTEXT_CONTROL)
    {
        TrapFrame->Spsr = Context->Cpsr;
        TrapFrame->Fp = Context->Fp;
        TrapFrame->Lr = Context->Lr;
        TrapFrame->Sp = Context->Sp;
        TrapFrame->Pc = Context->Pc;

        if (ExeptionFrame)
        {
            ExeptionFrame->Fp = Context->Fp;
            ExeptionFrame->Return = Context->Lr;
        }
    }
}

VOID
NTAPI
KeTrapFrameToContext(IN PKTRAP_FRAME TrapFrame,
                     IN PKEXCEPTION_FRAME ExceptionFrame,
                     IN OUT PCONTEXT Context)
{
    ULONG ContextFlags;

    ContextFlags = Context->ContextFlags;

    if (ContextFlags & CONTEXT_INTEGER)
    {
        Context->X0 = TrapFrame->X0;
        Context->X1 = TrapFrame->X1;
        Context->X2 = TrapFrame->X2;
        Context->X3 = TrapFrame->X3;
        Context->X4 = TrapFrame->X4;
        Context->X5 = TrapFrame->X5;
        Context->X6 = TrapFrame->X6;
        Context->X7 = TrapFrame->X7;
        Context->X8 = TrapFrame->X8;
        Context->X9 = TrapFrame->X9;
        Context->X10 = TrapFrame->X10;
        Context->X11 = TrapFrame->X11;
        Context->X12 = TrapFrame->X12;
        Context->X13 = TrapFrame->X13;
        Context->X14 = TrapFrame->X14;
        Context->X15 = TrapFrame->X15;
        Context->X16 = TrapFrame->X16;
        Context->X17 = TrapFrame->X17;
        Context->X18 = TrapFrame->X18;

        if (ExceptionFrame)
        {
            Context->X19 = ExceptionFrame->X19;
            Context->X20 = ExceptionFrame->X20;
            Context->X21 = ExceptionFrame->X21;
            Context->X22 = ExceptionFrame->X22;
            Context->X23 = ExceptionFrame->X23;
            Context->X24 = ExceptionFrame->X24;
            Context->X25 = ExceptionFrame->X25;
            Context->X26 = ExceptionFrame->X26;
            Context->X27 = ExceptionFrame->X27;
            Context->X28 = ExceptionFrame->X28;
        }
        else
        {
            Context->X19 = 0;
            Context->X20 = 0;
            Context->X21 = 0;
            Context->X22 = 0;
            Context->X23 = 0;
            Context->X24 = 0;
            Context->X25 = 0;
            Context->X26 = 0;
            Context->X27 = 0;
            Context->X28 = 0;
        }
    }

    if (ContextFlags & CONTEXT_CONTROL)
    {
        Context->Cpsr = TrapFrame->Spsr;
        Context->Fp = TrapFrame->Fp;
        Context->Lr = TrapFrame->Lr;
        Context->Sp = TrapFrame->Sp;
        Context->Pc = TrapFrame->Pc;

        if (ExceptionFrame)
        {
            Context->Fp = ExceptionFrame->Fp;
            Context->Lr = ExceptionFrame->Return;
        }
    }
}

VOID
NTAPI
KiDispatchException(IN PEXCEPTION_RECORD ExceptionRecord,
                    IN PKEXCEPTION_FRAME ExceptionFrame,
                    IN PKTRAP_FRAME Tf,
                    IN KPROCESSOR_MODE PreviousMode,
                    IN BOOLEAN SearchFrames)
{
    CONTEXT Context;

    KeGetCurrentPrcb()->KeExceptionDispatchCount++;

    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FULL;

    KeTrapFrameToContext(Tf, ExceptionFrame, &Context);

    if ((PreviousMode == KernelMode) && SearchFrames)
    {
        if (KiDebugRoutine(Tf,
                           ExceptionFrame,
                           ExceptionRecord,
                           &Context,
                           PreviousMode,
                           FALSE))
        {
            goto Handled;
        }

        if (RtlDispatchException(ExceptionRecord, &Context))
        {
            goto Handled;
        }
    }

    if ((PreviousMode == KernelMode) &&
        KiDebugRoutine(Tf,
                       ExceptionFrame,
                       ExceptionRecord,
                       &Context,
                       PreviousMode,
                       TRUE))
    {
        goto Handled;
    }

    KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED,
                 ExceptionRecord->ExceptionCode,
                 (ULONG_PTR)ExceptionRecord->ExceptionAddress,
                 (ULONG_PTR)Tf,
                 0);

Handled:
    KeContextToTrapFrame(&Context,
                         ExceptionFrame,
                         Tf,
                         Context.ContextFlags,
                         PreviousMode);
}

CODE_SEG("INIT")
VOID
NTAPI
KiInitMachineDependent(VOID)
{
}

VOID
NTAPI
KiSwapProcess(IN PKPROCESS NewProcess,
              IN PKPROCESS OldProcess)
{
    UNREFERENCED_PARAMETER(NewProcess);
    UNREFERENCED_PARAMETER(OldProcess);
}

VOID
NTAPI
KiInitializeContextThread(IN PKTHREAD Thread,
                          IN PKSYSTEM_ROUTINE SystemRoutine,
                          IN PKSTART_ROUTINE StartRoutine,
                          IN PVOID StartContext,
                          IN PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Thread);
    UNREFERENCED_PARAMETER(SystemRoutine);
    UNREFERENCED_PARAMETER(StartRoutine);
    UNREFERENCED_PARAMETER(StartContext);
    UNREFERENCED_PARAMETER(Context);
}

CODE_SEG("INIT")
VOID
NTAPI
MiInitializeSessionSpaceLayout(VOID)
{
}

CODE_SEG("INIT")
VOID
NTAPI
MiInitMachineDependent(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
}

VOID
FASTCALL
KeZeroPages(IN PVOID Address,
            IN ULONG Size)
{
    RtlZeroMemory(Address, Size);
}

BOOLEAN
NTAPI
MiArchCreateProcessAddressSpace(IN PEPROCESS Process,
                                IN PULONG_PTR DirectoryTableBase)
{
    UNREFERENCED_PARAMETER(Process);
    if (DirectoryTableBase) *DirectoryTableBase = 0;
    return TRUE;
}

VOID
NTAPI
MmDeletePageFileMapping(IN PEPROCESS Process,
                        IN PVOID Address,
                        IN SWAPENTRY *SwapEntry)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    if (SwapEntry) *SwapEntry = 0;
}

BOOLEAN
NTAPI
MmIsPageSwapEntry(IN PEPROCESS Process,
                  IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    return FALSE;
}

BOOLEAN
NTAPI
MmDeleteVirtualMapping(IN PEPROCESS Process,
                       IN PVOID Address,
                       OUT PBOOLEAN WasDirty,
                       OUT PPFN_NUMBER Page)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    if (WasDirty) *WasDirty = FALSE;
    if (Page) *Page = 0;
    return TRUE;
}

BOOLEAN
NTAPI
MmDeletePhysicalMapping(IN PEPROCESS Process,
                        IN PVOID Address,
                        OUT PBOOLEAN WasDirty,
                        OUT PPFN_NUMBER Page)
{
    return MmDeleteVirtualMapping(Process, Address, WasDirty, Page);
}

CODE_SEG("INIT")
VOID
NTAPI
MmInitGlobalKernelPageDirectory(VOID)
{
}

NTSTATUS
NTAPI
MmCreateVirtualMapping(IN PEPROCESS Process,
                       IN PVOID Address,
                       IN ULONG flProtect,
                       IN PFN_NUMBER Page)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(flProtect);
    UNREFERENCED_PARAMETER(Page);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MmCreatePageFileMapping(IN PEPROCESS Process,
                        IN PVOID Address,
                        IN SWAPENTRY SwapEntry)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(SwapEntry);
    return STATUS_SUCCESS;
}

VOID
NTAPI
MmSetDirtyBit(IN PEPROCESS Process,
              IN PVOID Address,
              IN BOOLEAN Bit)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(Bit);
}

PFN_NUMBER
NTAPI
MmGetPfnForProcess(IN PEPROCESS Process,
                   IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    return 0;
}

NTSTATUS
NTAPI
MmCreatePhysicalMapping(IN PEPROCESS Process,
                        IN PVOID Address,
                        IN ULONG flProtect,
                        IN PFN_NUMBER Page)
{
    return MmCreateVirtualMapping(Process, Address, flProtect, Page);
}

ULONG
NTAPI
MmGetPageProtect(IN PEPROCESS Process,
                 IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    return PAGE_READWRITE;
}

VOID
NTAPI
MmSetPageProtect(IN PEPROCESS Process,
                 IN PVOID Address,
                 IN ULONG flProtect)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(flProtect);
}

BOOLEAN
NTAPI
MmIsPagePresent(IN PEPROCESS Process,
                IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    return FALSE;
}

BOOLEAN
NTAPI
MmIsDisabledPage(IN PEPROCESS Process,
                 IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    return FALSE;
}

VOID
NTAPI
MmGetPageFileMapping(IN PEPROCESS Process,
                     IN PVOID Address,
                     OUT SWAPENTRY *SwapEntry)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(Address);
    if (SwapEntry) *SwapEntry = 0;
}

VOID
NTAPI
PspGetOrSetContextKernelRoutine(IN PKAPC Apc,
                                IN OUT PKNORMAL_ROUTINE *NormalRoutine,
                                IN OUT PVOID *NormalContext,
                                IN OUT PVOID *SystemArgument1,
                                IN OUT PVOID *SystemArgument2)
{
    UNREFERENCED_PARAMETER(Apc);
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
}

ULONG ProcessCount = 0;

PVOID
NTAPI
KeSwitchKernelStack(IN PVOID StackBase,
                    IN PVOID StackLimit)
{
    UNREFERENCED_PARAMETER(StackLimit);
    return StackBase;
}

BOOLEAN
NTAPI
RtlDispatchException(IN PEXCEPTION_RECORD ExceptionRecord,
                     IN PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(Context);
    return FALSE;
}

VOID
NTAPI
RtlInitializeContext(IN HANDLE ProcessHandle,
                     OUT PCONTEXT ThreadContext,
                     IN PVOID ThreadStartParam,
                     IN PTHREAD_START_ROUTINE ThreadStartAddress,
                     IN PINITIAL_TEB InitialTeb)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    RtlZeroMemory(ThreadContext, sizeof(*ThreadContext));
    ThreadContext->Pc = (ULONG_PTR)ThreadStartAddress;
    ThreadContext->Sp = (ULONG_PTR)InitialTeb->StackBase;
    ThreadContext->X0 = (ULONG_PTR)ThreadStartParam;
    ThreadContext->ContextFlags = CONTEXT_FULL;
}

double
__cdecl
floor(double x)
{
    return x;
}

double
__cdecl
pow(double x, double y)
{
    UNREFERENCED_PARAMETER(y);
    return x;
}

double
__cdecl
log10(double x)
{
    UNREFERENCED_PARAMETER(x);
    return 0.0;
}
