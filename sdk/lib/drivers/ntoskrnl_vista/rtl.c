/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rtl functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/* Mask of the CM_RESOURCE_MEMORY_LARGE_* length-scaling flags. */
#define CM_RESOURCE_MEMORY_LARGE_MASK \
    (CM_RESOURCE_MEMORY_LARGE_40 | CM_RESOURCE_MEMORY_LARGE_48 | CM_RESOURCE_MEMORY_LARGE_64)

/**
 * @brief
 * Counts the number of set bits in a pointer-sized value.
 *
 * @param[in] Target
 * The value whose set bits are counted.
 *
 * @return
 * The number of bits set in @p Target.
 */
ULONG
NTAPI
RtlNumberOfSetBitsUlongPtr(
    _In_ ULONG_PTR Target)
{
    ULONG Count = 0;

    while (Target != 0)
    {
        Target &= (Target - 1);
        Count++;
    }

    return Count;
}

/**
 * @brief
 * Finds the smallest length that is not less than @p SourceLength and can be
 * encoded in a large memory resource descriptor.
 *
 * @param[in] SourceLength
 * The requested length, in bytes.
 *
 * @param[out] TargetLength
 * Receives the closest encodable length.
 *
 * @return
 * STATUS_SUCCESS if an encodable length was found, or STATUS_INVALID_PARAMETER
 * if @p SourceLength cannot be represented.
 */
NTSTATUS
NTAPI
RtlFindClosestEncodableLength(
    _In_ ULONGLONG SourceLength,
    _Out_ PULONGLONG TargetLength)
{
    /* Values below 4 GB are always encodable as-is. */
    if (SourceLength <= 0xFFFFFFFFULL)
    {
        *TargetLength = SourceLength;
        return STATUS_SUCCESS;
    }

    /* 40-bit range: encodable when the low 8 bits are clear, else round up. */
    if (SourceLength <= 0xFFFFFFFF00ULL)
    {
        if ((SourceLength & 0xFFULL) == 0)
        {
            *TargetLength = SourceLength;
            return STATUS_SUCCESS;
        }

        *TargetLength = (SourceLength & ~0xFFULL) + 0x100ULL;
        if (*TargetLength <= 0xFFFFFFFF00ULL)
            return STATUS_SUCCESS;

        SourceLength = *TargetLength;
    }

    /* 48-bit range: encodable when the low 16 bits are clear, else round up. */
    if (SourceLength <= 0xFFFFFFFF0000ULL)
    {
        if ((SourceLength & 0xFFFFULL) == 0)
        {
            *TargetLength = SourceLength;
            return STATUS_SUCCESS;
        }

        *TargetLength = (SourceLength & ~0xFFFFULL) + 0x10000ULL;
        if (*TargetLength <= 0xFFFFFFFF0000ULL)
            return STATUS_SUCCESS;

        SourceLength = *TargetLength;
    }

    /* 64-bit range: encodable when the low 32 bits are clear, else round up. */
    if (SourceLength <= 0xFFFFFFFF00000000ULL)
    {
        if ((SourceLength & 0xFFFFFFFFULL) == 0)
        {
            *TargetLength = SourceLength;
        }
        else
        {
            *TargetLength = (SourceLength & ~0xFFFFFFFFULL) + 0x100000000ULL;
        }

        return STATUS_SUCCESS;
    }

    *TargetLength = 0;
    return STATUS_INVALID_PARAMETER;
}

/**
 * @brief
 * Encodes a memory or port length into a CM partial resource descriptor,
 * selecting a large-memory encoding when required.
 *
 * @param[in] Descriptor
 * The descriptor to populate.
 *
 * @param[in] Type
 * The resource type (CmResourceTypePort, CmResourceTypeMemory or
 * CmResourceTypeMemoryLarge).
 *
 * @param[in] Length
 * The length to encode, in bytes.
 *
 * @param[in] Start
 * The starting address of the resource.
 *
 * @return
 * STATUS_SUCCESS on success, STATUS_INVALID_PARAMETER for an unsupported type,
 * or STATUS_UNSUCCESSFUL if @p Length cannot be encoded.
 */
