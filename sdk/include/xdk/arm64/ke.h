$if (_WDMDDK_)
/** Kernel definitions for ARM64 **/

/* Exported kernel variables */
extern NTKERNELAPI volatile KSYSTEM_TIME KeTickCount;

/* Interrupt request levels */
#define PASSIVE_LEVEL           0
#define LOW_LEVEL               0
#define APC_LEVEL               1
#define DISPATCH_LEVEL          2
#define CMCI_LEVEL              5
#define CLOCK_LEVEL             13
#define IPI_LEVEL               14
#define DRS_LEVEL               14
#define POWER_LEVEL             14
#define PROFILE_LEVEL           15
#define HIGH_LEVEL              15

/* ARM64 shares the AMD64/x64 user-shared-data virtual address */
#define KI_USER_SHARED_DATA     0xFFFFF78000000000ULL
#define SharedUserData          ((KUSER_SHARED_DATA * const)KI_USER_SHARED_DATA)
#define SharedInterruptTime     (KI_USER_SHARED_DATA + 0x8)
#define SharedSystemTime        (KI_USER_SHARED_DATA + 0x14)
#define SharedTickCount         (KI_USER_SHARED_DATA + 0x320)

#define PAGE_SIZE               0x1000
#define PAGE_SHIFT              12L

/* Dummy save area — ARM64 hardware saves/restores FP state automatically */
typedef struct _KFLOATING_SAVE
{
    ULONG Reserved;
} KFLOATING_SAVE, *PKFLOATING_SAVE;

#define KeQueryInterruptTime() \
    (*(volatile ULONG64*)SharedInterruptTime)

#define KeQuerySystemTime(CurrentCount) \
    *(ULONG64*)(CurrentCount) = *(volatile ULONG64*)SharedSystemTime

#define KeQueryTickCount(CurrentCount) \
    *(ULONG64*)(CurrentCount) = *(volatile ULONG64*)SharedTickCount

/* ARM64 is cache-coherent for normal memory; cache fill-size hint unused */
#define KeGetDcacheFillSize() 1L

FORCEINLINE
VOID
YieldProcessor(
    VOID)
{
    __dmb(_ARM64_BARRIER_ISHST);
    __yield();
}

#define MemoryBarrier()               __dmb(_ARM64_BARRIER_SY)

FORCEINLINE
VOID
KeMemoryBarrier(
    VOID)
{
    _ReadWriteBarrier();
    MemoryBarrier();
}

#define KeMemoryBarrierWithoutFence() _ReadWriteBarrier()

_IRQL_requires_max_(HIGH_LEVEL)
NTHALAPI
VOID
FASTCALL
KfLowerIrql(
    _In_ _IRQL_restores_ _Notliteral_ KIRQL NewIrql);
#define KeLowerIrql(a) KfLowerIrql(a)

_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_raises_(NewIrql)
_IRQL_saves_
NTHALAPI
KIRQL
FASTCALL
KfRaiseIrql(
    _In_ KIRQL NewIrql);
#define KeRaiseIrql(a,b) *(b) = KfRaiseIrql(a)

_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_saves_
_IRQL_raises_(DISPATCH_LEVEL)
NTHALAPI
KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID);

NTHALAPI
KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID);

_Requires_lock_not_held_(*SpinLock)
_Acquires_lock_(*SpinLock)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_saves_
_IRQL_raises_(DISPATCH_LEVEL)
NTHALAPI
KIRQL
FASTCALL
KfAcquireSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock);
#define KeAcquireSpinLock(a,b) *(b) = KfAcquireSpinLock(a)

_Requires_lock_held_(*SpinLock)
_Releases_lock_(*SpinLock)
_IRQL_requires_(DISPATCH_LEVEL)
NTHALAPI
VOID
FASTCALL
KfReleaseSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ _IRQL_restores_ KIRQL NewIrql);
#define KeReleaseSpinLock(a,b) KfReleaseSpinLock(a,b)

_Requires_lock_not_held_(*SpinLock)
_Acquires_lock_(*SpinLock)
_IRQL_requires_min_(DISPATCH_LEVEL)
NTKERNELAPI
VOID
FASTCALL
KefAcquireSpinLockAtDpcLevel(
    _Inout_ PKSPIN_LOCK SpinLock);
#define KeAcquireSpinLockAtDpcLevel(SpinLock) KefAcquireSpinLockAtDpcLevel(SpinLock)

_Requires_lock_held_(*SpinLock)
_Releases_lock_(*SpinLock)
_IRQL_requires_min_(DISPATCH_LEVEL)
NTKERNELAPI
VOID
FASTCALL
KefReleaseSpinLockFromDpcLevel(
    _Inout_ PKSPIN_LOCK SpinLock);
#define KeReleaseSpinLockFromDpcLevel(SpinLock) KefReleaseSpinLockFromDpcLevel(SpinLock)

NTSYSAPI
PKTHREAD
NTAPI
KeGetCurrentThread(VOID);

