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
    ULONG First, Last;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *TargetCount = 0;
    *Target = NULL;

    /* A range with an end that has no vector drops out, which is not an error */
    First = HalpLineToVector(Source->u.Interrupt.MinimumVector);
    Last = HalpLineToVector(Source->u.Interrupt.MaximumVector);
    if ((First == 0) || (Last == 0))
        return STATUS_TRANSLATION_COMPLETE;

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
        if (Minimum != Maximum)
            continue;

        if (Minimum == HALP_ISA_CASCADE_TARGET)
            return FALSE;

        if (Minimum == HALP_ISA_CASCADE_LINE)
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

    Status = HalpLegacyTranslateVector(Context,
                                       &Line,
                                       Direction,
                                       AlternativesCount,
                                       Alternatives,
                                       PhysicalDeviceObject,
                                       Target);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((Direction == TranslateParentToChild) &&
        (Target->u.Interrupt.Level == HALP_ISA_CASCADE_TARGET) &&
        HalpDeviceWantsCascadeLine(AlternativesCount, Alternatives))
    {
        Target->u.Interrupt.Level = HALP_ISA_CASCADE_LINE;
        Target->u.Interrupt.Vector = HALP_ISA_CASCADE_LINE;
    }

    return Status;
}

/* The cascade split makes up to three ranges, and taking out a line adds at most one */
#define HALP_ISA_MAX_RANGES     (3 + HALP_ISA_LINE_COUNT)

typedef struct _HALP_LINE_RANGE
{
    ULONG First;
    ULONG Last;
} HALP_LINE_RANGE, *PHALP_LINE_RANGE;

typedef struct _HALP_LINE_RANGE_SET
{
    HALP_LINE_RANGE Range[HALP_ISA_MAX_RANGES];
    ULONG Count;
} HALP_LINE_RANGE_SET, *PHALP_LINE_RANGE_SET;

/* Trigger modes the ELCR reports, read once on EISA machines */
static BOOLEAN HalpEisaTriggersRead;
static ULONG HalpEisaLevelLines;

/* IRQ 12 is never checked against the ELCR */
static ULONG HalpEisaUncheckedLines = HALP_ISA_LINE_BIT(12);

static
VOID
NTAPI
HalpReadEisaTriggers(VOID)
{
    if (HalpEisaTriggersRead)
        return;

    HalpEisaLevelLines = READ_PORT_UCHAR((PUCHAR)EISA_ELCR_MASTER) |
                         (READ_PORT_UCHAR((PUCHAR)EISA_ELCR_SLAVE) << 8);

    /* All clear or all set means there is no usable ELCR */
    if ((HalpEisaLevelLines == 0) || (HalpEisaLevelLines == 0xFFFF))
        HalpEisaUncheckedLines = 0xFFFF;

    HalpEisaTriggersRead = TRUE;
}

static
VOID
NTAPI
HalpAddLineRange(
    _Inout_ PHALP_LINE_RANGE_SET Set,
    _In_ ULONG First,
    _In_ ULONG Last)
{
    ASSERT(Set->Count < HALP_ISA_MAX_RANGES);

    Set->Range[Set->Count].First = First;
    Set->Range[Set->Count].Last = Last;
    Set->Count++;
}

/* Takes a line out of every range, the part above it in a range moves to the end of the set */
static
VOID
NTAPI
HalpRemoveLine(
    _Inout_ PHALP_LINE_RANGE_SET Set,
    _In_ ULONG Line)
{
    PHALP_LINE_RANGE Range;
    ULONG Index, Count;

    /* Ranges split off here start above the line, so they need no visit */
    Count = Set->Count;
    for (Index = 0; Index < Count; Index++)
    {
        Range = &Set->Range[Index];

        if (Range->First == Line)
        {
            Range->First++;
        }
        else if (Range->Last == Line)
        {
            Range->Last -= 1;
        }
        else if ((Range->First < Line) && (Line < Range->Last))
        {
            HalpAddLineRange(Set, Line + 1, Range->Last);
            Range->Last = Line - 1;
        }
    }
}

