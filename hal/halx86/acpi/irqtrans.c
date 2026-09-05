/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ISA interrupt translation for the ACPI HALs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* ISA IRQ to global system interrupt mapping, defined in halacpi.c */
extern ULONG HalpPicVectorRedirect[16];

/* Values from this base up (except MAXULONG) are not ISA IRQs */
#define HALP_ISA_TOKEN_BASE     0xFFFF0000
#define HALP_ISA_IRQ_COUNT      16

/* IRQ 2 is the PIC cascade and is delivered as IRQ 9 */
#define HALP_ISA_CASCADE_IRQ    2
#define HALP_ISA_CASCADE_TARGET 9

/* A requirement is split into at most 3 ranges, plus one for the SCI split */
#define HALP_ISA_MAX_RANGES     4

typedef struct _HALP_IRQ_RANGE
{
    ULONG First;
    ULONG Last;
} HALP_IRQ_RANGE, *PHALP_IRQ_RANGE;

typedef struct _HALP_IRQ_RANGE_LIST
{
    HALP_IRQ_RANGE Range[HALP_ISA_MAX_RANGES];
    ULONG Count;
    ULONG SciIrq;
    HALP_IRQ_RANGE AfterSci;
    BOOLEAN HasAfterSci;
} HALP_IRQ_RANGE_LIST, *PHALP_IRQ_RANGE_LIST;

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
HalpIsInterruptToken(
    _In_ ULONG Value)
{
    return (Value >= HALP_ISA_TOKEN_BASE) && (Value != MAXULONG);
}

