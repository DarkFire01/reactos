/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GSIV to IDT vector allocation for the APIC and PIC models
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <ndk/haltypes.h>   // INTERRUPT_CONNECTION_DATA
#include "arbiter.h"   // RTL range-list declarations missing from the WDK
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

// PIC model: ISA IRQ vector is base + irq; base derived from the SCI line.
static ULONG UacpiPicVectorBase;      // resolved in UacpiIrqLibInitialize (PIC)
static BOOLEAN UacpiPicBaseValid;

// Missing from down-level WDK headers.
#ifndef KE_PROCESSOR_CHANGE_ADD_EXISTING
#define KE_PROCESSOR_CHANGE_ADD_EXISTING 0x00000001
#endif


// HAL routines resolved by name at runtime.
typedef ULONG (NTAPI *PHAL_GET_INTERRUPT_VECTOR)(
    INTERFACE_TYPE, ULONG, ULONG, ULONG, PKIRQL, PKAFFINITY);
typedef KIRQL (NTAPI *PHAL_CONVERT_IDT_TO_IRQL)(ULONG IdtEntry);

static PHAL_GET_INTERRUPT_VECTOR UacpipHalGetInterruptVector;
static PHAL_CONVERT_IDT_TO_IRQL  UacpipHalConvertDeviceIdtToIrql;

// Kernel routines resolved by name; the import libraries predate them.
typedef NTSTATUS (NTAPI *PIO_SET_DEVICE_PROPERTY_DATA)(
    PDEVICE_OBJECT, CONST DEVPROPKEY *, LCID, ULONG, DEVPROPTYPE, ULONG, PVOID);
typedef NTSTATUS (NTAPI *PIO_GET_DEVICE_PROPERTY_DATA)(
    PDEVICE_OBJECT, CONST DEVPROPKEY *, LCID, ULONG, ULONG, PVOID, PULONG,
    PDEVPROPTYPE);
typedef PVOID (NTAPI *PKE_REGISTER_PROCESSOR_CHANGE_CALLBACK)(
    PPROCESSOR_CALLBACK_FUNCTION, PVOID, ULONG);
typedef NTSTATUS (NTAPI *PKE_ALLOCATE_SECONDARY_VECTOR)(ULONG Gsiv, PULONG Vector);

static PIO_SET_DEVICE_PROPERTY_DATA           UacpipIoSetDevicePropertyData;
static PIO_GET_DEVICE_PROPERTY_DATA           UacpipIoGetDevicePropertyData;
static PKE_REGISTER_PROCESSOR_CHANGE_CALLBACK UacpipKeRegisterProcessorChangeCallback;
static PKE_ALLOCATE_SECONDARY_VECTOR          UacpipKeAllocateSecondaryVector;
static BOOLEAN                                UacpipKernelRoutinesResolved;

// Idempotent; called from both the property writer and initialization.
static VOID
UacpipResolveKernelRoutines(VOID)
{
    UNICODE_STRING name;

    if (UacpipKernelRoutinesResolved)
    {
        return;
    }
    RtlInitUnicodeString(&name, L"IoSetDevicePropertyData");
    UacpipIoSetDevicePropertyData =
        (PIO_SET_DEVICE_PROPERTY_DATA)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"IoGetDevicePropertyData");
    UacpipIoGetDevicePropertyData =
        (PIO_GET_DEVICE_PROPERTY_DATA)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"KeRegisterProcessorChangeCallback");
    UacpipKeRegisterProcessorChangeCallback =
        (PKE_REGISTER_PROCESSOR_CHANGE_CALLBACK)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"KeAllocateSecondaryVector");
    UacpipKeAllocateSecondaryVector =
        (PKE_ALLOCATE_SECONDARY_VECTOR)MmGetSystemRoutineAddress(&name);
    UacpipKernelRoutinesResolved = TRUE;
}

// Return STATUS_NOT_SUPPORTED when the kernel lacks the export.
static NTSTATUS
UacpipSetDevicePropertyData(PDEVICE_OBJECT Pdo, CONST DEVPROPKEY *Key, LCID Lcid,
                           ULONG Flags, DEVPROPTYPE Type, ULONG Size, PVOID Data)
{
    UacpipResolveKernelRoutines();
    if (UacpipIoSetDevicePropertyData == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    return UacpipIoSetDevicePropertyData(Pdo, Key, Lcid, Flags, Type, Size, Data);
}

static NTSTATUS
UacpipGetDevicePropertyData(PDEVICE_OBJECT Pdo, CONST DEVPROPKEY *Key, LCID Lcid,
                           ULONG Flags, ULONG Size, PVOID Data, PULONG RequiredSize,
                           PDEVPROPTYPE Type)
{
    UacpipResolveKernelRoutines();
    if (UacpipIoGetDevicePropertyData == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    return UacpipIoGetDevicePropertyData(Pdo, Key, Lcid, Flags, Size, Data,
                                         RequiredSize, Type);
}

// DEVPKEY for the INTERRUPT_CONNECTION_DATA device property.
static const DEVPROPKEY UacpiInterruptConnectionDataKey = {
    { 0xF0E20F09, 0xD97A, 0x49A9, { 0x80, 0x46, 0xBB, 0x6E, 0x22, 0xE6, 0xBB, 0x2E } },
    2
};
#ifndef DEVPROP_TYPE_BINARY
#define DEVPROP_TYPE_BINARY 0x00001003
#endif

// Property layout is the target kernel's (per-release gates in haltypes.h).
typedef INTERRUPT_VECTOR_DATA UACPI_VECTOR_DATA;
typedef INTERRUPT_CONNECTION_DATA UACPI_CONNECTION_DATA, *PUACPI_CONNECTION_DATA;

// Vista's TargetProcessors is a bare KAFFINITY; Win7+ wraps it in GROUP_AFFINITY.
#if (NTDDI_VERSION >= NTDDI_WIN7) || defined(__REACTOS__)
#define UACPI_VECTOR_TARGET(v) ((v).TargetProcessors.Mask)
#else
#define UACPI_VECTOR_TARGET(v) ((v).TargetProcessors)
#endif

// Sizes the kernel reads the property with: Vista from its ntoskrnl/HAL,
// later releases from x64 symbols. ReactOS has one layout for every release.
#if defined(__REACTOS__)
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x8);
#if defined(_WIN64)
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x58);
#else
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x50);
#endif
#elif (NTDDI_VERSION < NTDDI_WIN7)
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x8);
#if defined(_WIN64)
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x30);
#else
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x28);
#endif
#elif defined(_WIN64)
#if (NTDDI_VERSION >= NTDDI_WIN10_TH2)
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x8);
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x58);
#elif (NTDDI_VERSION >= NTDDI_WIN10)
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x8);
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x50);
#elif (NTDDI_VERSION >= NTDDI_WINBLUE)
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x60);
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x48);
#else
C_ASSERT(FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) == 0x8);
C_ASSERT(sizeof(UACPI_VECTOR_DATA) == 0x48);
#endif
#endif

// Per-processor occupied IDT entries (APIC model).
typedef struct _ACPI_PROCESSOR_IDT
{
    RTL_RANGE_LIST UsedEntries;   // occupied IDT entries (keyed by entry number)
    BOOLEAN        Valid;
} UACPI_PROCESSOR_IDT;

#define UACPI_MAX_PROCESSORS 256
static UACPI_PROCESSOR_IDT UacpiProcessorIdt[UACPI_MAX_PROCESSORS];
static ULONG              UacpiProcessorIdtCount;
static PVOID              UacpiProcessorChangeHandle;

