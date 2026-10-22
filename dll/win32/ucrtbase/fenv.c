/*
 * PROJECT:     ReactOS CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     C99 floating point environment
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include <fenv.h>
#include <xmmintrin.h>

/*
 * fenv_t and fexcept_t hold the x87 unit in bits 16-23, SSE in bits 24-31
 * and both units merged in the low word, the same as the Windows CRT.
 * Each unit byte is six flag bits (FE_ order, denormal in bit 5) and the
 * rounding mode in bits 6-7. The low word adds denormal control in bits
 * 10-11, x87 precision in bits 12-13 and x87 infinity control in bit 14.
 */
#define FP_FLAG_BITS        0x3F
#define FP_NATIVE_RC_MASK   0x3
#define FP_CSR_MASK_SHIFT   7
#define FP_CSR_RC_SHIFT     13
#define FP_CSR_DAZ          0x0040
#define FP_CSR_FZ           0x8000
#define FP_X87_RC_SHIFT     10
#define FP_X87_PC_SHIFT     8
#define FP_X87_IC           0x1000
#define FP_X87_FIELDS       0x1F3F
#define FP_X87_SUMMARY      0x8080
#define FP_LOW_RC_SHIFT     8
#define FP_LOW_DN_SHIFT     10
#define FP_LOW_PC_SHIFT     12
#define FP_LOW_IC           0x4000

typedef struct _FP_UNITS
{
    unsigned int Csr;
    unsigned int X87Control;
    unsigned int X87Status;
} FP_UNITS, *PFP_UNITS;

/* Native flag and mask bits run invalid, denormal, zero divide, overflow, underflow, precision */
static const unsigned char FpNativeOrder[6] = { 0x10, 0x20, 0x08, 0x04, 0x02, 0x01 };

static
unsigned int
FpNativeToBits(
    _In_ unsigned int Native)
{
    unsigned int Index, Bits = 0;

    for (Index = 0; Index < ARRAYSIZE(FpNativeOrder); Index++)
    {
        if (Native & (1 << Index))
            Bits |= FpNativeOrder[Index];
    }

    return Bits;
}

static
unsigned int
FpBitsToNative(
    _In_ unsigned int Bits)
{
    unsigned int Index, Native = 0;

    for (Index = 0; Index < ARRAYSIZE(FpNativeOrder); Index++)
    {
        if (Bits & FpNativeOrder[Index])
            Native |= 1 << Index;
    }

    return Native;
}

static
BOOLEAN
FpHasSse(VOID)
{
#if defined(_M_AMD64)
    return TRUE;
#elif defined(_M_IX86)
    static LONG Present = -1;

    if (Present < 0)
        Present = IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE) ? 1 : 0;

    return (BOOLEAN)Present;
#else
    return FALSE;
#endif
}

static
__ATTRIBUTE_SSE__
unsigned int
FpGetCsr(VOID)
{
    return _mm_getcsr();
}

static
__ATTRIBUTE_SSE__
void
FpSetCsr(
    _In_ unsigned int Csr)
{
    _mm_setcsr(Csr);
}

#ifdef _M_IX86

static
unsigned int
FpGetX87Control(VOID)
{
    unsigned short Control;

#ifdef _MSC_VER
    __asm fnstcw Control
#else
    __asm__ __volatile__("fnstcw %0" : "=m" (Control));
#endif

    return Control;
}

static
void
FpSetX87Control(
    _In_ unsigned int Value)
{
    unsigned short Control = (unsigned short)Value;

#ifdef _MSC_VER
    __asm fldcw Control
#else
    __asm__ __volatile__("fldcw %0" : : "m" (Control));
#endif
}

static
unsigned int
FpGetX87Status(VOID)
{
    unsigned short Status;

#ifdef _MSC_VER
    __asm fnstsw Status
#else
    __asm__ __volatile__("fnstsw %0" : "=m" (Status));
#endif

    return Status;
}

/* The status word is only writable through the saved environment */
static
void
FpSetX87Flags(
    _In_ unsigned int Flags)
{
    unsigned long Environment[7];
    unsigned long *Pointer = Environment;

#ifdef _MSC_VER
    __asm
    {
        mov eax, Pointer
        fnstenv [eax]
    }
#else
    __asm__ __volatile__("fnstenv %0" : "=m" (Environment));
#endif

    Environment[1] &= ~(FP_X87_SUMMARY | FP_FLAG_BITS);
    Environment[1] |= Flags;
    if (Flags & ~Environment[0] & FP_FLAG_BITS)
        Environment[1] |= FP_X87_SUMMARY;

#ifdef _MSC_VER
    __asm
    {
        mov eax, Pointer
        fldenv [eax]
    }
#else
    __asm__ __volatile__("fldenv %0" : : "m" (Environment));
#endif
    (void)Pointer;
}

