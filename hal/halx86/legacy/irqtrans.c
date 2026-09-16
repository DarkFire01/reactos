/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Interrupt translators of the legacy PC HALs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Lines used by PCI devices, defined in bussupp.c */
extern ULONG HalpPciIrqMask;

#define HALP_ISA_LINE_COUNT         16
#define HALP_ISA_LINE_BIT(Line)     (1UL << (Line))

/* IRQ 2 is the PIC cascade, a device set to it is wired to IRQ 9 */
#define HALP_ISA_CASCADE_LINE       2
#define HALP_ISA_CASCADE_TARGET     9

/* PRIVATE FUNCTIONS **********************************************************/

/* The translators have no state, so there is nothing to count */
static
VOID
NTAPI
HalpIrqTranslatorNoReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
ULONG
NTAPI
HalpLineToVector(
    _In_ ULONG Line)
{
    KAFFINITY Affinity;
    KIRQL Irql;

    return HalpGetRootInterruptVector(Line, Line, &Irql, &Affinity);
}

/* Returns the line a vector was mapped from, without mapping any line */
static
BOOLEAN
NTAPI
HalpVectorToLine(
    _In_ ULONG Vector,
    _Out_ PULONG Line)
{
    UCHAR Irq;

    if (Vector > MAXUCHAR)
        return FALSE;

    /* The APIC HAL only knows the lines that already have a vector */
    Irq = HalpVectorToIrq((UCHAR)Vector);
    if (HalpIrqToVector(Irq) != (UCHAR)Vector)
        return FALSE;

    /* Also rejects values that are not lines. A line with a vector is not mapped again */
    if (HalpLineToVector(Irq) != Vector)
        return FALSE;

    *Line = Irq;
    return TRUE;
}

