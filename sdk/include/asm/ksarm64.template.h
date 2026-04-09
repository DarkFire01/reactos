
RAW(""),
RAW("#include <kxarm64.h>"),
RAW(""),

/*
 * ARM64-specific CONTEXT flag values.
 * These #defines override the generic CONTEXT_CONTROL / CONTEXT_INTEGER etc.
 * symbols (which are architecture-specific) so that the CONSTANTX() calls in
 * ksx.template.h emit the correct ARM64 values into the generated ksarm64.h.
 */
#define CONTEXT_ARM64           0x00400000L
#define CONTEXT_CONTROL         (CONTEXT_ARM64 | 0x1L)
#define CONTEXT_INTEGER         (CONTEXT_ARM64 | 0x2L)
#define CONTEXT_FLOATING_POINT  (CONTEXT_ARM64 | 0x4L)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_ARM64 | 0x8L)
#define CONTEXT_X18             (CONTEXT_ARM64 | 0x10L)
#define CONTEXT_FULL            (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)

HEADER("Pointer size"),
SIZE(SizeofPointer, PVOID),

HEADER("PAGE constants"),
CONSTANT(PAGE_SHIFT),

HEADER("CONTEXT flags"),
CONSTANTX(CONTEXT_ARM64, CONTEXT_ARM64),
CONSTANT(CONTEXT_CONTROL),
CONSTANT(CONTEXT_INTEGER),
CONSTANT(CONTEXT_FLOATING_POINT),
CONSTANT(CONTEXT_DEBUG_REGISTERS),
CONSTANTX(CONTEXT_X18, CONTEXT_X18),
CONSTANT(CONTEXT_FULL),

/*
 * ARM64 PSTATE / SPSR exception-level and flag bits.
 * Defined inline here because they are not yet in the ReactOS C headers.
 * (Equivalent to CPSRM_* / CPSRF_* for ARM32 in ksarm.template.h.)
 */
#define PSTATE_M_EL0t   0x00    /* EL0 Thread  */
#define PSTATE_M_EL1t   0x04    /* EL1 Thread  */
#define PSTATE_M_EL1h   0x05    /* EL1 Handler */
#define PSTATE_M_EL2t   0x08    /* EL2 Thread  */
#define PSTATE_M_EL2h   0x09    /* EL2 Handler */
#define PSTATE_M_MASK   0x0F    /* Mode mask   */
#define PSTATE_F        0x40    /* FIQ mask    */
#define PSTATE_I        0x80    /* IRQ mask    */
#define PSTATE_A        0x100   /* SError mask */
#define PSTATE_D        0x200   /* Debug mask  */
#define PSTATE_IL       0x100000 /* Illegal execution state */
#define PSTATE_SS       0x200000 /* Software step */
#define PSTATE_V        0x10000000 /* Overflow flag */
#define PSTATE_C        0x20000000 /* Carry flag    */
#define PSTATE_Z        0x40000000 /* Zero flag     */
#define PSTATE_N        0x80000000 /* Negative flag */

HEADER("PSTATE / SPSR exception-level bits"),
CONSTANTX(PSTATE_M_EL0t, PSTATE_M_EL0t),
CONSTANTX(PSTATE_M_EL1t, PSTATE_M_EL1t),
CONSTANTX(PSTATE_M_EL1h, PSTATE_M_EL1h),
CONSTANTX(PSTATE_M_EL2t, PSTATE_M_EL2t),
CONSTANTX(PSTATE_M_EL2h, PSTATE_M_EL2h),
CONSTANTX(PSTATE_M_MASK, PSTATE_M_MASK),

HEADER("PSTATE / SPSR flag bits"),
CONSTANTX(PSTATE_F, PSTATE_F),
CONSTANTX(PSTATE_I, PSTATE_I),
CONSTANTX(PSTATE_A, PSTATE_A),
CONSTANTX(PSTATE_D, PSTATE_D),
CONSTANTX(PSTATE_IL, PSTATE_IL),
CONSTANTX(PSTATE_SS, PSTATE_SS),
CONSTANTX(PSTATE_V, PSTATE_V),
CONSTANTX(PSTATE_C, PSTATE_C),
CONSTANTX(PSTATE_Z, PSTATE_Z),
CONSTANTX(PSTATE_N, PSTATE_N),

/*
 * IRQL levels (from ndk/arm64/ketypes.h)
 */
