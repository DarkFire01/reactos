//
// PROJECT:         ReactOS Kernel
// LICENSE:         BSD - See COPYING.ARM in the top level directory
// FILE:            ntoskrnl/ke/arm64/exception.c
// PURPOSE:         ARM64 exception handling support for KD
//

#include <ntoskrnl.h>
#include <internal/ke.h>
#include <ndk/kdtypes.h>
#define NDEBUG
#include <debug.h>

/* LOCAL DEFINITIONS *********************************************************/
ULONG
DbgPrintEarly(const char *fmt, ...);

// ARM64 breakpoint instruction immediate values
#define ARM64_BREAKPOINT        0xF000  // __debugbreak
#define ARM64_DEBUG_SERVICE     0xF001  // __debugservice
#define ARM64_ASSERT_FAIL       0xF002  // __assertfail
#define ARM64_FAST_FAIL         0xF003  // __fastfail
#define ARM64_DIVIDE_BY_ZERO    0xF004  // __brkdiv0

#define ARM64_BRK_MASK          0xFFE0001F
#define ARM64_BRK_OPCODE        0xD4200000
#define ARM64_HLT_MASK          0xFFE0001F
#define ARM64_HLT_OPCODE        0xD4400000

VOID
NTAPI
KiSetVbarEl1(
    IN ULONG64 Vbar
);

/* EXTERNS *******************************************************************/

extern ULONG_PTR KiExceptionVector32;

static
BOOLEAN
KiTryDecodeBreakpointOpcode(
    IN ULONG64 Elr,
    OUT PULONG32 Immediate,
    OUT PULONG32 Instruction
)
{
    ULONG32 Op;

    Op = *(volatile ULONG32 *)(ULONG_PTR)Elr;
    *Instruction = Op;

    if (((Op & ARM64_BRK_MASK) == ARM64_BRK_OPCODE) ||
        ((Op & ARM64_HLT_MASK) == ARM64_HLT_OPCODE))
    {
        *Immediate = (Op >> 5) & 0xFFFF;
        return TRUE;
    }

    return FALSE;
}

/* FUNCTIONS *****************************************************************/

/**
 * KiSynchronousExceptionC - Handle synchronous exceptions from EL1
 * 
 * This function is called by the exception vector with register values passed as parameters.
 * It catches BRK instructions and routes them to the kernel debugger.
 * 
 * Parameters:
 *   x0 (Esr)  - Exception Status Register (includes EC field)
 *   x1 (Elr)  - Exception Link Register (faulting instruction address)
 *   x2 (Spsr) - Saved Processor State Register
 * 
 * We need to:
 * 1. Determine if this is a BRK instruction
 * 2. Extract the immediate value (breakpoint code)
 * 3. Route to appropriate KD handler
 * 4. Continue execution
 */