/**
 * @brief
 * Checks if an ISA requirement has to give up a line.
 *
 * @param[in] Line
 * The ISA line.
 *
 * @param[in] Latched
 * TRUE for an edge triggered requirement.
 *
 * @param[in,out] TriggerMismatch
 * Set to TRUE when the ELCR gives the line the other trigger mode.
 */
static
BOOLEAN
NTAPI
HalpIsIsaLineExcluded(
    _In_ ULONG Line,
    _In_ BOOLEAN Latched,
    _Inout_ PBOOLEAN TriggerMismatch)
{
    BOOLEAN Excluded = FALSE;
    BOOLEAN IsEisa = (HalpBusType == MACHINE_TYPE_EISA);

    /* A PCI line is only shared with a level triggered EISA device */
    if ((HalpPciIrqMask & HALP_ISA_LINE_BIT(Line)) && (Latched || !IsEisa))
        Excluded = TRUE;

    if (IsEisa && !(HalpEisaUncheckedLines & HALP_ISA_LINE_BIT(Line)))
    {
        if (Latched == !(HalpEisaLevelLines & HALP_ISA_LINE_BIT(Line)))
            return Excluded;

        *TriggerMismatch = TRUE;
        Excluded = TRUE;
    }

    return Excluded;
}

/**
 * @brief
 * Translates an ISA interrupt requirement into vector ranges. IRQ 2 gives way to
 * IRQ 9, lines ISA cannot use drop out, and each range left becomes one
 * alternative.
 */
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
    HALP_LINE_RANGE_SET Set;
    PHALP_LINE_RANGE Range;
    PIO_RESOURCE_DESCRIPTOR Output;
    ULONG Minimum, Maximum, Line, Index, Count, First, Last;
    BOOLEAN Latched, TriggerMismatch = FALSE;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *TargetCount = 0;
    *Target = NULL;

    Minimum = Source->u.Interrupt.MinimumVector;
    Maximum = Source->u.Interrupt.MaximumVector;
    RtlZeroMemory(&Set, sizeof(Set));

    /* IRQ 2 is never handed out, a range over it splits and IRQ 9 joins it */
    if ((Minimum <= HALP_ISA_CASCADE_LINE) && (Maximum >= HALP_ISA_CASCADE_LINE))
    {
        if (Minimum < HALP_ISA_CASCADE_LINE)
            HalpAddLineRange(&Set, Minimum, HALP_ISA_CASCADE_LINE - 1);

        if (Maximum > HALP_ISA_CASCADE_LINE)
            HalpAddLineRange(&Set, HALP_ISA_CASCADE_LINE + 1, Maximum);

        if ((Minimum > HALP_ISA_CASCADE_TARGET) || (Maximum < HALP_ISA_CASCADE_TARGET))
            HalpAddLineRange(&Set, HALP_ISA_CASCADE_TARGET, HALP_ISA_CASCADE_TARGET);
    }
    else
    {
        HalpAddLineRange(&Set, Minimum, Maximum);
    }

    if (HalpBusType == MACHINE_TYPE_EISA)
        HalpReadEisaTriggers();

    Latched = (Source->Flags & CM_RESOURCE_INTERRUPT_LATCHED) != 0;
    for (Line = 0; Line < HALP_ISA_LINE_COUNT; Line++)
    {
        if (HalpIsIsaLineExcluded(Line, Latched, &TriggerMismatch))
            HalpRemoveLine(&Set, Line);
    }

    Output = ExAllocatePoolWithTag(PagedPool, Set.Count * sizeof(*Output), TAG_HAL);
    if (Output == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Lines above 15 stay in their range, and a range that maps to no vector drops out */
    Count = 0;
    for (Index = 0; Index < Set.Count; Index++)
    {
        Range = &Set.Range[Index];
        if (Range->First > Range->Last)
            continue;

        First = HalpLineToVector(Range->First);
        Last = HalpLineToVector(Range->Last);
        if ((First == 0) || (Last == 0))
            continue;

        Output[Count] = *Source;
        if (Count > 0)
            Output[Count].Option = IO_RESOURCE_ALTERNATIVE;

        Output[Count].u.Interrupt.MinimumVector = First;
        Output[Count].u.Interrupt.MaximumVector = Last;
        Count++;
    }

    if (Count == 0)
    {
        ExFreePoolWithTag(Output, TAG_HAL);
        return TriggerMismatch ? STATUS_PNP_IRQ_TRANSLATION_FAILED : STATUS_TRANSLATION_COMPLETE;
    }

    *TargetCount = Count;
    *Target = Output;
    return STATUS_TRANSLATION_COMPLETE;
}