// GSIV -> assigned vector cache (APIC model); 0 = unassigned.
#define UACPI_MAX_GSIV 256
static ULONG UacpiGsivVector[UACPI_MAX_GSIV];
static ULONG UacpiGsivLevelMask[UACPI_MAX_GSIV / 32];   // 1 bit/GSIV: level/active-low
static ULONG UacpiGsivIsoEdge[UACPI_MAX_GSIV / 32];     // 1 bit/GSIV: MADT ISO pinned EDGE
static ULONG UacpiGsivIsoHigh[UACPI_MAX_GSIV / 32];     // 1 bit/GSIV: MADT ISO pinned active-high
static ULONG UacpiGsivIsoLow[UACPI_MAX_GSIV / 32];      // 1 bit/GSIV: MADT ISO pinned active-low

static FAST_MUTEX UacpiIrqLibLock;
static BOOLEAN    UacpiIrqLibReady;

VOID
UacpiIrqLibNoteLevelGsiv(ULONG Gsiv)
{
    if (Gsiv < UACPI_MAX_GSIV)
    {
        UacpiGsivLevelMask[Gsiv / 32] |= (1ul << (Gsiv % 32));
    }
}
// Pin a GSIV edge-triggered; overrides a later level _CRS descriptor.
VOID
UacpiIrqLibNoteEdgeGsiv(ULONG Gsiv)
{
    if (Gsiv < UACPI_MAX_GSIV)
    {
        UacpiGsivIsoEdge[Gsiv / 32] |= (1ul << (Gsiv % 32));
    }
}
BOOLEAN
UacpiIrqLibGsivForcedEdge(ULONG Gsiv)
{
    return (Gsiv < UACPI_MAX_GSIV) &&
           ((UacpiGsivIsoEdge[Gsiv / 32] >> (Gsiv % 32)) & 1) != 0;
}
static BOOLEAN UacpipGsivIsLevel(ULONG Gsiv)
{
    return (Gsiv < UACPI_MAX_GSIV) &&
           ((UacpiGsivLevelMask[Gsiv / 32] >> (Gsiv % 32)) & 1);
}
// A MADT ISO polarity wins; otherwise level lines are active-low and edge lines active-high.
static BOOLEAN UacpipGsivIsActiveLow(ULONG Gsiv)
{
    if (Gsiv < UACPI_MAX_GSIV)
    {
        if ((UacpiGsivIsoHigh[Gsiv / 32] >> (Gsiv % 32)) & 1)
        {
            return FALSE;
        }
        if ((UacpiGsivIsoLow[Gsiv / 32] >> (Gsiv % 32)) & 1)
        {
            return TRUE;
        }
    }
    return UacpipGsivIsLevel(Gsiv);
}

// IDT vectors the HAL granted the ACPI root at start; only these are ours.
static ULONG UacpiHalVectorMask[256 / 32];
static ULONG UacpiHalVectorCount;

static BOOLEAN
UacpipHalGrantedVector(ULONG Vector)
{
    return (Vector <= 0xFF) &&
           ((UacpiHalVectorMask[Vector / 32] >> (Vector % 32)) & 1) != 0;
}

// Record the grant from the raw start list (PNPBus: Vector is the IDT entry).
VOID
UacpiIrqLibSetHalVectors(PCM_RESOURCE_LIST Resources)
{
    PCM_PARTIAL_RESOURCE_LIST part;
    ULONG i;

    RtlZeroMemory(UacpiHalVectorMask, sizeof(UacpiHalVectorMask));
    UacpiHalVectorCount = 0;

    if (Resources == NULL || Resources->Count == 0)
    {
        UacpiTrace("[acpi] irqlib: no HAL vector grant; using fixed band 0x50-0xBF\n");
        return;
    }

    part = &Resources->List[0].PartialResourceList;
    for (i = 0; i < part->Count; i++)
    {
        ULONG vector = part->PartialDescriptors[i].u.Interrupt.Vector;

        if (part->PartialDescriptors[i].Type != CmResourceTypeInterrupt || vector > 0xFF)
        {
            continue;
        }
        if (!UacpipHalGrantedVector(vector))
        {
            UacpiHalVectorMask[vector / 32] |= (1ul << (vector % 32));
            UacpiHalVectorCount++;
        }
    }
}

// Everything the HAL did not grant is occupied; the rest are unexpected-
// interrupt stubs KeConnectInterrupt accepts.
static NTSTATUS
UacpipSeedIdtState(PRTL_RANGE_LIST Used)
{
    ULONG v, start;

    RtlInitializeRangeList(Used);

    if (UacpiHalVectorCount == 0)
    {
        (void)RtlAddRange(Used, 0x00, 0x4F, 0, 0, NULL, NULL);   // exceptions/APC/DISPATCH
        (void)RtlAddRange(Used, 0xC0, 0xFF, 0, 0, NULL, NULL);   // SYNCH/CLOCK/IPI/HIGH
        return STATUS_SUCCESS;
    }

    for (v = 0; v <= 0xFF; )
    {
        if (UacpipHalGrantedVector(v))
        {
            v++;
            continue;
        }
        start = v;
        while (v <= 0xFF && !UacpipHalGrantedVector(v))
        {
            v++;
        }
        (void)RtlAddRange(Used, start, v - 1, 0, 0, NULL, NULL);
    }

    // HAL line vectors stay free here; UacpipVectorIsHalLine guards them.
    return STATUS_SUCCESS;
}

/*
 * HAL vector per line, or 0. These must not be handed to message interrupts.
 *
 * Under the APIC model this driver allocates every line vector itself and
 * records it in the per-processor IDT range lists, so the HAL is never asked
 * for one and this map stays empty. It must stay that way: HalGetInterruptVector
 * is not a query there. HalpGetRootInterruptVector calls
 * HalpAllocateSystemInterrupt for a line that has no vector yet, so asking about
 * a line assigns one. Walking every line to fill this map therefore spent a
 * vector on each I/O APIC input, out of the same band the HAL granted this
 * driver to allocate from, and left nothing for the message interrupts the map
 * exists to protect.
 */
static ULONG UacpiHalLineVector[UACPI_MAX_GSIV];

// TRUE if the HAL maps Vector to a line other than Gsiv (UACPI_MAX_GSIV: any).
static BOOLEAN
UacpipVectorIsHalLine(ULONG Vector, ULONG Gsiv)
{
    ULONG line;

    for (line = 0; line < UACPI_MAX_GSIV; line++)
    {
        if ((UacpiHalLineVector[line] == Vector) && (line != Gsiv))
        {
            return TRUE;
        }
    }

    return FALSE;
}

// Processor-change callback: add each CPU's free-IDT range list.
_Function_class_(PROCESSOR_CALLBACK_FUNCTION)
static VOID NTAPI
UacpiProcessorChangeCallback(PVOID CallbackContext,
                            PKE_PROCESSOR_CHANGE_NOTIFY_CONTEXT Change,
                            PNTSTATUS OperationStatus)
{
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(OperationStatus);

    if (Change->State != KeProcessorAddCompleteNotify)
    {
        return;
    }
    ExAcquireFastMutex(&UacpiIrqLibLock);
    if (Change->NtNumber < UACPI_MAX_PROCESSORS &&
        !UacpiProcessorIdt[Change->NtNumber].Valid)
        {
        if (NT_SUCCESS(UacpipSeedIdtState(&UacpiProcessorIdt[Change->NtNumber].UsedEntries)))
        {
            UacpiProcessorIdt[Change->NtNumber].Valid = TRUE;
            if (Change->NtNumber + 1 > UacpiProcessorIdtCount)
            {
                UacpiProcessorIdtCount = Change->NtNumber + 1;
            }
            UacpiTrace("[acpi] irqlib: processor %u IDT state seeded\n",
                      Change->NtNumber);
        }
    }
    ExReleaseFastMutex(&UacpiIrqLibLock);
}

