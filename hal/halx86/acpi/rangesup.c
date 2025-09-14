/*
 * ReactOS HAL ACPI - Supported range adjustment (ACPI variant)
 */

#include <hal.h>

#define NDEBUG
#include <debug.h>

#include "dispatch.h"

typedef struct _NRPARAMS {
    PIO_RESOURCE_DESCRIPTOR     InDesc;
    PIO_RESOURCE_DESCRIPTOR     OutDesc;
    PSUPPORTED_RANGE            CurrentPosition;
    LONGLONG                    Base;
    LONGLONG                    Limit;
    UCHAR                       DescOpt;
    BOOLEAN                     AnotherListPending;
} NRPARAMS, *PNRPARAMS;

static
ULONG
HalpSortRangesAcpi(IN PSUPPORTED_RANGE pRange)
{
    ULONG count = 0;
    for (; pRange; pRange = pRange->Next) count++;
    return count ? count : 0;
}

static
PIO_RESOURCE_DESCRIPTOR
HalpGetNextSupportedRangeAcpi(
    IN LONGLONG MinimumAddress,
    IN LONGLONG MaximumAddress,
    IN OUT PNRPARAMS P)
{
    LONGLONG base, limit;

    while (P->CurrentPosition) {
        base  = P->CurrentPosition->Base;
        limit = P->CurrentPosition->Limit;
        P->CurrentPosition = P->CurrentPosition->Next;

        if (base < MinimumAddress) base = MinimumAddress;
        if (limit > MaximumAddress) limit = MaximumAddress;
        if (base > limit) continue;

        P->Base  = base;
        P->Limit = limit;

        *P->OutDesc = *P->InDesc;
        P->OutDesc->Option = P->DescOpt;
        P->OutDesc += 1;
        return P->OutDesc - 1;
    }

    return NULL;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
HaliAdjustResourceListRange(
    PSUPPORTED_RANGES SRanges,
    PSUPPORTED_RANGE InterruptRange,
    PIO_RESOURCE_REQUIREMENTS_LIST *pResourceList)
{
    PIO_RESOURCE_REQUIREMENTS_LIST  InCompleteList, OutCompleteList;
    PIO_RESOURCE_LIST               InResourceList, OutResourceList;
    PIO_RESOURCE_DESCRIPTOR         HeadOutDesc, SetDesc;
    NRPARAMS                        Pos;
    ULONG                           len, alt, cnt, i;
    ULONG                           icnt;

    PAGED_CODE();

    if (!SRanges) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!SRanges->Sorted) {
        SRanges->NoIO = HalpSortRangesAcpi(&SRanges->IO);
        SRanges->NoMemory = HalpSortRangesAcpi(&SRanges->Memory);
        SRanges->NoPrefetchMemory = HalpSortRangesAcpi(&SRanges->PrefetchMemory);
        SRanges->NoDma = HalpSortRangesAcpi(&SRanges->Dma);
        SRanges->Sorted = TRUE;
    }

    icnt = HalpSortRangesAcpi(InterruptRange);

    InCompleteList = *pResourceList;
    len = InCompleteList->ListSize;

    i = 1;
    InResourceList = InCompleteList->List;
    for (alt = 0; alt < InCompleteList->AlternativeLists; alt++) {
        if (InResourceList->Version != 1 || InResourceList->Revision < 1) {
            return STATUS_INVALID_PARAMETER;
        }

        Pos.InDesc = InResourceList->Descriptors;
        for (cnt = InResourceList->Count; cnt; cnt--) {
            switch (Pos.InDesc->Type) {
            case CmResourceTypeInterrupt:  i += icnt;           break;
            case CmResourceTypePort:       i += SRanges->NoIO;  break;
            case CmResourceTypeDma:        i += SRanges->NoDma; break;
            case CmResourceTypeMemory:
                i += SRanges->NoMemory;
                if (Pos.InDesc->Flags & CM_RESOURCE_MEMORY_PREFETCHABLE) {
                    i += SRanges->NoPrefetchMemory;
                }
                break;
            default:
                return STATUS_INVALID_PARAMETER;
            }

            i -= 1;
            Pos.InDesc++;
        }

        InResourceList = (PIO_RESOURCE_LIST)Pos.InDesc;
    }
    len += i * sizeof(IO_RESOURCE_DESCRIPTOR);

    OutCompleteList = (PIO_RESOURCE_REQUIREMENTS_LIST)
        ExAllocatePoolWithTag(PagedPool, len, ' laH');
    if (!OutCompleteList) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(OutCompleteList, len);

    InResourceList = InCompleteList->List;
    *OutCompleteList = *InCompleteList;
    OutResourceList = OutCompleteList->List;

    for (alt = 0; alt < InCompleteList->AlternativeLists; alt++) {
        OutResourceList->Version  = 1;
        OutResourceList->Revision = 1;

        Pos.InDesc  = InResourceList->Descriptors;
        Pos.OutDesc = OutResourceList->Descriptors;
        HeadOutDesc = Pos.OutDesc;

        for (cnt = InResourceList->Count; cnt; cnt--) {
            Pos.DescOpt = Pos.InDesc->Option;
            Pos.AnotherListPending = FALSE;

            switch (Pos.InDesc->Type) {
            case CmResourceTypePort:
                Pos.CurrentPosition = &SRanges->IO;
                do {
                    SetDesc = HalpGetNextSupportedRangeAcpi(
                        Pos.InDesc->u.Port.MinimumAddress.QuadPart,
                        Pos.InDesc->u.Port.MaximumAddress.QuadPart,
                        &Pos);
                    if (SetDesc) {
                        SetDesc->u.Port.MinimumAddress.QuadPart = Pos.Base;
                        SetDesc->u.Port.MaximumAddress.QuadPart = Pos.Limit;
                    }
                } while (SetDesc);
                break;

            case CmResourceTypeInterrupt:
                Pos.CurrentPosition = InterruptRange;
                do {
                    SetDesc = HalpGetNextSupportedRangeAcpi(
                        Pos.InDesc->u.Interrupt.MinimumVector,
                        Pos.InDesc->u.Interrupt.MaximumVector,
                        &Pos);
                    if (SetDesc) {
                        SetDesc->u.Interrupt.MinimumVector = (ULONG)Pos.Base;
                        SetDesc->u.Interrupt.MaximumVector = (ULONG)Pos.Limit;
                    }
                } while (SetDesc);
                break;

            case CmResourceTypeMemory:
                if (Pos.InDesc->Flags & CM_RESOURCE_MEMORY_PREFETCHABLE) {
                    Pos.AnotherListPending = TRUE;
                    Pos.CurrentPosition = &SRanges->PrefetchMemory;
                    do {
                        SetDesc = HalpGetNextSupportedRangeAcpi(
                            Pos.InDesc->u.Memory.MinimumAddress.QuadPart,
                            Pos.InDesc->u.Memory.MaximumAddress.QuadPart,
                            &Pos);
                        if (SetDesc) {
                            SetDesc->u.Memory.MinimumAddress.QuadPart = Pos.Base;
                            SetDesc->u.Memory.MaximumAddress.QuadPart = Pos.Limit;
                            SetDesc->Option |= IO_RESOURCE_PREFERRED;
                        }
                    } while (SetDesc);
                    Pos.AnotherListPending = FALSE;
                }

                Pos.CurrentPosition = &SRanges->Memory;
                do {
                    SetDesc = HalpGetNextSupportedRangeAcpi(
                        Pos.InDesc->u.Memory.MinimumAddress.QuadPart,
                        Pos.InDesc->u.Memory.MaximumAddress.QuadPart,
                        &Pos);
                    if (SetDesc) {
                        SetDesc->u.Memory.MinimumAddress.QuadPart = Pos.Base;
                        SetDesc->u.Memory.MaximumAddress.QuadPart = Pos.Limit;
                    }
                } while (SetDesc);
                break;

            case CmResourceTypeDma:
                Pos.CurrentPosition = &SRanges->Dma;
                do {
                    SetDesc = HalpGetNextSupportedRangeAcpi(
                        Pos.InDesc->u.Dma.MinimumChannel,
                        Pos.InDesc->u.Dma.MaximumChannel,
                        &Pos);
                    if (SetDesc) {
                        SetDesc->u.Dma.MinimumChannel = (ULONG)Pos.Base;
                        SetDesc->u.Dma.MaximumChannel = (ULONG)Pos.Limit;
                    }
                } while (SetDesc);
                break;

            default:
                ExFreePool(OutCompleteList);
                return STATUS_INVALID_PARAMETER;
            }

            Pos.InDesc++;
            OutResourceList->Count = (USHORT)(Pos.OutDesc - HeadOutDesc);

            if (Pos.AnotherListPending) {
                *(Pos.OutDesc) = *(Pos.OutDesc-1);
                HeadOutDesc = Pos.OutDesc;
                Pos.OutDesc += 1;
            }
        }

        InResourceList  = (PIO_RESOURCE_LIST)Pos.InDesc;
        OutResourceList = (PIO_RESOURCE_LIST)Pos.OutDesc;
    }

    ExFreePool(*pResourceList);
    *pResourceList = OutCompleteList;
    return STATUS_SUCCESS;
}


