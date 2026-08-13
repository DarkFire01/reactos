/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Root Arbiter General Handlers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are the resource-type-agnostic arbiter callbacks shared by the Root Port
 * (CmResourceTypePort) and Root Memory (CmResourceTypeMemory) arbiters.  Both
 * govern a simple [Minimum, Maximum] address window with a Length and Alignment,
 * so the same unpack/pack/score/translate routines serve both - only the
 * resource type and (for memory) a couple of specialised overrides differ.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PROTOTYPES ***************************************************************/

/*
 * The packed port/memory descriptor decoders (they handle every sub-encoding,
 * including the 40/48/64-bit "large memory" forms).  wdm.h hides their
 * declarations below NTDDI_VISTA and ntoskrnl builds at the WS03 level, so
 * they are declared locally; the implementations live in sdk/lib/rtl/memres.c.
 */
ULONGLONG
NTAPI
RtlIoDecodeMemIoResource(
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _Out_opt_ PULONGLONG Alignment,
    _Out_opt_ PULONGLONG MinimumAddress,
    _Out_opt_ PULONGLONG MaximumAddress);

ULONGLONG
NTAPI
RtlCmDecodeMemIoResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Out_opt_ PULONGLONG Start);

NTSTATUS NTAPI IopSharedUnpackRequirement(PIO_RESOURCE_DESCRIPTOR, PUINT64, PUINT64, PUINT64, PUINT64);
NTSTATUS NTAPI IopSharedPackResource(PIO_RESOURCE_DESCRIPTOR, UINT64, PCM_PARTIAL_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR, PUINT64, PUINT64);
INT32 NTAPI IopSharedScoreRequirement(PIO_RESOURCE_DESCRIPTOR);
NTSTATUS NTAPI IopSharedTranslateOrdering(PIO_RESOURCE_DESCRIPTOR, PIO_RESOURCE_DESCRIPTOR);

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Translates a single bus-relative address to a system-physical
 * one on the ISA bus, and reports which space it landed in as a
 * CM resource type.
 *
 * @param[in] SourceType
 * The CM resource type of the address being translated (port,
 * memory or large memory).
 *
 * @param[in] SourceAddress
 * The bus-relative address to translate.
 *
 * @param[out] TranslatedAddress
 * Receives the system-physical address.
 *
 * @param[out] TranslatedType
 * Receives the post-translation resource type. The HAL may remap
 * one space onto another (e.g. memory-mapped ports), so the type
 * is derived from the address space the translation actually
 * returned.
 *
 * @return
 * Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER for a
 * non-address resource type, or STATUS_UNSUCCESSFUL when the HAL
 * cannot translate the address.
 */