NTSTATUS
NTAPI
RtlCmEncodeMemIoResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _In_ UCHAR Type,
    _In_ ULONGLONG Length,
    _In_ ULONGLONG Start)
{
    if (Type == CmResourceTypePort)
    {
        if ((Length >> 32) != 0)
            return STATUS_INVALID_PARAMETER;

        Descriptor->Type = CmResourceTypePort;
        Descriptor->u.Generic.Start.QuadPart = Start;
        Descriptor->u.Generic.Length = (ULONG)Length;
        return STATUS_SUCCESS;
    }

    if (Type != CmResourceTypeMemory && Type != CmResourceTypeMemoryLarge)
        return STATUS_INVALID_PARAMETER;

    Descriptor->Flags &= (USHORT)~CM_RESOURCE_MEMORY_LARGE_MASK;
    Descriptor->u.Generic.Start.QuadPart = Start;

    /* A length that fits in 32 bits uses the plain memory encoding. */
    if ((Length >> 32) == 0)
    {
        Descriptor->Type = CmResourceTypeMemory;
        Descriptor->u.Generic.Length = (ULONG)Length;
        return STATUS_SUCCESS;
    }

    if (Length <= 0xFFFFFFFF00ULL)
    {
        if ((Length & 0xFFULL) != 0)
            return STATUS_UNSUCCESSFUL;

        Descriptor->u.Generic.Length = (ULONG)(Length >> 8);
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_40;
    }
    else if (Length <= 0xFFFFFFFF0000ULL)
    {
        if ((Length & 0xFFFFULL) != 0)
            return STATUS_UNSUCCESSFUL;

        Descriptor->u.Generic.Length = (ULONG)(Length >> 16);
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_48;
    }
    else if (Length <= 0xFFFFFFFF00000000ULL)
    {
        if ((Length & 0xFFFFFFFFULL) != 0)
            return STATUS_UNSUCCESSFUL;

        Descriptor->u.Generic.Length = (ULONG)(Length >> 32);
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_64;
    }
    else
    {
        return STATUS_UNSUCCESSFUL;
    }

    Descriptor->Type = CmResourceTypeMemoryLarge;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Decodes the length stored in a CM partial resource descriptor, accounting
 * for the large-memory scaling flags.
 *
 * @param[in] Descriptor
 * The descriptor to decode.
 *
 * @param[out] Start
 * Optionally receives the starting address of the resource.
 *
 * @return
 * The decoded length, in bytes.
 */
ULONGLONG
NTAPI
RtlCmDecodeMemIoResource(
    _In_ struct _CM_PARTIAL_RESOURCE_DESCRIPTOR *Descriptor,
    _Out_opt_ PULONGLONG Start)
{
    ULONGLONG Length = 0;

    if (Start != NULL)
        *Start = (ULONGLONG)Descriptor->u.Generic.Start.QuadPart;

    switch (Descriptor->Type)
    {
        case CmResourceTypePort:
        case CmResourceTypeMemory:
            Length = Descriptor->u.Generic.Length;
            break;

        case CmResourceTypeMemoryLarge:
            if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_40)
                Length = (ULONGLONG)Descriptor->u.Generic.Length << 8;
            else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_48)
                Length = (ULONGLONG)Descriptor->u.Generic.Length << 16;
            else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_64)
                Length = (ULONGLONG)Descriptor->u.Generic.Length << 32;
            else
                Length = Descriptor->u.Generic.Length;
            break;

        default:
            break;
    }

    return Length;
}

/**
 * @brief
 * Encodes a length, alignment and address range into an I/O resource
 * descriptor, selecting a large-memory encoding when required.
 *
 * @param[in] Descriptor
 * The descriptor to populate.
 *
 * @param[in] Type
 * The resource type (CmResourceTypePort, CmResourceTypeMemory or
 * CmResourceTypeMemoryLarge).
 *
 * @param[in] Length
 * The length to encode, in bytes.
 *
 * @param[in] Alignment
 * The required alignment, in bytes.
 *
 * @param[in] MinimumAddress
 * The lowest acceptable starting address.
 *
 * @param[in] MaximumAddress
 * The highest acceptable ending address.
 *
 * @return
 * STATUS_SUCCESS on success, STATUS_INVALID_PARAMETER for an unsupported type,
 * or STATUS_UNSUCCESSFUL if the values cannot be encoded.
 */
