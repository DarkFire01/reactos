/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel mode environment and callbacks for the SymCrypt library
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"
#include <symcrypt.h>
#include <sc_lib.h>

#include <debug.h>

/* DEFINES ********************************************************************/

#define TAG_KSEC_SYMCRYPT       'yScK'

/* Bug check Windows raises when its crypto library hits an internal error */
#define KSEC_CRYPTO_LIBRARY_BUGCHECK    0x00000171

typedef struct _KSEC_SYMCRYPT_MUTEX
{
    KSPIN_LOCK Lock;
    KIRQL OldIrql;
} KSEC_SYMCRYPT_MUTEX, *PKSEC_SYMCRYPT_MUTEX;

#ifdef _M_IX86
C_ASSERT(sizeof(KFLOATING_SAVE) <= SYMCRYPT_XSTATE_SAVE_SIZE);
#endif

SYMCRYPT_ENVIRONMENT_DEFS(KsecKernel);

/* ENVIRONMENT ****************************************************************/

SYMCRYPT_CPU_FEATURES
SYMCRYPT_CALL
SymCryptCpuFeaturesNeverPresentEnvKsecKernel(void)
{
    return 0;
}

/*
 * Nothing in the kernel saves the Ymm or Zmm halves for us, so the AVX code
 * paths stay off. On x64 the Xmm registers are already safe to use.
 */
VOID
SYMCRYPT_CALL
SymCryptInitEnvKsecKernel(
    _In_ UINT32 version)
{
    if (g_SymCryptFlags & SYMCRYPT_FLAG_LIB_INITIALIZED)
        return;

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
    SymCryptDetectCpuFeaturesByCpuid(0);
    g_SymCryptCpuFeaturesNotPresent |= SYMCRYPT_CPU_FEATURE_AVX2 | SYMCRYPT_CPU_FEATURE_AVX512 |
                                       SYMCRYPT_CPU_FEATURE_VAES;
#if SYMCRYPT_CPU_AMD64
    g_SymCryptCpuFeaturesNotPresent &= ~SYMCRYPT_CPU_FEATURE_SAVEXMM_NOFAIL;
#else
    g_SymCryptCpuFeaturesNotPresent |= SYMCRYPT_CPU_FEATURE_SAVEXMM_NOFAIL;
#endif
#endif

    SymCryptInitEnvCommon(version);
}

_Analysis_noreturn_
VOID
SYMCRYPT_CALL
SymCryptFatalEnvKsecKernel(
    _In_ UINT32 fatalCode)
{
    SymCryptFatalIntercept(fatalCode);
    KeBugCheckEx(KSEC_CRYPTO_LIBRARY_BUGCHECK, fatalCode, 0, 0, 0);
}

#if SYMCRYPT_CPU_AMD64 | SYMCRYPT_CPU_X86

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveXmmEnvKsecKernel(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea)
{
#ifdef _M_IX86
    if (!NT_SUCCESS(KeSaveFloatingPointState((PKFLOATING_SAVE)pSaveArea->data)))
        return SYMCRYPT_EXTERNAL_FAILURE;
#else
    UNREFERENCED_PARAMETER(pSaveArea);
#endif

    return SYMCRYPT_NO_ERROR;
}