// Seed the IDT state of every processor already running.
static VOID
UacpipSeedExistingProcessors(VOID)
{
    ULONG count = (ULONG)(UCHAR)KeNumberProcessors;
    ULONG i;

    if (count > UACPI_MAX_PROCESSORS)
        count = UACPI_MAX_PROCESSORS;

    ExAcquireFastMutex(&UacpiIrqLibLock);
    for (i = 0; i < count; i++)
    {
        if (UacpiProcessorIdt[i].Valid)
            continue;

        if (NT_SUCCESS(UacpipSeedIdtState(&UacpiProcessorIdt[i].UsedEntries)))
        {
            UacpiProcessorIdt[i].Valid = TRUE;
            if (i + 1 > UacpiProcessorIdtCount)
                UacpiProcessorIdtCount = i + 1;
        }
    }
    ExReleaseFastMutex(&UacpiIrqLibLock);

    UacpiTrace("[acpi] irqlib: seeded %u of %u processor IDT set(s)\n",
              UacpiProcessorIdtCount, count);
}

// APIC allocation: first-fit an IDT entry available on every active processor.
static NTSTATUS
UacpipApicAllocateVector(ULONG Gsiv, PULONG Vector)
{
    ULONG cand;
    ULONG i;

    if (Gsiv < UACPI_MAX_GSIV && UacpiGsivVector[Gsiv] != 0)
    {
        *Vector = UacpiGsivVector[Gsiv];
        return STATUS_SUCCESS;   // GSIV already assigned (shared/re-query)
    }
    if (UacpiProcessorIdtCount == 0)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // Use the HAL's vector for the line when it has one; else first-fit below.
    if (UacpipHalGetInterruptVector != NULL)
    {
        KIRQL     halIrql = 0;
        KAFFINITY halAffinity = 0;
        ULONG     halVector;

        halVector = UacpipHalGetInterruptVector(Internal, 0, Gsiv, Gsiv,
                                                &halIrql, &halAffinity);
        if (halVector != 0 && halVector <= 0xFF)
        {
            // Take it unconditionally; no other line resolves to this vector.
            for (i = 0; i < UacpiProcessorIdtCount; i++)
            {
                if (UacpiProcessorIdt[i].Valid)
                {
                    (void)RtlAddRange(&UacpiProcessorIdt[i].UsedEntries,
                                      halVector, halVector, 0, 0, NULL,
                                      (PVOID)(ULONG_PTR)(Gsiv + 1));
                }
            }
            if (Gsiv < UACPI_MAX_GSIV)
            {
                UacpiGsivVector[Gsiv] = halVector;

                // The map is only ever filled from a line the HAL really owns.
                UacpiHalLineVector[Gsiv] = halVector;
            }
            *Vector = halVector;
            return STATUS_SUCCESS;
        }
    }

    // First-fit over the device band 0x50-0xBF.
    for (cand = 0x50; cand <= 0xBF; cand++)
    {
        BOOLEAN okAll = TRUE;

        /* Never take a vector the HAL owes a different line */
        if (UacpipVectorIsHalLine(cand, Gsiv))
        {
            continue;
        }

        for (i = 0; i < UacpiProcessorIdtCount; i++)
        {
            BOOLEAN avail = FALSE;
            if (!UacpiProcessorIdt[i].Valid)
            {
                continue;
            }
            if (!NT_SUCCESS(RtlIsRangeAvailable(&UacpiProcessorIdt[i].UsedEntries,
                                                cand, cand, 0, 0, NULL, NULL, &avail)) ||
                !avail)
                {
                okAll = FALSE;
                break;
            }
        }
        if (!okAll)
        {
            continue;
        }
        // Claim it on every CPU (mark the entry occupied).
        for (i = 0; i < UacpiProcessorIdtCount; i++)
        {
            if (UacpiProcessorIdt[i].Valid)
            {
                (void)RtlAddRange(&UacpiProcessorIdt[i].UsedEntries, cand, cand,
                                  0, 0, NULL, (PVOID)(ULONG_PTR)(Gsiv + 1));
            }
        }
        if (Gsiv < UACPI_MAX_GSIV)
        {
            UacpiGsivVector[Gsiv] = cand;
        }
        *Vector = cand;
        return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

// MSI/MSI-X: runs of consecutive IDT entries from the same device band.
#define UACPI_MSI_OWNER ((PVOID)(ULONG_PTR)0xACB1A5E1)   // RTL range owner tag

// Count free consecutive entries on every CPU, aligned to Count. Lock held.
static NTSTATUS
UacpipApicAllocateVectorRange(ULONG Count, PULONG BaseVector)
{
    ULONG cand, k, i, align, first;

    if (Count == 0)
    {
        Count = 1;
    }
    if (UacpiProcessorIdtCount == 0)
    {
        return STATUS_UNSUCCESSFUL;
    }
    align = ((Count & (Count - 1)) == 0) ? Count : 1;
    first = (0x50 + align - 1) & ~(align - 1);
    for (cand = first; (cand + Count - 1) <= 0xBF; cand += align)
    {
        BOOLEAN okAll = TRUE;
        for (k = 0; k < Count && okAll; k++)
        {
            // Skip HAL line vectors; mixed trigger modes cannot share one.
            if (UacpipVectorIsHalLine(cand + k, UACPI_MAX_GSIV))
            {
                okAll = FALSE;
                break;
            }

            for (i = 0; i < UacpiProcessorIdtCount; i++)
            {
                BOOLEAN avail = FALSE;
                if (!UacpiProcessorIdt[i].Valid)
                {
                    continue;
                }
                if (!NT_SUCCESS(RtlIsRangeAvailable(&UacpiProcessorIdt[i].UsedEntries,
                                                    cand + k, cand + k, 0, 0, NULL, NULL,
                                                    &avail)) || !avail)
                                                    {
                    okAll = FALSE;
                    break;
                }
            }
        }
        if (!okAll)
        {
            continue;
        }
        for (i = 0; i < UacpiProcessorIdtCount; i++)
        {
            if (UacpiProcessorIdt[i].Valid)
            {
                (void)RtlAddRange(&UacpiProcessorIdt[i].UsedEntries,
                                  cand, cand + Count - 1, 0, 0, NULL, UACPI_MSI_OWNER);
            }
        }
        *BaseVector = cand;
        return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static VOID
UacpipApicFreeVectorRange(ULONG BaseVector, ULONG Count)
{
    ULONG i;
    if (Count == 0)
    {
        Count = 1;
    }
    for (i = 0; i < UacpiProcessorIdtCount; i++)
    {
        if (UacpiProcessorIdt[i].Valid)
        {
            (void)RtlDeleteRange(&UacpiProcessorIdt[i].UsedEntries,
                                 BaseVector, BaseVector + Count - 1, UACPI_MSI_OWNER);
        }
    }
}

// Message vector runs, allocated at arbitration commit.
typedef struct _ACPI_MSG_ASSIGN
{
    PVOID   Owner;       // owning PDO, NULL = free slot
    ULONG   MsgGsiv;     // arbiter placement (>= 0xFFF00000), 0 = HAL-originated
    ULONG   Base;        // first IDT vector of the run
    ULONG   Count;       // messages
    BOOLEAN Claimed;     // handed to the HAL by slot 27 (see below)
} UACPI_MSG_ASSIGN;
#define UACPI_MSG_MAX 128
static UACPI_MSG_ASSIGN UacpiMsgAssigns[UACPI_MSG_MAX];

// Find or allocate a message run, keyed by owner PDO, placement, and count.
NTSTATUS
UacpiIrqLibResolveMessageVector(PVOID Owner, ULONG MsgGsiv, ULONG Count,
                               PULONG BaseVector, PKIRQL Irql, PKAFFINITY Affinity)
{
    ULONG i, base = 0;
    NTSTATUS status = STATUS_SUCCESS;
    LONG freeSlot = -1;

    if (Count == 0)
    {
        Count = 1;
    }
    if (g_AcpiInterruptModel != 1)
    {
        return STATUS_NOT_SUPPORTED;   // message interrupts are APIC-only
    }
    ExAcquireFastMutex(&UacpiIrqLibLock);
    for (i = 0; i < UACPI_MSG_MAX; i++)
    {
        if (UacpiMsgAssigns[i].Owner == Owner &&
            UacpiMsgAssigns[i].MsgGsiv == MsgGsiv &&
            UacpiMsgAssigns[i].Count == Count)
            {
            base = UacpiMsgAssigns[i].Base;
            break;
        }
        if (freeSlot < 0 && UacpiMsgAssigns[i].Owner == NULL)
        {
            freeSlot = (LONG)i;
        }
    }
    if (i == UACPI_MSG_MAX)
    {
        if (freeSlot < 0)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            status = UacpipApicAllocateVectorRange(Count, &base);
            // Fall back to a single message if the full run is unavailable.
            if (!NT_SUCCESS(status) && Count > 1)
            {
                NTSTATUS retry = UacpipApicAllocateVectorRange(1, &base);

                UacpiTrace("[acpi] irqlib: MESSAGE run of %u for msg-gsiv 0x%X "
                          "unavailable (0x%X) - retrying with 1: 0x%X\n",
                          Count, MsgGsiv, status, retry);
                if (NT_SUCCESS(retry))
                {
                    Count  = 1;
                    status = retry;
                }
            }
            if (NT_SUCCESS(status))
            {
                UacpiMsgAssigns[freeSlot].Owner   = Owner;
                UacpiMsgAssigns[freeSlot].MsgGsiv = MsgGsiv;
                UacpiMsgAssigns[freeSlot].Base    = base;
                UacpiMsgAssigns[freeSlot].Count   = Count;
                UacpiMsgAssigns[freeSlot].Claimed = FALSE;  // slot 27 claims it
            }
        }
    }
    ExReleaseFastMutex(&UacpiIrqLibLock);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] irqlib: MESSAGE vector alloc (msg-gsiv 0x%X x%u) failed 0x%X\n",
                  MsgGsiv, Count, status);
        return status;
    }
    *BaseVector = base;
    *Irql       = UacpipHalConvertDeviceIdtToIrql ? UacpipHalConvertDeviceIdtToIrql(base)
                                                 : (KIRQL)(base >> 4);
    // Lowest active processor; must match the connection data element.
    *Affinity   = KeQueryActiveProcessors();
    *Affinity  &= (~*Affinity + 1);
    if (*Affinity == 0)
    {
        *Affinity = 1;                 // no active set reported; CPU 0
    }
    return STATUS_SUCCESS;
}