NTSTATUS
NTAPI
RtlIoEncodeMemIoResource(
    _In_ struct _IO_RESOURCE_DESCRIPTOR *Descriptor,
    _In_ UCHAR Type,
    _In_ ULONGLONG Length,
    _In_ ULONGLONG Alignment,
    _In_ ULONGLONG MinimumAddress,
    _In_ ULONGLONG MaximumAddress)
{
    ULONG Shift;

    if (Type == CmResourceTypePort)
    {
        if ((Length >> 32) != 0 || (Alignment >> 32) != 0)
            return STATUS_INVALID_PARAMETER;

        Descriptor->Type = CmResourceTypePort;
        Descriptor->u.Port.MinimumAddress.QuadPart = MinimumAddress;
        Descriptor->u.Port.MaximumAddress.QuadPart = MaximumAddress;
        Descriptor->u.Port.Length = (ULONG)Length;
        Descriptor->u.Port.Alignment = (ULONG)Alignment;
        return STATUS_SUCCESS;
    }

    if (Type != CmResourceTypeMemory && Type != CmResourceTypeMemoryLarge)
        return STATUS_INVALID_PARAMETER;

    Descriptor->Flags &= (USHORT)~CM_RESOURCE_MEMORY_LARGE_MASK;
    Descriptor->u.Memory.MinimumAddress.QuadPart = MinimumAddress;
    Descriptor->u.Memory.MaximumAddress.QuadPart = MaximumAddress;

    /* A length and alignment that fit in 32 bits use the plain encoding. */
    if ((Length >> 32) == 0 && (Alignment >> 32) == 0)
    {
        Descriptor->Type = CmResourceTypeMemory;
        Descriptor->u.Memory.Length = (ULONG)Length;
        Descriptor->u.Memory.Alignment = (ULONG)Alignment;
        return STATUS_SUCCESS;
    }

    if (Length <= 0xFFFFFFFF00ULL)
    {
        Shift = 8;
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_40;
    }
    else if (Length <= 0xFFFFFFFF0000ULL)
    {
        Shift = 16;
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_48;
    }
    else if (Length <= 0xFFFFFFFF00000000ULL)
    {
        Shift = 32;
        Descriptor->Flags |= CM_RESOURCE_MEMORY_LARGE_64;
    }
    else
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* The scaled-out low bits of both length and alignment must be zero. */
    if ((Length & ((1ULL << Shift) - 1)) != 0 ||
        (Alignment & ((1ULL << Shift) - 1)) != 0)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Descriptor->Type = CmResourceTypeMemoryLarge;
    Descriptor->u.Memory.Length = (ULONG)(Length >> Shift);
    Descriptor->u.Memory.Alignment = (ULONG)(Alignment >> Shift);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Decodes the length, alignment and address range stored in an I/O resource
 * descriptor, accounting for the large-memory scaling flags.
 *
 * @param[in] Descriptor
 * The descriptor to decode.
 *
 * @param[out] Alignment
 * Optionally receives the decoded alignment.
 *
 * @param[out] MinimumAddress
 * Optionally receives the minimum address.
 *
 * @param[out] MaximumAddress
 * Optionally receives the maximum address.
 *
 * @return
 * The decoded length, in bytes.
 */
ULONGLONG
NTAPI
RtlIoDecodeMemIoResource(
    _In_ struct _IO_RESOURCE_DESCRIPTOR *Descriptor,
    _Out_opt_ PULONGLONG Alignment,
    _Out_opt_ PULONGLONG MinimumAddress,
    _Out_opt_ PULONGLONG MaximumAddress)
{
    ULONGLONG Length = 0;
    ULONGLONG DecodedAlignment = 0;
    ULONG Shift = 0;

    if (Descriptor->Type == CmResourceTypePort ||
        Descriptor->Type == CmResourceTypeMemory)
    {
        Length = Descriptor->u.Memory.Length;
        DecodedAlignment = Descriptor->u.Memory.Alignment;
    }
    else
    {
        if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_40)
            Shift = 8;
        else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_48)
            Shift = 16;
        else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_64)
            Shift = 32;

        Length = (ULONGLONG)Descriptor->u.Memory.Length << Shift;
        DecodedAlignment = (ULONGLONG)Descriptor->u.Memory.Alignment << Shift;
    }

    if (Alignment != NULL)
        *Alignment = DecodedAlignment;

    if (MinimumAddress != NULL)
        *MinimumAddress = (ULONGLONG)Descriptor->u.Memory.MinimumAddress.QuadPart;

    if (MaximumAddress != NULL)
        *MaximumAddress = (ULONGLONG)Descriptor->u.Memory.MaximumAddress.QuadPart;

    return Length;
}

/**
 * @brief
 * Builds the inverse of a range list within given bounds.
 *
 * @param[out] InvertedRangeList
 * Receives the inverted range list (an RTL_RANGE_LIST).
 *
 * @param[in] RangeList
 * The range list to invert (an RTL_RANGE_LIST).
 *
 * @param[in] Minimum
 * The lower bound of the range to invert.
 *
 * @param[in] Maximum
 * The upper bound of the range to invert.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not yet implement bounded range-list inversion.
 */
NTSTATUS
NTAPI
RtlInvertRangeListEx(
    _Out_ PVOID InvertedRangeList,
    _In_ PVOID RangeList,
    _In_ ULONGLONG Minimum,
    _In_ ULONGLONG Maximum)
{
    UNREFERENCED_PARAMETER(InvertedRangeList);
    UNREFERENCED_PARAMETER(RangeList);
    UNREFERENCED_PARAMETER(Minimum);
    UNREFERENCED_PARAMETER(Maximum);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Queries multiple registry values in a single call, rejecting the unchecked
 * RTL_QUERY_REGISTRY_DIRECT form for security.
 *
 * @param[in] RelativeTo
 * The registry root the @p Path is relative to.
 *
 * @param[in] Path
 * The registry key path to query.
 *
 * @param[in,out] QueryTable
 * The table describing the values to query.
 *
 * @param[in] Context
 * Optional context passed to query-routine callbacks.
 *
 * @param[in] Environment
 * Optional environment used to expand REG_EXPAND_SZ values.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @remarks
 * ReactOS forwards this to RtlQueryRegistryValues(). Callers are expected to
 * set RTL_QUERY_REGISTRY_TYPECHECK for every RTL_QUERY_REGISTRY_DIRECT entry.
 */
NTSTATUS
NTAPI
RtlQueryRegistryValuesEx(
    _In_ ULONG RelativeTo,
    _In_ PCWSTR Path,
    _Inout_ PRTL_QUERY_REGISTRY_TABLE QueryTable,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Environment)
{
    return RtlQueryRegistryValues(RelativeTo, Path, QueryTable, Context, Environment);
}