_Always_(_Post_satisfies_(return<=0))
_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
_Kernel_float_saved_
_At_(*FloatSave, _Kernel_requires_resource_not_held_(FloatState) _Kernel_acquires_resource_(FloatState))
FORCEINLINE
NTSTATUS
KeSaveFloatingPointState(
    _Out_ PKFLOATING_SAVE FloatSave)
{
    UNREFERENCED_PARAMETER(FloatSave);
    return STATUS_SUCCESS;
}

_Success_(1)
_Kernel_float_restored_
_At_(*FloatSave, _Kernel_requires_resource_held_(FloatState) _Kernel_releases_resource_(FloatState))
FORCEINLINE
NTSTATUS
KeRestoreFloatingPointState(
    _In_ PKFLOATING_SAVE FloatSave)
{
    UNREFERENCED_PARAMETER(FloatSave);
    return STATUS_SUCCESS;
}

/* ARM64 normal-memory DMA is coherent; no explicit buffer flush required */
#define KeFlushIoBuffers(_Mdl, _ReadOperation, _DmaOperation)

#define DbgRaiseAssertionFailure() __break(0xf001)

#if (NTDDI_VERSION >= NTDDI_WIN7)
NTSYSAPI
ULONG
NTAPI
KeGetCurrentProcessorIndex(VOID);
#endif

_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
NTHALAPI
KIRQL
NTAPI
KeGetCurrentIrql(VOID);

#if (NTDDI_VERSION < NTDDI_WIN7) || !defined(NT_PROCESSOR_GROUPS)
#if (NTDDI_VERSION >= NTDDI_WIN7)
_CRT_DEPRECATE_TEXT("KeGetCurrentProcessorNumber is deprecated. " \
    "Use KeGetCurrentProcessorNumberEx or KeGetCurrentProcessorIndex instead.")
#endif
NTSYSAPI
ULONG
NTAPI
KeGetCurrentProcessorNumber(VOID);
#endif /* (NTDDI_VERSION < NTDDI_WIN7) || !defined(NT_PROCESSOR_GROUPS) */

$endif /* _WDMDDK_ */
$if (_NTDDK_)

#define PAUSE_PROCESSOR __yield();

#define KERNEL_STACK_SIZE                   0x6000
#define KERNEL_LARGE_STACK_SIZE             0x12000
#define KERNEL_LARGE_STACK_COMMIT           KERNEL_STACK_SIZE

#define KERNEL_MCA_EXCEPTION_STACK_SIZE     0x2000

#define EXCEPTION_READ_FAULT    0
#define EXCEPTION_WRITE_FAULT   1
#define EXCEPTION_EXECUTE_FAULT 8

#define ARM64_MAX_BREAKPOINTS 8
#define ARM64_MAX_WATCHPOINTS 2

#if !defined(RC_INVOKED)

#define CONTEXT_ARM64           0x00400000L
#define CONTEXT_CONTROL         (CONTEXT_ARM64 | 0x1L)
#define CONTEXT_INTEGER         (CONTEXT_ARM64 | 0x2L)
#define CONTEXT_FLOATING_POINT  (CONTEXT_ARM64 | 0x4L)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_ARM64 | 0x8L)
#define CONTEXT_X18             (CONTEXT_ARM64 | 0x10L)
#define CONTEXT_FULL            (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL             (CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS | CONTEXT_X18)

#define CONTEXT_EXCEPTION_ACTIVE    0x08000000L
#define CONTEXT_SERVICE_ACTIVE      0x10000000L
#define CONTEXT_EXCEPTION_REQUEST   0x40000000L
#define CONTEXT_EXCEPTION_REPORTING 0x80000000L

#endif /* !defined(RC_INVOKED) */

#define INITIAL_FPCR    0x00000000UL

typedef union NEON128 {
    struct {
        ULONGLONG Low;
        LONGLONG High;
    } DUMMYSTRUCTNAME;
    double D[2];
    float  S[4];
    USHORT H[8];
    UCHAR  B[16];
} NEON128, *PNEON128;

typedef struct _CONTEXT {

    //
    // Control flags.
    //

    ULONG ContextFlags;

    //
    // Integer registers
    //

    ULONG Cpsr;
    union {
        struct {
            ULONG64 X0;
            ULONG64 X1;
            ULONG64 X2;
            ULONG64 X3;
            ULONG64 X4;
            ULONG64 X5;
            ULONG64 X6;
            ULONG64 X7;
            ULONG64 X8;
            ULONG64 X9;
            ULONG64 X10;
            ULONG64 X11;
            ULONG64 X12;
            ULONG64 X13;
            ULONG64 X14;
            ULONG64 X15;
            ULONG64 X16;
            ULONG64 X17;
            ULONG64 X18;
            ULONG64 X19;
            ULONG64 X20;
            ULONG64 X21;
            ULONG64 X22;
            ULONG64 X23;
            ULONG64 X24;
            ULONG64 X25;
            ULONG64 X26;
            ULONG64 X27;
            ULONG64 X28;
            ULONG64 Fp;
            ULONG64 Lr;
        } DUMMYSTRUCTNAME;
        ULONG64 X[31];
    } DUMMYUNIONNAME;

    ULONG64 Sp;
    ULONG64 Pc;

    //
    // Floating Point/NEON Registers
    //

    NEON128 V[32];
    ULONG Fpcr;
    ULONG Fpsr;

    //
    // Debug registers
    //

    ULONG Bcr[ARM64_MAX_BREAKPOINTS];
    ULONG64 Bvr[ARM64_MAX_BREAKPOINTS];
    ULONG Wcr[ARM64_MAX_WATCHPOINTS];
    ULONG64 Wvr[ARM64_MAX_WATCHPOINTS];

} CONTEXT, *PCONTEXT;