/*
 * ACPI Root IRQ Translation (mirror base semantics)
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
HalIrqTranslateResourcesRoot(
    PVOID Context,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    RESOURCE_TRANSLATION_DIRECTION Direction,
    ULONG AlternativesCount,
    IO_RESOURCE_DESCRIPTOR Alternatives[],
    PDEVICE_OBJECT PhysicalDeviceObject,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;
    ULONG minimumVector, vector;
    PIO_RESOURCE_DESCRIPTOR alternative;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    Target->Type = Source->Type;
    Target->ShareDisposition = Source->ShareDisposition;
    Target->Flags = Source->Flags;

    if (Direction == TranslateChildToParent)
    {
        Target->u.Interrupt.Vector = HalGetInterruptVector(PCIBus,
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
        for (ULONG k = 0; k < AlternativesCount; k++)
        {
            alternative = Alternatives + k;
            ASSERT(alternative->Type == CmResourceTypeInterrupt);

            minimumVector = alternative->u.Interrupt.MinimumVector;
            while (minimumVector <= alternative->u.Interrupt.MaximumVector)
            {
                vector = HalGetInterruptVector(PCIBus,
                                               0,
                                               minimumVector,
                                               minimumVector,
                                               &Irql,
                                               &Affinity);

                if (vector == Source->u.Interrupt.Vector)
                {
                    Target->u.Interrupt.Affinity = (KAFFINITY)-1;
                    Target->u.Interrupt.Vector = minimumVector;
                    Target->u.Interrupt.Level = minimumVector;
                    return STATUS_SUCCESS;
                }

                minimumVector++;
            }
        }
    }

    return STATUS_UNSUCCESSFUL;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
HalIrqTranslateResourceRequirementsRoot(
    PVOID Context,
    PIO_RESOURCE_DESCRIPTOR Source,
    PDEVICE_OBJECT PhysicalDeviceObject,
    PULONG TargetCount,
    PIO_RESOURCE_DESCRIPTOR *Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    *Target = ExAllocatePoolWithTag(PagedPool,
                                    sizeof(IO_RESOURCE_DESCRIPTOR),
                                    'trIH');
    if (!*Target)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(*Target, sizeof(IO_RESOURCE_DESCRIPTOR));
    *TargetCount = 1;

    (*Target)->Type = CmResourceTypeInterrupt;
    (*Target)->u.Interrupt.MinimumVector = HalGetInterruptVector(PCIBus,
                                                                 0,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 &Irql,
                                                                 &Affinity);
    (*Target)->u.Interrupt.MaximumVector = HalGetInterruptVector(PCIBus,
                                                                 0,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 &Irql,
                                                                 &Affinity);

    return STATUS_TRANSLATION_COMPLETE;
}


