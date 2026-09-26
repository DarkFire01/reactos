/*
 * PROJECT:     ReactOS Kernel Cryptography
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Algorithm providers and their properties
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "bcryptk.h"

/* GLOBALS ********************************************************************/

/*
 * The algorithms this provider offers. Key lengths are in bits, as the
 * BCRYPT_KEY_LENGTHS property reports them.
 */
static const BCRYPTK_ALGORITHM BcryptkAlgorithms[] =
{
    { BCRYPT_AES_ALGORITHM, BcryptkClassCipher,
      &SymCryptAesBlockCipher, NULL, NULL, 128, 256, 64 },
    { BCRYPT_DES_ALGORITHM, BcryptkClassCipher,
      &SymCryptDesBlockCipher, NULL, NULL, 64, 64, 0 },
    { BCRYPT_3DES_ALGORITHM, BcryptkClassCipher,
      &SymCrypt3DesBlockCipher, NULL, NULL, 192, 192, 0 },
    { BCRYPT_MD5_ALGORITHM, BcryptkClassHash,
      NULL, &SymCryptMd5Algorithm, &SymCryptHmacMd5Algorithm, 0, 0, 0 },
    { BCRYPT_SHA1_ALGORITHM, BcryptkClassHash,
      NULL, &SymCryptSha1Algorithm, &SymCryptHmacSha1Algorithm, 0, 0, 0 },
    { BCRYPT_SHA256_ALGORITHM, BcryptkClassHash,
      NULL, &SymCryptSha256Algorithm, &SymCryptHmacSha256Algorithm, 0, 0, 0 },
    { BCRYPT_SHA384_ALGORITHM, BcryptkClassHash,
      NULL, &SymCryptSha384Algorithm, &SymCryptHmacSha384Algorithm, 0, 0, 0 },
    { BCRYPT_SHA512_ALGORITHM, BcryptkClassHash,
      NULL, &SymCryptSha512Algorithm, &SymCryptHmacSha512Algorithm, 0, 0, 0 },
    { BCRYPT_RNG_ALGORITHM, BcryptkClassRandom, NULL, NULL, NULL, 0, 0, 0 },
};

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Brings the algorithm library up. A driver calls this once before it uses
 * anything else here.
 */
VOID
BcryptkInitialize(VOID)
{
    SymCryptInit();
}

/**
 * @brief
 * Looks an algorithm name up in the table.
 *
 * @return
 * The algorithm, or NULL when this provider does not offer it.
 */
PCBCRYPTK_ALGORITHM
BcryptkFindAlgorithm(
    _In_ PCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(BcryptkAlgorithms); Index++)
    {
        if (_wcsicmp(Name, BcryptkAlgorithms[Index].Name) == 0)
            return &BcryptkAlgorithms[Index];
    }

    return NULL;
}

/**
 * @brief
 * Reports how much room a key of this algorithm needs.
 */
ULONG
BcryptkKeyObjectLength(
    _In_ PCBCRYPTK_ALGORITHM Algorithm)
{
    return (ULONG)(FIELD_OFFSET(BCRYPTK_KEY, Storage) +
                   (*Algorithm->Cipher)->expandedKeySize + BCRYPTK_ALIGNMENT);
}

/**
 * @brief
 * Reports how much room a hash or a keyed hash of this algorithm needs.
 */
ULONG
BcryptkHashObjectLength(
    _In_ PCBCRYPTK_ALGORITHM Algorithm,
    _In_ BOOLEAN Mac)
{
    SIZE_T Length = FIELD_OFFSET(BCRYPTK_HASH, Storage);

    if (Mac)
    {
        Length += (*Algorithm->Mac)->stateSize + BCRYPTK_ALIGNMENT;
        Length += (*Algorithm->Mac)->expandedKeySize + BCRYPTK_ALIGNMENT;
    }
    else
    {
        Length += (*Algorithm->Hash)->stateSize + BCRYPTK_ALIGNMENT;
    }

    return (ULONG)Length;
}

/**
 * @brief
 * Takes the buffer an object lives in, either the caller's or a fresh one.
 *
 * @param[in,out] Caller
 * The buffer the caller offered, or NULL to have one allocated.
 *
 * @param[out] Allocated
 * Set when the buffer has to be given back on destruction.
 */