HEADER("Interrupt request levels"),
CONSTANT(PASSIVE_LEVEL),
CONSTANT(APC_LEVEL),
CONSTANT(DISPATCH_LEVEL),
CONSTANT(CLOCK_LEVEL),
CONSTANT(IPI_LEVEL),
CONSTANT(PROFILE_LEVEL),
CONSTANT(HIGH_LEVEL),

HEADER("Exception active frame type codes"),
CONSTANT(KEXCEPTION_ACTIVE_INTERRUPT_FRAME),
CONSTANT(KEXCEPTION_ACTIVE_EXCEPTION_FRAME),
CONSTANT(KEXCEPTION_ACTIVE_SERVICE_FRAME),

/*
 * ARM64 breakpoint / debug service codes embedded in BRK #imm16 instructions.
 */
#define ARM64_BREAKPOINT        0xF000  /* __debugbreak BRK #0xF000 */
#define ARM64_DEBUG_SERVICE     0xF001  /* __debugservice */
#define ARM64_ASSERT_FAIL       0xF002  /* __assertfail   */
#define ARM64_FAST_FAIL         0xF003  /* __fastfail     */
#define ARM64_DIVIDE_BY_ZERO    0xF004  /* __brkdiv0      */

HEADER("ARM64 BRK immediate codes"),
CONSTANTX(ARM64_BREAKPOINT,     ARM64_BREAKPOINT),
CONSTANTX(ARM64_DEBUG_SERVICE,  ARM64_DEBUG_SERVICE),
CONSTANTX(ARM64_ASSERT_FAIL,    ARM64_ASSERT_FAIL),
CONSTANTX(ARM64_FAST_FAIL,      ARM64_FAST_FAIL),
CONSTANTX(ARM64_DIVIDE_BY_ZERO, ARM64_DIVIDE_BY_ZERO),

/*
 * Trap / exception entry type discriminators stored in KTRAP_FRAME.
 * Mirror the ARM32 TRAP_TYPE_* values for consistency.
 */
#define TRAP_TYPE_INTERRUPT         0x1
#define TRAP_TYPE_SYSCALL           0x2
#define TRAP_TYPE_UNDEFINED         0x3
#define TRAP_TYPE_DATA_ABORT        0x4
#define TRAP_TYPE_PREFETCH_ABORT    0x5
#define TRAP_TYPE_RESET             0x6
#define TRAP_TYPE_FIQ               0x7
#define TRAP_TYPE_IRQ               0x8

HEADER("Trap entry types (KTRAP_FRAME.TrapType)"),
CONSTANTX(TRAP_TYPE_INTERRUPT,      TRAP_TYPE_INTERRUPT),
CONSTANTX(TRAP_TYPE_SYSCALL,        TRAP_TYPE_SYSCALL),
CONSTANTX(TRAP_TYPE_UNDEFINED,      TRAP_TYPE_UNDEFINED),
CONSTANTX(TRAP_TYPE_DATA_ABORT,     TRAP_TYPE_DATA_ABORT),
CONSTANTX(TRAP_TYPE_PREFETCH_ABORT, TRAP_TYPE_PREFETCH_ABORT),
CONSTANTX(TRAP_TYPE_RESET,          TRAP_TYPE_RESET),
CONSTANTX(TRAP_TYPE_FIQ,            TRAP_TYPE_FIQ),
CONSTANTX(TRAP_TYPE_IRQ,            TRAP_TYPE_IRQ),

/* =====================================================================
 * CONTEXT offsets (ARM64)
 * Structure defined in sdk/include/xdk/arm64/ke.h
 * =====================================================================*/