/* Maps an assigned line to its vector, and a vector back to its line */
static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateVector(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    KAFFINITY Affinity = 0;
    KIRQL Irql = 0;
    ULONG Vector, Line;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(AlternativesCount);
    UNREFERENCED_PARAMETER(Alternatives);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *Target = *Source;

    switch (Direction)
    {
        case TranslateChildToParent:
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

        case TranslateParentToChild:
            if (!HalpVectorToLine(Source->u.Interrupt.Vector, &Line))
                return STATUS_UNSUCCESSFUL;

            Target->u.Interrupt.Level = Line;
            Target->u.Interrupt.Vector = Line;
            Target->u.Interrupt.Affinity = (KAFFINITY)-1;
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateVectorRange(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR Output;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *TargetCount = 0;
    *Target = NULL;

    /* Lines arbitrated on the HAL bus FDO are never translated as a range */
    if (HalpLegacyPCArbitratesIrqs())
        return STATUS_NOT_SUPPORTED;

    Output = ExAllocatePoolWithTag(PagedPool, sizeof(*Output), TAG_HAL);
    if (Output == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Output = *Source;
    Output->u.Interrupt.MinimumVector = HalpLineToVector(Source->u.Interrupt.MinimumVector);
    Output->u.Interrupt.MaximumVector = HalpLineToVector(Source->u.Interrupt.MaximumVector);

    *TargetCount = 1;
    *Target = Output;
    return STATUS_TRANSLATION_COMPLETE;
}

/* Checks if IRQ 9 stands for IRQ 2, which is when a device takes IRQ 2 but not IRQ 9 */
static
BOOLEAN
NTAPI
HalpDeviceWantsCascadeLine(
    _In_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[])
{
    BOOLEAN TakesCascade = FALSE;
    ULONG Index;

    if (Alternatives == NULL)
        return FALSE;

    for (Index = 0; Index < AlternativesCount; Index++)
    {
        ULONG Minimum = Alternatives[Index].u.Interrupt.MinimumVector;
        ULONG Maximum = Alternatives[Index].u.Interrupt.MaximumVector;

        /* Only an alternative naming the one line counts */
        if ((Minimum >= HALP_ISA_CASCADE_TARGET) && (Maximum <= HALP_ISA_CASCADE_TARGET))
            return FALSE;

        if ((Minimum >= HALP_ISA_CASCADE_LINE) && (Maximum <= HALP_ISA_CASCADE_LINE))
            TakesCascade = TRUE;
    }

    return TakesCascade;
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateIsaLine(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR Line = *Source;
    NTSTATUS Status;

    PAGED_CODE();

    if ((Direction == TranslateChildToParent) &&
        (Line.u.Interrupt.Vector == HALP_ISA_CASCADE_LINE))
    {
        Line.u.Interrupt.Level = HALP_ISA_CASCADE_TARGET;
        Line.u.Interrupt.Vector = HALP_ISA_CASCADE_TARGET;
    }

    if (!HalpLegacyPCArbitratesIrqs())
    {
        Status = HalpLegacyTranslateVector(Context,
                                           &Line,
                                           Direction,
                                           AlternativesCount,
                                           Alternatives,
                                           PhysicalDeviceObject,
                                           Target);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        /* The line stays a line for the arbiter of the HAL bus FDO */
        *Target = Line;
        if ((Direction != TranslateChildToParent) && (Direction != TranslateParentToChild))
            return STATUS_INVALID_PARAMETER;

        Status = STATUS_SUCCESS;
    }

    if ((Direction == TranslateParentToChild) &&
        (Target->u.Interrupt.Level == HALP_ISA_CASCADE_TARGET) &&
        HalpDeviceWantsCascadeLine(AlternativesCount, Alternatives))
    {
        Target->u.Interrupt.Level = HALP_ISA_CASCADE_LINE;
        Target->u.Interrupt.Vector = HALP_ISA_CASCADE_LINE;
    }

    return Status;
}

/* Returns the lines an ISA requirement can really use */
static
ULONG
NTAPI
HalpGetUsableIsaLines(
    _In_ PIO_RESOURCE_DESCRIPTOR Source)
{
    ULONG Minimum = Source->u.Interrupt.MinimumVector;
    ULONG Maximum = Source->u.Interrupt.MaximumVector;
    ULONG Lines = 0;
    ULONG Line;

    for (Line = Minimum; (Line <= Maximum) && (Line < HALP_ISA_LINE_COUNT); Line++)
    {
        Lines |= HALP_ISA_LINE_BIT(Line);
    }

    if (Lines & HALP_ISA_LINE_BIT(HALP_ISA_CASCADE_LINE))
    {
        Lines &= ~HALP_ISA_LINE_BIT(HALP_ISA_CASCADE_LINE);
        Lines |= HALP_ISA_LINE_BIT(HALP_ISA_CASCADE_TARGET);
    }

    /* ISA devices can't share a line with PCI devices */
    return Lines & ~HalpPciIrqMask;
}

/* Writes one alternative for each run of consecutive lines, or only counts them */
static
ULONG
NTAPI
HalpBuildIsaAlternatives(
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ ULONG Lines,
    _In_ BOOLEAN ToVectors,
    _Out_opt_ PIO_RESOURCE_DESCRIPTOR Output)
{
    ULONG Count = 0;
    ULONG First, Last;

    for (First = 0; First < HALP_ISA_LINE_COUNT; First = Last + 1)
    {
        Last = First;
        if (!(Lines & HALP_ISA_LINE_BIT(First)))
            continue;

        while ((Last + 1 < HALP_ISA_LINE_COUNT) && (Lines & HALP_ISA_LINE_BIT(Last + 1)))
        {
            Last++;
        }

        if (Output != NULL)
        {
            Output[Count] = *Source;
            if (Count > 0)
                Output[Count].Option = IO_RESOURCE_ALTERNATIVE;

            Output[Count].u.Interrupt.MinimumVector = ToVectors ? HalpLineToVector(First) : First;
            Output[Count].u.Interrupt.MaximumVector = ToVectors ? HalpLineToVector(Last) : Last;
        }

        Count++;
    }

    return Count;
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateIsaRange(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR Output;
    NTSTATUS SuccessStatus;
    BOOLEAN ToVectors;
    ULONG Lines, Count;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *TargetCount = 0;
    *Target = NULL;

    ToVectors = !HalpLegacyPCArbitratesIrqs();
    SuccessStatus = ToVectors ? STATUS_TRANSLATION_COMPLETE : STATUS_SUCCESS;

    Lines = HalpGetUsableIsaLines(Source);
    Count = HalpBuildIsaAlternatives(Source, Lines, ToVectors, NULL);
    if (Count == 0)
        return SuccessStatus;

    Output = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Output), TAG_HAL);
    if (Output == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    HalpBuildIsaAlternatives(Source, Lines, ToVectors, Output);

    *TargetCount = Count;
    *Target = Output;
    return SuccessStatus;
}

/* On the bus FDO, lines go to its own arbiter, or else they are ISA lines for the root arbiter */
static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateBusLine(
    _Inout_opt_ PVOID Context,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
    _In_opt_ ULONG AlternativesCount,
    _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    PAGED_CODE();

    if (HalpLegacyPCArbitratesIrqs())
    {
        return HalpLegacyTranslateVector(Context,
                                         Source,
                                         Direction,
                                         AlternativesCount,
                                         Alternatives,
                                         PhysicalDeviceObject,
                                         Target);
    }

    return HalpLegacyTranslateIsaLine(Context,
                                      Source,
                                      Direction,
                                      AlternativesCount,
                                      Alternatives,
                                      PhysicalDeviceObject,
                                      Target);
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyTranslateBusLineRange(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PAGED_CODE();

    if (HalpLegacyPCArbitratesIrqs())
        return HalpLegacyTranslateVectorRange(Context,
                                              Source,
                                              PhysicalDeviceObject,
                                              TargetCount,
                                              Target);

    return HalpLegacyTranslateIsaRange(Context, Source, PhysicalDeviceObject, TargetCount, Target);
}

static
VOID
NTAPI
HalpFillIrqTranslator(
    _Out_ PTRANSLATOR_INTERFACE Translator,
    _In_opt_ PVOID Context,
    _In_ PTRANSLATE_RESOURCE_HANDLER TranslateResources,
    _In_ PTRANSLATE_RESOURCE_REQUIREMENTS_HANDLER TranslateRequirements)
{
    RtlZeroMemory(Translator, sizeof(*Translator));
    Translator->Size = sizeof(*Translator);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->Context = Context;
    Translator->InterfaceReference = HalpIrqTranslatorNoReference;
    Translator->InterfaceDereference = HalpIrqTranslatorNoReference;
    Translator->TranslateResources = TranslateResources;
    Translator->TranslateResourceRequirements = TranslateRequirements;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Checks if interrupt lines are arbitrated on the HAL bus FDO, before they
 * are translated to vectors. Otherwise the bridges translate lines to vectors
 * and the vectors are arbitrated at the root.
 */
BOOLEAN
NTAPI
HalpLegacyPCArbitratesIrqs(VOID)
{
    /* The APIC HAL has no fixed line to vector mapping, it assigns a vector
       when a line is first mapped. There is no $PIR routing on the PIC HAL */
    return (HalpInterruptControllerType == 1);
}

/**
 * @brief
 * Returns the interrupt translator of the HAL bus FDO. It maps lines to
 * vectors behind the IRQ arbiter of the FDO, or translates ISA lines when
 * the root arbitrates vectors.
 *
 * @param[in] BusFdo
 * The HAL bus FDO.
 *
 * @param[out] Interface
 * Receives the TRANSLATOR_INTERFACE.
 *
 * @param[in] Size
 * Size of the Interface buffer, in bytes.
 *
 * @param[out] Length
 * Receives the size of a TRANSLATOR_INTERFACE.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_BUFFER_TOO_SMALL if the buffer is too small.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCQueryIrqTranslator(
    _In_ PDEVICE_OBJECT BusFdo,
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length)
{
    PAGED_CODE();

    *Length = sizeof(TRANSLATOR_INTERFACE);
    if (Size < sizeof(TRANSLATOR_INTERFACE))
        return STATUS_BUFFER_TOO_SMALL;

    HalpFillIrqTranslator(Interface,
                          BusFdo,
                          HalpLegacyTranslateBusLine,
                          HalpLegacyTranslateBusLineRange);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns the interrupt translator of a bridge. ISA and EISA bridges get
 * the ISA translator. MCA and PCI bridges get the vector translator, unless
 * their lines are arbitrated on the HAL bus FDO.
 *
 * @param[in] BridgeInterfaceType
 * Bus type below the bridge.
 *
 * @param[out] Translator
 * Receives the translator interface.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_NOT_SUPPORTED if the bridge has no translator.
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
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(BridgeBusNumber);

    switch (BridgeInterfaceType)
    {
        case InterfaceTypeUndefined:
        case Isa:
        case Eisa:
            HalpFillIrqTranslator(Translator,
                                  NULL,
                                  HalpLegacyTranslateIsaLine,
                                  HalpLegacyTranslateIsaRange);
            return STATUS_SUCCESS;

        case MicroChannel:
        case PCIBus:
            if (HalpLegacyPCArbitratesIrqs())
                return STATUS_NOT_SUPPORTED;

            HalpFillIrqTranslator(Translator,
                                  NULL,
                                  HalpLegacyTranslateVector,
                                  HalpLegacyTranslateVectorRange);
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

/* EOF */