static
NTSTATUS
IopArbTranslateBusAddress(
    _In_ UCHAR SourceType,
    _In_ PHYSICAL_ADDRESS SourceAddress,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress,
    _Out_ PUCHAR TranslatedType)
{
    ULONG AddressSpace;

    if (SourceType == CmResourceTypeMemory || SourceType == CmResourceTypeMemoryLarge)
        AddressSpace = 0; /* system memory */
    else if (SourceType == CmResourceTypePort)
        AddressSpace = 1; /* I/O port space */
    else
        return STATUS_INVALID_PARAMETER;

    if (!HalTranslateBusAddress(Isa, 0, SourceAddress, &AddressSpace, TranslatedAddress))
        return STATUS_UNSUCCESSFUL;

    if (AddressSpace == 1 || AddressSpace == 3)
    {
        *TranslatedType = CmResourceTypePort;
    }
    else
    {
        *TranslatedType = (SourceType == CmResourceTypeMemoryLarge)
                              ? CmResourceTypeMemoryLarge
                              : CmResourceTypeMemory;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Extracts the placement window from one port/memory requirement
 * descriptor. The shared UnpackRequirement callback of the Root
 * Port and Root Memory arbiters.
 *
 * @param[in] IoDescriptor
 * The requirement to decode. Every port/memory sub-encoding is
 * handled, including the 40/48/64-bit large-memory forms.
 *
 * @param[out] OutMinimumAddress
 * Receives the lowest address the requirement accepts.
 *
 * @param[out] OutMaximumAddress
 * Receives the highest address the requirement accepts. A legacy
 * 24-bit ISA memory card (CM_RESOURCE_MEMORY_24) is clamped to
 * the low 16 MB it can physically decode.
 *
 * @param[out] OutLength
 * Receives the requirement's length.
 *
 * @param[out] OutAlignment
 * Receives the requirement's alignment; a zero alignment is
 * normalised to byte alignment.
 *
 * @return
 * Returns STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
IopSharedUnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor,
    _Out_ PUINT64 OutMinimumAddress,
    _Out_ PUINT64 OutMaximumAddress,
    _Out_ PUINT64 OutLength,
    _Out_ PUINT64 OutAlignment)
{
    PAGED_CODE();

    *OutLength = RtlIoDecodeMemIoResource(IoDescriptor,
                                          OutAlignment,
                                          OutMinimumAddress,
                                          OutMaximumAddress);

    if (*OutAlignment == 0)
        *OutAlignment = 1;

    if (IoDescriptor->Type == CmResourceTypeMemory &&
        (IoDescriptor->Flags & CM_RESOURCE_MEMORY_24) &&
        *OutMaximumAddress > 0xFFFFFF)
    {
        *OutMaximumAddress = 0xFFFFFF;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Materialises the arbiter's chosen placement as an assigned CM
 * descriptor. The shared PackResource callback of the Root Port
 * and Root Memory arbiters.
 *
 * @param[in] IoDescriptor
 * The requirement the placement satisfies. Type, Flags and
 * ShareDisposition (which carry any large-memory encoding) are
 * copied from it, and the still-encoded Length carries across
 * unchanged.
 *
 * @param[in] Start
 * The chosen start address.
 *
 * @param[out] CmDescriptor
 * Receives the assigned descriptor.
 *
 * @return
 * Returns STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
IopSharedPackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor,
    _In_ UINT64 Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDescriptor)
{
    PAGED_CODE();

    CmDescriptor->Type = IoDescriptor->Type;
    CmDescriptor->Flags = IoDescriptor->Flags;
    CmDescriptor->ShareDisposition = IoDescriptor->ShareDisposition;

    CmDescriptor->u.Generic.Start.QuadPart = Start;
    CmDescriptor->u.Generic.Length = IoDescriptor->u.Generic.Length;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the placement back out of an assigned CM descriptor -
 * e.g. a firmware boot configuration - so the arbiter can mark
 * that span occupied. The inverse of IopSharedPackResource and
 * the shared UnpackResource callback of the Root Port and Root
 * Memory arbiters.
 *
 * @param[in] CmDescriptor
 * The assigned descriptor to decode; the large-memory forms are
 * handled.
 *
 * @param[out] Start
 * Receives the assigned start address.
 *
 * @param[out] OutLength
 * Receives the assigned length.
 *
 * @return
 * Returns STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
IopSharedUnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDescriptor,
    _Out_ PUINT64 Start,
    _Out_ PUINT64 OutLength)
{
    PAGED_CODE();

    *OutLength = RtlCmDecodeMemIoResource(CmDescriptor, Start);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Scores a requirement's constrainedness: the number of distinct
 * aligned positions at which it could be placed inside its own
 * window, ignoring what is already allocated. The shared
 * ScoreRequirement callback of the Root Port and Root Memory
 * arbiters.
 *
 * @param[in] IoDescriptor
 * The requirement to score.
 *
 * @return
 * Returns the placement count - the solver places the
 * most-constrained device (lowest score) first. A window that
 * cannot hold even one copy scores -1 (invalid); a huge count
 * saturates to MAXLONG.
 */
INT32
NTAPI
IopSharedScoreRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    UINT64 Length, Alignment, Minimum, Maximum, AlignedMinimum;
    INT64 Placements;

    PAGED_CODE();

    Length = RtlIoDecodeMemIoResource(IoDescriptor, &Alignment, &Minimum, &Maximum);

    if (Alignment == 0)
        Alignment = 1;

    /* Round the window's lower bound up to the next alignment boundary. */
    AlignedMinimum = (Minimum + Alignment - 1) & ~(Alignment - 1);

    Placements = (INT64)((Maximum - AlignedMinimum - Length + 1) / Alignment) + 1;

    if (Placements < 0)
        return -1;

    if (Placements > MAXLONG)
        return MAXLONG;

    return (INT32)Placements;
}

/**
 * @brief
 * Translates one registry allocation-ordering window from
 * bus-relative addresses into the system-physical space the
 * arbiter allocates in. The shared TranslateOrdering function of
 * the Root Port and Root Memory arbiters.
 *
 * @param[out] OutIoDescriptor
 * Receives the translated copy. When either endpoint cannot be
 * translated the entry is marked CmResourceTypeNull so the
 * ordering reader drops it; otherwise the (possibly remapped)
 * translated type is adopted.
 *
 * @param[in] IoDescriptor
 * The ordering window to translate. Non-address descriptors pass
 * through unchanged.
 *
 * @return
 * Returns STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
IopSharedTranslateOrdering(
    _Out_ PIO_RESOURCE_DESCRIPTOR OutIoDescriptor,
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    UCHAR SourceType;
    UCHAR MinType;
    UCHAR MaxType;

    PAGED_CODE();

    *OutIoDescriptor = *IoDescriptor;

    SourceType = IoDescriptor->Type;
    if (SourceType != CmResourceTypePort &&
        SourceType != CmResourceTypeMemory &&
        SourceType != CmResourceTypeMemoryLarge)
    {
        return STATUS_SUCCESS;
    }

    MinType = SourceType;
    MaxType = SourceType;

    if (!NT_SUCCESS(IopArbTranslateBusAddress(SourceType,
                                              IoDescriptor->u.Generic.MinimumAddress,
                                              &OutIoDescriptor->u.Generic.MinimumAddress,
                                              &MinType)) ||
        !NT_SUCCESS(IopArbTranslateBusAddress(SourceType,
                                              IoDescriptor->u.Generic.MaximumAddress,
                                              &OutIoDescriptor->u.Generic.MaximumAddress,
                                              &MaxType)))
    {
        OutIoDescriptor->Type = CmResourceTypeNull;
    }
    else
    {
        OutIoDescriptor->Type = MaxType;
    }

    return STATUS_SUCCESS;
}
