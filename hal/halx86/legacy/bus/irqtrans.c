/*
 * PROJECT:         ReactOS HAL (x86 Legacy)
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/legacy/bus/irqtrans.c
 * PURPOSE:         Interrupt Translator Interface for HAL
 */

#include <hal.h>
#define NDEBUG
#include <debug.h>

static VOID NTAPI HalpIrqTranslatorNull(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static NTSTATUS
NTAPI
HalpIrqTranslateResource(IN OUT PVOID Context OPTIONAL,
                         IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
                         IN RESOURCE_TRANSLATION_DIRECTION Direction,
                         IN ULONG AlternativesCount OPTIONAL,
                         IN IO_RESOURCE_DESCRIPTOR Alternatives[] OPTIONAL,
                         IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                         OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;
    ULONG MinimumVector, Vector, k;
    PIO_RESOURCE_DESCRIPTOR Alternative;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    /* Copy common information */
    Target->Type = Source->Type;
    Target->ShareDisposition = Source->ShareDisposition;
    Target->Flags = Source->Flags;

    if (Direction == TranslateChildToParent)
    {
        /* Child-to-parent: map bus interrupt to system vector */
        Target->u.Interrupt.Vector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                           0,
                                                           Source->u.Interrupt.Vector,
                                                           Source->u.Interrupt.Vector,
                                                           &Irql,
                                                           &Affinity);
        Target->u.Interrupt.Level = Irql;
        Target->u.Interrupt.Affinity = Affinity;
        return STATUS_TRANSLATION_COMPLETE;
    }
    else if (Direction == TranslateParentToChild)
    {
        /* Parent-to-child: find a matching child vector by trial */
        for (k = 0; k < AlternativesCount; k++)
        {
            Alternative = &(Alternatives[k]);
            ASSERT(Alternative->Type == CmResourceTypeInterrupt);

            MinimumVector = Alternative->u.Interrupt.MinimumVector;
            while (MinimumVector <= Alternative->u.Interrupt.MaximumVector)
            {
                Vector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                               0,
                                               MinimumVector,
                                               MinimumVector,
                                               &Irql,
                                               &Affinity);
                if (Vector == Source->u.Interrupt.Vector)
                {
                    Target->u.Interrupt.Affinity = (KAFFINITY)-1;
                    Target->u.Interrupt.Vector = MinimumVector;
                    Target->u.Interrupt.Level = MinimumVector;
                    return STATUS_SUCCESS;
                }
                MinimumVector++;
            }
        }
    }

    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS
NTAPI
HalpIrqTranslateRequirement(IN OUT PVOID Context OPTIONAL,
                            IN PIO_RESOURCE_DESCRIPTOR Source,
                            IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                            OUT PULONG TargetCount,
                            OUT PIO_RESOURCE_DESCRIPTOR *Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    *Target = ExAllocatePoolWithTag(PagedPool,
                                    sizeof(IO_RESOURCE_DESCRIPTOR),
                                    'ltrH');
    if (!*Target) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(*Target, sizeof(IO_RESOURCE_DESCRIPTOR));
    *TargetCount = 1;

    (*Target)->Type = CmResourceTypeInterrupt;
    (*Target)->ShareDisposition = Source->ShareDisposition;
    (*Target)->Flags = Source->Flags;

    (*Target)->u.Interrupt.MinimumVector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                                 0,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 &Irql,
                                                                 &Affinity);

    (*Target)->u.Interrupt.MaximumVector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                                 0,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 &Irql,
                                                                 &Affinity);

    return STATUS_TRANSLATION_COMPLETE;
}

NTSTATUS
NTAPI
HalpGetInterruptTranslator(IN INTERFACE_TYPE ParentInterfaceType,
                           IN ULONG ParentBusNumber,
                           IN INTERFACE_TYPE BridgeInterfaceType,
                           IN USHORT Size,
                           IN USHORT Version,
                           OUT PTRANSLATOR_INTERFACE Translator,
                           OUT PULONG BridgeBusNumber)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ParentInterfaceType);
    UNREFERENCED_PARAMETER(ParentBusNumber);
    UNREFERENCED_PARAMETER(BridgeBusNumber);

    ASSERT(Version == HAL_IRQ_TRANSLATOR_VERSION);
    ASSERT(Size >= sizeof(TRANSLATOR_INTERFACE));

    /* Only classic PC buses supported here */
    if (BridgeInterfaceType == Internal || BridgeInterfaceType >= MicroChannel)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->Context = UlongToPtr((BridgeInterfaceType == InterfaceTypeUndefined) ? Isa : BridgeInterfaceType);
    Translator->InterfaceReference = HalpIrqTranslatorNull;
    Translator->InterfaceDereference = HalpIrqTranslatorNull;
    Translator->TranslateResources = HalpIrqTranslateResource;
    Translator->TranslateResourceRequirements = HalpIrqTranslateRequirement;

    return STATUS_SUCCESS;
}