HEADER("CONTEXT offsets"),
OFFSET(CxContextFlags, CONTEXT, ContextFlags),
OFFSET(CxCpsr, CONTEXT, Cpsr),
OFFSET(CxX0, CONTEXT, X0),
OFFSET(CxX1, CONTEXT, X1),
OFFSET(CxX2, CONTEXT, X2),
OFFSET(CxX3, CONTEXT, X3),
OFFSET(CxX4, CONTEXT, X4),
OFFSET(CxX5, CONTEXT, X5),
OFFSET(CxX6, CONTEXT, X6),
OFFSET(CxX7, CONTEXT, X7),
OFFSET(CxX8, CONTEXT, X8),
OFFSET(CxX9, CONTEXT, X9),
OFFSET(CxX10, CONTEXT, X10),
OFFSET(CxX11, CONTEXT, X11),
OFFSET(CxX12, CONTEXT, X12),
OFFSET(CxX13, CONTEXT, X13),
OFFSET(CxX14, CONTEXT, X14),
OFFSET(CxX15, CONTEXT, X15),
OFFSET(CxX16, CONTEXT, X16),
OFFSET(CxX17, CONTEXT, X17),
OFFSET(CxX18, CONTEXT, X18),
OFFSET(CxX19, CONTEXT, X19),
OFFSET(CxX20, CONTEXT, X20),
OFFSET(CxX21, CONTEXT, X21),
OFFSET(CxX22, CONTEXT, X22),
OFFSET(CxX23, CONTEXT, X23),
OFFSET(CxX24, CONTEXT, X24),
OFFSET(CxX25, CONTEXT, X25),
OFFSET(CxX26, CONTEXT, X26),
OFFSET(CxX27, CONTEXT, X27),
OFFSET(CxX28, CONTEXT, X28),
OFFSET(CxFp, CONTEXT, Fp),   /* x29 */
OFFSET(CxLr, CONTEXT, Lr),   /* x30 */
OFFSET(CxSp, CONTEXT, Sp),
OFFSET(CxPc, CONTEXT, Pc),
OFFSET(CxV, CONTEXT, V),
OFFSET(CxFpcr, CONTEXT, Fpcr),
OFFSET(CxFpsr, CONTEXT, Fpsr),
OFFSET(CxBcr, CONTEXT, Bcr),
OFFSET(CxBvr, CONTEXT, Bvr),
OFFSET(CxWcr, CONTEXT, Wcr),
OFFSET(CxWvr, CONTEXT, Wvr),
SIZE(CONTEXT_FRAME_LENGTH, CONTEXT),

/* =====================================================================
 * KARM64_VFP_STATE offsets (ARM64)
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KARM64_VFP_STATE offsets"),
OFFSET(VfLink, KARM64_VFP_STATE, Link),
OFFSET(VfFpcr, KARM64_VFP_STATE, Fpcr),
OFFSET(VfFpsr, KARM64_VFP_STATE, Fpsr),
OFFSET(VfV,    KARM64_VFP_STATE, V),
SIZE(KARM64_VFP_STATE_LENGTH, KARM64_VFP_STATE),

/* =====================================================================
 * KTRAP_FRAME offsets (ARM64)
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KTRAP_FRAME offsets"),
OFFSET(TrExceptionActive,           KTRAP_FRAME, ExceptionActive),
OFFSET(TrContextFromKFramesUnwound, KTRAP_FRAME, ContextFromKFramesUnwound),
OFFSET(TrDebugRegistersValid,       KTRAP_FRAME, DebugRegistersValid),
OFFSET(TrPreviousMode,              KTRAP_FRAME, PreviousMode),
OFFSET(TrPreviousIrql,              KTRAP_FRAME, PreviousIrql),
OFFSET(TrFaultStatus,               KTRAP_FRAME, FaultStatus),
OFFSET(TrFaultAddress,              KTRAP_FRAME, FaultAddress),
OFFSET(TrTrapFrame,                 KTRAP_FRAME, TrapFrame),
OFFSET(TrVfpState,                  KTRAP_FRAME, VfpState),
OFFSET(TrBcr,                       KTRAP_FRAME, Bcr),
OFFSET(TrBvr,                       KTRAP_FRAME, Bvr),
OFFSET(TrWcr,                       KTRAP_FRAME, Wcr),
OFFSET(TrWvr,                       KTRAP_FRAME, Wvr),
OFFSET(TrSpsr,                      KTRAP_FRAME, Spsr),
OFFSET(TrEsr,                       KTRAP_FRAME, Esr),
OFFSET(TrSp,                        KTRAP_FRAME, Sp),
OFFSET(TrX0,                        KTRAP_FRAME, X0),
OFFSET(TrX1,                        KTRAP_FRAME, X1),
OFFSET(TrX2,                        KTRAP_FRAME, X2),
OFFSET(TrX3,                        KTRAP_FRAME, X3),
OFFSET(TrX4,                        KTRAP_FRAME, X4),
OFFSET(TrX5,                        KTRAP_FRAME, X5),
OFFSET(TrX6,                        KTRAP_FRAME, X6),
OFFSET(TrX7,                        KTRAP_FRAME, X7),
OFFSET(TrX8,                        KTRAP_FRAME, X8),
OFFSET(TrX9,                        KTRAP_FRAME, X9),
OFFSET(TrX10,                       KTRAP_FRAME, X10),
OFFSET(TrX11,                       KTRAP_FRAME, X11),
OFFSET(TrX12,                       KTRAP_FRAME, X12),
OFFSET(TrX13,                       KTRAP_FRAME, X13),
OFFSET(TrX14,                       KTRAP_FRAME, X14),
OFFSET(TrX15,                       KTRAP_FRAME, X15),
OFFSET(TrX16,                       KTRAP_FRAME, X16),
OFFSET(TrX17,                       KTRAP_FRAME, X17),
OFFSET(TrX18,                       KTRAP_FRAME, X18),
OFFSET(TrLr,                        KTRAP_FRAME, Lr),
OFFSET(TrFp,                        KTRAP_FRAME, Fp),
OFFSET(TrPc,                        KTRAP_FRAME, Pc),
SIZE(KTRAP_FRAME_LENGTH, KTRAP_FRAME),

/* =====================================================================
 * KEXCEPTION_FRAME offsets (ARM64)
 * Callee-saved integer registers x19-x28, fp (x29), return (x30/lr).
 * Matches Windows 10 ARM64 _KEXCEPTION_FRAME (struct 2904).
 * =====================================================================*/
