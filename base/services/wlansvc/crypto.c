/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Cryptographic primitives the supplicant uses, on SymCrypt
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <ntsecapi.h>
#include <symcrypt.h>

#include "wlcrypto.h"

SYMCRYPT_ENVIRONMENT_DEFS(WindowsUsermodeWin8_1nLater);

UINT32 g_SymCryptFipsSelftestsPerformed = 0;

/* The callbacks SymCrypt expects from the program linking it */

PVOID
SYMCRYPT_CALL
SymCryptCallbackAlloc(
    _In_ SIZE_T nBytes)
{
    return _aligned_malloc(nBytes, SYMCRYPT_ASYM_ALIGN_VALUE);
}

VOID
SYMCRYPT_CALL
SymCryptCallbackFree(
    _In_ PVOID pMem)
{
    _aligned_free(pMem);
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptCallbackRandom(
    _Out_writes_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ SIZE_T cbBuffer)
{
    if (!RtlGenRandom(pbBuffer, (ULONG)cbBuffer))
        return SYMCRYPT_EXTERNAL_FAILURE;

    return SYMCRYPT_NO_ERROR;
}

PVOID
SYMCRYPT_CALL
SymCryptCallbackAllocateMutexFastInproc(VOID)
{
    PCRITICAL_SECTION Lock;

    Lock = HeapAlloc(GetProcessHeap(), 0, sizeof(*Lock));
    if (Lock != NULL)
        InitializeCriticalSection(Lock);

    return Lock;
}

VOID
SYMCRYPT_CALL
SymCryptCallbackFreeMutexFastInproc(
    _Inout_ PVOID pMutex)
{
    DeleteCriticalSection(pMutex);
    HeapFree(GetProcessHeap(), 0, pMutex);
}

VOID
SYMCRYPT_CALL
SymCryptCallbackAcquireMutexFastInproc(
    _Inout_ PVOID pMutex)
{
    EnterCriticalSection(pMutex);
}

VOID
SYMCRYPT_CALL
SymCryptCallbackReleaseMutexFastInproc(
    _Inout_ PVOID pMutex)
{
    LeaveCriticalSection(pMutex);
}

VOID
WlanCryptoInitialize(VOID)
{
    SymCryptInit();
}

BOOL
WlanCryptoRandom(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    return SymCryptCallbackRandom(Buffer, Length) == SYMCRYPT_NO_ERROR;
}

BOOL
WlanCryptoPbkdf2Sha1(
    _In_reads_bytes_(PasswordLength) const UCHAR *Password,
    _In_ ULONG PasswordLength,
    _In_reads_bytes_(SaltLength) const UCHAR *Salt,
    _In_ ULONG SaltLength,
    _In_ ULONG Iterations,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    return SymCryptPbkdf2(SymCryptHmacSha1Algorithm, Password, PasswordLength,
                          Salt, SaltLength, Iterations,
                          Output, OutputLength) == SYMCRYPT_NO_ERROR;
}

VOID
WlanCryptoHmacSha1(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_SHA1_LENGTH) PUCHAR Mac)
{
    SYMCRYPT_HMAC_SHA1_EXPANDED_KEY Expanded;

    SymCryptHmacSha1ExpandKey(&Expanded, Key, KeyLength);
    SymCryptHmacSha1(&Expanded, Data, DataLength, Mac);
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
}

VOID
WlanCryptoHmacSha256(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_SHA256_LENGTH) PUCHAR Mac)
{
    SYMCRYPT_HMAC_SHA256_EXPANDED_KEY Expanded;

    SymCryptHmacSha256ExpandKey(&Expanded, Key, KeyLength);
    SymCryptHmacSha256(&Expanded, Data, DataLength, Mac);
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
}

BOOL
WlanCryptoAesCmac(
    _In_reads_bytes_(16) const UCHAR *Key,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_CMAC_LENGTH) PUCHAR Mac)
{
    SYMCRYPT_AES_CMAC_EXPANDED_KEY Expanded;

    if (SymCryptAesCmacExpandKey(&Expanded, Key, 16) != SYMCRYPT_NO_ERROR)
        return FALSE;

    SymCryptAesCmac(&Expanded, Data, DataLength, Mac);
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
    return TRUE;
}