#define PCR_MINOR_VERSION 1
#define PCR_MAJOR_VERSION 1

/*
 * Public (driver-visible) subset of the per-processor control region.
 * Private members (Idt, IdtExt, PcrAlign, Prcb) are NOT exposed here.
 */
typedef struct _KPCR
{
    _ANONYMOUS_UNION union
    {
        NT_TIB NtTib;
        _ANONYMOUS_STRUCT struct
        {
            PVOID TibPad0[2];                   /* +0  (16 B) */
            PVOID Spare1;                        /* +16 ( 8 B) */
            struct _KPCR *Self;                  /* +24 ( 8 B) */
            PVOID PcrReserved0;                  /* +32 ( 8 B) */
            PKSPIN_LOCK_QUEUE LockArray;         /* +40 ( 8 B) */
            PVOID Used_Self;                     /* +48 ( 8 B) */
        };                                       /* = 56 B     */
    };
    KIRQL   CurrentIrql;                         /* +56 */
    UCHAR   SecondLevelCacheAssociativity;       /* +57 */
    UCHAR   Pad1[2];                             /* +58 */
    USHORT  MajorVersion;                        /* +60 */
    USHORT  MinorVersion;                        /* +62 */
    ULONG   StallScaleFactor;                    /* +64 */
    ULONG   SecondLevelCacheSize;                /* +68 */
    _ANONYMOUS_UNION union
    {
        USHORT SoftwareInterruptPending;         /* +72 */
        _ANONYMOUS_STRUCT struct
        {
            UCHAR ApcInterrupt;                  /* +72 */
            UCHAR DispatchInterrupt;             /* +73 */
        };
    };
    USHORT  InterruptPad;                        /* +74 */
    UCHAR   BtiMitigation;                       /* +76 */
    _ANONYMOUS_STRUCT struct
    {
        UCHAR SsbMitigationFirmware : 1;         /* +77 */
        UCHAR SsbMitigationDynamic  : 1;
        UCHAR SsbMitigationKernel   : 1;
        UCHAR SsbMitigationUser     : 1;
        UCHAR SsbMitigationReserved : 4;
    };
    UCHAR   Pad2[2];                             /* +78 */
    ULONG64 PanicStorage[6];                     /* +80 */
    PVOID   KdVersionBlock;                      /* +128 */
    PVOID   HalReserved[15];                     /* +136 */
} KPCR, *PKPCR;

/*
 * KeGetPcr — read the current processor's PCR.
 * On ARM64 the kernel stores the PCR base in TPIDR_EL1.
 */

/* ARM64_SYSREG encodes a system register as an immediate for MRS/MSR.
 * Defined by MSVC <intrin.h>; provide a fallback for other toolchains. */
#ifndef ARM64_SYSREG
#define ARM64_SYSREG(op0, op1, crn, crm, op2) \
    ( (((op0) & 1) << 14) | (((op1) & 7) << 11) | (((crn) & 15) << 7) | \
      (((crm) & 15) << 3) | ((op2) & 7) )
#endif

FORCEINLINE
PKPCR
KeGetPcr(VOID)
{
#if defined(_MSC_VER)
    return (PKPCR)_ReadStatusReg(ARM64_SYSREG(3, 0, 13, 0, 4));
#else
    PKPCR Pcr;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r" (Pcr));
    return Pcr;
#endif
}

_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
FORCEINLINE
KIRQL
KeGetCurrentIrql(
    VOID)
{
    return KeGetPcr()->CurrentIrql;
}

#if (NTDDI_VERSION < NTDDI_WIN7) || !defined(NT_PROCESSOR_GROUPS)
#if (NTDDI_VERSION >= NTDDI_WIN7)
_CRT_DEPRECATE_TEXT("KeGetCurrentProcessorNumber is deprecated. " \
    "Use KeGetCurrentProcessorNumberEx or KeGetCurrentProcessorIndex instead.")
#endif
NTSYSAPI
ULONG
NTAPI
KeGetCurrentProcessorNumber(VOID);
#endif /* (NTDDI_VERSION < NTDDI_WIN7) || !defined(NT_PROCESSOR_GROUPS) */

$endif /* _NTDDK_ */
