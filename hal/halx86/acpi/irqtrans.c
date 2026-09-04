/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ISA interrupt translation for the ACPI HALs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Devices behind an ISA bridge describe their interrupts as ISA IRQ lines,
 * while everything above them (the interrupt arbiters, the ACPI driver, the
 * APIC programming) works in global system interrupts. The firmware may
 * remap individual lines (interrupt source overrides in the MADT), the
 * cascade line IRQ 2 is never usable and shows up as IRQ 9, and the SCI line
 * is the ACPI driver's alone. This file provides the translator the PnP manager
 * obtains through HalGetInterruptTranslator to move descriptors between the
 * two views.
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* ISA IRQ line -> global system interrupt (identity unless the firmware
   overrides a line), and the polarity/trigger flags of each override */
extern ULONG HalpPicVectorRedirect[16];
extern ULONG HalpPicVectorFlags[16];

/* Interrupt values at or above this are tokens (message interrupts and the
   like), never ISA lines; they pass through untouched */
#define HALP_ISA_TOKEN_BASE     0xFFFF0000
#define HALP_ISA_LINE_COUNT     16
#define HALP_ISA_CASCADE_LINE   2
#define HALP_ISA_CASCADE_TARGET 9

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Maps a global system interrupt back to the ISA line that produces it.
 */