PVOID
BcryptkAllocateObject(
    _In_ ULONG Length,
    _Inout_opt_ PUCHAR Caller,
    _In_ ULONG CallerLength,
    _Out_ PBOOLEAN Allocated)
{
    PVOID Object;

    *Allocated = FALSE;

    if (Caller != NULL)
    {
        if (CallerLength < Length)
            return NULL;

        RtlZeroMemory(Caller, Length);

        return Caller;
    }

    Object = ExAllocatePoolZero(NonPagedPool, Length, BCRYPTK_POOL_TAG);
    if (Object == NULL)
        return NULL;

    *Allocated = TRUE;

    return Object;
}

/**
 * @brief
 * Releases an object buffer this library owns.
 */
VOID
BcryptkFreeObject(
    _In_ PVOID Object,
    _In_ BOOLEAN Allocated)
{
    if (Allocated)
        ExFreePoolWithTag(Object, BCRYPTK_POOL_TAG);
}

/**
 * @brief
 * Opens a provider for one algorithm.
 *
 * @param[out] Algorithm
 * Receives the provider handle.
 *
 * @param[in] AlgorithmId
 * Names the algorithm, as the BCRYPT_*_ALGORITHM strings spell it.
 *
 * @param[in] Implementation
 * The provider to take the algorithm from. Only the primitive provider is
 * offered here, so a name other than that one is refused.
 *
 * @param[in] Flags
 * BCRYPT_ALG_HANDLE_HMAC_FLAG opens a hash algorithm as a keyed hash.
 */
NTSTATUS
WINAPI
BCryptOpenAlgorithmProvider(
    _Out_ BCRYPT_ALG_HANDLE *Algorithm,
    _In_ LPCWSTR AlgorithmId,
    _In_opt_ LPCWSTR Implementation,
    _In_ ULONG Flags)
{
    PCBCRYPTK_ALGORITHM Found;
    PBCRYPTK_PROVIDER Provider;

    if (Algorithm == NULL || AlgorithmId == NULL)
        return STATUS_INVALID_PARAMETER;

    *Algorithm = NULL;

    if (Implementation != NULL && _wcsicmp(Implementation, MS_PRIMITIVE_PROVIDER) != 0)
        return STATUS_NOT_FOUND;

    if ((Flags & ~(BCRYPT_ALG_HANDLE_HMAC_FLAG | BCRYPT_PROV_DISPATCH)) != 0)
        return STATUS_INVALID_PARAMETER;

    Found = BcryptkFindAlgorithm(AlgorithmId);
    if (Found == NULL)
        return STATUS_NOT_SUPPORTED;

    if ((Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0 && Found->Class != BcryptkClassHash)
        return STATUS_INVALID_PARAMETER;

    Provider = ExAllocatePoolZero(NonPagedPool, sizeof(*Provider), BCRYPTK_POOL_TAG);
    if (Provider == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Provider->Tag = BCRYPTK_PROVIDER_TAG;
    Provider->Algorithm = Found;
    Provider->Mac = (Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;

    if (Found->Class == BcryptkClassCipher)
    {
        /* Block ciphers start out chaining, the way the primitive provider does */
        Provider->ChainingMode = BcryptkChainCbc;
        Provider->MessageBlockLength = (ULONG)(*Found->Cipher)->blockSize;
    }

    *Algorithm = Provider;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Closes a provider.
 */
NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Algorithm;

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL || Provider->Tag != BCRYPTK_PROVIDER_TAG)
        return STATUS_INVALID_HANDLE;

    Provider->Tag = 0;
    ExFreePoolWithTag(Provider, BCRYPTK_POOL_TAG);

    return STATUS_SUCCESS;
}

/* Hands back a property whose value is a ULONG */
static
NTSTATUS
BcryptkReturnUlong(
    _In_ ULONG Value,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    *ResultLength = sizeof(Value);

    if (Output == NULL)
        return STATUS_SUCCESS;

    if (OutputLength < sizeof(Value))
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, &Value, sizeof(Value));

    return STATUS_SUCCESS;
}

/* Hands back a property whose value is a string, terminator included */
static
NTSTATUS
BcryptkReturnString(
    _In_ PCWSTR Value,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    SIZE_T Length = (wcslen(Value) + 1) * sizeof(WCHAR);

    *ResultLength = (ULONG)Length;

    if (Output == NULL)
        return STATUS_SUCCESS;

    if (OutputLength < Length)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, Value, Length);

    return STATUS_SUCCESS;
}