VOID
SYMCRYPT_CALL
SymCryptRestoreXmmEnvKsecKernel(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea)
{
#ifdef _M_IX86
    KeRestoreFloatingPointState((PKFLOATING_SAVE)pSaveArea->data);
#else
    UNREFERENCED_PARAMETER(pSaveArea);
#endif
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveYmmEnvKsecKernel(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea)
{
    UNREFERENCED_PARAMETER(pSaveArea);

    return SYMCRYPT_EXTERNAL_FAILURE;
}

VOID
SYMCRYPT_CALL
SymCryptRestoreYmmEnvKsecKernel(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea)
{
    UNREFERENCED_PARAMETER(pSaveArea);
}

VOID
SYMCRYPT_CALL
SymCryptCpuidExFuncEnvKsecKernel(
    int cpuInfo[4],
    int function_id,
    int subfunction_id)
{
    __cpuidex(cpuInfo, function_id, subfunction_id);
}

#endif

VOID
SYMCRYPT_CALL
SymCryptTestInjectErrorEnvKsecKernel(
    PBYTE pbBuf,
    SIZE_T cbBuf)
{
    UNREFERENCED_PARAMETER(pbBuf);
    UNREFERENCED_PARAMETER(cbBuf);
}

/* CALLBACKS ******************************************************************/

/* Pool blocks are not aligned enough for the big number code, so the real start sits just before */
PVOID
SYMCRYPT_CALL
SymCryptCallbackAlloc(
    SIZE_T nBytes)
{
    PUCHAR Block, Aligned;

    Block = ExAllocatePoolWithTag(NonPagedPool,
                                  nBytes + SYMCRYPT_ASYM_ALIGN_VALUE + sizeof(PVOID),
                                  TAG_KSEC_SYMCRYPT);
    if (Block == NULL)
        return NULL;

    Aligned = (PUCHAR)ALIGN_UP_POINTER_BY(Block + sizeof(PVOID), SYMCRYPT_ASYM_ALIGN_VALUE);
    ((PVOID *)Aligned)[-1] = Block;

    return Aligned;
}

VOID
SYMCRYPT_CALL
SymCryptCallbackFree(
    PVOID pMem)
{
    if (pMem != NULL)
        ExFreePoolWithTag(((PVOID *)pMem)[-1], TAG_KSEC_SYMCRYPT);
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptCallbackRandom(
    PBYTE pbBuffer,
    SIZE_T cbBuffer)
{
    return NT_SUCCESS(KsecGenerateRandom(pbBuffer, cbBuffer)) ? SYMCRYPT_NO_ERROR : SYMCRYPT_EXTERNAL_FAILURE;
}

PVOID
SYMCRYPT_CALL
SymCryptCallbackAllocateMutexFastInproc(void)
{
    PKSEC_SYMCRYPT_MUTEX Mutex;

    Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Mutex), TAG_KSEC_SYMCRYPT);
    if (Mutex != NULL)
        KeInitializeSpinLock(&Mutex->Lock);

    return Mutex;
}

VOID
SYMCRYPT_CALL
SymCryptCallbackFreeMutexFastInproc(
    PVOID pMutex)
{
    ExFreePoolWithTag(pMutex, TAG_KSEC_SYMCRYPT);
}

VOID
SYMCRYPT_CALL
SymCryptCallbackAcquireMutexFastInproc(
    PVOID pMutex)
{
    PKSEC_SYMCRYPT_MUTEX Mutex = pMutex;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Mutex->Lock, &OldIrql);
    Mutex->OldIrql = OldIrql;
}

VOID
SYMCRYPT_CALL
SymCryptCallbackReleaseMutexFastInproc(
    PVOID pMutex)
{
    PKSEC_SYMCRYPT_MUTEX Mutex = pMutex;

    KeReleaseSpinLock(&Mutex->Lock, Mutex->OldIrql);
}

/* FIPS HOOKS *****************************************************************/

/* ReactOS does not run the FIPS self tests or pairwise consistency tests */
UINT32 g_SymCryptFipsSelftestsPerformed;

VOID
SYMCRYPT_CALL
SymCryptRsaSelftest(void)
{
}

VOID
SYMCRYPT_CALL
SymCryptEcDsaSelftest(void)
{
}

VOID
SYMCRYPT_CALL
SymCryptEcDhSecretAgreementSelftest(void)
{
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptRsaSignVerifyPct(
    PCSYMCRYPT_RSAKEY pkRsakey)
{
    UNREFERENCED_PARAMETER(pkRsakey);

    return SYMCRYPT_NO_ERROR;
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptEcDsaPct(
    PCSYMCRYPT_ECKEY pkEckey)
{
    UNREFERENCED_PARAMETER(pkEckey);

    return SYMCRYPT_NO_ERROR;
}
