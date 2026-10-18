/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel run time queries of system data, tokens, strings and bitmaps
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* System data RtlGetSystemGlobalData knows about */
#define RTL_GLOBAL_INTERRUPT_TIME       2
#define RTL_GLOBAL_IMAGE_NUMBER_LOW     4
#define RTL_GLOBAL_IMAGE_NUMBER_HIGH    5
#define RTL_GLOBAL_MAJOR_VERSION        7
#define RTL_GLOBAL_MINOR_VERSION        8
#define RTL_GLOBAL_QPC_BIAS             0x12

/* FUNCTIONS *****************************************************************/

/**
 * @brief Copies a value the system keeps in the shared user data page.
 *
 * @param[in] DataId
 * Which value.
 *
 * @param[out] Buffer
 * Receives the value.
 *
 * @param[in] Size
 * Size of the buffer, it must be the size of the value.
 */
NTSTATUS
NTAPI
RtlGetSystemGlobalData(
    _In_ ULONG DataId,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size)
{
    ULONG64 Value64;

    switch (DataId)
    {
        case RTL_GLOBAL_INTERRUPT_TIME:
            if (Size != sizeof(ULONG64))
                return STATUS_INFO_LENGTH_MISMATCH;
            Value64 = KeQueryInterruptTime();
            RtlCopyMemory(Buffer, &Value64, sizeof(Value64));
            return STATUS_SUCCESS;

        case RTL_GLOBAL_IMAGE_NUMBER_LOW:
        case RTL_GLOBAL_IMAGE_NUMBER_HIGH:
            if (Size != sizeof(USHORT))
                return STATUS_INFO_LENGTH_MISMATCH;
            *(PUSHORT)Buffer = (DataId == RTL_GLOBAL_IMAGE_NUMBER_LOW) ?
                               SharedUserData->ImageNumberLow : SharedUserData->ImageNumberHigh;
            return STATUS_SUCCESS;

        case RTL_GLOBAL_MAJOR_VERSION:
        case RTL_GLOBAL_MINOR_VERSION:
            if (Size != sizeof(ULONG))
                return STATUS_INFO_LENGTH_MISMATCH;
            *(PULONG)Buffer = (DataId == RTL_GLOBAL_MAJOR_VERSION) ?
                              SharedUserData->NtMajorVersion : SharedUserData->NtMinorVersion;
            return STATUS_SUCCESS;

        case RTL_GLOBAL_QPC_BIAS:
            if (Size != sizeof(ULONG64))
                return STATUS_INFO_LENGTH_MISMATCH;
            Value64 = SharedUserData->QpcBias;
            RtlCopyMemory(Buffer, &Value64, sizeof(Value64));
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief Gets the product suites of the system.
 */
ULONG
NTAPI
RtlGetSuiteMask(VOID)
{
    return SharedUserData->SuiteMask;
}

/**
 * @brief Gets the system root path, such as C:\ReactOS.
 */
PCWSTR
NTAPI
RtlGetNtSystemRoot(VOID)
{
    return SharedUserData->NtSystemRoot;
}

/**
 * @brief Records that a code path that should never run did.
 */
VOID
NTAPI
RtlLogUnexpectedCodepath(
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    DPRINT("Unexpected code path reported by %p\n", _ReturnAddress());
}

/**
 * @brief Samples several clocks at the same instant.
 * @return STATUS_NOT_SUPPORTED, there is no hypervisor reference time to sample with.
 */
NTSTATUS
NTAPI
RtlGetMultiTimePrecise(
    _Out_ PVOID Times,
    _In_ ULONG Flags,
    _Out_opt_ PULONG ValidFlags)
{
    UNREFERENCED_PARAMETER(Times);
    UNREFERENCED_PARAMETER(Flags);

    if (ValidFlags)
        *ValidFlags = 0;

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Converts a performance counter value of the host to the one of this partition.
 *
 * @param[in] HostPerfCounter
 * Value read on the host.
 *
 * @param[in] Partition
 * Partition to convert for, -1 for the current one.
 *
 * @param[out] PerfCounter
 * Receives the value, the same one since the system always runs as the host.
 */
NTSTATUS
NTAPI
RtlConvertHostPerfCounterToPerfCounter(
    _In_ ULONG64 HostPerfCounter,
    _In_ ULONG64 Partition,
    _Out_ PULONG64 PerfCounter)
{
    UNREFERENCED_PARAMETER(Partition);

    if (!PerfCounter)
        return STATUS_INVALID_PARAMETER;

    *PerfCounter = HostPerfCounter;
    return STATUS_SUCCESS;
}

/**
 * @brief Tells whether a buffer holds only zeroes.
 */
BOOLEAN
NTAPI
RtlIsZeroMemory(
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ SIZE_T Length)
{
    PUCHAR Byte = Buffer;
    SIZE_T i;

    for (i = 0; i < Length; i++)
    {
        if (Byte[i] != 0)
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief Finds the first place a string occurs in another one.
 *
 * @param[in] FullString
 * String to look in.
 *
 * @param[in] SearchString
 * String to look for.
 *
 * @param[in] CaseInSensitive
 * TRUE to ignore case.
 *
 * @return Where SearchString starts in FullString, or NULL.
 */
PWCHAR
NTAPI
RtlFindUnicodeSubstring(
    _In_ PCUNICODE_STRING FullString,
    _In_ PCUNICODE_STRING SearchString,
    _In_ BOOLEAN CaseInSensitive)
{
    USHORT FullChars = FullString->Length / sizeof(WCHAR);
    USHORT SearchChars = SearchString->Length / sizeof(WCHAR);
    USHORT Start, i;
    WCHAR A, B;

    if (SearchChars == 0)
        return FullString->Buffer;

    for (Start = 0; (Start + SearchChars) <= FullChars; Start++)
    {
        for (i = 0; i < SearchChars; i++)
        {
            A = FullString->Buffer[Start + i];
            B = SearchString->Buffer[i];
            if (CaseInSensitive)
            {
                A = RtlUpcaseUnicodeChar(A);
                B = RtlUpcaseUnicodeChar(B);
            }

            if (A != B)
                break;
        }

        if (i == SearchChars)
            return &FullString->Buffer[Start];
    }

    return NULL;
}

/**
 * @brief Counts the set bits in a range of a bitmap.
 */
ULONG
NTAPI
RtlNumberOfSetBitsInRange(
    _In_ PRTL_BITMAP BitMapHeader,
    _In_ ULONG StartingIndex,
    _In_ ULONG Length)
{
    ULONG Count = 0, Index;

    ASSERT((StartingIndex + Length) <= BitMapHeader->SizeOfBitMap);

    for (Index = StartingIndex; Index < (StartingIndex + Length); Index++)
    {
        if (RtlCheckBit(BitMapHeader, Index))
            Count++;
    }

    return Count;
}

#ifdef _WIN64
/**
 * @brief Clears the bits of a bitmap that are clear in another one.
 *
 * @param[in,out] Destination
 * Bitmap that keeps only the bits set in both.
 *
 * @param[in] Source
 * Bitmap to intersect with. Bits of Destination past its end are left alone.
 */
VOID
NTAPI
RtlIntersectBitMapsEx(
    _Inout_ PRTL_BITMAP_EX Destination,
    _In_ PRTL_BITMAP_EX Source)
{
    ULONG64 Bits, Word = 0, Mask;

    Bits = min(Destination->SizeOfBitMap, Source->SizeOfBitMap);

    for (; Bits >= 64; Bits -= 64, Word++)
        Destination->Buffer[Word] &= Source->Buffer[Word];

    if (Bits != 0)
    {
        /* Only the low bits of the last word are shared */
        Mask = (1ULL << Bits) - 1;
        Destination->Buffer[Word] &= (Source->Buffer[Word] | ~Mask);
    }
}
#endif

/**
 * @brief Adds up the size of all the ACEs of an ACL.
 *
 * @param[in] Acl
 * ACL to look at.
 *
 * @param[out] AcesBufferSize
 * Receives the bytes the ACEs take after the ACL header.
 */
NTSTATUS
NTAPI
RtlGetAcesBufferSize(
    _In_ PACL Acl,
    _Out_ PULONG AcesBufferSize)
{
    PACE_HEADER Ace;
    ULONG Size = 0, i;

    *AcesBufferSize = 0;

    if (!Acl || !RtlValidAcl(Acl))
        return STATUS_INVALID_PARAMETER;

    Ace = (PACE_HEADER)(Acl + 1);
    for (i = 0; i < Acl->AceCount; i++)
    {
        Size += Ace->AceSize;
        Ace = (PACE_HEADER)((PUCHAR)Ace + Ace->AceSize);
    }

    *AcesBufferSize = Size;
    return STATUS_SUCCESS;
}

/**
 * @brief Builds a SID from an authority and a list of sub authorities.
 *
 * @param[out] Sid
 * Buffer large enough for SubAuthorityCount sub authorities.
 *
 * @param[in] IdentifierAuthority
 * Authority of the SID.
 *
 * @param[in] SubAuthorityCount
 * Number of ULONG sub authorities that follow.
 */
NTSTATUS
__cdecl
RtlInitializeSidEx(
    _Out_ PSID Sid,
    _In_ PSID_IDENTIFIER_AUTHORITY IdentifierAuthority,
    _In_ UCHAR SubAuthorityCount,
    ...)
{
    va_list Args;
    UCHAR i;

    if (SubAuthorityCount > SID_MAX_SUB_AUTHORITIES)
        return STATUS_INVALID_PARAMETER;

    RtlInitializeSid(Sid, IdentifierAuthority, SubAuthorityCount);

    va_start(Args, SubAuthorityCount);
    for (i = 0; i < SubAuthorityCount; i++)
        *RtlSubAuthoritySid(Sid, i) = va_arg(Args, ULONG);
    va_end(Args);

    return STATUS_SUCCESS;
}

/**
 * @brief Tells whether a token holds an enabled group SID.
 *
 * @param[in] Token
 * Token to look in, NULL for the effective token of the current thread.
 *
 * @param[in] Sid
 * Group to look for.
 *
 * @param[out] IsMember
 * Receives TRUE if the token has the group enabled.
 */
NTSTATUS
NTAPI
RtlCheckTokenMembership(
    _In_opt_ PACCESS_TOKEN Token,
    _In_ PSID Sid,
    _Out_ PBOOLEAN IsMember)
{
    PACCESS_TOKEN EffectiveToken = Token;
    SECURITY_IMPERSONATION_LEVEL Level;
    BOOLEAN EffectiveOnly;
    TOKEN_TYPE TokenType;

    if (!Sid || !IsMember)
        return STATUS_INVALID_PARAMETER;

    if (!EffectiveToken)
    {
        EffectiveToken = PsReferenceEffectiveToken(PsGetCurrentThread(),
                                                   &TokenType,
                                                   &EffectiveOnly,
                                                   &Level);
        if (!EffectiveToken)
            return STATUS_NO_TOKEN;
    }

    *IsMember = SepSidInToken(EffectiveToken, Sid);

    if (!Token)
        ObDereferenceObject(EffectiveToken);

    return STATUS_SUCCESS;
}

/**
 * @brief Tells whether a token may use a capability.
 *
 * @param[in] Token
 * Token to check, NULL for the current one.
 *
 * @param[in] CapabilityName
 * Capability asked for.
 *
 * @param[out] HasCapability
 * Receives TRUE, no token is ever an app container, so every capability is granted.
 */
NTSTATUS
NTAPI
RtlCapabilityCheck(
    _In_opt_ HANDLE Token,
    _In_ PCUNICODE_STRING CapabilityName,
    _Out_ PBOOLEAN HasCapability)
{
    UNREFERENCED_PARAMETER(Token);

    if (!CapabilityName || !HasCapability)
        return STATUS_INVALID_PARAMETER;

    *HasCapability = TRUE;
    return STATUS_SUCCESS;
}

/**
 * @brief Gets the package a token was created for.
 * @return STATUS_NOT_FOUND, no process ever runs as part of a package.
 */
NTSTATUS
NTAPI
RtlQueryPackageIdentity(
    _In_ PVOID TokenObject,
    _Out_writes_bytes_to_opt_(*PackageSize, *PackageSize) PWSTR PackageFullName,
    _Inout_opt_ PSIZE_T PackageSize,
    _Out_writes_bytes_to_opt_(*AppIdSize, *AppIdSize) PWSTR AppId,
    _Inout_opt_ PSIZE_T AppIdSize,
    _Out_opt_ PBOOLEAN Packaged)
{
    UNREFERENCED_PARAMETER(TokenObject);
    UNREFERENCED_PARAMETER(PackageFullName);
    UNREFERENCED_PARAMETER(PackageSize);
    UNREFERENCED_PARAMETER(AppId);
    UNREFERENCED_PARAMETER(AppIdSize);

    if (Packaged)
        *Packaged = FALSE;

    return STATUS_NOT_FOUND;
}

/**
 * @brief Gets the package claims of a token.
 * @return STATUS_NOT_FOUND, no process ever runs as part of a package.
 */
NTSTATUS
NTAPI
RtlQueryPackageClaims(
    _In_ PVOID TokenObject,
    _Out_writes_bytes_to_opt_(*PackageSize, *PackageSize) PWSTR PackageFullName,
    _Inout_opt_ PSIZE_T PackageSize,
    _Out_writes_bytes_to_opt_(*AppIdSize, *AppIdSize) PWSTR AppId,
    _Inout_opt_ PSIZE_T AppIdSize,
    _Out_opt_ PGUID DynamicId,
    _Out_opt_ PVOID PkgClaim,
    _Out_opt_ PULONG64 AttributesPresent)
{
    UNREFERENCED_PARAMETER(TokenObject);
    UNREFERENCED_PARAMETER(PackageFullName);
    UNREFERENCED_PARAMETER(PackageSize);
    UNREFERENCED_PARAMETER(AppId);
    UNREFERENCED_PARAMETER(AppIdSize);
    UNREFERENCED_PARAMETER(DynamicId);
    UNREFERENCED_PARAMETER(PkgClaim);

    if (AttributesPresent)
        *AttributesPresent = 0;

    return STATUS_NOT_FOUND;
}

/**
 * @brief Tells whether a token carries a security attribute.
 * @return FALSE, tokens never carry security attributes.
 */
BOOLEAN
NTAPI
SeSecurityAttributePresent(
    _In_ PACCESS_TOKEN Token,
    _In_ PCUNICODE_STRING AttributeName)
{
    UNREFERENCED_PARAMETER(Token);
    UNREFERENCED_PARAMETER(AttributeName);

    return FALSE;
}
