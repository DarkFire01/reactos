#pragma once

//
// ARM64 Kernel Execution Macros
//

#define KiServiceExit2 KiExceptionExit

#define SYNCH_LEVEL DISPATCH_LEVEL
#define PCR                     ((KPCR * const)KIP0PCRADDRESS)

//
// ARM64 uses BRK instruction for breakpoints (4 bytes)
//
#define KD_BREAKPOINT_TYPE        ULONG
#define KD_BREAKPOINT_SIZE        sizeof(ULONG)
#define KD_BREAKPOINT_VALUE       0xD4200000  // BRK #0

//
// Maximum interrupt vectors for ARM64
//
#define MAXIMUM_VECTOR          256

//
// Program counter and register access macros
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
//
#define KeGetContextSwitches(Prcb)  \
    (Prcb)->KeContextSwitches

//
// Macro to get the data cache size field
//
#define KiGetSecondLevelDCacheSize() ((PKIPCR)KeGetPcr())->SecondLevelDcacheSize

//
// Returns the Interrupt State from a Trap Frame (ARM64 uses DAIF flags)
//
#define KeGetTrapFrameInterruptState(TrapFrame) \
    (((TrapFrame)->Spsr & DAIF_IRQF) == 0)
