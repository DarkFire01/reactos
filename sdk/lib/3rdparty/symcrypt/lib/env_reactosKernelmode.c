/*
 * PROJECT:     ReactOS Cryptographic Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SymCrypt environment for kernel mode
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The library carries its own environment, so a driver that links it must not
 * declare one of its own. It only has to call SymCryptInit() once before using
 * any algorithm.
 *
 * The rest of the library is built from the same headers as the user mode
 * build, so the handful of kernel routines used here are declared locally
 * rather than pulling the driver headers into this translation unit.
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

/* The kernel routines this environment stands on */
DECLSPEC_NORETURN
VOID
NTAPI
KeBugCheckEx(
    _In_ ULONG BugCheckCode,
    _In_ ULONG_PTR Parameter1,
    _In_ ULONG_PTR Parameter2,
    _In_ ULONG_PTR Parameter3,
    _In_ ULONG_PTR Parameter4);

#if SYMCRYPT_CPU_X86

typedef struct _KFLOATING_SAVE
{
    ULONG ControlWord;
    ULONG StatusWord;
    ULONG ErrorOffset;
    ULONG ErrorSelector;
    ULONG DataOffset;
    ULONG DataSelector;
    ULONG Cr0NpxState;
    ULONG Spare1;
} KFLOATING_SAVE, *PKFLOATING_SAVE;

LONG
NTAPI
KeSaveFloatingPointState(
    _Out_ PKFLOATING_SAVE FloatSave);

LONG
NTAPI
KeRestoreFloatingPointState(
    _In_ PKFLOATING_SAVE FloatSave);

C_ASSERT(sizeof(KFLOATING_SAVE) <= SYMCRYPT_XSTATE_SAVE_SIZE);

#endif

/* SECURITY_SYSTEM, the bug check the security subsystem stops the machine with */
#define SYMCRYPT_FATAL_BUG_CHECK_CODE 0x29

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Reports the features that are locked out whatever the processor offers.
 *
 * @remarks
 * Saving the upper halves of the Ymm registers needs
 * KeSaveExtendedProcessorState(), which the kernel does not have, so
 * everything that runs on them stays out of reach. Ruling the features out
 * here lets the compiler drop that code entirely.
 */
SYMCRYPT_CPU_FEATURES
SYMCRYPT_CALL
SymCryptCpuFeaturesNeverPresentEnvWindowsKernelmodeWin8_1nLater(void)
{
#if SYMCRYPT_CPU_X86 || SYMCRYPT_CPU_AMD64
    return SYMCRYPT_CPU_FEATURE_AVX2 |
           SYMCRYPT_CPU_FEATURE_AVX512 |
           SYMCRYPT_CPU_FEATURE_VAES;
#else
    return 0;
#endif
}

/**
 * @brief
 * Brings the library up for kernel mode use.
 */
VOID
SYMCRYPT_CALL
SymCryptInitEnvWindowsKernelmodeWin8_1nLater(
    UINT32 Version)
{
    if (g_SymCryptFlags & SYMCRYPT_FLAG_LIB_INITIALIZED)
        return;

#if SYMCRYPT_CPU_X86 || SYMCRYPT_CPU_AMD64
    /*
     * Ask the processor what it has. The Ymm registers are never used here, so
     * there is no reason to go looking for operating system support for them.
     */
    SymCryptDetectCpuFeaturesByCpuid(0);

#if SYMCRYPT_CPU_AMD64
    /*
     * The Xmm registers are saved over a context switch and over an interrupt
     * on this architecture, so a driver may use them as they are and the save
     * below cannot fail.
     */
    g_SymCryptCpuFeaturesNotPresent &= ~SYMCRYPT_CPU_FEATURE_SAVEXMM_NOFAIL;
#endif

#elif SYMCRYPT_CPU_ARM

    g_SymCryptCpuFeaturesNotPresent = (SYMCRYPT_CPU_FEATURES)~SYMCRYPT_CPU_FEATURE_NEON;

#endif

    SymCryptInitEnvCommon(Version);
}

/**
 * @brief
 * Stops the machine when the library finds itself in a state it cannot go on
 * from.
 */
_Analysis_noreturn_
VOID
SYMCRYPT_CALL
SymCryptFatalEnvWindowsKernelmodeWin8_1nLater(
    UINT32 FatalCode)
{
    SymCryptFatalIntercept(FatalCode);

    KeBugCheckEx(SYMCRYPT_FATAL_BUG_CHECK_CODE, FatalCode, 0, 0, 0);
}

#if SYMCRYPT_CPU_X86 || SYMCRYPT_CPU_AMD64

/**
 * @brief
 * Takes the Xmm registers for the calling thread.
 */
SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveXmmEnvWindowsKernelmodeWin8_1nLater(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveArea)
{
#if SYMCRYPT_CPU_AMD64
    UNREFERENCED_PARAMETER(SaveArea);

    return SYMCRYPT_NO_ERROR;
#else
    if (KeSaveFloatingPointState((PKFLOATING_SAVE)&SaveArea->data[0]) < 0)
        return SYMCRYPT_EXTERNAL_FAILURE;

    return SYMCRYPT_NO_ERROR;
#endif
}

/**
 * @brief
 * Hands the Xmm registers back.
 */
VOID
SYMCRYPT_CALL
SymCryptRestoreXmmEnvWindowsKernelmodeWin8_1nLater(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveArea)
{
#if SYMCRYPT_CPU_AMD64
    UNREFERENCED_PARAMETER(SaveArea);
#else
    KeRestoreFloatingPointState((PKFLOATING_SAVE)&SaveArea->data[0]);
#endif
}

/**
 * @brief
 * Refuses the Ymm registers, which cannot be saved here.
 */
SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveYmmEnvWindowsKernelmodeWin8_1nLater(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveArea)
{
    UNREFERENCED_PARAMETER(SaveArea);

    return SYMCRYPT_EXTERNAL_FAILURE;
}

/**
 * @brief
 * Counterpart of the Ymm save, which never succeeds.
 */
VOID
SYMCRYPT_CALL
SymCryptRestoreYmmEnvWindowsKernelmodeWin8_1nLater(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveArea)
{
    UNREFERENCED_PARAMETER(SaveArea);
}

#endif

/**
 * @brief
 * Hook the test code uses to corrupt a buffer. It does nothing in a shipping
 * build.
 */
VOID
SYMCRYPT_CALL
SymCryptTestInjectErrorEnvWindowsKernelmodeWin8_1nLater(
    PBYTE Buffer,
    SIZE_T Length)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
}

#if SYMCRYPT_CPU_X86 || SYMCRYPT_CPU_AMD64

/**
 * @brief
 * Reads a processor identification leaf.
 */
VOID
SYMCRYPT_CALL
SymCryptCpuidExFuncEnvWindowsKernelmodeWin8_1nLater(
    int CpuInfo[4],
    int FunctionId,
    int SubFunctionId)
{
    __cpuidex(CpuInfo, FunctionId, SubFunctionId);
}

#endif

/*
 * Emit the forwarding stubs the library calls into. The environment macro is
 * spelled out because the one in the header is gated on a newer NTDDI than the
 * drivers here are built with.
 */
SYMCRYPT_ENVIRONMENT_DEFS(WindowsKernelmodeWin8_1nLater);

/* EOF */
