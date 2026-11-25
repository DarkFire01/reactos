/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Key derivation functions and secret agreement derivation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

#define CNG_PBKDF2_ITERATIONS   10000
#define CNG_TLS_MASTER_LENGTH   48
#define CNG_CAPI_BLOCK          64
#define CNG_DERIVE_FLAGS        KDF_USE_SECRET_AS_HMAC_KEY_FLAG

/* TYPES **********************************************************************/

/* The buffers of a parameter list, sorted out by type */
typedef struct _CNG_KDF_PARAMETERS
{
    const BCryptBuffer *Hash;
    const BCryptBuffer *HmacKey;
    const BCryptBuffer *Label;
    const BCryptBuffer *Seed;
    const BCryptBuffer *Protocol;
    const BCryptBuffer *AlgorithmId;
    const BCryptBuffer *Context;
    const BCryptBuffer *Salt;
    const BCryptBuffer *Iterations;
    const BCryptBuffer *Info;
    const BCryptBuffer *Generic;
    const BCryptBufferDesc *List;
} CNG_KDF_PARAMETERS, *PCNG_KDF_PARAMETERS;

/* HELPERS ********************************************************************/

static
NTSTATUS
CngParseParameters(
    _In_opt_ const BCryptBufferDesc *List,
    _Out_ PCNG_KDF_PARAMETERS Parameters)
{
    const BCryptBuffer *Buffer;
    ULONG Index;

    RtlZeroMemory(Parameters, sizeof(*Parameters));
    Parameters->List = List;
    if (List == NULL)
        return STATUS_SUCCESS;

    if (List->ulVersion != BCRYPTBUFFER_VERSION || (List->pBuffers == NULL && List->cBuffers != 0))
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < List->cBuffers; Index++)
    {
        Buffer = &List->pBuffers[Index];
        if (Buffer->pvBuffer == NULL && Buffer->cbBuffer != 0)
            return STATUS_INVALID_PARAMETER;

        switch (Buffer->BufferType)
        {
            case KDF_HASH_ALGORITHM: Parameters->Hash = Buffer; break;
            case KDF_HMAC_KEY: Parameters->HmacKey = Buffer; break;
            case KDF_TLS_PRF_LABEL: Parameters->Label = Buffer; break;
            case KDF_LABEL: Parameters->Label = Buffer; break;
            case KDF_TLS_PRF_SEED: Parameters->Seed = Buffer; break;
            case KDF_TLS_PRF_PROTOCOL: Parameters->Protocol = Buffer; break;
            case KDF_ALGORITHMID: Parameters->AlgorithmId = Buffer; break;
            case KDF_CONTEXT: Parameters->Context = Buffer; break;
            case KDF_SALT: Parameters->Salt = Buffer; break;
            case KDF_ITERATION_COUNT: Parameters->Iterations = Buffer; break;
            case KDF_HKDF_INFO: Parameters->Info = Buffer; break;
            case KDF_GENERIC_PARAMETER: Parameters->Generic = Buffer; break;
            default: break;
        }
    }

    return STATUS_SUCCESS;
}

static
PCBYTE
CngBufferData(
    _In_opt_ const BCryptBuffer *Buffer)
{
    return (Buffer != NULL) ? Buffer->pvBuffer : NULL;
}

static
ULONG
CngBufferLength(
    _In_opt_ const BCryptBuffer *Buffer)
{
    return (Buffer != NULL) ? Buffer->cbBuffer : 0;
}

/* A string buffer only counts when it carries its terminator */
static
PCWSTR
CngBufferString(
    _In_opt_ const BCryptBuffer *Buffer)
{
    PCWSTR Text;

    if (Buffer == NULL || Buffer->pvBuffer == NULL || Buffer->cbBuffer < sizeof(WCHAR))
        return NULL;

    Text = Buffer->pvBuffer;
    if (Text[Buffer->cbBuffer / sizeof(WCHAR) - 1] != UNICODE_NULL)
        return NULL;

    return Text;
}

/**
 * @brief
 * Resolves a KDF_HASH_ALGORITHM buffer to a hash algorithm.
 */