static
NTSTATUS
HalpGlobalInterruptToIsaLine(
    _In_ ULONG GlobalInterrupt,
    _Out_ PULONG IsaLine)
{
    ULONG Line;

    for (Line = 0; Line < HALP_ISA_LINE_COUNT; Line++)
    {
        if (HalpPicVectorRedirect[Line] == GlobalInterrupt)
        {
            *IsaLine = Line;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * The ISA line the ACPI SCI is wired to.
 */
static
ULONG
HalpSciLine(VOID)
{
    return HalpFixedAcpiDescTable.sci_int_vector;
}

/**
 * @brief
 * Copies a requirement descriptor with a new line range.
 */
static
VOID
HalpSetLineRange(
    _Out_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _In_ PIO_RESOURCE_DESCRIPTOR Template,
    _In_ ULONG Minimum,
    _In_ ULONG Maximum)
{
    *Descriptor = *Template;
    Descriptor->u.Interrupt.MinimumVector = Minimum;
    Descriptor->u.Interrupt.MaximumVector = Maximum;
}

/**
 * @brief
 * Translates an ISA interrupt requirement into global-interrupt terms.
 * The requested line range is split around the cascade line, gets the
 * cascade target added when it is not already covered, loses the SCI line,
 * and every remaining run
 * of lines is mapped through the firmware overrides. Runs whose targets
 * stay contiguous are emitted as one alternative.
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
    PIO_RESOURCE_DESCRIPTOR Pieces, Output;
    ULONG Minimum, Maximum, PieceCount, OutputCount, SciLine, i;
    SIZE_T Size;
    BOOLEAN SciSplit = FALSE;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    Minimum = Source->u.Interrupt.MinimumVector;
    Maximum = Source->u.Interrupt.MaximumVector;

    /* Tokens are not ISA lines: hand them up unchanged */
    if ((Minimum >= HALP_ISA_TOKEN_BASE) && (Minimum != MAXULONG))
    {
        *Target = ExAllocatePoolWithTag(PagedPool, sizeof(IO_RESOURCE_DESCRIPTOR), TAG_HAL);
        if (*Target == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        **Target = *Source;
        *TargetCount = 1;
        return STATUS_SUCCESS;
    }
    if ((Maximum >= HALP_ISA_TOKEN_BASE) && (Maximum != MAXULONG))
    {
        return STATUS_UNSUCCESSFUL;
    }
    if ((Minimum >= HALP_ISA_LINE_COUNT) || (Maximum >= HALP_ISA_LINE_COUNT) ||
        (Maximum < Minimum))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Room for every line of the range plus the pieces the splits add */
    Size = sizeof(IO_RESOURCE_DESCRIPTOR) * (Maximum - Minimum + 4);
    Pieces = ExAllocatePoolWithTag(PagedPool, Size, TAG_HAL);
    if (Pieces == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Pieces, Size);
    PieceCount = 0;

    /* Split around the cascade line and make sure its target is offered */
    if ((Minimum > HALP_ISA_CASCADE_LINE) || (Maximum < HALP_ISA_CASCADE_LINE))
    {
        HalpSetLineRange(&Pieces[PieceCount++], Source, Minimum, Maximum);
    }
    else
    {
        if (Minimum < HALP_ISA_CASCADE_LINE)
        {
            HalpSetLineRange(&Pieces[PieceCount++], Source, Minimum, HALP_ISA_CASCADE_LINE - 1);
        }
        if (Maximum > HALP_ISA_CASCADE_LINE)
        {
            HalpSetLineRange(&Pieces[PieceCount++], Source, HALP_ISA_CASCADE_LINE + 1, Maximum);
        }
        if ((Minimum > HALP_ISA_CASCADE_TARGET) || (Maximum < HALP_ISA_CASCADE_TARGET))
        {
            HalpSetLineRange(&Pieces[PieceCount++],
                             Source,
                             HALP_ISA_CASCADE_TARGET,
                             HALP_ISA_CASCADE_TARGET);
        }
    }

    /* The SCI line belongs to the ACPI driver: take it out of the range,
       keeping whatever lies on either side of it */
    SciLine = HalpSciLine();
    i = 0;
    while (i < PieceCount)
    {
        ULONG PieceMin = Pieces[i].u.Interrupt.MinimumVector;
        ULONG PieceMax = Pieces[i].u.Interrupt.MaximumVector;

        if ((PieceMin > SciLine) || (PieceMax < SciLine))
        {
            i++;
            continue;
        }
        if (SciSplit)
        {
            ExFreePoolWithTag(Pieces, TAG_HAL);
            return STATUS_INTERNAL_ERROR;
        }
        SciSplit = TRUE;

        if (PieceMax > SciLine)
        {
            HalpSetLineRange(&Pieces[PieceCount++], Source, SciLine + 1, PieceMax);
        }
        if (PieceMin < SciLine)
        {
            Pieces[i].u.Interrupt.MaximumVector = SciLine - 1;
            i++;
        }
        else
        {
            /* Nothing left of this piece: drop it and look at its successor */
            RtlMoveMemory(&Pieces[i], &Pieces[i + 1],
                          (PieceCount - i - 1) * sizeof(IO_RESOURCE_DESCRIPTOR));
            PieceCount--;
        }
    }

    /* Map every piece to global interrupts, one alternative per contiguous run */
    Size = sizeof(IO_RESOURCE_DESCRIPTOR) * (Maximum - Minimum + 4 + PieceCount);
    Output = ExAllocatePoolWithTag(PagedPool, Size, TAG_HAL);
    if (Output == NULL)
    {
        ExFreePoolWithTag(Pieces, TAG_HAL);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Output, Size);
    OutputCount = 0;

    for (i = 0; i < PieceCount; i++)
    {
        ULONG Line = Pieces[i].u.Interrupt.MinimumVector;
        ULONG Last = Pieces[i].u.Interrupt.MaximumVector;

        while (Line <= Last)
        {
            ULONG RunStart = Line;

            while ((Line < Last) &&
                   (HalpPicVectorRedirect[Line] + 1 == HalpPicVectorRedirect[Line + 1]))
            {
                Line++;
            }

            Output[OutputCount] = Pieces[i];
            if (OutputCount != 0)
            {
                Output[OutputCount].Option = IO_RESOURCE_ALTERNATIVE;
            }
            Output[OutputCount].u.Interrupt.MinimumVector = HalpPicVectorRedirect[RunStart];
            Output[OutputCount].u.Interrupt.MaximumVector = HalpPicVectorRedirect[Line];
            OutputCount++;
            Line++;
        }
    }

    ExFreePoolWithTag(Pieces, TAG_HAL);

    *TargetCount = OutputCount;
    if (OutputCount == 0)
    {
        ExFreePoolWithTag(Output, TAG_HAL);
        *Target = NULL;
    }
    else
    {
        *Target = Output;
    }
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Translates an assigned ISA interrupt between the line view of the child
 * and the global-interrupt view of the parent. Going down, a result of IRQ 9
 * that the device could not have asked for is the cascaded IRQ 2.
 */
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
    ULONG Vector, Line;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    *Target = *Source;

    Vector = Source->u.Interrupt.Vector;
    if ((Vector >= HALP_ISA_TOKEN_BASE) && (Vector != MAXULONG))
    {
        return STATUS_SUCCESS;
    }

    if (Direction == TranslateChildToParent)
    {
        if ((Source->u.Interrupt.Level >= HALP_ISA_LINE_COUNT) ||
            (Vector >= HALP_ISA_LINE_COUNT))
        {
            return STATUS_UNSUCCESSFUL;
        }
        Target->u.Interrupt.Level = HalpPicVectorRedirect[Source->u.Interrupt.Level];
        Target->u.Interrupt.Vector = HalpPicVectorRedirect[Vector];
        return STATUS_SUCCESS;
    }

    if (Direction != TranslateParentToChild)
    {
        return STATUS_SUCCESS;
    }

    Status = HalpGlobalInterruptToIsaLine(Source->u.Interrupt.Level, &Line);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    Target->u.Interrupt.Level = Line;

    Status = HalpGlobalInterruptToIsaLine(Vector, &Line);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    Target->u.Interrupt.Vector = Line;

    /* IRQ 9 also stands for the cascaded IRQ 2: pick whichever the device
       could actually have requested */
    if ((Target->u.Interrupt.Level == HALP_ISA_CASCADE_TARGET) && (AlternativesCount != 0))
    {
        BOOLEAN UseCascadeLine = FALSE;
        ULONG i;

        for (i = 0; i < AlternativesCount; i++)
        {
            ULONG AltMin = Alternatives[i].u.Interrupt.MinimumVector;
            ULONG AltMax = Alternatives[i].u.Interrupt.MaximumVector;

            if ((AltMin <= HALP_ISA_CASCADE_TARGET) && (AltMax >= HALP_ISA_CASCADE_TARGET))
            {
                UseCascadeLine = FALSE;
                break;
            }
            if ((AltMin <= HALP_ISA_CASCADE_LINE) && (AltMax >= HALP_ISA_CASCADE_LINE))
            {
                UseCascadeLine = TRUE;
            }
        }

        if (UseCascadeLine)
        {
            Target->u.Interrupt.Level = HALP_ISA_CASCADE_LINE;
            Target->u.Interrupt.Vector = HALP_ISA_CASCADE_LINE;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * The translator has no state to keep alive.
 */
static
VOID
NTAPI
HalpIsaTranslatorReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * The HalGetInterruptTranslator dispatch entry: hands out the ISA line
 * translator for (E)ISA and Micro Channel bridges. Other bridge types
 * (PCI in particular) get no translator from the HAL; their bus drivers
 * or the ACPI driver provide their own.
 *
 * @param[in] ParentInterfaceType
 * The bus above the bridge, unused here.
 *
 * @param[in] ParentBusNumber
 * The bus number above the bridge, unused here.
 *
 * @param[in] BridgeInterfaceType
 * The bus below the bridge whose interrupts need translating.
 *
 * @param[in] Size
 * The size of the caller's TRANSLATOR_INTERFACE.
 *
 * @param[in] Version
 * The interface version the caller wants.
 *
 * @param[out] Translator
 * Receives the translator interface.
 *
 * @param[out] BridgeBusNumber
 * The bus number below the bridge, unused here.
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

    if ((BridgeInterfaceType != InterfaceTypeUndefined) &&
        (BridgeInterfaceType != Isa) &&
        (BridgeInterfaceType != Eisa) &&
        (BridgeInterfaceType != MicroChannel))
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (Size < sizeof(TRANSLATOR_INTERFACE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Translator, sizeof(TRANSLATOR_INTERFACE));
    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->Context = NULL;
    Translator->InterfaceReference = HalpIsaTranslatorReference;
    Translator->InterfaceDereference = HalpIsaTranslatorReference;
    Translator->TranslateResources = HalpTranslateIsaInterruptResource;
    Translator->TranslateResourceRequirements = HalpTranslateIsaInterruptRequirement;

    return STATUS_SUCCESS;
}

/* EOF */
