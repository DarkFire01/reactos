$if (_WDMDDK_)
/** Kernel definitions for ARM64 **/

/* Interrupt request levels */
#define PASSIVE_LEVEL           0
#define LOW_LEVEL               0
#define APC_LEVEL               1
#define DISPATCH_LEVEL          2
#define CLOCK_LEVEL             13
#define IPI_LEVEL               14
#define DRS_LEVEL               14
#define POWER_LEVEL             14
#define PROFILE_LEVEL           15
#define HIGH_LEVEL              15

#define KIP0PCRADDRESS          0xFFDFF000
#define KI_USER_SHARED_DATA     0xFFFF9000
#define SharedUserData          ((KUSER_SHARED_DATA * const)KI_USER_SHARED_DATA)

#define PAGE_SIZE               0x1000
#define PAGE_SHIFT              12L

NTSYSAPI
PKTHREAD
NTAPI
KeGetCurrentThread(VOID);

$endif (_WDMDDK_)
$if (_NTDDK_)

#define ARM64_MAX_BREAKPOINTS 8
#define ARM64_MAX_WATCHPOINTS 2

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
typedef NEON128 NEON128, *PNEON128;

typedef struct _CONTEXT {

    //
    // Control flags.
    //

    DWORD ContextFlags;

    //
    // Integer registers
    //

    DWORD Cpsr;
    union {
        struct {
            DWORD64 X0;
            DWORD64 X1;
            DWORD64 X2;
            DWORD64 X3;
            DWORD64 X4;
            DWORD64 X5;
            DWORD64 X6;
            DWORD64 X7;
            DWORD64 X8;
            DWORD64 X9;
            DWORD64 X10;
            DWORD64 X11;
            DWORD64 X12;
            DWORD64 X13;
            DWORD64 X14;
            DWORD64 X15;
            DWORD64 X16;
            DWORD64 X17;
            DWORD64 X18;
            DWORD64 X19;
            DWORD64 X20;
            DWORD64 X21;
            DWORD64 X22;
            DWORD64 X23;
            DWORD64 X24;
            DWORD64 X25;
            DWORD64 X26;
            DWORD64 X27;
            DWORD64 X28;
    		DWORD64 Fp;
            DWORD64 Lr;
        } DUMMYSTRUCTNAME;
        DWORD64 X[31];
    } DUMMYUNIONNAME;

    DWORD64 Sp;
    DWORD64 Pc;

    //
    // Floating Point/NEON Registers
    //

    NEON128 V[32];
    DWORD Fpcr;
    DWORD Fpsr;

    //
    // Debug registers
    //

    DWORD Bcr[ARM64_MAX_BREAKPOINTS];
    DWORD64 Bvr[ARM64_MAX_BREAKPOINTS];
    DWORD Wcr[ARM64_MAX_WATCHPOINTS];
    DWORD64 Wvr[ARM64_MAX_WATCHPOINTS];

} _CONTEXT, *P_CONTEXT;
typedef _CONTEXT CONTEXT, *PCONTEXT;;
$endif