// TRUE if the HAL reports this GSIV as behind a secondary controller.
static BOOLEAN
UacpipIsSecondaryGsiv(ULONG Gsiv)
{
#if (NTDDI_VERSION >= NTDDI_WIN8)
    if (HALPRIVATEDISPATCH->HalIsInterruptTypeSecondary == NULL)
    {
        return FALSE;
    }

    return HALPRIVATEDISPATCH->HalIsInterruptTypeSecondary(InterruptTypeControllerInput, Gsiv);
#else
    // No secondary controllers before Win8.
    UNREFERENCED_PARAMETER(Gsiv);
    return FALSE;
#endif
}

// Secondary pin: vector from the kernel, IRQL from the primary GSIV's line.
static NTSTATUS
UacpipResolveSecondaryVector(ULONG Gsiv, PULONG Vector, PKIRQL Irql)
{
    UACPI_CONNECTION_DATA query;
    ULONG primaryGsiv = 0;
    ULONG primaryVector = 0;
    NTSTATUS status;

    UacpipResolveKernelRoutines();

    if (UacpipKeAllocateSecondaryVector == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    status = UacpipKeAllocateSecondaryVector(Gsiv, Vector);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    *Irql = HIGH_LEVEL;

#if (NTDDI_VERSION >= NTDDI_WIN8)
    if (HALPRIVATEDISPATCH->HalSecondaryInterruptQueryPrimaryInformation == NULL)
    {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&query, sizeof(query));
    query.Count = 1;
    query.Vectors[0].Type = InterruptTypeControllerInput;
    query.Vectors[0].ControllerInput.Gsiv = Gsiv;

#if (NTDDI_VERSION >= NTDDI_WIN10)
    status = HALPRIVATEDISPATCH->HalSecondaryInterruptQueryPrimaryInformation(
                 &query.Vectors[0], &primaryGsiv);   // RS1: vector data
#else
    status = HALPRIVATEDISPATCH->HalSecondaryInterruptQueryPrimaryInformation(
                 (PINTERRUPT_CONNECTION_DATA)&query, &primaryGsiv);   // Win8: connection data
#endif
    if (!NT_SUCCESS(status))
    {
        // No controller owns the pin yet; keep the vector and HIGH_LEVEL.
        UacpiTrace("[acpi] irqlib: no primary for secondary GSIV %u (0x%08lX)\n",
                  Gsiv, status);
        return STATUS_SUCCESS;
    }

    ExAcquireFastMutex(&UacpiIrqLibLock);
    status = UacpipApicAllocateVector(primaryGsiv, &primaryVector);
    ExReleaseFastMutex(&UacpiIrqLibLock);
    if (NT_SUCCESS(status) && UacpipHalConvertDeviceIdtToIrql != NULL)
    {
        *Irql = UacpipHalConvertDeviceIdtToIrql(primaryVector);
    }
#else
    // No primary lookup before Win8; IRQL stays HIGH_LEVEL.
    UNREFERENCED_PARAMETER(query);
    UNREFERENCED_PARAMETER(primaryGsiv);
    UNREFERENCED_PARAMETER(primaryVector);
    UNREFERENCED_PARAMETER(status);
#endif

    return STATUS_SUCCESS;
}

NTSTATUS
UacpiIrqLibResolveVector(ULONG Gsiv,
                        PULONG Vector, PKIRQL Irql, PKAFFINITY Affinity,
                        PULONG Polarity, PULONG Mode)
{
    NTSTATUS status;
    ULONG vector = 0;
    KIRQL irql = 0;
    KAFFINITY affinity = 0;

    // Secondary pin ISRs run inside the controller's interrupt, at its IRQL.
    if (UacpipIsSecondaryGsiv(Gsiv))
    {
        status = UacpipResolveSecondaryVector(Gsiv, &vector, &irql);
        if (!NT_SUCCESS(status))
        {
            UacpiTrace("[acpi] irqlib: secondary GSIV %u unresolved 0x%08lX\n",
                      Gsiv, status);
            return status;
        }

        *Vector   = vector;
        *Irql     = irql;
        *Affinity = KeQueryActiveProcessors();
        *Polarity = UacpipGsivIsActiveLow(Gsiv) ? 2 : 1;
        *Mode     = UacpipGsivIsLevel(Gsiv) ? 0 : 1;

        UacpiTrace("[acpi] irqlib: secondary GSIV %u -> vector 0x%X irql %u\n",
                  Gsiv, vector, irql);
        return STATUS_SUCCESS;
    }

    if (g_AcpiInterruptModel == 1)
    {
        // APIC: this driver owns the allocation.
        ExAcquireFastMutex(&UacpiIrqLibLock);
        status = UacpipApicAllocateVector(Gsiv, &vector);
        ExReleaseFastMutex(&UacpiIrqLibLock);
        if (!NT_SUCCESS(status))
        {
            UacpiTrace("[acpi] irqlib: APIC alloc for GSIV %u failed 0x%X\n",
                      Gsiv, status);
            return status;
        }
        irql = UacpipHalConvertDeviceIdtToIrql
                   ? UacpipHalConvertDeviceIdtToIrql(vector)
                   : (KIRQL)(vector >> 4);
        affinity = KeQueryActiveProcessors();
    }
    else
    {
        // PIC: ask the HAL; the calibrated base is the fallback.
        KIRQL     halIrql = 0;
        KAFFINITY halAffinity = 0;
        ULONG     halVector = 0;

        if (UacpipHalGetInterruptVector != NULL)
        {
            halVector = UacpipHalGetInterruptVector(Isa, 0, Gsiv, Gsiv,
                                                    &halIrql, &halAffinity);
        }

        if (halVector != 0)
        {
            vector   = halVector;
            irql     = halIrql;
            affinity = halAffinity ? halAffinity : KeQueryActiveProcessors();
        }
        else
        {
            if (!UacpiPicBaseValid)
            {
                return STATUS_NOT_SUPPORTED;
            }
            vector = UacpiPicVectorBase + Gsiv;
            irql = UacpipHalConvertDeviceIdtToIrql
                       ? UacpipHalConvertDeviceIdtToIrql(vector)
                       : (KIRQL)(vector >> 4);
            affinity = KeQueryActiveProcessors();
        }
    }

    *Vector   = vector;
    *Irql     = irql;

    if (affinity == 0)
    {
        affinity = KeQueryActiveProcessors();
    }

    // Target the lowest processor; physical mode takes a single CPU.
    affinity &= (~affinity + 1);
    if (affinity == 0)
    {
        affinity = 1;                  // no active set reported; CPU 0
    }

    *Affinity = affinity;
    *Polarity = UacpipGsivIsActiveLow(Gsiv) ? 2: 1;           // ActiveLow: ActiveHigh
    *Mode     = UacpipGsivIsLevel(Gsiv) ? 0 /*Level*/: 1 /*Latched*/;

    UacpiTrace("[acpi] irqlib: GSIV %u -> vector 0x%X irql %u affinity 0x%p %s/%s (%s)\n",
              Gsiv, vector, irql, (PVOID)*Affinity,
              *Mode == 0 ? "level" : "edge",
              *Polarity == 2 ? "low" : "high",
              g_AcpiInterruptModel == 1 ? "APIC" : "PIC");
    return STATUS_SUCCESS;
}

// Publish a device's assignment as its INTERRUPT_CONNECTION_DATA property.
NTSTATUS
UacpiIrqLibWriteConnectionData(PDEVICE_OBJECT Pdo, ULONG Gsiv, ULONG Count)
{
    UACPI_CONNECTION_DATA data;
    ULONG vector = 0, polarity = 0, mode = 0;
    KIRQL irql = 0;
    KAFFINITY affinity = 0;
    NTSTATUS status;

    // Message window: one Type-3 element per message this device owns.
    if (Gsiv >= 0xFFF00000)
    {
        PUACPI_CONNECTION_DATA msg;
        ULONG count = (Count == 0) ? 1 : Count, i, size, base = 0;
        KIRQL baseIrql = 0;
        KAFFINITY procs = 0;

        ULONG total, written, slot;
        KAFFINITY msgProcs;
        ULONG lastBase = 0;

        status = UacpiIrqLibResolveMessageVector(Pdo, Gsiv, count, &base, &baseIrql, &procs);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        // Emit every run for this PDO in ascending base-vector order, so
        // element 0 is the base. Plain multi-message MSI needs this.
        // Physical destination mode requires a single target processor.
        msgProcs = procs & (~procs + 1);
        if (msgProcs == 0)
        {
            msgProcs = 1;            // no affinity handed down; CPU 0
        }
        ExAcquireFastMutex(&UacpiIrqLibLock);
        total = 0;
        for (i = 0; i < UACPI_MSG_MAX; i++)
        {
            if (UacpiMsgAssigns[i].Owner == Pdo)
            {
                total += UacpiMsgAssigns[i].Count;
            }
        }
        ExReleaseFastMutex(&UacpiIrqLibLock);
        if (total == 0)
        {
            total = count;   // unexpected; resolve above recorded one
        }

        size = FIELD_OFFSET(UACPI_CONNECTION_DATA, Vectors) + total * sizeof(UACPI_VECTOR_DATA);
        msg = (PUACPI_CONNECTION_DATA)ExAllocatePoolWithTag(PagedPool, size, UACPI_POOL_TAG);
        if (msg == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(msg, size);
        msg->Count = total;

        written = 0;
        ExAcquireFastMutex(&UacpiIrqLibLock);
        while (written < total)
        {
            ULONG bestBase = 0xFFFFFFFF;
            LONG  best = -1;

            // Next run by ascending base vector (arbiter and HAL-originated runs
            // alike), so message 0 lands on the lowest vector.
            for (slot = 0; slot < UACPI_MSG_MAX; slot++)
            {
                if (UacpiMsgAssigns[slot].Owner == Pdo &&
                    UacpiMsgAssigns[slot].Count != 0 &&
                    UacpiMsgAssigns[slot].Base > lastBase &&
                    UacpiMsgAssigns[slot].Base < bestBase)
                    {
                    bestBase = UacpiMsgAssigns[slot].Base;
                    best = (LONG)slot;
                }
            }
            if (best < 0)
            {
                break;
            }
            lastBase = bestBase;

            for (i = 0; i < UacpiMsgAssigns[best].Count && written < total; i++, written++)
            {
                ULONG v = UacpiMsgAssigns[best].Base + i;

                msg->Vectors[written].Type             = 3;   // InterruptTypeMessageRequest
                msg->Vectors[written].Vector           = v;
                msg->Vectors[written].Irql             = (UCHAR)(UacpipHalConvertDeviceIdtToIrql
                                                             ? UacpipHalConvertDeviceIdtToIrql(v)
                                                             : (KIRQL)(v >> 4));
                msg->Vectors[written].Polarity         = 1;   // ActiveHigh (message = edge/high)
                msg->Vectors[written].Mode             = 1;   // Latched
                UACPI_VECTOR_TARGET(msg->Vectors[written]) = msgProcs;
                // 1 = ApicDestinationModePhysical
                msg->Vectors[written].MessageRequest.DestinationMode = 1;
            }
        }
        ExReleaseFastMutex(&UacpiIrqLibLock);
        msg->Count = written ? written : total;

        status = UacpipSetDevicePropertyData(Pdo, &UacpiInterruptConnectionDataKey,
                                             0, 0, DEVPROP_TYPE_BINARY, size, msg);
        UacpiTrace("[acpi] irqlib: PDO %p MESSAGE connection data (msg-gsiv 0x%X): "
                  "wrote %u of %u vector(s) base 0x%X..0x%X irql %u aff 0x%p phys: 0x%X\n",
                  Pdo, Gsiv, msg->Count, total,
                  msg->Vectors[0].Vector,
                  msg->Vectors[msg->Count ? msg->Count - 1 : 0].Vector,
                  baseIrql, (PVOID)msgProcs, status);
        ExFreePoolWithTag(msg, UACPI_POOL_TAG);

        // Diagnostic readback.
        if (NT_SUCCESS(status))
        {
            ULONG rbSize = 0;
            DEVPROPTYPE rbType = 0;
            PUACPI_CONNECTION_DATA rb =
                (PUACPI_CONNECTION_DATA)ExAllocatePoolWithTag(PagedPool, size, UACPI_POOL_TAG);

            if (rb != NULL)
            {
                NTSTATUS rbSt;

                RtlZeroMemory(rb, size);
                rbSt = UacpipGetDevicePropertyData(Pdo, &UacpiInterruptConnectionDataKey,
                                                   0, 0, size, rb, &rbSize, &rbType);
                UacpiTrace("[acpi] irqlib: readback MESSAGE st=0x%X type=0x%X size=%u(want %u) "
                          "rdCount=%u rdVec0=0x%X rdVecN=0x%X\n",
                          rbSt, rbType, rbSize, size, rb->Count,
                          rb->Count ? rb->Vectors[0].Vector : 0,
                          rb->Count ? rb->Vectors[rb->Count - 1].Vector : 0);
                ExFreePoolWithTag(rb, UACPI_POOL_TAG);
            }
        }
        return status;
    }

    // Never overwrite message elements with a line element.
    {
        ULONG mi;
        BOOLEAN hasMessages = FALSE;

        ExAcquireFastMutex(&UacpiIrqLibLock);
        for (mi = 0; mi < UACPI_MSG_MAX; mi++)
        {
            if (UacpiMsgAssigns[mi].Owner == Pdo && UacpiMsgAssigns[mi].Count != 0)
            {
                hasMessages = TRUE;
                break;
            }
        }
        ExReleaseFastMutex(&UacpiIrqLibLock);

        if (hasMessages)
        {
            UacpiTrace("[acpi] irqlib: PDO %p GSIV %u line write SUPPRESSED - device "
                      "holds message assignments (property left message-based)\n",
                      Pdo, Gsiv);
            return STATUS_SUCCESS;
        }
    }

    status = UacpiIrqLibResolveVector(Gsiv, &vector, &irql, &affinity, &polarity, &mode);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    RtlZeroMemory(&data, sizeof(data));
    data.Count                            = 1;
    data.Vectors[0].Type                  = 0;   // InterruptTypeControllerInput
    data.Vectors[0].Vector                = vector;
    data.Vectors[0].Irql                  = (UCHAR)irql;
    data.Vectors[0].Polarity              = polarity;
    data.Vectors[0].Mode                  = mode;
    UACPI_VECTOR_TARGET(data.Vectors[0]) = affinity;
    data.Vectors[0].ControllerInput.Gsiv  = Gsiv;

    status = UacpipSetDevicePropertyData(Pdo, &UacpiInterruptConnectionDataKey,
                                         0, 0, DEVPROP_TYPE_BINARY, sizeof(data), &data);
    UacpiTrace("[acpi] irqlib: PDO %p connection data (GSIV %u vector 0x%X irql %u): 0x%X\n",
              Pdo, Gsiv, vector, irql, status);

    // Diagnostic readback.
    if (NT_SUCCESS(status))
    {
        UACPI_CONNECTION_DATA rb;
        ULONG rbSize = 0;
        DEVPROPTYPE rbType = 0;
        NTSTATUS rbSt;

        RtlZeroMemory(&rb, sizeof(rb));
        rbSt = UacpipGetDevicePropertyData(Pdo, &UacpiInterruptConnectionDataKey,
                                           0, 0, sizeof(rb), &rb, &rbSize, &rbType);
        UacpiTrace("[acpi] irqlib: readback GSIV %u st=0x%X type=0x%X size=%u(want %u) "
                  "rdVec=0x%X rdIrql=%u rdAff=0x%p rdGsiv=%u\n",
                  Gsiv, rbSt, rbType, rbSize, (ULONG)sizeof(data),
                  rb.Vectors[0].Vector, rb.Vectors[0].Irql,
                  (PVOID)UACPI_VECTOR_TARGET(rb.Vectors[0]),
                  rb.Vectors[0].ControllerInput.Gsiv);
    }
    return status;
}

// HalPrivateDispatchTable overrides, slots 22/23/27/28 (APIC model).

// MADT Interrupt Source Overrides: ISA IRQ to GSIV remaps.
typedef struct _ACPI_ISA_OVERRIDE
{
    ULONG   Gsiv;
    BOOLEAN Valid;
} UACPI_ISA_OVERRIDE;
static UACPI_ISA_OVERRIDE UacpiIsaOverride[16];

static VOID
UacpiIrqLibParseMadt(VOID)
{
    uacpi_table tbl;
    struct acpi_madt *madt;
    uacpi_u8 *p, *end;

    if (uacpi_unlikely_error(
            uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl)) ||
        tbl.ptr == NULL)
        {
        return;
    }
    madt = (struct acpi_madt *)tbl.ptr;
    p   = (uacpi_u8 *)madt->entries;
    end = (uacpi_u8 *)madt + madt->hdr.length;

    while (p + sizeof(struct acpi_entry_hdr) <= end)
    {
        struct acpi_entry_hdr *h = (struct acpi_entry_hdr *)p;
        if (h->length < sizeof(struct acpi_entry_hdr) || p + h->length > end)
        {
            break;
        }
        if (h->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE &&
            h->length >= sizeof(struct acpi_madt_interrupt_source_override))
            {
            struct acpi_madt_interrupt_source_override *iso =
                (struct acpi_madt_interrupt_source_override *)p;
            // Only ISA (bus 0) sources below 16 are IRQ remaps.
            if (iso->bus == 0 && iso->source < 16)
            {
                ULONG trig = iso->flags & ACPI_MADT_TRIGGERING_MASK;
                ULONG pol  = iso->flags & ACPI_MADT_POLARITY_MASK;
                UacpiIsaOverride[iso->source].Gsiv  = iso->gsi;
                UacpiIsaOverride[iso->source].Valid = TRUE;
                // Explicit ISO trigger wins over _CRS; conforming stays edge.
                if (trig == ACPI_MADT_TRIGGERING_LEVEL)
                {
                    UacpiIrqLibNoteLevelGsiv(iso->gsi);
                }
                else if (trig == ACPI_MADT_TRIGGERING_EDGE)
                {
                    UacpiIrqLibNoteEdgeGsiv(iso->gsi);
                }
                // An explicit ISO polarity also wins; conforming follows the trigger.
                if (iso->gsi < UACPI_MAX_GSIV)
                {
                    if (pol == ACPI_MADT_POLARITY_ACTIVE_HIGH)
                    {
                        UacpiGsivIsoHigh[iso->gsi / 32] |= (1ul << (iso->gsi % 32));
                    }
                    else if (pol == ACPI_MADT_POLARITY_ACTIVE_LOW)
                    {
                        UacpiGsivIsoLow[iso->gsi / 32] |= (1ul << (iso->gsi % 32));
                    }
                }
                UacpiTrace("[acpi] irqlib: MADT ISO IRQ %u -> GSIV %u flags 0x%X\n",
                          iso->source, iso->gsi, iso->flags);
            }
        }
        p += h->length;
    }
    uacpi_table_unref(&tbl);
}

// IRQ to GSIV: identity unless remapped by a MADT ISO.
static ULONG
UacpipGsivFromIrq(ULONG Irq)
{
    if (Irq < 16 && UacpiIsaOverride[Irq].Valid)
    {
        return UacpiIsaOverride[Irq].Gsiv;
    }
    return Irq;
}

// Slot 22 (+0x58): same ABI as HalGetInterruptVector.
static ULONG NTAPI
UacpiHalGetInterruptVectorOverride(INTERFACE_TYPE InterfaceType, ULONG BusNumber,
                                  ULONG BusInterruptLevel, ULONG BusInterruptVector,
                                  PKIRQL Irql, PKAFFINITY Affinity)
{
    ULONG gsiv, vector = 0, polarity = 0, mode = 0;
    KIRQL irql = 0;
    KAFFINITY affinity = 0;

    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(polarity);
    UNREFERENCED_PARAMETER(mode);

    // A line interrupt presents Level == Vector.
    if (BusInterruptLevel != BusInterruptVector)
    {
        return 0;
    }
    gsiv = UacpipGsivFromIrq(BusInterruptLevel);

    // Return only an already committed vector; never allocate here.
    if (gsiv >= UACPI_MAX_GSIV || UacpiGsivVector[gsiv] == 0)
    {
        UacpiTrace("[acpi] irqlib: SLOT22 GetInterruptVector(bus %u irq %u) -> no committed "
                  "assignment (0)\n", BusNumber, BusInterruptLevel);
        return 0;
    }
    vector = UacpiGsivVector[gsiv];
    UacpiTrace("[acpi] irqlib: SLOT22 GetInterruptVector(bus %u irq %u) -> gsiv %u vector 0x%X\n",
              BusNumber, BusInterruptLevel, gsiv, vector);
    irql = UacpipHalConvertDeviceIdtToIrql ? UacpipHalConvertDeviceIdtToIrql(vector)
                                          : (KIRQL)(vector >> 4);
    affinity = KeQueryActiveProcessors();
    if (Irql)     *Irql = irql;
    if (Affinity) *Affinity = affinity;
    return vector;
}

// Slot 23 (+0x5c x86, +0xB8 x64): vector to GSIV and polarity. Win7+ passes a
// GROUP_AFFINITY and a remapping-info out-param (see pHalGetVectorInput).
#if (NTDDI_VERSION >= NTDDI_WIN7)
static NTSTATUS NTAPI
UacpiHalGetVectorInputOverride(ULONG Vector, PGROUP_AFFINITY Affinity,
                              PULONG Input, PKINTERRUPT_POLARITY Polarity,
                              PINTERRUPT_REMAPPING_INFO IntRemapInfo)
#else
static NTSTATUS NTAPI
UacpiHalGetVectorInputOverride(ULONG Vector, KAFFINITY Affinity,
                              PULONG Input, PKINTERRUPT_POLARITY Polarity)
#endif
{
    ULONG i;

    UNREFERENCED_PARAMETER(Affinity);

    if (Vector == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    // Each committed vector maps to one GSIV.
    for (i = 0; i < UACPI_MAX_GSIV; i++)
    {
        if (UacpiGsivVector[i] == Vector)
        {
            if (Input)    *Input    = i;
            if (Polarity) *Polarity = UacpipGsivIsActiveLow(i) ? InterruptActiveLow
                                                               : InterruptActiveHigh;
#if (NTDDI_VERSION >= NTDDI_WIN7)
            // No interrupt remapping.
            if (IntRemapInfo) RtlZeroMemory(IntRemapInfo, sizeof(*IntRemapInfo));
#endif
            UacpiTrace("[acpi] irqlib: SLOT23 GetVectorInput(vector 0x%X) -> gsiv %u %s/%s\n",
                      Vector, i, UacpipGsivIsLevel(i) ? "level" : "edge",
                      UacpipGsivIsActiveLow(i) ? "low" : "high");
            return STATUS_SUCCESS;
        }
    }
    // Message vector: STATUS_INVALID_PARAMETER selects the message path.
    for (i = 0; i < UACPI_MSG_MAX; i++)
    {
        if (UacpiMsgAssigns[i].Owner != NULL &&
            UacpiMsgAssigns[i].Count != 0 &&
            Vector >= UacpiMsgAssigns[i].Base &&
            Vector < UacpiMsgAssigns[i].Base + UacpiMsgAssigns[i].Count)
            {
            UacpiTrace("[acpi] irqlib: SLOT23 GetVectorInput(vector 0x%X) -> "
                      "message (msg-gsiv 0x%X, base 0x%X count %u), no line input\n",
                      Vector, UacpiMsgAssigns[i].MsgGsiv,
                      UacpiMsgAssigns[i].Base, UacpiMsgAssigns[i].Count);
            return STATUS_INVALID_PARAMETER;
        }
    }

    // Not ours.
    UacpiTrace("[acpi] irqlib: SLOT23 GetVectorInput(vector 0x%X) -> NOT_FOUND\n", Vector);
    return STATUS_NOT_FOUND;
}

// Slot 27 (+0x6c x86, +0xD8 x64): allocate an MSI target; returns base vector.
static NTSTATUS NTAPI
UacpiHalAllocateMessageTargetOverride(PDEVICE_OBJECT Owner, PVOID ProcessorSet,
                                     ULONG NumberOfIdtEntries, ULONG Mode,
                                     UCHAR ShareVector, PULONG Vector,
                                     PKIRQL Irql, PULONG IdtEntry)
{
    ULONG    base = 0;
    ULONG    count = NumberOfIdtEntries ? NumberOfIdtEntries : 1;
    KIRQL    irql;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ProcessorSet);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(ShareVector);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (g_AcpiInterruptModel != 1)
    {
        return STATUS_NOT_SUPPORTED;   // MSI is an APIC-only capability
    }

    // Claim the arbiter's run for this owner; allocate (MsgGsiv 0) if none.
    ExAcquireFastMutex(&UacpiIrqLibLock);
    {
        ULONG i;
        LONG  freeSlot = -1;

        // Lowest base vector first, matching the ascending connection data order.
        base = 0;
        status = STATUS_NOT_FOUND;
        {
            ULONG bestBase = 0xFFFFFFFF;
            LONG  best = -1;

            for (i = 0; i < UACPI_MSG_MAX; i++)
            {
                if (UacpiMsgAssigns[i].Owner == Owner &&
                    UacpiMsgAssigns[i].Count == count &&
                    !UacpiMsgAssigns[i].Claimed &&
                    UacpiMsgAssigns[i].Base < bestBase)
                    {
                    bestBase = UacpiMsgAssigns[i].Base;
                    best = (LONG)i;
                }
                if (freeSlot < 0 && UacpiMsgAssigns[i].Owner == NULL)
                {
                    freeSlot = (LONG)i;
                }
            }
            if (best >= 0)
            {
                UacpiMsgAssigns[best].Claimed = TRUE;
                base = UacpiMsgAssigns[best].Base;
                status = STATUS_SUCCESS;
                UacpiTrace("[acpi] irqlib: SLOT27 PDO %p claim msg-gsiv 0x%X -> "
                          "vector 0x%X (%u entr%s)\n",
                          Owner, UacpiMsgAssigns[best].MsgGsiv, base, count,
                          count == 1 ? "y" : "ies");
            }
        }
        if (!NT_SUCCESS(status))
        {
            if (freeSlot < 0)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else
            {
                status = UacpipApicAllocateVectorRange(count, &base);
                if (NT_SUCCESS(status))
                {
                    UacpiMsgAssigns[freeSlot].Owner   = Owner;
                    UacpiMsgAssigns[freeSlot].MsgGsiv = 0;   // HAL-originated
                    UacpiMsgAssigns[freeSlot].Base    = base;
                    UacpiMsgAssigns[freeSlot].Count   = count;
                    UacpiMsgAssigns[freeSlot].Claimed = TRUE;
                }
            }
        }
    }
    ExReleaseFastMutex(&UacpiIrqLibLock);

    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] irqlib: MSI allocate (%u entries) for PDO %p failed 0x%X\n",
                  count, Owner, status);
        return status;
    }

    irql = UacpipHalConvertDeviceIdtToIrql ? UacpipHalConvertDeviceIdtToIrql(base)
                                          : (KIRQL)(base >> 4);
    if (Vector)   *Vector   = base;
    if (IdtEntry) *IdtEntry = base;
    if (Irql)     *Irql     = irql;
    UacpiTrace("[acpi] irqlib: MSI target PDO %p -> %u vector(s) base 0x%X irql %u\n",
              Owner, count, base, irql);
    return STATUS_SUCCESS;
}
// Slot 28 (+0x70 x86, +0xE0 x64): release an MSI target.
static VOID NTAPI
UacpiHalFreeMessageTargetOverride(PDEVICE_OBJECT Owner, ULONG Vector, PVOID ProcessorSet)
{
    ULONG count;

    UNREFERENCED_PARAMETER(ProcessorSet);

    ExAcquireFastMutex(&UacpiIrqLibLock);
    {
        ULONG i;

        count = 0;
        for (i = 0; i < UACPI_MSG_MAX; i++)
        {
            if (UacpiMsgAssigns[i].Owner == Owner &&
                Vector >= UacpiMsgAssigns[i].Base &&
                Vector <  UacpiMsgAssigns[i].Base + UacpiMsgAssigns[i].Count)
                {
                count = UacpiMsgAssigns[i].Count;
                UacpiMsgAssigns[i].Claimed = FALSE;
                // Free only HAL-originated runs; arbiter runs persist.
                if (UacpiMsgAssigns[i].MsgGsiv == 0)
                {
                    UacpipApicFreeVectorRange(UacpiMsgAssigns[i].Base, count);
                    RtlZeroMemory(&UacpiMsgAssigns[i], sizeof(UacpiMsgAssigns[i]));
                }
                break;
            }
        }
    }
    ExReleaseFastMutex(&UacpiIrqLibLock);
    UacpiTrace("[acpi] irqlib: MSI free PDO %p vector 0x%X (%u vector(s))\n",
              Owner, Vector, count);
}