static
VOID
NTAPI
HalpFillIrqTranslator(
    _Out_ PTRANSLATOR_INTERFACE Translator,
    _In_ PTRANSLATE_RESOURCE_HANDLER TranslateResources,
    _In_ PTRANSLATE_RESOURCE_REQUIREMENTS_HANDLER TranslateRequirements)
{
    RtlZeroMemory(Translator, sizeof(*Translator));
    Translator->Size = sizeof(*Translator);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
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
    /* Lines are arbitrated on the bus FDO only with $PIR routing, which exists on the PIC HAL
       alone. The APIC HAL has no fixed line to vector mapping, so it never arbitrates lines */
    return HalpLegacyPCIrqRoutingActive();
}

/**
 * @brief
 * Returns the interrupt translator of the HAL bus FDO, which maps the lines
 * its IRQ arbiter assigns to vectors. Only offered while lines are arbitrated
 * on the FDO.
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
 * STATUS_SUCCESS, STATUS_NOT_SUPPORTED without line arbitration, or
 * STATUS_BUFFER_TOO_SMALL if the buffer is too small.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCQueryIrqTranslator(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length)
{
    PTRANSLATOR_INTERFACE Translator = Interface;

    PAGED_CODE();

    *Length = 0;
    if (!HalpLegacyPCArbitratesIrqs())
        return STATUS_NOT_SUPPORTED;

    *Length = sizeof(*Translator);
    if (Size < sizeof(*Translator))
        return STATUS_BUFFER_TOO_SMALL;

    HalpFillIrqTranslator(Translator,
                          HalpLegacyTranslateVector,
                          HalpLegacyTranslateVectorRange);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns the interrupt translator of a bridge. Bridges have none while lines
 * are arbitrated on the HAL bus FDO. Otherwise ISA, EISA and undefined bridges
 * get the ISA translator, and MCA and PCI bridges the vector translator.
 *
 * @param[in] BridgeInterfaceType
 * Bus type below the bridge.
 *
 * @param[in] Size
 * Size of the Translator buffer, in bytes.
 *
 * @param[out] Translator
 * Receives the translator interface.
 *
 * @return
 * STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, or STATUS_NOT_SUPPORTED if the
 * bridge has no translator.
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

    /* With $PIR routing the HAL bus FDO translates every interrupt */
    if (HalpLegacyPCArbitratesIrqs())
        return STATUS_NOT_SUPPORTED;

    if (Size < sizeof(*Translator))
        return STATUS_BUFFER_TOO_SMALL;

    switch (BridgeInterfaceType)
    {
        case InterfaceTypeUndefined:
        case Isa:
        case Eisa:
            HalpFillIrqTranslator(Translator,
                                  HalpLegacyTranslateIsaLine,
                                  HalpLegacyTranslateIsaRange);
            return STATUS_SUCCESS;

        case MicroChannel:
        case PCIBus:
            HalpFillIrqTranslator(Translator,
                                  HalpLegacyTranslateVector,
                                  HalpLegacyTranslateVectorRange);
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

/* EOF */