/* Spells out a chaining mode the way the BCRYPT_CHAIN_MODE_* strings do */
static
PCWSTR
BcryptkChainingModeName(
    _In_ BCRYPTK_CHAIN ChainingMode)
{
    switch (ChainingMode)
    {
        case BcryptkChainEcb:
            return BCRYPT_CHAIN_MODE_ECB;

        case BcryptkChainCbc:
            return BCRYPT_CHAIN_MODE_CBC;

        case BcryptkChainCfb:
            return BCRYPT_CHAIN_MODE_CFB;

        default:
            return BCRYPT_CHAIN_MODE_NA;
    }
}

/**
 * @brief
 * Reads a property of a provider, a key or a hash.
 *
 * @param[in] Object
 * The object to read from.
 *
 * @param[in] Property
 * Names the property, as the BCRYPT_* property strings spell it.
 *
 * @param[out] Output
 * Receives the value, or NULL to ask for its size alone.
 *
 * @param[out] ResultLength
 * Receives the size of the value.
 */
NTSTATUS
WINAPI
BCryptGetProperty(
    _In_ BCRYPT_HANDLE Object,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Object;
    PBCRYPTK_KEY Key = Object;
    PBCRYPTK_HASH Hash = Object;
    PCBCRYPTK_ALGORITHM Algorithm;
    BCRYPTK_CHAIN ChainingMode = BcryptkChainNone;
    BCRYPT_KEY_LENGTHS_STRUCT KeyLengths;
    BOOLEAN Mac = FALSE;
    ULONG Value;

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || Property == NULL || ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;

    switch (Provider->Tag)
    {
        case BCRYPTK_PROVIDER_TAG:
            Algorithm = Provider->Algorithm;
            ChainingMode = Provider->ChainingMode;
            Mac = Provider->Mac;
            break;

        case BCRYPTK_KEY_TAG:
            Algorithm = Key->Algorithm;
            ChainingMode = Key->ChainingMode;
            break;

        case BCRYPTK_HASH_TAG:
            Algorithm = Hash->Algorithm;
            Mac = Hash->Mac;
            break;

        default:
            return STATUS_INVALID_HANDLE;
    }

    if (_wcsicmp(Property, BCRYPT_ALGORITHM_NAME) == 0)
        return BcryptkReturnString(Algorithm->Name, Output, OutputLength, ResultLength);

    if (_wcsicmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
    {
        if (Algorithm->Class == BcryptkClassRandom)
            return STATUS_NOT_SUPPORTED;

        if (Algorithm->Class == BcryptkClassCipher)
            Value = BcryptkKeyObjectLength(Algorithm);
        else
            Value = BcryptkHashObjectLength(Algorithm, Mac);

        return BcryptkReturnUlong(Value, Output, OutputLength, ResultLength);
    }

    if (_wcsicmp(Property, BCRYPT_BLOCK_LENGTH) == 0)
    {
        if (Algorithm->Class != BcryptkClassCipher)
            return STATUS_NOT_SUPPORTED;

        Value = (ULONG)(*Algorithm->Cipher)->blockSize;

        return BcryptkReturnUlong(Value, Output, OutputLength, ResultLength);
    }

    if (_wcsicmp(Property, BCRYPT_CHAINING_MODE) == 0)
    {
        return BcryptkReturnString(BcryptkChainingModeName(ChainingMode),
                                   Output,
                                   OutputLength,
                                   ResultLength);
    }

    if (_wcsicmp(Property, BCRYPT_KEY_LENGTHS) == 0)
    {
        if (Algorithm->Class != BcryptkClassCipher)
            return STATUS_NOT_SUPPORTED;

        *ResultLength = sizeof(KeyLengths);

        if (Output == NULL)
            return STATUS_SUCCESS;

        if (OutputLength < sizeof(KeyLengths))
            return STATUS_BUFFER_TOO_SMALL;

        KeyLengths.dwMinLength = Algorithm->MinKeyLength;
        KeyLengths.dwMaxLength = Algorithm->MaxKeyLength;
        KeyLengths.dwIncrement = Algorithm->KeyIncrement;
        RtlCopyMemory(Output, &KeyLengths, sizeof(KeyLengths));

        return STATUS_SUCCESS;
    }

    if (_wcsicmp(Property, BCRYPT_HASH_LENGTH) == 0)
    {
        if (Algorithm->Class != BcryptkClassHash)
            return STATUS_NOT_SUPPORTED;

        if (Mac)
            Value = (ULONG)(*Algorithm->Mac)->resultSize;
        else
            Value = (*Algorithm->Hash)->resultSize;

        return BcryptkReturnUlong(Value, Output, OutputLength, ResultLength);
    }

    if (_wcsicmp(Property, BCRYPT_HASH_BLOCK_LENGTH) == 0)
    {
        if (Algorithm->Class != BcryptkClassHash)
            return STATUS_NOT_SUPPORTED;

        return BcryptkReturnUlong((*Algorithm->Hash)->inputBlockSize,
                                  Output,
                                  OutputLength,
                                  ResultLength);
    }

    if (_wcsicmp(Property, BCRYPT_MESSAGE_BLOCK_LENGTH) == 0)
    {
        if (Algorithm->Class != BcryptkClassCipher)
            return STATUS_NOT_SUPPORTED;

        if (Provider->Tag == BCRYPTK_PROVIDER_TAG)
            Value = Provider->MessageBlockLength;
        else
            Value = Key->MessageBlockLength;

        return BcryptkReturnUlong(Value, Output, OutputLength, ResultLength);
    }

    *ResultLength = 0;

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Writes a property of a provider or a key.
 *
 * @remarks
 * The chaining mode and the shift a CFB run works in are the properties a
 * caller may change. Both belong to the object they are set on, so a key that
 * was made earlier keeps the mode it was made with.
 */
NTSTATUS
WINAPI
BCryptSetProperty(
    _Inout_ BCRYPT_HANDLE Object,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Object;
    PBCRYPTK_KEY Key = Object;
    PCBCRYPTK_ALGORITHM Algorithm;
    PBCRYPTK_CHAIN Target;
    PULONG MessageBlockLength;
    PCWSTR Mode;
    ULONG Value;

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || Property == NULL || Input == NULL)
        return STATUS_INVALID_PARAMETER;

    switch (Provider->Tag)
    {
        case BCRYPTK_PROVIDER_TAG:
            Algorithm = Provider->Algorithm;
            Target = &Provider->ChainingMode;
            MessageBlockLength = &Provider->MessageBlockLength;
            break;

        case BCRYPTK_KEY_TAG:
            Algorithm = Key->Algorithm;
            Target = &Key->ChainingMode;
            MessageBlockLength = &Key->MessageBlockLength;
            break;

        default:
            return STATUS_INVALID_HANDLE;
    }

    if (_wcsicmp(Property, BCRYPT_CHAINING_MODE) == 0)
    {
        if (Algorithm->Class != BcryptkClassCipher)
            return STATUS_NOT_SUPPORTED;

        /*
         * The value is a string, which a caller is free to hand over without
         * its terminator, so only the bytes that are there are compared.
         */
        if (InputLength < sizeof(WCHAR))
            return STATUS_INVALID_PARAMETER;

        Mode = (PCWSTR)Input;

        if (_wcsnicmp(Mode, BCRYPT_CHAIN_MODE_CBC, InputLength / sizeof(WCHAR)) == 0)
            *Target = BcryptkChainCbc;
        else if (_wcsnicmp(Mode, BCRYPT_CHAIN_MODE_ECB, InputLength / sizeof(WCHAR)) == 0)
            *Target = BcryptkChainEcb;
        else if (_wcsnicmp(Mode, BCRYPT_CHAIN_MODE_CFB, InputLength / sizeof(WCHAR)) == 0)
            *Target = BcryptkChainCfb;
        else
            return STATUS_NOT_SUPPORTED;

        return STATUS_SUCCESS;
    }

    if (_wcsicmp(Property, BCRYPT_MESSAGE_BLOCK_LENGTH) == 0)
    {
        if (Algorithm->Class != BcryptkClassCipher)
            return STATUS_NOT_SUPPORTED;

        if (InputLength != sizeof(Value))
            return STATUS_INVALID_PARAMETER;

        RtlCopyMemory(&Value, Input, sizeof(Value));

        /* A CFB run shifts by one byte or by a whole block, nothing between */
        if (Value != 1 && Value != (*Algorithm->Cipher)->blockSize)
            return STATUS_INVALID_PARAMETER;

        *MessageBlockLength = Value;

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