// Install the system-wide HAL interrupt overrides; independent of IrqArbEnabled.
int UacpiIrqLibHalOverrides = 1;

// The exported symbol is the table itself; tbl[0] is Version.
static VOID
UacpiHalInstallInterruptOverrides(VOID)
{
    UNICODE_STRING name;
    PULONG_PTR tbl;

    if (!UacpiIrqLibHalOverrides)
    {
        UacpiTrace("[acpi] irqlib: HAL interrupt overrides disabled by "
                  "IrqLibHalOverrides\n");
        return;
    }

    RtlInitUnicodeString(&name, L"HalPrivateDispatchTable");
    tbl = (PULONG_PTR)MmGetSystemRoutineAddress(&name);
    if (tbl == NULL)
    {
        UacpiTrace("[acpi] irqlib: HalPrivateDispatchTable not found - overrides NOT installed\n");
        return;
    }
    // The message-target slots arrive in v6 (Vista SP1); the indices hold on
    // every later release, x86 and x64.
    tbl[22] = (ULONG_PTR)UacpiHalGetInterruptVectorOverride;          // +0x58 / +0xB0
    tbl[23] = (ULONG_PTR)UacpiHalGetVectorInputOverride;              // +0x5c / +0xB8
    if ((ULONG)tbl[0] >= 6)
    {
        tbl[27] = (ULONG_PTR)UacpiHalAllocateMessageTargetOverride;   // +0x6c / +0xD8
        tbl[28] = (ULONG_PTR)UacpiHalFreeMessageTargetOverride;       // +0x70 / +0xE0
    }
    UacpiTrace("[acpi] irqlib: HalPrivateDispatchTable v%u interrupt overrides "
              "installed (slots 22/23%s)\n", (ULONG)tbl[0],
              ((ULONG)tbl[0] >= 6) ? "/27/28" : "");
}