const CNG_ALGORITHM *
NTAPI
CngHashFromBuffer(
    _In_ const BCryptBuffer *Buffer,
    _Out_ NTSTATUS *Status)
{
    const CNG_ALGORITHM *Algorithm;
    PCWSTR Name = CngBufferString(Buffer);

    if (Name == NULL)
    {
        *Status = STATUS_INVALID_PARAMETER;
        return NULL;
    }

    Algorithm = CngFindAlgorithm(Name);
    if (Algorithm == NULL || Algorithm->Class != CngClassHash)
    {
        *Status = STATUS_NOT_SUPPORTED;
        return NULL;
    }

    *Status = STATUS_SUCCESS;
    return Algorithm;
}

/* Picks the hash of a parameter list, or the given default without one */
static
const CNG_ALGORITHM *
CngParameterHash(
    _In_ PCNG_KDF_PARAMETERS Parameters,
    _In_opt_ PCWSTR Default,
    _Out_ NTSTATUS *Status)
{
    if (Parameters->Hash != NULL)
        return CngHashFromBuffer(Parameters->Hash, Status);

    if (Default == NULL)
    {
        *Status = STATUS_INVALID_PARAMETER;
        return NULL;
    }

    *Status = STATUS_SUCCESS;
    return CngFindAlgorithm(Default);
}

/* Feeds every buffer of one type into a hash or MAC state, in list order */
static
VOID
CngAppendBuffers(
    _In_ PCNG_KDF_PARAMETERS Parameters,
    _In_ ULONG Type,
    _In_ const CNG_ALGORITHM *Hash,
    _In_ BOOLEAN Mac,
    _Inout_ PVOID State)
{
    const BCryptBuffer *Buffer;
    ULONG Index;

    if (Parameters->List == NULL)
        return;

    for (Index = 0; Index < Parameters->List->cBuffers; Index++)
    {
        Buffer = &Parameters->List->pBuffers[Index];
        if (Buffer->BufferType != Type)
            continue;

        if (Mac)
            CngMacForHash(Hash)->appendFunc(State, Buffer->pvBuffer, Buffer->cbBuffer);
        else
            SymCryptHashAppend(*Hash->Hash, State, Buffer->pvBuffer, Buffer->cbBuffer);
    }
}

/* Joins buffers into one, as SP 800-56A wants its other info contiguous */
static
PUCHAR
CngJoinBuffers(
    _In_reads_(Count) const BCryptBuffer **Buffers,
    _In_ ULONG Count,
    _Out_ PULONG Length)
{
    ULONG Index, Total = 0;
    PUCHAR Joined, Cursor;

    for (Index = 0; Index < Count; Index++)
        Total += CngBufferLength(Buffers[Index]);

    Joined = CngAllocate(max(Total, 1));
    if (Joined == NULL)
        return NULL;

    Cursor = Joined;
    for (Index = 0; Index < Count; Index++)
    {
        if (CngBufferLength(Buffers[Index]) != 0)
        {
            RtlCopyMemory(Cursor, Buffers[Index]->pvBuffer, Buffers[Index]->cbBuffer);
            Cursor += Buffers[Index]->cbBuffer;
        }
    }

    *Length = Total;
    return Joined;
}