#endif

static
void
FpReadUnits(
    _Out_ PFP_UNITS Units)
{
    Units->Csr = FpHasSse() ? FpGetCsr() : 0;
#ifdef _M_IX86
    Units->X87Control = FpGetX87Control();
    Units->X87Status = FpGetX87Status();
#else
    Units->X87Control = 0;
    Units->X87Status = 0;
#endif
}

static
void
FpWriteUnits(
    _In_ const FP_UNITS *Units,
    _In_ const FP_UNITS *Current)
{
    if (FpHasSse() && Units->Csr != Current->Csr)
        FpSetCsr(Units->Csr);

#ifdef _M_IX86
    if ((Units->X87Status ^ Current->X87Status) & FP_FLAG_BITS)
        FpSetX87Flags(Units->X87Status & FP_FLAG_BITS);
    if (Units->X87Control != Current->X87Control)
        FpSetX87Control(Units->X87Control);
#else
    (void)Current;
#endif
}

/* Spreads FE_ flags over every field of fexcept_t this architecture has */
static
unsigned long
FpExpandFlags(
    _In_ int Flags)
{
    unsigned long Bits = (unsigned long)(Flags & FE_ALL_EXCEPT);

#ifdef _M_IX86
    return Bits | (Bits << 16) | (Bits << 24);
#else
    return Bits | (Bits << 24);
#endif
}

static
unsigned long
FpEncodeControl(
    _In_ const FP_UNITS *Units)
{
    unsigned int Sse = 0, X87 = 0, Merged, Low = 0;
    static const unsigned char PrecisionCode[4] = { 2, 0, 1, 0 };

    if (FpHasSse())
    {
        Sse = FpNativeToBits(Units->Csr >> FP_CSR_MASK_SHIFT) |
              (((Units->Csr >> FP_CSR_RC_SHIFT) & FP_NATIVE_RC_MASK) << 6);

        if ((Units->Csr & FP_CSR_DAZ) && (Units->Csr & FP_CSR_FZ))
            Low |= 1 << FP_LOW_DN_SHIFT;
        else if (Units->Csr & FP_CSR_DAZ)
            Low |= 2 << FP_LOW_DN_SHIFT;
        else if (Units->Csr & FP_CSR_FZ)
            Low |= 3 << FP_LOW_DN_SHIFT;
    }

#ifdef _M_IX86
    X87 = FpNativeToBits(Units->X87Control) |
          (((Units->X87Control >> FP_X87_RC_SHIFT) & FP_NATIVE_RC_MASK) << 6);
    Low |= PrecisionCode[(Units->X87Control >> FP_X87_PC_SHIFT) & 3] << FP_LOW_PC_SHIFT;
    if (Units->X87Control & FP_X87_IC)
        Low |= FP_LOW_IC;
#else
    (void)PrecisionCode;
#endif

    Merged = Sse | X87;
    Low |= (Merged & FP_FLAG_BITS) | ((Merged >> 6) << FP_LOW_RC_SHIFT);

    return Low | ((unsigned long)X87 << 16) | ((unsigned long)Sse << 24);
}

static
unsigned long
FpEncodeStatus(
    _In_ const FP_UNITS *Units)
{
    unsigned int Sse = 0, X87 = 0;

    if (FpHasSse())
        Sse = FpNativeToBits(Units->Csr & FP_FLAG_BITS);
#ifdef _M_IX86
    X87 = FpNativeToBits(Units->X87Status & FP_FLAG_BITS);
#endif

    return (Sse | X87) | ((unsigned long)X87 << 16) | ((unsigned long)Sse << 24);
}

static
void
FpDecodeStatus(
    _In_ unsigned long Status,
    _Inout_ PFP_UNITS Units)
{
    Units->Csr = (Units->Csr & ~FP_FLAG_BITS) | FpBitsToNative((Status >> 24) & FP_FLAG_BITS);
    Units->X87Status = (Units->X87Status & ~FP_FLAG_BITS) | FpBitsToNative((Status >> 16) & FP_FLAG_BITS);
}