ULONG64
NTAPI
KiSynchronousExceptionC(
    IN ULONG64 Esr,
    IN ULONG64 Elr,
    IN ULONG64 Spsr,
    IN ULONG64 X0,
    IN ULONG64 X1,
    IN ULONG64 X2,
    IN ULONG64 Sp,
    IN ULONG64 Fp,
    IN ULONG64 Lr
)
{
    EXCEPTION_RECORD ExceptionRecord;
    KTRAP_FRAME TrapFrame;
    KEXCEPTION_FRAME ExceptionFrame;
    CONTEXT Context;
    ULONG32 Ec;
    ULONG32 OpCode = 0;
    ULONG32 ImmediateValue;
    BOOLEAN IsSoftwareBreakpoint = FALSE;
    ULONG64 ResumeElr = Elr;

    RtlZeroMemory(&TrapFrame, sizeof(TrapFrame));
    RtlZeroMemory(&ExceptionFrame, sizeof(ExceptionFrame));
    RtlZeroMemory(&Context, sizeof(Context));
    RtlZeroMemory(&ExceptionRecord, sizeof(ExceptionRecord));

    TrapFrame.ExceptionActive = KEXCEPTION_ACTIVE_EXCEPTION_FRAME;
    TrapFrame.Spsr = (ULONG)Spsr;
    TrapFrame.Esr = (ULONG)Esr;
    TrapFrame.Sp = Sp;
    TrapFrame.Pc = Elr;
    TrapFrame.Fp = Fp;
    TrapFrame.Lr = Lr;
    TrapFrame.X0 = X0;
    TrapFrame.X1 = X1;
    TrapFrame.X2 = X2;

    //
    // Extract the Exception Class (EC) field from bits [31:26] of ESR_EL1
    //
    Ec = (ULONG32)((Esr >> 26) & 0x3F);

    if (Ec == 0x3C)
    {
        ImmediateValue = (ULONG32)(Esr & 0xFFFF);
        IsSoftwareBreakpoint = TRUE;
    }
    else if (Ec == 0x00)
    {
        /*
         * Some early bringup paths can surface debug opcodes under EC=0.
         * Decode the faulting instruction and treat BRK/HLT as breakpoints.
         */
        IsSoftwareBreakpoint = KiTryDecodeBreakpointOpcode(Elr,
                                                           &ImmediateValue,
                                                           &OpCode);

        if (!IsSoftwareBreakpoint)
        {
            DbgPrintEarly("Unhandled EC=0 at %p, opcode=0x%08lx\n",
                          (PVOID)Elr,
                          OpCode);

            /* Avoid re-entering forever on the same unknown instruction. */
            return Elr + sizeof(ULONG32);
        }
    }

    //
    // Treat decoded software breakpoints via the regular KD path.
    //
    if (IsSoftwareBreakpoint)
    {
        DbgPrintEarly("ARM64 breakpoint immediate=0x%04x at %p\n",
                      ImmediateValue,
                      (PVOID)Elr);

        ExceptionRecord.ExceptionFlags = 0;
        ExceptionRecord.ExceptionRecord = NULL;
        ExceptionRecord.ExceptionAddress = (PVOID)Elr;

        switch (ImmediateValue)
        {
        case ARM64_BREAKPOINT:  // 0xF000 - __debugbreak()
            ExceptionRecord.ExceptionCode = STATUS_BREAKPOINT;
            ExceptionRecord.NumberParameters = 1;
            ExceptionRecord.ExceptionInformation[0] = BREAKPOINT_BREAK;
            break;

        case ARM64_DEBUG_SERVICE: // 0xF001 - __debugservice / DebugService2
            ExceptionRecord.ExceptionCode = STATUS_BREAKPOINT;
            ExceptionRecord.NumberParameters = 3;
            ExceptionRecord.ExceptionInformation[0] = (ULONG_PTR)TrapFrame.X2;
            ExceptionRecord.ExceptionInformation[1] = (ULONG_PTR)TrapFrame.X0;
            ExceptionRecord.ExceptionInformation[2] = (ULONG_PTR)TrapFrame.X1;
            break;

        case ARM64_DIVIDE_BY_ZERO:  // 0xF004
            ExceptionRecord.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO;
            ExceptionRecord.NumberParameters = 0;
            break;

        case ARM64_ASSERT_FAIL:     // 0xF002
            ExceptionRecord.ExceptionCode = STATUS_ASSERTION_FAILURE;
            ExceptionRecord.NumberParameters = 0;
            break;

        case ARM64_FAST_FAIL:       // 0xF003
            ExceptionRecord.ExceptionCode = STATUS_BREAKPOINT;
            ExceptionRecord.NumberParameters = 1;
            ExceptionRecord.ExceptionInformation[0] = BREAKPOINT_BREAK;
            break;

        default:
            ExceptionRecord.ExceptionCode = STATUS_BREAKPOINT;
            ExceptionRecord.NumberParameters = 1;
            ExceptionRecord.ExceptionInformation[0] = BREAKPOINT_BREAK;
            break;
        }

        Context.ContextFlags = CONTEXT_FULL;
        KeTrapFrameToContext(&TrapFrame, &ExceptionFrame, &Context);

        if (KiDebugRoutine(&TrapFrame,
                           &ExceptionFrame,
                           &ExceptionRecord,
                           &Context,
                           KiGetPreviousMode(&TrapFrame),
                           FALSE))
        {
            KeContextToTrapFrame(&Context,
                                 &ExceptionFrame,
                                 &TrapFrame,
                                 Context.ContextFlags,
                                 KiGetPreviousMode(&TrapFrame));

            /* KdpTrap updates context PC when needed; default to +4 fallback. */
            ResumeElr = TrapFrame.Pc;
            if (ResumeElr == Elr)
            {
                ResumeElr += sizeof(ULONG32);
            }
        }
        else
        {
            ResumeElr += sizeof(ULONG32);
        }
    }
    else
    {
        DbgPrintEarly("Unhandled synchronous exception EC=0x%x at %p\n",
                      Ec,
                      (PVOID)Elr);

        /* Avoid re-entering forever on non-debug synchronous exceptions. */
        ResumeElr += sizeof(ULONG32);
    }

    return ResumeElr;
}

/**
 * KiInitializeExceptionHandling - Set up the ARM64 exception vector table
 * 
 * This function must be called during kernel initialization to:
 * 1. Set the VBAR_EL1 register to point to our exception vector table
 * 2. Ensure interrupts are properly routed through our handlers
 */
VOID
NTAPI
KiInitializeExceptionHandling(VOID)
{
    ULONG64 Vbar;

    //
    // The exception vector table must be 2KB aligned (bit [10:0] must be 0)
    //
    Vbar = (ULONG64)&KiExceptionVector32;

    DbgPrintEarly("Setting VBAR_EL1 to %p (exception vector)\n", (PVOID)Vbar);

    if ((Vbar & 0x7FF) != 0)
    {
        DbgPrintEarly("ERROR: Exception vector not properly aligned (0x%p)\n", (PVOID)Vbar);
        DbgPrintEarly("       Must be 2KB aligned (0x800 boundary)\n");
        return;
    }

    //
    // Call assembly function to set VBAR_EL1 and issue barriers
    //
    KiSetVbarEl1(Vbar);

    DbgPrintEarly("Exception vector table installed at %p\n", (PVOID)Vbar);
}