HEADER("KEXCEPTION_FRAME offsets"),
OFFSET(ExX19,   KEXCEPTION_FRAME, X19),
OFFSET(ExX20,   KEXCEPTION_FRAME, X20),
OFFSET(ExX21,   KEXCEPTION_FRAME, X21),
OFFSET(ExX22,   KEXCEPTION_FRAME, X22),
OFFSET(ExX23,   KEXCEPTION_FRAME, X23),
OFFSET(ExX24,   KEXCEPTION_FRAME, X24),
OFFSET(ExX25,   KEXCEPTION_FRAME, X25),
OFFSET(ExX26,   KEXCEPTION_FRAME, X26),
OFFSET(ExX27,   KEXCEPTION_FRAME, X27),
OFFSET(ExX28,   KEXCEPTION_FRAME, X28),
OFFSET(ExFp,    KEXCEPTION_FRAME, Fp),
OFFSET(ExReturn, KEXCEPTION_FRAME, Return),
SIZE(KEXCEPTION_FRAME_LENGTH, KEXCEPTION_FRAME),

/* =====================================================================
 * KSWITCH_FRAME offsets (ARM64)
 * Matches Windows 10 ARM64 _KSWITCH_FRAME (struct 1282).
 * =====================================================================*/
HEADER("KSWITCH_FRAME offsets"),
OFFSET(SwApcBypass, KSWITCH_FRAME, ApcBypass),
OFFSET(SwTpidr,     KSWITCH_FRAME, Tpidr),
OFFSET(SwFp,        KSWITCH_FRAME, Fp),
OFFSET(SwReturn,    KSWITCH_FRAME, Return),
SIZE(KSWITCH_FRAME_LENGTH, KSWITCH_FRAME),

/* =====================================================================
 * KSPECIAL_REGISTERS offsets (ARM64)
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KSPECIAL_REGISTERS offsets"),
OFFSET(KsElr_El1,       KSPECIAL_REGISTERS, Elr_El1),
OFFSET(KsSpsr_El1,      KSPECIAL_REGISTERS, Spsr_El1),
OFFSET(KsTpidr_El0,     KSPECIAL_REGISTERS, Tpidr_El0),
OFFSET(KsTpidrro_El0,   KSPECIAL_REGISTERS, Tpidrro_El0),
OFFSET(KsTpidr_El1,     KSPECIAL_REGISTERS, Tpidr_El1),
OFFSET(KsKernelBvr,     KSPECIAL_REGISTERS, KernelBvr),
OFFSET(KsKernelBcr,     KSPECIAL_REGISTERS, KernelBcr),
OFFSET(KsKernelWvr,     KSPECIAL_REGISTERS, KernelWvr),
OFFSET(KsKernelWcr,     KSPECIAL_REGISTERS, KernelWcr),
SIZE(KSPECIAL_REGISTERS_LENGTH, KSPECIAL_REGISTERS),

/* =====================================================================
 * KARM64_ARCH_STATE offsets
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KARM64_ARCH_STATE offsets"),
OFFSET(AaMidr_El1,          KARM64_ARCH_STATE, Midr_El1),
OFFSET(AaSctlr_El1,         KARM64_ARCH_STATE, Sctlr_El1),
OFFSET(AaActlr_El1,         KARM64_ARCH_STATE, Actlr_El1),
OFFSET(AaCpacr_El1,         KARM64_ARCH_STATE, Cpacr_El1),
OFFSET(AaTcr_El1,           KARM64_ARCH_STATE, Tcr_El1),
OFFSET(AaTtbr0_El1,         KARM64_ARCH_STATE, Ttbr0_El1),
OFFSET(AaTtbr1_El1,         KARM64_ARCH_STATE, Ttbr1_El1),
OFFSET(AaEsr_El1,           KARM64_ARCH_STATE, Esr_El1),
OFFSET(AaFar_El1,           KARM64_ARCH_STATE, Far_El1),
OFFSET(AaPmcr_El0,          KARM64_ARCH_STATE, Pmcr_El0),
OFFSET(AaPmcntenset_El0,    KARM64_ARCH_STATE, Pmcntenset_El0),
OFFSET(AaPmccntr_El0,       KARM64_ARCH_STATE, Pmccntr_El0),
OFFSET(AaMair_El1,          KARM64_ARCH_STATE, Mair_El1),
OFFSET(AaVbar_El1,          KARM64_ARCH_STATE, Vbar_El1),

/* =====================================================================
 * KPROCESSOR_STATE offsets
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KPROCESSOR_STATE offsets"),
OFFSET(PsSpecialRegisters, KPROCESSOR_STATE, SpecialRegisters),
OFFSET(PsArchState,        KPROCESSOR_STATE, ArchState),
OFFSET(PsContextFrame,     KPROCESSOR_STATE, ContextFrame),
SIZE(ProcessorStateLength, KPROCESSOR_STATE),

/* =====================================================================
 * KIPCR offsets
 * Structure defined in sdk/include/ndk/arm64/ketypes.h
 * =====================================================================*/