// Bring-up (FDO start, after the PM handshake set g_AcpiInterruptModel).
NTSTATUS
UacpiIrqLibInitialize(VOID)
{
    UNICODE_STRING name;

    if (UacpiIrqLibReady)
    {
        return STATUS_SUCCESS;
    }
    ExInitializeFastMutex(&UacpiIrqLibLock);
    UacpipResolveKernelRoutines();

    // Harvest MADT Interrupt Source Overrides before any GSIV is resolved.
    UacpiIrqLibParseMadt();

    RtlInitUnicodeString(&name, L"HalGetInterruptVector");
    UacpipHalGetInterruptVector =
        (PHAL_GET_INTERRUPT_VECTOR)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"HalConvertDeviceIdtToIrql");
    UacpipHalConvertDeviceIdtToIrql =
        (PHAL_CONVERT_IDT_TO_IRQL)MmGetSystemRoutineAddress(&name);

    if (g_AcpiInterruptModel == 1)
    {
        // APIC: build the per-CPU IDT sets for running processors.
        UacpipSeedExistingProcessors();

        // Hot-add tracking only; a NULL handle is not a failure.
        UacpiProcessorChangeHandle =
            UacpipKeRegisterProcessorChangeCallback != NULL
                ? UacpipKeRegisterProcessorChangeCallback(UacpiProcessorChangeCallback,
                                                          NULL,
                                                          KE_PROCESSOR_CHANGE_ADD_EXISTING)
                : NULL;
        if (UacpiProcessorChangeHandle == NULL)
        {
            UacpiTrace("[acpi] irqlib: no processor-change callback; "
                      "hot-added processors will not be tracked\n");
        }
        UacpiTrace("[acpi] irqlib: APIC model - %u processor IDT set(s), "
                  "HalConvertDeviceIdtToIrql %p\n",
                  UacpiProcessorIdtCount, UacpipHalConvertDeviceIdtToIrql);
        if (UacpiProcessorIdtCount == 0)
        {
            UacpiTrace("[acpi] irqlib: FATAL - no processor IDT sets built\n");
            KeBugCheckEx(0xA3 /* ACPI_DRIVER_INTERNAL */, 0x22, 0, 0, 0);
        }

        // The SCI takes its vector from this allocator when resolved.

        // Route HalGetInterruptVector callers to this allocator.
        UacpiHalInstallInterruptOverrides();
    }
    else
    {
        // PIC: base = HAL vector for the SCI line minus the SCI GSI.
        struct acpi_fadt *fadt = NULL;
        ULONG     sciGsi = (ULONG)-1;
        ULONG     sciVector = 0;
        KIRQL     sciIrql = 0;
        KAFFINITY sciAffinity = 0;

        if (uacpi_likely_success(uacpi_table_fadt(&fadt)) && fadt != NULL)
        {
            sciGsi = fadt->sci_int;
        }

        if (sciGsi != (ULONG)-1 && UacpipHalGetInterruptVector != NULL)
        {
            sciVector = UacpipHalGetInterruptVector(Isa, 0, sciGsi, sciGsi,
                                                    &sciIrql, &sciAffinity);
        }

        if (sciVector != 0 && sciVector >= sciGsi)
        {
            UacpiPicVectorBase = sciVector - sciGsi;
            UacpiPicBaseValid  = TRUE;
            UacpiTrace("[acpi] irqlib: PIC model - vector base 0x%X from the HAL "
                      "(SCI GSI %u -> vector 0x%X irql %u)\n",
                      UacpiPicVectorBase, sciGsi, sciVector, sciIrql);
        }
        else
        {
            UacpiPicVectorBase = 0x30;   // classic PC/AT base (line + 0x30)
            UacpiPicBaseValid  = TRUE;
            UacpiTrace("[acpi] irqlib: PIC model - HAL did not resolve SCI GSI %u "
                      "(vector 0x%X); default base 0x30\n",
                      sciGsi, sciVector);
        }
    }

    UacpiIrqLibReady = TRUE;
    return STATUS_SUCCESS;
}