/* RFC 3394 key unwrap, as the key data of EAPOL-Key frames uses it */
BOOL
WlanCryptoAesUnwrap(
    _In_reads_bytes_(KekLength) const UCHAR *Kek,
    _In_ ULONG KekLength,
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_(OutputSize, *OutputLength) PUCHAR Output,
    _In_ ULONG OutputSize,
    _Out_ PULONG OutputLength)
{
    SYMCRYPT_AES_EXPANDED_KEY Expanded;
    SIZE_T Unwrapped = 0;
    SYMCRYPT_ERROR Error;

    *OutputLength = 0;

    if (SymCryptAesExpandKey(&Expanded, Kek, KekLength) != SYMCRYPT_NO_ERROR)
        return FALSE;

    Error = SymCryptAesKwDecrypt(&Expanded, Input, InputLength, Output, OutputSize, &Unwrapped);
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
    if (Error != SYMCRYPT_NO_ERROR)
        return FALSE;

    *OutputLength = (ULONG)Unwrapped;
    return TRUE;
}

/* The 802.11 PRF: HMAC-SHA1(K, Label || 0 || Data || i), one byte counter from 0 */
BOOL
WlanCryptoPrfSha1(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_ PCSTR Label,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    SYMCRYPT_HMAC_SHA1_EXPANDED_KEY Expanded;
    SYMCRYPT_HMAC_SHA1_STATE State;
    UCHAR Digest[WLAN_SHA1_LENGTH];
    UCHAR Separator = 0;
    UCHAR Counter;
    ULONG Done;

    SymCryptHmacSha1ExpandKey(&Expanded, Key, KeyLength);

    for (Done = 0, Counter = 0; Done < OutputLength; Counter++)
    {
        ULONG Take = min(sizeof(Digest), OutputLength - Done);

        SymCryptHmacSha1Init(&State, &Expanded);
        SymCryptHmacSha1Append(&State, (PCBYTE)Label, strlen(Label));
        SymCryptHmacSha1Append(&State, &Separator, sizeof(Separator));
        SymCryptHmacSha1Append(&State, Data, DataLength);
        SymCryptHmacSha1Append(&State, &Counter, sizeof(Counter));
        SymCryptHmacSha1Result(&State, Digest);

        RtlCopyMemory(Output + Done, Digest, Take);
        Done += Take;
    }

    SymCryptWipeKnownSize(Digest, sizeof(Digest));
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
    return TRUE;
}

/* The 802.11 KDF: HMAC-SHA256(K, i || Label || Context || Length), with i
   counting from 1 and both i and the bit length little-endian 16-bit */
BOOL
WlanCryptoKdfSha256(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_ PCSTR Label,
    _In_reads_bytes_(ContextLength) const UCHAR *Context,
    _In_ ULONG ContextLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    SYMCRYPT_HMAC_SHA256_EXPANDED_KEY Expanded;
    SYMCRYPT_HMAC_SHA256_STATE State;
    UCHAR Digest[WLAN_SHA256_LENGTH];
    UCHAR Bits[2];
    UCHAR Counter[2];
    USHORT Iteration;
    ULONG Done;

    if (OutputLength > 0xFFFF / 8)
        return FALSE;

    Bits[0] = (UCHAR)(OutputLength * 8);
    Bits[1] = (UCHAR)((OutputLength * 8) >> 8);

    SymCryptHmacSha256ExpandKey(&Expanded, Key, KeyLength);

    for (Done = 0, Iteration = 1; Done < OutputLength; Iteration++)
    {
        ULONG Take = min(sizeof(Digest), OutputLength - Done);

        Counter[0] = (UCHAR)Iteration;
        Counter[1] = (UCHAR)(Iteration >> 8);

        SymCryptHmacSha256Init(&State, &Expanded);
        SymCryptHmacSha256Append(&State, Counter, sizeof(Counter));
        SymCryptHmacSha256Append(&State, (PCBYTE)Label, strlen(Label));
        SymCryptHmacSha256Append(&State, Context, ContextLength);
        SymCryptHmacSha256Append(&State, Bits, sizeof(Bits));
        SymCryptHmacSha256Result(&State, Digest);

        RtlCopyMemory(Output + Done, Digest, Take);
        Done += Take;
    }

    SymCryptWipeKnownSize(Digest, sizeof(Digest));
    SymCryptWipeKnownSize(&Expanded, sizeof(Expanded));
    return TRUE;
}

VOID
WlanCryptoWipe(
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    SymCryptWipe(Buffer, Length);
}