static
NTSTATUS
CngConcatKdf(
    _In_ PCNG_KDF_PARAMETERS Parameters,
    _In_reads_bytes_(SecretLength) PCBYTE Secret,
    _In_ ULONG SecretLength,
    _In_ PCWSTR DefaultHash,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const BCryptBuffer *Parts[5];
    const CNG_ALGORITHM *Hash;
    const BCryptBuffer *Buffer;
    ULONG Index, InfoLength;
    PUCHAR Info;
    NTSTATUS Status;

    Hash = CngParameterHash(Parameters, DefaultHash, &Status);
    if (Hash == NULL)
        return Status;
    if (Parameters->AlgorithmId == NULL)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = (ULONG)SymCryptHashResultSize(*Hash->Hash);
    if (Output == NULL)
        return STATUS_SUCCESS;

    RtlZeroMemory((PVOID)Parts, sizeof(Parts));
    Parts[0] = Parameters->AlgorithmId;
    for (Index = 0; Index < Parameters->List->cBuffers; Index++)
    {
        Buffer = &Parameters->List->pBuffers[Index];
        if (Buffer->BufferType == KDF_PARTYUINFO)
            Parts[1] = Buffer;
        else if (Buffer->BufferType == KDF_PARTYVINFO)
            Parts[2] = Buffer;
        else if (Buffer->BufferType == KDF_SUPPPUBINFO)
            Parts[3] = Buffer;
        else if (Buffer->BufferType == KDF_SUPPPRIVINFO)
            Parts[4] = Buffer;
    }

    Info = CngJoinBuffers(Parts, ARRAYSIZE(Parts), &InfoLength);
    if (Info == NULL)
        return STATUS_NO_MEMORY;

    Status = STATUS_SUCCESS;
    if (SymCryptSskdfHash(*Hash->Hash, 0, Secret, SecretLength, Info, InfoLength,
                          Output, OutputLength) != SYMCRYPT_NO_ERROR)
    {
        Status = STATUS_INVALID_PARAMETER;
    }

    CngFree(Info);
    if (NT_SUCCESS(Status))
        *ResultLength = OutputLength;

    return Status;
}

