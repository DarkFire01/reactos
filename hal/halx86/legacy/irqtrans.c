/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Legacy HAL IRQ Translator
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>

#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
NTAPI
HalpLegacyPCTranslatorReference(PVOID Context)
{
    NOTHING;
}

static
NTSTATUS
NTAPI
HalpIrqTranslateResourcesRoot(
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
    ULONG Vector;

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
            Target->u.Interrupt.Level = HalpVectorToIrq((UCHAR)Source->u.Interrupt.Vector);
            Target->u.Interrupt.Vector = Target->u.Interrupt.Level;
            Target->u.Interrupt.Affinity = (KAFFINITY)-1;
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/* Translates an IRQ requirement to the system vectors of its IRQs */
static
NTSTATUS
NTAPI
HalpIrqTranslateRequirementsRoot(
    _Inout_opt_ PVOID Context,
    _In_ PIO_RESOURCE_DESCRIPTOR Source,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG TargetCount,
    _Out_ PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR Output;
    KAFFINITY Affinity;
    KIRQL Irql;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    Output = ExAllocatePoolWithTag(PagedPool, sizeof(*Output), TAG_HAL);
    *Target = Output;
    if (Output == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    *TargetCount = 1;
    *Output = *Source;
    Output->u.Interrupt.MinimumVector = HalpGetRootInterruptVector(Source->u.Interrupt.MinimumVector,
                                                                   Source->u.Interrupt.MinimumVector,
                                                                   &Irql,
                                                                   &Affinity);
    Output->u.Interrupt.MaximumVector = HalpGetRootInterruptVector(Source->u.Interrupt.MaximumVector,
                                                                   Source->u.Interrupt.MaximumVector,
                                                                   &Irql,
                                                                   &Affinity);

    return STATUS_TRANSLATION_COMPLETE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Query the legacy PC IRQ translator, which pairs with the legacy PC arbiter
 * on the HAL bus FDO.
 *
 * @param[in] BusFdo
 * Hal Bus FDO deviceobject
 *
 * @param[out] Interface
 * Caller-supplied buffer that receives the TRANSLATOR_INTERFACE.
 *
 * @param[in] Size
 * Size of the Interface buffer, in bytes.
 *
 * @param[out] Length
 * Receives sizeof(TRANSLATOR_INTERFACE), whether or not the buffer was large
 * enough.
 *
 * @return
 * STATUS_SUCCESS on success, or STATUS_BUFFER_TOO_SMALL if Size is smaller
 * than sizeof(TRANSLATOR_INTERFACE).
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
    PTRANSLATOR_INTERFACE Translator = Interface;

    PAGED_CODE();

    *Length = sizeof(TRANSLATOR_INTERFACE);
    if (Size < sizeof(TRANSLATOR_INTERFACE))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Translator, sizeof(TRANSLATOR_INTERFACE));
    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = 0;
    Translator->Context = BusFdo;
    Translator->InterfaceReference = HalpLegacyPCTranslatorReference;
    Translator->InterfaceDereference = HalpLegacyPCTranslatorReference;
    Translator->TranslateResources = HalpIrqTranslateResourcesRoot;
    Translator->TranslateResourceRequirements = HalpIrqTranslateRequirementsRoot;

    return STATUS_SUCCESS;
}

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
    UNREFERENCED_PARAMETER(BridgeInterfaceType);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Translator);
    UNREFERENCED_PARAMETER(BridgeBusNumber);

    PAGED_CODE();

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
