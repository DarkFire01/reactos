/*
 * PROJECT:         ReactOS HAL
 * PURPOSE:         Minimal Interrupt Translator Interface (legacy PIC mapping)
 */

#include <hal.h>
#include <ntifs.h>
#include <ntddk.h>
#include <iotypes.h>

#define NDEBUG
#include <debug.h>

/* No-op reference handlers */
static VOID NTAPI HalpIrqTransRef(_In_ PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }
static VOID NTAPI HalpIrqTransDeref(_In_ PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }

static NTSTATUS NTAPI
HalpIrqTranslateResources(_Inout_opt_ PVOID Context,
                          _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
                          _In_ RESOURCE_TRANSLATION_DIRECTION Direction,
                          _In_opt_ ULONG AlternativesCount,
                          _In_reads_opt_(AlternativesCount) IO_RESOURCE_DESCRIPTOR Alternatives[],
                          _In_ PDEVICE_OBJECT PhysicalDeviceObject,
                          _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Direction);
    UNREFERENCED_PARAMETER(AlternativesCount);
    UNREFERENCED_PARAMETER(Alternatives);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    if (!Source || !Target) return STATUS_INVALID_PARAMETER;
    if (Source->Type != CmResourceTypeInterrupt) return STATUS_INVALID_PARAMETER;
    *Target = *Source; /* Identity mapping for legacy PIC stage */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
HalpIrqTranslateRequirements(_Inout_opt_ PVOID Context,
                             _In_ PIO_RESOURCE_DESCRIPTOR Source,
                             _In_ PDEVICE_OBJECT PhysicalDeviceObject,
                             _Out_ PULONG TargetCount,
                             _Out_writes_(*TargetCount) PIO_RESOURCE_DESCRIPTOR *Target)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    if (!Source || !TargetCount || !Target) return STATUS_INVALID_PARAMETER;
    if (Source->Type != CmResourceTypeInterrupt) return STATUS_INVALID_PARAMETER;
    *Target = ExAllocatePoolWithTag(PagedPool, sizeof(IO_RESOURCE_DESCRIPTOR), 'rqrI');
    if (!*Target) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(*Target, Source, sizeof(IO_RESOURCE_DESCRIPTOR));
    *TargetCount = 1;
    return STATUS_TRANSLATION_COMPLETE; /* Mirror kernel fstub behaviour */
}

/* HAL-exported implementation (assigned to dispatch table) */
NTSTATUS NTAPI
HalpGetInterruptTranslator(IN INTERFACE_TYPE ParentInterfaceType,
                           IN ULONG ParentBusNumber,
                           IN INTERFACE_TYPE BridgeInterfaceType,
                           IN USHORT Size,
                           IN USHORT Version,
                           OUT PTRANSLATOR_INTERFACE Translator,
                           OUT PULONG BridgeBusNumber)
{
    UNREFERENCED_PARAMETER(ParentInterfaceType);
    UNREFERENCED_PARAMETER(ParentBusNumber);
    if (!Translator || Size < sizeof(TRANSLATOR_INTERFACE)) return STATUS_INVALID_PARAMETER;
    /* Simple legacy implementation only supports ISA, EISA, Internal fallback */
    if (BridgeInterfaceType == Internal) BridgeInterfaceType = Isa;
    if (BridgeInterfaceType >= MicroChannel) return STATUS_NOT_IMPLEMENTED;

    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = 1; /* match callers expecting version 1 */
    Translator->Context = UlongToPtr(BridgeInterfaceType);
    Translator->InterfaceReference = HalpIrqTransRef;
    Translator->InterfaceDereference = HalpIrqTransDeref;
    Translator->TranslateResources = HalpIrqTranslateResources;
    Translator->TranslateResourceRequirements = HalpIrqTranslateRequirements;
    if (BridgeBusNumber) *BridgeBusNumber = 0; /* root */
    return STATUS_SUCCESS;
}