static
NTSTATUS
HalpGlobalInterruptToIsaIrq(
    _In_ ULONG GlobalInterrupt,
    _Out_ PULONG Irq)
{
    ULONG i;

    for (i = 0; i < HALP_ISA_IRQ_COUNT; i++)
    {
        if (HalpPicVectorRedirect[i] == GlobalInterrupt)
        {
            *Irq = i;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Adds an IRQ range to the list without the SCI IRQ. The part above the SCI
 * is kept aside and goes at the end of the list.
 */
static
VOID
HalpAddIrqRange(
    _Inout_ PHALP_IRQ_RANGE_LIST List,
    _In_ ULONG First,
    _In_ ULONG Last)
{
    ULONG Sci = List->SciIrq;

    if ((Sci < First) || (Sci > Last))
    {
        List->Range[List->Count].First = First;
        List->Range[List->Count].Last = Last;
        List->Count++;
        return;
    }

    if (Sci > First)
    {
        List->Range[List->Count].First = First;
        List->Range[List->Count].Last = Sci - 1;
        List->Count++;
    }

    if (Sci < Last)
    {
        List->AfterSci.First = Sci + 1;
        List->AfterSci.Last = Last;
        List->HasAfterSci = TRUE;
    }
}

/* Returns the last IRQ of a run that maps to consecutive global interrupts */
static
ULONG
HalpGetIrqRunEnd(
    _In_ ULONG Irq,
    _In_ ULONG Last)
{
    while ((Irq < Last) &&
           (HalpPicVectorRedirect[Irq + 1] - HalpPicVectorRedirect[Irq] == 1))
    {
        Irq++;
    }

    return Irq;
}

static
BOOLEAN
HalpRangeHasIrq(
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _In_ ULONG Irq)
{
    return (Descriptor->u.Interrupt.MinimumVector <= Irq) &&
           (Descriptor->u.Interrupt.MaximumVector >= Irq);
}

/**
 * @brief
 * Translates an ISA interrupt requirement to global system interrupts.
 * IRQ 2 is replaced with IRQ 9, the SCI IRQ is removed, and each run of
 * consecutive global interrupts becomes one alternative.
 */
static
NTSTATUS
NTAPI
HalpTranslateIsaInterruptRequirement(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    HALP_IRQ_RANGE_LIST List;
    PIO_RESOURCE_DESCRIPTOR Output;
    ULONG Minimum, Maximum, Irq, RunEnd, Count, i;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    Minimum = Source->u.Interrupt.MinimumVector;
    Maximum = Source->u.Interrupt.MaximumVector;

    if (HalpIsInterruptToken(Minimum))
    {
        Output = ExAllocatePoolWithTag(PagedPool, sizeof(*Output), TAG_HAL);
        if (Output == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        *Output = *Source;
        *Target = Output;
        *TargetCount = 1;
        return STATUS_SUCCESS;
    }

    if ((Minimum >= HALP_ISA_IRQ_COUNT) ||
        (Maximum >= HALP_ISA_IRQ_COUNT) ||
        (Maximum < Minimum))
    {
        return STATUS_UNSUCCESSFUL;
    }

    RtlZeroMemory(&List, sizeof(List));
    List.SciIrq = HalpFixedAcpiDescTable.sci_int_vector;

    if ((Minimum <= HALP_ISA_CASCADE_IRQ) && (Maximum >= HALP_ISA_CASCADE_IRQ))
    {
        if (Minimum < HALP_ISA_CASCADE_IRQ)
        {
            HalpAddIrqRange(&List, Minimum, HALP_ISA_CASCADE_IRQ - 1);
        }
        if (Maximum > HALP_ISA_CASCADE_IRQ)
        {
            HalpAddIrqRange(&List, HALP_ISA_CASCADE_IRQ + 1, Maximum);
        }
        if ((Minimum > HALP_ISA_CASCADE_TARGET) || (Maximum < HALP_ISA_CASCADE_TARGET))
        {
            HalpAddIrqRange(&List, HALP_ISA_CASCADE_TARGET, HALP_ISA_CASCADE_TARGET);
        }
    }
    else
    {
        HalpAddIrqRange(&List, Minimum, Maximum);
    }

    if (List.HasAfterSci)
    {
        List.Range[List.Count++] = List.AfterSci;
    }

    /* Count the runs to size the output */
    Count = 0;
    for (i = 0; i < List.Count; i++)
    {
        for (Irq = List.Range[i].First; Irq <= List.Range[i].Last; Irq = RunEnd + 1)
        {
            RunEnd = HalpGetIrqRunEnd(Irq, List.Range[i].Last);
            Count++;
        }
    }

    *TargetCount = Count;
    if (Count == 0)
    {
        *Target = NULL;
        return STATUS_SUCCESS;
    }

    Output = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Output), TAG_HAL);
    if (Output == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Count = 0;
    for (i = 0; i < List.Count; i++)
    {
        for (Irq = List.Range[i].First; Irq <= List.Range[i].Last; Irq = RunEnd + 1)
        {
            RunEnd = HalpGetIrqRunEnd(Irq, List.Range[i].Last);

            Output[Count] = *Source;
            if (Count > 0)
            {
                Output[Count].Option = IO_RESOURCE_ALTERNATIVE;
            }
            Output[Count].u.Interrupt.MinimumVector = HalpPicVectorRedirect[Irq];
            Output[Count].u.Interrupt.MaximumVector = HalpPicVectorRedirect[RunEnd];
            Count++;
        }
    }

    *Target = Output;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Checks if an IRQ 9 assignment is really the cascaded IRQ 2. That is the
 * case when a requirement allows IRQ 2 before any requirement allows IRQ 9.
 */
static
BOOLEAN
HalpIsCascadeAssignment(
    _In_ ULONG AlternativesCount,
    _In_reads_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[])
{
    BOOLEAN AllowsCascade = FALSE;
    ULONG i;

    for (i = 0; i < AlternativesCount; i++)
    {
        if (HalpRangeHasIrq(&Alternatives[i], HALP_ISA_CASCADE_TARGET))
        {
            return FALSE;
        }

        if (HalpRangeHasIrq(&Alternatives[i], HALP_ISA_CASCADE_IRQ))
        {
            AllowsCascade = TRUE;
        }
    }

    return AllowsCascade;
}

/* Translates an assigned ISA interrupt between IRQs and global system interrupts */
static
NTSTATUS
NTAPI
HalpTranslateIsaInterruptResource(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    ULONG Level, Vector, Irq;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *Target = *Source;

    Level = Source->u.Interrupt.Level;
    Vector = Source->u.Interrupt.Vector;
    if (HalpIsInterruptToken(Vector))
    {
        return STATUS_SUCCESS;
    }

    switch (Direction)
    {
        case TranslateChildToParent:
            if ((Level >= HALP_ISA_IRQ_COUNT) || (Vector >= HALP_ISA_IRQ_COUNT))
            {
                return STATUS_UNSUCCESSFUL;
            }
            Target->u.Interrupt.Level = HalpPicVectorRedirect[Level];
            Target->u.Interrupt.Vector = HalpPicVectorRedirect[Vector];
            return STATUS_SUCCESS;

        case TranslateParentToChild:
            break;

        default:
            return STATUS_SUCCESS;
    }

    Status = HalpGlobalInterruptToIsaIrq(Level, &Irq);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    Target->u.Interrupt.Level = Irq;

    Status = HalpGlobalInterruptToIsaIrq(Vector, &Irq);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    Target->u.Interrupt.Vector = Irq;

    if ((Target->u.Interrupt.Level == HALP_ISA_CASCADE_TARGET) &&
        HalpIsCascadeAssignment(AlternativesCount, Alternatives))
    {
        Target->u.Interrupt.Level = HALP_ISA_CASCADE_IRQ;
        Target->u.Interrupt.Vector = HALP_ISA_CASCADE_IRQ;
    }

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HalpIsaTranslatorReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/* Maps a line of the PIC HAL to its vector, or a vector back to its line */
static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpTranslatePicLine(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    KAFFINITY Affinity;
    KIRQL Irql;
    ULONG Vector;
    UCHAR Line;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(AlternativesCount);
    UNREFERENCED_PARAMETER(Alternatives);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *Target = *Source;

    if (Direction == TranslateChildToParent)
    {
        Vector = HalpGetRootInterruptVector(Source->u.Interrupt.Level,
                                            Source->u.Interrupt.Vector,
                                            &Irql,
                                            &Affinity);
        if (Vector == 0)
            return STATUS_UNSUCCESSFUL;

        Target->u.Interrupt.Level = Irql;
        Target->u.Interrupt.Vector = Vector;
        Target->u.Interrupt.Affinity = Affinity;
        return STATUS_TRANSLATION_COMPLETE;
    }

    if (Direction == TranslateParentToChild)
    {
        if (Source->u.Interrupt.Vector > MAXUCHAR)
            return STATUS_UNSUCCESSFUL;

        Line = HalpVectorToIrq((UCHAR)Source->u.Interrupt.Vector);
        if (Line >= HALP_ISA_IRQ_COUNT)
            return STATUS_UNSUCCESSFUL;

        Target->u.Interrupt.Level = Line;
        Target->u.Interrupt.Vector = Line;
        Target->u.Interrupt.Affinity = (KAFFINITY)-1;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpTranslatePicLineRequirement(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR Output;
    KAFFINITY Affinity;
    KIRQL Irql;
    ULONG First, Last;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *TargetCount = 0;
    *Target = NULL;

    First = HalpGetRootInterruptVector(Source->u.Interrupt.MinimumVector,
                                       Source->u.Interrupt.MinimumVector,
                                       &Irql,
                                       &Affinity);
    Last = HalpGetRootInterruptVector(Source->u.Interrupt.MaximumVector,
                                      Source->u.Interrupt.MaximumVector,
                                      &Irql,
                                      &Affinity);
    if ((First == 0) || (Last == 0))
        return STATUS_UNSUCCESSFUL;

    Output = ExAllocatePoolWithTag(PagedPool, sizeof(*Output), TAG_HAL);
    if (Output == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Output = *Source;
    Output->u.Interrupt.MinimumVector = First;
    Output->u.Interrupt.MaximumVector = Last;

    *TargetCount = 1;
    *Target = Output;
    return STATUS_TRANSLATION_COMPLETE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Returns the interrupt translator of the HAL bus devices on the PIC HAL,
 * which turns the SCI line into its vector.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpQueryPicLineTranslator(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length)
{
    PTRANSLATOR_INTERFACE Translator = Interface;

    PAGED_CODE();

    *Length = sizeof(TRANSLATOR_INTERFACE);
    if (Size < sizeof(TRANSLATOR_INTERFACE))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Translator, sizeof(TRANSLATOR_INTERFACE));
    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->InterfaceReference = HalpIsaTranslatorReference;
    Translator->InterfaceDereference = HalpIsaTranslatorReference;
    Translator->TranslateResources = HalpTranslatePicLine;
    Translator->TranslateResourceRequirements = HalpTranslatePicLineRequirement;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns the ISA interrupt translator for ISA, EISA and MCA bridges.
 *
 * @param[in] BridgeInterfaceType
 * Bus type below the bridge.
 *
 * @param[in] Size
 * Size of the translator interface buffer.
 *
 * @param[out] Translator
 * Receives the translator interface.
 */
NTSTATUS
NTAPI
HaliGetInterruptTranslator(
    _In_ INTERFACE_TYPE ParentInterfaceType,
    _In_ ULONG ParentBusNumber,
    _In_ INTERFACE_TYPE BridgeInterfaceType,
    _In_ USHORT Size,
    _In_ USHORT Version,
    _Out_ PTRANSLATOR_INTERFACE Translator,
    _Out_ PULONG BridgeBusNumber)
{
    UNREFERENCED_PARAMETER(ParentInterfaceType);
    UNREFERENCED_PARAMETER(ParentBusNumber);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(BridgeBusNumber);

    PAGED_CODE();

    switch (BridgeInterfaceType)
    {
        case InterfaceTypeUndefined:
        case Isa:
        case Eisa:
        case MicroChannel:
            break;

        default:
            return STATUS_NOT_IMPLEMENTED;
    }

    if (Size < sizeof(TRANSLATOR_INTERFACE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Translator, sizeof(TRANSLATOR_INTERFACE));
    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->InterfaceReference = HalpIsaTranslatorReference;
    Translator->InterfaceDereference = HalpIsaTranslatorReference;
    Translator->TranslateResources = HalpTranslateIsaInterruptResource;
    Translator->TranslateResourceRequirements = HalpTranslateIsaInterruptRequirement;

    return STATUS_SUCCESS;
}

/* EOF */