static
void
FpDecodeControl(
    _In_ unsigned long Control,
    _Inout_ PFP_UNITS Units)
{
    unsigned int Sse = (Control >> 24) & 0xFF;
    unsigned int X87 = (Control >> 16) & 0xFF;
    static const unsigned short DenormalCsr[4] = { 0, FP_CSR_DAZ | FP_CSR_FZ, FP_CSR_DAZ, FP_CSR_FZ };
    static const unsigned char PrecisionNative[4] = { 3, 2, 0, 3 };
    unsigned int Csr;

    Csr = Units->Csr & FP_FLAG_BITS;
    Csr |= FpBitsToNative(Sse & FP_FLAG_BITS) << FP_CSR_MASK_SHIFT;
    Csr |= (Sse >> 6) << FP_CSR_RC_SHIFT;
    Csr |= DenormalCsr[(Control >> FP_LOW_DN_SHIFT) & 3];
    Units->Csr = Csr;

    Units->X87Control &= ~FP_X87_FIELDS;
    Units->X87Control |= FpBitsToNative(X87 & FP_FLAG_BITS);
    Units->X87Control |= (X87 >> 6) << FP_X87_RC_SHIFT;
    Units->X87Control |= PrecisionNative[(Control >> FP_LOW_PC_SHIFT) & 3] << FP_X87_PC_SHIFT;
    if (Control & FP_LOW_IC)
        Units->X87Control |= FP_X87_IC;
}

int
__cdecl
fegetenv(
    _Out_ fenv_t *env)
{
    FP_UNITS Units;

    FpReadUnits(&Units);
    env->_Fe_ctl = FpEncodeControl(&Units);
    env->_Fe_stat = FpEncodeStatus(&Units);
    return 0;
}

int
__cdecl
fesetenv(
    _In_ const fenv_t *env)
{
    FP_UNITS Current, Units;
    fenv_t Result;

    FpReadUnits(&Current);
    Units = Current;
    FpDecodeControl(env->_Fe_ctl, &Units);
    FpDecodeStatus(env->_Fe_stat, &Units);
    FpWriteUnits(&Units, &Current);

    fegetenv(&Result);
    return (Result._Fe_ctl != env->_Fe_ctl || Result._Fe_stat != env->_Fe_stat) ? 1 : 0;
}

int
__cdecl
fegetround(void)
{
    fenv_t Env;

    fegetenv(&Env);
    return (int)(Env._Fe_ctl & FE_ROUND_MASK);
}

int
__cdecl
fesetround(
    _In_ int round)
{
    unsigned long Code;
    fenv_t Env;

    if (round & ~FE_ROUND_MASK)
        return 1;

    Code = (unsigned long)round >> FP_LOW_RC_SHIFT;
    fegetenv(&Env);
    Env._Fe_ctl &= ~((unsigned long)FE_ROUND_MASK | (0xC0UL << 16) | (0xC0UL << 24));
    Env._Fe_ctl |= (unsigned long)round;
#ifdef _M_IX86
    Env._Fe_ctl |= (Code << 6) << 16;
#endif
    if (FpHasSse())
        Env._Fe_ctl |= (Code << 6) << 24;

    return fesetenv(&Env);
}

int
__cdecl
fetestexcept(
    _In_ int excepts)
{
    fenv_t Env;

    fegetenv(&Env);
    return (int)(Env._Fe_stat & (unsigned long)(excepts & FE_ALL_EXCEPT));
}

int
__cdecl
fegetexceptflag(
    _Out_ fexcept_t *flagp,
    _In_ int excepts)
{
    fenv_t Env;

    fegetenv(&Env);
    *flagp = Env._Fe_stat & FpExpandFlags(excepts);
    return 0;
}

static
unsigned long
FpReplaceStatus(
    _In_ unsigned long Wanted)
{
    FP_UNITS Current, Units;

    FpReadUnits(&Current);
    Units = Current;
    FpDecodeStatus(Wanted, &Units);
    FpWriteUnits(&Units, &Current);

    FpReadUnits(&Units);
    return FpEncodeStatus(&Units);
}

int
__cdecl
fesetexceptflag(
    _In_ const fexcept_t *flagp,
    _In_ int excepts)
{
    unsigned long Mask = FpExpandFlags(excepts);
    unsigned long Wanted;
    fenv_t Env;

    if (!Mask)
        return 0;

    fegetenv(&Env);
    Wanted = (Env._Fe_stat & ~Mask) | (*flagp & Mask);
    return (Wanted & ~FpReplaceStatus(Wanted) & Mask) ? 1 : 0;
}

int
__cdecl
feclearexcept(
    _In_ int excepts)
{
    unsigned long Mask = FpExpandFlags(excepts);
    fenv_t Env;

    if (!Mask)
        return 0;

    fegetenv(&Env);
    return (FpReplaceStatus(Env._Fe_stat & ~Mask) & Mask) ? 1 : 0;
}

/* Traps stay as they are; a caller with unmasked exceptions gets a failure back */
int
__cdecl
feholdexcept(
    _Out_ fenv_t *env)
{
    fegetenv(env);
    feclearexcept(FE_ALL_EXCEPT);
    return ((env->_Fe_ctl & FE_ALL_EXCEPT) != FE_ALL_EXCEPT) ? 1 : 0;
}
