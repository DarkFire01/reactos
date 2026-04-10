//
// PROJECT:         ReactOS Kernel
// LICENSE:         BSD - See COPYING.ARM in the top level directory
// FILE:            ntoskrnl/include/arm64/exception.h
// PURPOSE:         ARM64 exception handling declarations
//

#ifndef _ARM64_EXCEPTION_H_
#define _ARM64_EXCEPTION_H_

//
// Forward declarations
//

//
// Initialize the exception handling subsystem (set VBAR_EL1, etc.)
// Must be called during kernel initialization, before enabling exceptions
//
VOID
NTAPI
KiInitializeExceptionHandling(VOID);

//
// ARM64 exception handler - called from assembly vector table
// Handles synchronous exceptions (BRK, page faults, etc.)
// Parameters:
//   x0 (Esr)  - Exception Status Register
//   x1 (Elr)  - Exception Link Register
//   x2 (Spsr) - Saved Processor State Register
//
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
);

//
// Set VBAR_EL1 (Vector Base Address Register, EL1)
// Called from C code to install the exception vector table
//
VOID
NTAPI
KiSetVbarEl1(
    IN ULONG64 VbarAddress
);

//
// Exception vectors (defined in exception.S, must be 2KB aligned)
//
extern ULONG_PTR KiExceptionVector32;

#endif // _ARM64_EXCEPTION_H_
