#pragma once
#define KiServiceExit2 KiExceptionExit

#define SYNCH_LEVEL DISPATCH_LEVEL
#define PCR                     ((KPCR * const)KIP0PCRADDRESS)

//
//Lockdown TLB entries
//
#define PCR_ENTRY            0
#define PDR_ENTRY            2

//
// BKPT is 4 bytes long
//
#define KD_BREAKPOINT_TYPE        ULONG
#define KD_BREAKPOINT_SIZE        sizeof(ULONG)
#define KD_BREAKPOINT_VALUE       0xDEFE

//
// Maximum IRQs
//
#define MAXIMUM_VECTOR          16

//
// Macros for getting and setting special purpose registers in portable code
//
#define KeGetContextPc(Context) \
    ((Context)->Pc)

#define KeSetContextPc(Context, ProgramCounter) \
    ((Context)->Pc = (ProgramCounter))

#define KeGetTrapFramePc(TrapFrame) \
    ((TrapFrame)->Pc)

#define KeGetContextReturnRegister(Context) \
    ((Context)->X0)

#define KeSetContextReturnRegister(Context, ReturnValue) \
    ((Context)->X0 = (ReturnValue))

//
// Macro to get trap and exception frame from a thread stack
//
#define KeGetTrapFrame(Thread) \
    (PKTRAP_FRAME)((ULONG_PTR)((Thread)->InitialStack) - \
                   sizeof(KTRAP_FRAME))

#define KeGetExceptionFrame(Thread) \
    (PKEXCEPTION_FRAME)((ULONG_PTR)KeGetTrapFrame(Thread) - \
                        sizeof(KEXCEPTION_FRAME))

//
// Macro to get context switches from the PRCB
// All architectures but x86 have it in the PRCB's KeContextSwitches
//
#define KeGetContextSwitches(Prcb)  \
    (Prcb)->KeContextSwitches

//
// Macro to get the second level cache size field name which differs between
// CISC and RISC architectures, as the former has unified I/D cache
//
#define KiGetSecondLevelDCacheSize() ((PKIPCR)KeGetPcr())->SecondLevelDcacheSize

//
// Returns the Interrupt State from a Trap Frame.
// ON = TRUE, OFF = FALSE
//
#define KeGetTrapFrameInterruptState(TrapFrame) 0

FORCEINLINE
BOOLEAN
KeDisableInterrupts(VOID)
{
    return 1;
}

FORCEINLINE
VOID
KeRestoreInterrupts(BOOLEAN WereEnabled)
{
    if (WereEnabled) _enable();
}

#define Ki386PerfEnd()
#define KiEndInterrupt(x,y)

#define KiGetLinkedTrapFrame(x) \
    (PKTRAP_FRAME)((x)->TrapFrame)

FORCEINLINE
VOID
KiRundownThread(IN PKTHREAD Thread)
{
    /* FIXME */
}

FORCEINLINE
VOID
KeFlushProcessTb(VOID)
{
    //TODO: ARM64
}

//
FORCEINLINE
VOID
KeInvalidateTlbEntry(IN PVOID Address)
{
    //TODO: ARM64
}


FORCEINLINE
VOID
KeSweepICache(IN PVOID BaseAddress,
              IN SIZE_T FlushSize)
{
    //
    // Always sweep the whole cache
    //
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(FlushSize);
 ///   UNIMPLEMENTED;
}

// win64 uses DMA macros, this one is not defined
NTHALAPI
NTSTATUS
NTAPI
HalAllocateAdapterChannel(
  IN PADAPTER_OBJECT  AdapterObject,
  IN PWAIT_CONTEXT_BLOCK  Wcb,
  IN ULONG  NumberOfMapRegisters,
  IN PDRIVER_CONTROL  ExecutionRoutine);

// HACK
extern NTKERNELAPI volatile KSYSTEM_TIME KeTickCount;