static
NTSTATUS
CngTlsPrf(
    _In_ PCNG_KDF_PARAMETERS Parameters,
    _In_reads_bytes_(SecretLength) PCBYTE Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Protocol,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const CNG_ALGORITHM *Hash;
    SYMCRYPT_ERROR Error;
    NTSTATUS Status;

    if (Parameters->Seed == NULL)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = CNG_TLS_MASTER_LENGTH;
    if (Output == NULL)
        return STATUS_SUCCESS;

    if (Protocol == TLS1_PROTOCOL_VERSION || Protocol == TLS1_1_PROTOCOL_VERSION || Protocol == DTLS1_0_PROTOCOL_VERSION)
    {
        Error = SymCryptTlsPrf1_1(Secret, SecretLength,
                                  CngBufferData(Parameters->Label), CngBufferLength(Parameters->Label),
                                  CngBufferData(Parameters->Seed), CngBufferLength(Parameters->Seed),
                                  Output, OutputLength);
    }
    else if (Protocol == TLS1_2_PROTOCOL_VERSION || Protocol == DTLS1_2_PROTOCOL_VERSION)
    {
        Hash = CngParameterHash(Parameters, BCRYPT_SHA256_ALGORITHM, &Status);
        if (Hash == NULL)
            return Status;

        Error = SymCryptTlsPrf1_2(CngMacForHash(Hash), Secret, SecretLength,
                                  CngBufferData(Parameters->Label), CngBufferLength(Parameters->Label),
                                  CngBufferData(Parameters->Seed), CngBufferLength(Parameters->Seed),
                                  Output, OutputLength);
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Error != SYMCRYPT_NO_ERROR)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = OutputLength;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * The CryptDeriveKey expansion: the digest itself when it is long enough,
 * otherwise the two hashes of the digest XORed with the HMAC pads.
 */
NTSTATUS
NTAPI
CngCapiExpand(
    _In_ PCSYMCRYPT_HASH Hash,
    _In_reads_bytes_(DigestLength) const UCHAR *Digest,
    _In_ ULONG DigestLength,
    _In_ BOOLEAN Force,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    UCHAR Pad[CNG_CAPI_BLOCK], Expanded[2 * SYMCRYPT_HASH_MAX_RESULT_SIZE];
    ULONG Index;

    if (OutputLength > 2 * DigestLength)
        return STATUS_INVALID_PARAMETER;

    if (!Force && OutputLength <= DigestLength)
    {
        RtlCopyMemory(Output, Digest, OutputLength);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < CNG_CAPI_BLOCK; Index++)
        Pad[Index] = 0x36 ^ ((Index < DigestLength) ? Digest[Index] : 0);
    SymCryptHash(Hash, Pad, sizeof(Pad), Expanded, DigestLength);

    for (Index = 0; Index < CNG_CAPI_BLOCK; Index++)
        Pad[Index] = 0x5C ^ ((Index < DigestLength) ? Digest[Index] : 0);
    SymCryptHash(Hash, Pad, sizeof(Pad), Expanded + DigestLength, DigestLength);

    RtlCopyMemory(Output, Expanded, OutputLength);
    SymCryptWipe(Pad, sizeof(Pad));
    SymCryptWipe(Expanded, sizeof(Expanded));
    return STATUS_SUCCESS;
}

/* Tells whether a CAPI derivation for this target always expands */
static
BOOLEAN
CngCapiForced(
    _In_ const CNG_ALGORITHM *Hash,
    _In_opt_ PCWSTR Target)
{
    if (Target == NULL)
        return FALSE;
    if (wcscmp(Target, BCRYPT_AES_ALGORITHM) != 0 && wcscmp(Target, BCRYPT_3DES_ALGORITHM) != 0 &&
        wcscmp(Target, BCRYPT_3DES_112_ALGORITHM) != 0)
    {
        return FALSE;
    }

    return *Hash->Hash != SymCryptSha256Algorithm && *Hash->Hash != SymCryptSha384Algorithm &&
           *Hash->Hash != SymCryptSha512Algorithm;
}

/**
 * @brief
 * Runs a key derivation function over a secret agreement.
 */
NTSTATUS
NTAPI
CngDeriveFromSecret(
    _In_ PCNG_SECRET Secret,
    _In_ LPCWSTR Kdf,
    _In_opt_ BCryptBufferDesc *ParameterList,
    _Out_writes_bytes_to_opt_(DerivedLength, *ResultLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    UCHAR Digest[SYMCRYPT_HASH_MAX_RESULT_SIZE];
    SYMCRYPT_MAC_EXPANDED_KEY MacKey;
    SYMCRYPT_MAC_STATE MacState;
    SYMCRYPT_HASH_STATE HashState;
    CNG_KDF_PARAMETERS Parameters;
    const CNG_ALGORITHM *Hash;
    PCSYMCRYPT_MAC Mac;
    ULONG Length, Index, Protocol;
    PCBYTE Key;
    ULONG KeyLength;
    NTSTATUS Status;

    if (Flags & ~CNG_DERIVE_FLAGS)
        return STATUS_INVALID_PARAMETER;

    Status = CngParseParameters(ParameterList, &Parameters);
    if (!NT_SUCCESS(Status))
        return Status;

    if (wcscmp(Kdf, BCRYPT_KDF_RAW_SECRET) == 0)
    {
        /* The raw secret comes out least significant byte first */
        *ResultLength = Secret->Length;
        if (DerivedKey == NULL)
            return STATUS_SUCCESS;

        Length = min(DerivedLength, Secret->Length);
        for (Index = 0; Index < Length; Index++)
            DerivedKey[Index] = Secret->Value[Length - 1 - Index];

        *ResultLength = Length;
        return STATUS_SUCCESS;
    }

    if (wcscmp(Kdf, BCRYPT_KDF_SP80056A_CONCAT) == 0)
    {
        if (ParameterList == NULL)
            return STATUS_INVALID_PARAMETER;

        return CngConcatKdf(&Parameters, Secret->Value, Secret->Length, BCRYPT_SHA1_ALGORITHM,
                            DerivedKey, DerivedLength, ResultLength);
    }

    if (wcscmp(Kdf, BCRYPT_KDF_TLS_PRF) == 0)
    {
        if (Parameters.Protocol == NULL || Parameters.Protocol->cbBuffer != sizeof(ULONG))
            return STATUS_INVALID_PARAMETER;

        Protocol = *(PULONG)Parameters.Protocol->pvBuffer;
        return CngTlsPrf(&Parameters, Secret->Value, Secret->Length, Protocol, DerivedKey, DerivedLength, ResultLength);
    }

    if (wcscmp(Kdf, BCRYPT_KDF_HASH) != 0 && wcscmp(Kdf, BCRYPT_KDF_HMAC) != 0)
        return STATUS_INVALID_PARAMETER;

    Hash = CngParameterHash(&Parameters, BCRYPT_SHA1_ALGORITHM, &Status);
    if (Hash == NULL)
        return Status;

    Length = (ULONG)SymCryptHashResultSize(*Hash->Hash);
    *ResultLength = Length;
    if (DerivedKey == NULL)
        return STATUS_SUCCESS;

    if (wcscmp(Kdf, BCRYPT_KDF_HASH) == 0)
    {
        SymCryptHashInit(*Hash->Hash, &HashState);
        CngAppendBuffers(&Parameters, KDF_SECRET_PREPEND, Hash, FALSE, &HashState);
        SymCryptHashAppend(*Hash->Hash, &HashState, Secret->Value, Secret->Length);
        CngAppendBuffers(&Parameters, KDF_SECRET_APPEND, Hash, FALSE, &HashState);
        SymCryptHashResult(*Hash->Hash, &HashState, Digest, Length);
    }
    else
    {
        if (Flags & KDF_USE_SECRET_AS_HMAC_KEY_FLAG)
        {
            Key = Secret->Value;
            KeyLength = Secret->Length;
        }
        else if (Parameters.HmacKey != NULL)
        {
            Key = Parameters.HmacKey->pvBuffer;
            KeyLength = Parameters.HmacKey->cbBuffer;
        }
        else
        {
            return STATUS_INVALID_PARAMETER;
        }

        Mac = CngMacForHash(Hash);
        if (Mac->expandKeyFunc(&MacKey, Key, KeyLength) != SYMCRYPT_NO_ERROR)
            return STATUS_INVALID_PARAMETER;

        Mac->initFunc(&MacState, &MacKey);
        CngAppendBuffers(&Parameters, KDF_SECRET_PREPEND, Hash, TRUE, &MacState);
        Mac->appendFunc(&MacState, Secret->Value, Secret->Length);
        CngAppendBuffers(&Parameters, KDF_SECRET_APPEND, Hash, TRUE, &MacState);
        Mac->resultFunc(&MacState, Digest);
        SymCryptWipe(&MacKey, sizeof(MacKey));
    }

    Length = min(DerivedLength, Length);
    RtlCopyMemory(DerivedKey, Digest, Length);
    SymCryptWipe(Digest, sizeof(Digest));

    *ResultLength = Length;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Sets the HKDF properties of a key derivation key.
 */
NTSTATUS
NTAPI
CngSetKdfProperty(
    _In_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength)
{
    PCNG_KDF_STATE State = Key->Expanded;
    const CNG_ALGORITHM *Hash;
    BCryptBuffer Buffer;
    NTSTATUS Status;
    ULONG Length;

    if (Key->Provider->Algorithm->Kdf != CngKdfHkdf)
        return STATUS_NOT_SUPPORTED;

    if (wcscmp(Property, BCRYPT_HKDF_HASH_ALGORITHM) == 0)
    {
        if (State->Final)
            return STATUS_INVALID_PARAMETER;

        Buffer.BufferType = KDF_HASH_ALGORITHM;
        Buffer.cbBuffer = InputLength;
        Buffer.pvBuffer = Input;
        Hash = CngHashFromBuffer(&Buffer, &Status);
        if (Hash == NULL)
            return Status;

        State->Mac = CngMacForHash(Hash);
        return STATUS_SUCCESS;
    }

    if (wcscmp(Property, BCRYPT_HKDF_SALT_AND_FINALIZE) == 0)
    {
        if (State->Final || State->Mac == NULL)
            return STATUS_INVALID_PARAMETER;

        Length = (ULONG)State->Mac->resultSize;
        if (SymCryptHkdfExtractPrk(State->Mac, Key->Secret, Key->SecretLength, Input, InputLength,
                                   State->Prk, Length) != SYMCRYPT_NO_ERROR)
        {
            return STATUS_INVALID_PARAMETER;
        }

        State->PrkLength = Length;
        State->Final = TRUE;
        return STATUS_SUCCESS;
    }

    if (wcscmp(Property, BCRYPT_HKDF_PRK_AND_FINALIZE) == 0)
    {
        if (State->Final || State->Mac == NULL || InputLength != 0 || Key->SecretLength > sizeof(State->Prk))
            return STATUS_INVALID_PARAMETER;

        RtlCopyMemory(State->Prk, Key->Secret, Key->SecretLength);
        State->PrkLength = Key->SecretLength;
        State->Final = TRUE;
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* EXPORTS ********************************************************************/

/**
 * @brief
 * PBKDF2 with the HMAC of an HMAC provider.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE for a provider that is not an
 * HMAC, or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptDeriveKeyPBKDF2(
    _In_ BCRYPT_ALG_HANDLE Prf,
    _In_reads_bytes_opt_(PasswordLength) PUCHAR Password,
    _In_ ULONG PasswordLength,
    _In_reads_bytes_opt_(SaltLength) PUCHAR Salt,
    _In_ ULONG SaltLength,
    _In_ ULONGLONG Iterations,
    _Out_writes_bytes_(DerivedKeyLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedKeyLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Prf);

    if (Provider == NULL || Provider->Algorithm->Class != CngClassHash ||
        !(Provider->Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG))
    {
        return STATUS_INVALID_HANDLE;
    }

    if (Iterations == 0 || Flags != 0 || DerivedKey == NULL ||
        (Password == NULL && PasswordLength != 0) || (Salt == NULL && SaltLength != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (SymCryptPbkdf2(CngMacForHash(Provider->Algorithm), Password, PasswordLength, Salt, SaltLength,
                       Iterations, DerivedKey, DerivedKeyLength) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Derives key material from a key derivation key: PBKDF2, SP800-108 counter
 * mode HMAC, SP800-56A concatenation, HKDF, CAPI or the TLS PRFs.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptKeyDerivation(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_opt_ BCryptBufferDesc *ParameterList,
    _Out_writes_bytes_to_(DerivedKeyLength, *ResultLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedKeyLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);
    UCHAR Digest[SYMCRYPT_HASH_MAX_RESULT_SIZE];
    SYMCRYPT_HKDF_EXPANDED_KEY HkdfKey;
    CNG_KDF_PARAMETERS Parameters;
    const CNG_ALGORITHM *Hash;
    PCNG_KDF_STATE State;
    ULONGLONG Iterations;
    SYMCRYPT_ERROR Error;
    PCWSTR Target = NULL;
    NTSTATUS Status;

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (Object->Provider->Algorithm->Class != CngClassKdf)
        return STATUS_NOT_SUPPORTED;
    if (ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = 0;
    if (DerivedKey == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    Status = CngParseParameters(ParameterList, &Parameters);
    if (!NT_SUCCESS(Status))
        return Status;

    State = Object->Expanded;
    Error = SYMCRYPT_NO_ERROR;

    switch (Object->Provider->Algorithm->Kdf)
    {
        case CngKdfPbkdf2:
            Hash = CngParameterHash(&Parameters, NULL, &Status);
            if (Hash == NULL)
                return Status;

            Iterations = CNG_PBKDF2_ITERATIONS;
            if (Parameters.Iterations != NULL)
            {
                if (Parameters.Iterations->cbBuffer != sizeof(ULONGLONG))
                    return STATUS_INVALID_PARAMETER;
                Iterations = *(ULONGLONG *)Parameters.Iterations->pvBuffer;
            }
            if (Iterations == 0)
                return STATUS_INVALID_PARAMETER;

            Error = SymCryptPbkdf2(CngMacForHash(Hash), Object->Secret, Object->SecretLength,
                                   CngBufferData(Parameters.Salt), CngBufferLength(Parameters.Salt),
                                   Iterations, DerivedKey, DerivedKeyLength);
            break;

        case CngKdfSp800108:
            Hash = CngParameterHash(&Parameters, BCRYPT_SHA256_ALGORITHM, &Status);
            if (Hash == NULL)
                return Status;

            Error = SymCryptSp800_108(CngMacForHash(Hash), Object->Secret, Object->SecretLength,
                                      CngBufferData(Parameters.Label), CngBufferLength(Parameters.Label),
                                      CngBufferData(Parameters.Context), CngBufferLength(Parameters.Context),
                                      DerivedKey, DerivedKeyLength);
            break;

        case CngKdfSp80056a:
            if (ParameterList == NULL)
                return STATUS_INVALID_PARAMETER;
            return CngConcatKdf(&Parameters, Object->Secret, Object->SecretLength, BCRYPT_SHA1_ALGORITHM,
                                DerivedKey, DerivedKeyLength, ResultLength);

        case CngKdfHkdf:
            if (!State->Final)
                return STATUS_INVALID_PARAMETER;

            if (SymCryptHkdfPrkExpandKey(&HkdfKey, State->Mac, State->Prk, State->PrkLength) != SYMCRYPT_NO_ERROR)
                return STATUS_INVALID_PARAMETER;

            Error = SymCryptHkdfDerive(&HkdfKey, CngBufferData(Parameters.Info), CngBufferLength(Parameters.Info),
                                       DerivedKey, DerivedKeyLength);
            SymCryptWipe(&HkdfKey, sizeof(HkdfKey));
            break;

        case CngKdfCapi:
            Hash = CngParameterHash(&Parameters, NULL, &Status);
            if (Hash == NULL)
                return Status;

            Target = CngBufferString(Parameters.AlgorithmId);
            SymCryptHash(*Hash->Hash, Object->Secret, Object->SecretLength, Digest, sizeof(Digest));
            Status = CngCapiExpand(*Hash->Hash, Digest, (ULONG)SymCryptHashResultSize(*Hash->Hash),
                                   CngCapiForced(Hash, Target), DerivedKey, DerivedKeyLength);
            SymCryptWipe(Digest, sizeof(Digest));
            if (!NT_SUCCESS(Status))
                return Status;
            break;

        case CngKdfTls11:
            return CngTlsPrf(&Parameters, Object->Secret, Object->SecretLength, TLS1_1_PROTOCOL_VERSION,
                             DerivedKey, DerivedKeyLength, ResultLength);

        case CngKdfTls12:
            return CngTlsPrf(&Parameters, Object->Secret, Object->SecretLength, TLS1_2_PROTOCOL_VERSION,
                             DerivedKey, DerivedKeyLength, ResultLength);

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (Error != SYMCRYPT_NO_ERROR)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = DerivedKeyLength;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finishes a hash object and expands the digest the way CryptDeriveKey does.
 *
 * @param[in] TargetAlgorithm
 * Optional provider of the key the bytes are meant for. AES and 3DES keys
 * from a hash outside SHA-2 always take the expanded form.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptDeriveKeyCapi(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _In_opt_ BCRYPT_ALG_HANDLE TargetAlgorithm,
    _Out_writes_bytes_(DerivedKeyLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedKeyLength,
    _In_ ULONG Flags)
{
    PCNG_HASH Object = CngLookupHash(Hash);
    UCHAR Digest[SYMCRYPT_HASH_MAX_RESULT_SIZE];
    PCNG_PROVIDER Target = NULL;
    const CNG_ALGORITHM *Algorithm;
    ULONG DigestLength;
    NTSTATUS Status;

    if (Object == NULL || (Object->Flags & (CNG_HASH_FINISHED | CNG_HASH_MULTI)))
        return STATUS_INVALID_HANDLE;

    Algorithm = Object->Provider->Algorithm;
    if (Algorithm->Class != CngClassHash || (Object->Provider->Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return STATUS_INVALID_HANDLE;

    if (TargetAlgorithm != NULL)
    {
        Target = CngLookupProvider(TargetAlgorithm);
        if (Target == NULL)
            return STATUS_INVALID_HANDLE;
    }

    DigestLength = CngDigestLength(Object->Provider);
    if (DerivedKey == NULL || Flags != 0 || DerivedKeyLength > 2 * DigestLength)
        return STATUS_INVALID_PARAMETER;

    CngHashFinal(Object, Digest, DigestLength);
    Status = CngCapiExpand(*Algorithm->Hash, Digest, DigestLength,
                           CngCapiForced(Algorithm, (Target != NULL) ? Target->Algorithm->Name : NULL),
                           DerivedKey, DerivedKeyLength);

    SymCryptWipe(Digest, sizeof(Digest));
    return Status;
}

/* EOF */