HEADER("KIPCR / KPCR offsets"),
OFFSET(PcSelf,              KIPCR, Self),
OFFSET(PcLockArray,         KIPCR, LockArray),
OFFSET(PcTeb,               KIPCR, Used_Self),
OFFSET(PcCurrentIrql,       KIPCR, CurrentIrql),
OFFSET(PcStallScaleFactor,  KIPCR, StallScaleFactor),
OFFSET(PcHalReserved,       KIPCR, HalReserved),
OFFSET(PcKdVersionBlock,    KIPCR, KdVersionBlock),
OFFSET(PcKvaUserModeTtbr1,  KIPCR, KvaUserModeTtbr1),
OFFSET(PcPrcb,              KIPCR, Prcb),
SIZE(ProcessorControlRegisterLength, KIPCR),

/*
 * KPRCB offsets
 * NOTE: KPRCB for ARM64 uses the same shared body from ndk/arm64/ketypes.h.
 * Only the size is emitted here; individual field offsets should be added
 * once the ReactOS ARM64 KPRCB body separates from the x86/x64 stub.
 */
HEADER("KPRCB offsets"),
SIZE(KprcbLength, KPRCB),

/* =====================================================================
 * Miscellaneous ARM64 kernel constants
 * =====================================================================*/
HEADER("ARM64 stack / alignment constants"),
#define ARM64_RED_ZONE_BYTES 0   /* ARM64 ABI has no red zone */
CONSTANTX(ARM64_RED_ZONE_BYTES, ARM64_RED_ZONE_BYTES),

HEADER("DPC stack layout constants"),
#define DpSp 0x8
#define DpPc 0x10
CONSTANTX(DpSp, DpSp),
CONSTANTX(DpPc, DpPc),

HEADER("TRAPFRAME_LOG_ENTRY offsets"),
OFFSET(TlThread,    TRAPFRAME_LOG_ENTRY, Thread),
OFFSET(TlCpuNumber, TRAPFRAME_LOG_ENTRY, CpuNumber),
OFFSET(TlTrapType,  TRAPFRAME_LOG_ENTRY, TrapType),
OFFSET(TlX0,        TRAPFRAME_LOG_ENTRY, X0),
OFFSET(TlX1,        TRAPFRAME_LOG_ENTRY, X1),
OFFSET(TlFp,        TRAPFRAME_LOG_ENTRY, Fp),
OFFSET(TlLr,        TRAPFRAME_LOG_ENTRY, Lr),
OFFSET(TlSp,        TRAPFRAME_LOG_ENTRY, Sp),
OFFSET(TlPc,        TRAPFRAME_LOG_ENTRY, Pc),
OFFSET(TlFar,       TRAPFRAME_LOG_ENTRY, Far),
OFFSET(TlEsr,       TRAPFRAME_LOG_ENTRY, Esr),
SIZE(TrapFrameLogEntryLength, TRAPFRAME_LOG_ENTRY),
