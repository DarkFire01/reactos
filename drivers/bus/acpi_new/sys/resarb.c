/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Memory, I/O and bus-number arbiters for a PCI root bridge
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <wdmguid.h>        // GUID_ARBITER_INTERFACE_STANDARD
#include <limits.h>         // LONG_MAX
#include "arbiter.h"        // ARBITER_INSTANCE + the Arb* lib
#include <uacpi/resources.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>   // uacpi_namespace_for_each_child_simple / _root

int UacpiResArbEnabled = 1;

// Per-root memory, I/O and bus arbiters, created on first query (Pdo->ResArb).
typedef struct _ACPI_RES_ARBITERS
{
    ARBITER_INSTANCE Mem;
    ARBITER_INSTANCE Io;
    ARBITER_INSTANCE Bus;
    BOOLEAN MemUp;
    BOOLEAN IoUp;
    BOOLEAN BusUp;
} UACPI_RES_ARBITERS, *PUACPI_RES_ARBITERS;

// Plain address ranges for all three instances; MemoryLarge goes with Memory.
static NTSTATUS NTAPI
UacpiResUnpackRequirement(PIO_RESOURCE_DESCRIPTOR Descriptor, PULONGLONG Minimum,
                         PULONGLONG Maximum, PULONGLONG Length, PULONGLONG Alignment)
{
    if (Descriptor == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    switch (Descriptor->Type)
    {
    case CmResourceTypeMemory:
    case CmResourceTypeMemoryLarge:
        *Minimum   = (ULONGLONG)Descriptor->u.Memory.MinimumAddress.QuadPart;
        *Maximum   = (ULONGLONG)Descriptor->u.Memory.MaximumAddress.QuadPart;
        *Length    = Descriptor->u.Memory.Length;
        *Alignment = Descriptor->u.Memory.Alignment ? Descriptor->u.Memory.Alignment: 1;
        return STATUS_SUCCESS;
    case CmResourceTypePort:
        *Minimum   = (ULONGLONG)Descriptor->u.Port.MinimumAddress.QuadPart;
        *Maximum   = (ULONGLONG)Descriptor->u.Port.MaximumAddress.QuadPart;
        *Length    = Descriptor->u.Port.Length;
        *Alignment = Descriptor->u.Port.Alignment ? Descriptor->u.Port.Alignment: 1;
        return STATUS_SUCCESS;
    case CmResourceTypeBusNumber:
        *Minimum   = Descriptor->u.BusNumber.MinBusNumber;
        *Maximum   = Descriptor->u.BusNumber.MaxBusNumber;
        *Length    = Descriptor->u.BusNumber.Length;
        *Alignment = 1;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS NTAPI
UacpiResPackResource(PIO_RESOURCE_DESCRIPTOR Requirement, ULONGLONG Start,
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor)
{
    if (Requirement == NULL || Descriptor == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    Descriptor->ShareDisposition = Requirement->ShareDisposition;
    Descriptor->Flags            = Requirement->Flags;
    switch (Requirement->Type)
    {
    case CmResourceTypeMemory:
    case CmResourceTypeMemoryLarge:
        Descriptor->Type = CmResourceTypeMemory;
        Descriptor->u.Memory.Start.QuadPart = (LONGLONG)Start;
        Descriptor->u.Memory.Length = Requirement->u.Memory.Length;
        return STATUS_SUCCESS;
    case CmResourceTypePort:
        Descriptor->Type = CmResourceTypePort;
        Descriptor->u.Port.Start.QuadPart = (LONGLONG)Start;
        Descriptor->u.Port.Length = Requirement->u.Port.Length;
        return STATUS_SUCCESS;
    case CmResourceTypeBusNumber:
        Descriptor->Type = CmResourceTypeBusNumber;
        Descriptor->u.BusNumber.Start  = (ULONG)Start;
        Descriptor->u.BusNumber.Length = Requirement->u.BusNumber.Length;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS NTAPI
UacpiResUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
                      PULONGLONG Start, PULONGLONG Length)
{
    if (Descriptor == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    switch (Descriptor->Type)
    {
    case CmResourceTypeMemory:
    case CmResourceTypeMemoryLarge:
        *Start  = (ULONGLONG)Descriptor->u.Memory.Start.QuadPart;
        *Length = Descriptor->u.Memory.Length;
        return STATUS_SUCCESS;
    case CmResourceTypePort:
        *Start  = (ULONGLONG)Descriptor->u.Port.Start.QuadPart;
        *Length = Descriptor->u.Port.Length;
        return STATUS_SUCCESS;
    case CmResourceTypeBusNumber:
        *Start  = Descriptor->u.BusNumber.Start;
        *Length = Descriptor->u.BusNumber.Length;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static INT32 NTAPI
UacpiResScoreRequirement(PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    ULONGLONG min = 0, max = 0, len = 0, align = 0, span;

    if (!NT_SUCCESS(UacpiResUnpackRequirement(Descriptor, &min, &max, &len, &align)) ||
        max < min)
        {
        return -1;   // unsatisfiable -> arbitrated first / rejected
    }
    // Score is the placement count, saturated so 0..MAX does not wrap to 0.
    if (max - min >= (ULONGLONG)LONG_MAX)
    {
        return LONG_MAX;
    }
    span = max - min + 1;
    return (LONG)span;
}

// Get (start, len) from a producer address descriptor of range type wantRange.
static BOOLEAN
UacpipAddressProducerWindow(uacpi_resource *r, UCHAR wantRange,
                           ULONGLONG *start, ULONGLONG *len)
{
    UCHAR rtype = 0, dir = 0;

    switch (r->type)
    {
    case UACPI_RESOURCE_TYPE_ADDRESS16:
        rtype = r->address16.common.type; dir = r->address16.common.direction;
        *start = r->address16.minimum;    *len = r->address16.address_length;
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS32:
        rtype = r->address32.common.type; dir = r->address32.common.direction;
        *start = r->address32.minimum;    *len = r->address32.address_length;
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS64:
        rtype = r->address64.common.type; dir = r->address64.common.direction;
        *start = r->address64.minimum;    *len = r->address64.address_length;
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
        rtype = r->address64_extended.common.type;
        dir   = r->address64_extended.common.direction;
        *start = r->address64_extended.minimum;
        *len   = r->address64_extended.address_length;
        break;
    default:
        return FALSE;
    }
    return (dir == UACPI_PRODUCER && rtype == wantRange && *len != 0);
}

// Range the root does not decode, as opposed to one already allocated.
#define UACPI_RANGE_OUTSIDE_DECODE 0x20

// Invert the root's producer windows into Allocation; only windows stay free.
static VOID
UacpipSeedArbiter(PARBITER_INSTANCE Arbiter, uacpi_namespace_node *node,
                 UCHAR wantRange, const char *label)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    RTL_RANGE_LIST windows;
    RTL_RANGE_LIST gaps;
    RTL_RANGE_LIST_ITERATOR it;
    PRTL_RANGE gap;
    NTSTATUS status;
    ULONG count = 0;

    UacpiTrace("[acpi] resarb: %s evaluating _CRS...\n", label);
    if (uacpi_unlikely_error(uacpi_get_current_resources(node, &res)) || res == NULL)
    {
        UacpiTrace("[acpi] resarb: %s _CRS unavailable\n", label);
        return;
    }
    UacpiTrace("[acpi] resarb: %s _CRS returned, walking descriptors\n", label);
    RtlInitializeRangeList(&windows);
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        ULONGLONG start = 0, len = 0;
        if (UacpipAddressProducerWindow(r, wantRange, &start, &len))
        {
            // Shared: overlapping windows describe one decode, not a conflict.
            (void)RtlAddRange(&windows, start, start + len - 1, 0,
                              RTL_RANGE_LIST_ADD_IF_CONFLICT |
                              RTL_RANGE_LIST_ADD_SHARED, NULL, NULL);
            UacpiTrace("[acpi] resarb: %s window %010I64X..%010I64X\n",
                      label, start, start + len - 1);
            count++;
        }
    }

    // No window of this type: leave the pool open rather than block everything.
    if (count == 0)
    {
        UacpiTrace("[acpi] resarb: %s no producer window in _CRS - pool left open\n",
                  label);
        RtlFreeRangeList(&windows);
        uacpi_free_resources(res);
        return;
    }

    // Tag gaps so UacpiResFindSuitableRange tells undecoded from allocated.
    RtlInitializeRangeList(&gaps);
    status = RtlInvertRangeList(&gaps, &windows);
    if (NT_SUCCESS(status) && NT_SUCCESS(RtlGetFirstRange(&gaps, &it, &gap)))
    {
        do
        {
            status = RtlAddRange(Arbiter->Allocation, gap->Start, gap->End,
                                 UACPI_RANGE_OUTSIDE_DECODE,
                                 RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);
            if (!NT_SUCCESS(status))
            {
                break;
            }
        } while (NT_SUCCESS(RtlGetNextRange(&it, &gap, TRUE)));
    }
    RtlFreeRangeList(&gaps);

    if (!NT_SUCCESS(status))
    {
        // Discard a partial pool and run unbounded.
        UacpiTrace("[acpi] resarb: %s bounding failed 0x%X - pool left open\n",
                  label, status);
        RtlFreeRangeList(Arbiter->Allocation);
        RtlInitializeRangeList(Arbiter->Allocation);
    }
    else
    {
        UacpiTrace("[acpi] resarb: %s pool bounded by %u window(s)\n", label, count);
    }
    RtlFreeRangeList(&windows);
    uacpi_free_resources(res);
}

// Instance bring-up.
static VOID NTAPI UacpiResArbRef(PVOID c)   { UNREFERENCED_PARAMETER(c); }
static VOID NTAPI UacpiResArbDeref(PVOID c) { UNREFERENCED_PARAMETER(c); }

// Ordering-list dump and Test/Commit/Rollback success lines.
extern int UacpiResVerbose;

// Per-window placement tracing in UacpiResFindSuitableRange. High volume.
int UacpiResTraceWindows = 1;

static NTSTATUS NTAPI UacpiResUnpackRequirement(PIO_RESOURCE_DESCRIPTOR, PULONGLONG,
                                               PULONGLONG, PULONGLONG, PULONGLONG);

// Ring of the windows the engine last offered, dumped on a placement failure.
#define UACPI_RES_TRY_RING 24

typedef struct _ACPI_RES_TRY
{
    ULONGLONG Min;
    ULONGLONG Max;
    ULONGLONG Length;
    ULONGLONG Alignment;
    ULONG     Attributes;
    ULONGLONG Placed;
    BOOLEAN   Ok;
} UACPI_RES_TRY;

static UACPI_RES_TRY UacpiResTryRing[UACPI_RES_TRY_RING];
static ULONG        UacpiResTryIndex;

// Unlocked on purpose; a lock would perturb the arbitration being traced.
static VOID
UacpipRecordTry(PARBITER_ALLOCATION_STATE State, BOOLEAN Ok)
{
    UACPI_RES_TRY *slot = &UacpiResTryRing[UacpiResTryIndex++ % UACPI_RES_TRY_RING];

    slot->Min        = State->CurrentMinimum;
    slot->Max        = State->CurrentMaximum;
    slot->Length     = State->CurrentAlternative ? State->CurrentAlternative->Length : 0;
    slot->Alignment  = State->CurrentAlternative ? State->CurrentAlternative->Alignment : 0;
    slot->Attributes = State->RangeAvailableAttributes;
    slot->Placed     = Ok ? State->Start : 0;
    slot->Ok         = Ok;
}

static VOID
UacpipDumpTryRing(VOID)
{
    ULONG count = UacpiResTryIndex;
    ULONG i, first;

    if (count == 0)
    {
        UacpiTrace("[acpi] resarb:   no windows were offered at all "
                  "<== GetNextAllocationRange never produced one\n");
        return;
    }
    first = (count > UACPI_RES_TRY_RING) ? (count - UACPI_RES_TRY_RING) : 0;
    for (i = first; i < count; i++)
    {
        UACPI_RES_TRY *slot = &UacpiResTryRing[i % UACPI_RES_TRY_RING];

        if (slot->Ok)
        {
            UacpiTrace("[acpi] resarb:   tried %010I64X..%010I64X len %I64X align %I64X "
                      "avail 0x%X -> OK @ %010I64X\n",
                      slot->Min, slot->Max, slot->Length, slot->Alignment,
                      slot->Attributes, slot->Placed);
        }
        else
        {
            UacpiTrace("[acpi] resarb:   tried %010I64X..%010I64X len %I64X align %I64X "
                      "avail 0x%X -> refused\n",
                      slot->Min, slot->Max, slot->Length, slot->Alignment,
                      slot->Attributes);
        }
    }
}

static PCWSTR
UacpipArbName(PARBITER_INSTANCE Arbiter)
{
    return (Arbiter != NULL && Arbiter->Name != NULL) ? Arbiter->Name : L"?";
}

// Name a range owner PDO by its hardware ID. Failure path only.
static VOID
UacpipNameOwner(PVOID Owner, char *Buf, ULONG Cb)
{
    PWSTR ids = NULL;
    ULONG needed = 0;
    NTSTATUS status;

    Buf[0] = '\0';

    if (Owner == NULL)
    {
        RtlStringCbCopyA(Buf, Cb, "(unowned)");
        return;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        RtlStringCbCopyA(Buf, Cb, "(irql)");
        return;
    }

    status = IoGetDeviceProperty((PDEVICE_OBJECT)Owner, DevicePropertyHardwareID,
                                 0, NULL, &needed);
    if (status != STATUS_BUFFER_TOO_SMALL || needed == 0 || needed > 4096)
    {
        RtlStringCbPrintfA(Buf, Cb, "(no hwid 0x%X)", status);
        return;
    }
    ids = (PWSTR)ExAllocatePoolWithTag(PagedPool, needed, 'iAcA');
    if (ids == NULL)
    {
        RtlStringCbCopyA(Buf, Cb, "(no pool)");
        return;
    }
    status = IoGetDeviceProperty((PDEVICE_OBJECT)Owner, DevicePropertyHardwareID,
                                 needed, ids, &needed);
    if (NT_SUCCESS(status))
    {
        // First string of the REG_MULTI_SZ is the most specific ID.
        RtlStringCbPrintfA(Buf, Cb, "%S", ids);
    }
    else
    {
        RtlStringCbPrintfA(Buf, Cb, "(hwid 0x%X)", status);
    }
    ExFreePoolWithTag(ids, 'iAcA');
}

// Dump one range list (committed or tentative pool).
static VOID
UacpipDumpPool(PRTL_RANGE_LIST List, const char *Label)
{
    RTL_RANGE_LIST_ITERATOR iterator;
    PRTL_RANGE range;
    ULONG shown = 0;

    if (List == NULL || !NT_SUCCESS(RtlGetFirstRange(List, &iterator, &range)))
    {
        UacpiTrace("[acpi] resarb:   %s pool is EMPTY (everything free)\n", Label);
        return;
    }
    do
    {
        char owner[96];

        UacpipNameOwner(range->Owner, owner, sizeof(owner));
        UacpiTrace("[acpi] resarb:   %s held %010I64X..%010I64X owner %p [%s] "
                   "attr 0x%X flags 0x%X\n",
                  Label, range->Start, range->End, range->Owner, owner,
                  range->Attributes, range->Flags);
        if (++shown >= 96)
        {
            UacpiTrace("[acpi] resarb:   ... (more ranges not shown)\n");
            break;
        }
    } while (NT_SUCCESS(RtlGetNextRange(&iterator, &range, TRUE)));
}

static VOID
UacpipDumpFailure(PARBITER_INSTANCE Arbiter,
                 PARBITER_TEST_ALLOCATION_PARAMETERS Parameters)
{
    PLIST_ENTRY listEntry;

    if (Parameters != NULL && Parameters->ArbitrationList != NULL)
    {
        for (listEntry = Parameters->ArbitrationList->Flink;
             listEntry != Parameters->ArbitrationList;
             listEntry = listEntry->Flink)
             {
            PARBITER_LIST_ENTRY entry =
                CONTAINING_RECORD(listEntry, ARBITER_LIST_ENTRY, ListEntry);
            ULONG i;

            UacpiTrace("[acpi] resarb:   want PDO %p src %u flags 0x%X, %u alternative(s)\n",
                      entry->PhysicalDeviceObject, entry->RequestSource,
                      entry->Flags, entry->AlternativeCount);

            for (i = 0; i < entry->AlternativeCount && i < 8; i++)
            {
                PIO_RESOURCE_DESCRIPTOR d = &entry->Alternatives[i];
                ULONGLONG min = 0, max = 0, len = 0, align = 0;

                if (!NT_SUCCESS(UacpiResUnpackRequirement(d, &min, &max, &len, &align)))
                {
                    UacpiTrace("[acpi] resarb:     [%u] type %u (not unpackable)\n", i, d->Type);
                    continue;
                }
                UacpiTrace("[acpi] resarb:     [%u] type %u opt 0x%X flags 0x%X "
                          "%010I64X..%010I64X len %I64X align %I64X\n",
                          i, d->Type, d->Option, d->Flags, min, max, len, align);
            }
        }
    }

    // PossibleAllocation is already freed on the failure path; dump Allocation.
    UacpipDumpPool(Arbiter->Allocation, "committed");

    // Windows the engine offered, then the registry ordering and reserved lists.
    UacpipDumpTryRing();

    {
        ULONG w;

        UacpiTrace("[acpi] resarb:   ordering %u window(s), reserved %u\n",
                  Arbiter->OrderingList.Count, Arbiter->ReservedList.Count);
        for (w = 0; w < Arbiter->OrderingList.Count && w < 32; w++)
        {
            UacpiTrace("[acpi] resarb:     order[%u] %010I64X..%010I64X\n", w,
                      Arbiter->OrderingList.Orderings[w].Start,
                      Arbiter->OrderingList.Orderings[w].End);
        }
        for (w = 0; w < Arbiter->ReservedList.Count && w < 32; w++)
        {
            UacpiTrace("[acpi] resarb:     resv[%u]  %010I64X..%010I64X\n", w,
                      Arbiter->ReservedList.Orderings[w].Start,
                      Arbiter->ReservedList.Orderings[w].End);
        }
    }
}

// Dump PossibleAllocation on failure, before the test path frees it.
static NTSTATUS NTAPI
UacpiResAllocateEntry(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    NTSTATUS status = ArbiterLibAllocateEntry(Arbiter, State);

    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] resarb: %ws AllocateEntry -> 0x%X, pool as the solver saw it:\n",
                  UacpipArbName(Arbiter), status);
        UacpipDumpPool(Arbiter->PossibleAllocation, "tentative");
    }
    return status;
}

static NTSTATUS NTAPI
UacpiResTestAllocation(PARBITER_INSTANCE Arbiter,
                      PARBITER_TEST_ALLOCATION_PARAMETERS Parameters)
{
    NTSTATUS status;

    // Reset the try-ring so a failure dump shows only this arbitration.
    UacpiResTryIndex = 0;

    status = ArbiterLibTestAllocation(Arbiter, Parameters);

    // Failures always print; success lines are verbose-only.
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] resarb: %ws TestAllocation -> 0x%X (NO PLACEMENT)\n",
                  UacpipArbName(Arbiter), status);
        UacpipDumpFailure(Arbiter, Parameters);
    }
    else if (UacpiResVerbose)
    {
        UacpiTrace("[acpi] resarb: DIAG %ws TestAllocation -> 0x%X (placement ok)\n",
                  UacpipArbName(Arbiter), status);
    }
    return status;
}

static NTSTATUS NTAPI
UacpiResRollbackAllocation(PARBITER_INSTANCE Arbiter)
{
    if (UacpiResVerbose)
    {
        UacpiTrace("[acpi] resarb: DIAG %ws RollbackAllocation\n", UacpipArbName(Arbiter));
    }
    return ArbiterLibRollbackAllocation(Arbiter);
}

static NTSTATUS NTAPI
UacpiResCommitAllocation(PARBITER_INSTANCE Arbiter)
{
    NTSTATUS status = ArbiterLibCommitAllocation(Arbiter);

    if (UacpiResVerbose)
    {
        UacpiTrace("[acpi] resarb: DIAG %ws CommitAllocation -> 0x%X\n",
                  UacpipArbName(Arbiter), status);
    }
    return status;
}

// Boot-config entries may take boot-allocated ranges; fixed ports need this.
static BOOLEAN NTAPI
UacpiResFindSuitableRange(PARBITER_INSTANCE Arbiter,
                         PARBITER_ALLOCATION_STATE State)
{
    BOOLEAN ok;

    if (State != NULL && State->Entry != NULL &&
        (State->Entry->Flags & ARBITER_FLAG_BOOT_CONFIG))
        {
        State->RangeAvailableAttributes |= ARBITER_RANGE_BOOT_ALLOCATED;

        // Fixed boot-config alternatives may also sit outside the root's decode.
        if (State->CurrentAlternative != NULL &&
            (State->CurrentAlternative->Flags & ARBITER_ALTERNATIVE_FLAG_FIXED))
        {
            State->RangeAvailableAttributes |= UACPI_RANGE_OUTSIDE_DECODE;
        }
    }
    ok = ArbiterLibFindSuitableRange(Arbiter, State);

    if (State != NULL)
    {
        UacpipRecordTry(State, ok);
    }

    if (UacpiResTraceWindows && State != NULL)
    {
        UacpiTrace("[acpi] resarb:   try %010I64X..%010I64X len %I64X align %I64X "
                  "avail 0x%X -> %s%s\n",
                  State->CurrentMinimum, State->CurrentMaximum,
                  State->CurrentAlternative ? State->CurrentAlternative->Length : 0,
                  State->CurrentAlternative ? State->CurrentAlternative->Alignment : 0,
                  State->RangeAvailableAttributes,
                  ok ? "OK @ " : "no",
                  "");
        if (ok)
        {
            UacpiTrace("[acpi] resarb:        placed %010I64X..%010I64X\n",
                      State->Start, State->End);
        }
    }
    return ok;
}

// ISA 10/12-bit decode aliases, port arbiter only: every mirror is claimed too.

#ifndef CM_RESOURCE_PORT_10_BIT_DECODE
#define CM_RESOURCE_PORT_10_BIT_DECODE  0x0004
#endif
#ifndef CM_RESOURCE_PORT_12_BIT_DECODE
#define CM_RESOURCE_PORT_12_BIT_DECODE  0x0008
#endif

// Tags alias ranges in dumps; aliases are removed with RtlDeleteRange.
#define UACPI_RANGE_ALIAS 0x10

// Next mirror of Last; FALSE for full 16-bit decode or past the top of I/O.
static BOOLEAN
UacpipGetNextAlias(ULONG DescriptorFlags, ULONGLONG *Alias, ULONGLONG Last)
{
    ULONGLONG next;

    if (DescriptorFlags & CM_RESOURCE_PORT_10_BIT_DECODE)
    {
        next = Last + 0x400;
    }
    else if (DescriptorFlags & CM_RESOURCE_PORT_12_BIT_DECODE)
    {
        next = Last + 0x1000;
    }
    else
    {
        return FALSE;   // decodes the full 16 bits, no mirrors
    }
    if (next > 0xFFFF || next < Last)
    {
        return FALSE;
    }
    *Alias = next;
    return TRUE;
}

// TRUE when every mirror of the candidate placement is free too.
static BOOLEAN
UacpipAliasesAvailable(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    PARBITER_ALTERNATIVE alt = State->CurrentAlternative;
    ULONGLONG alias = 0, last;
    ULONG descFlags, flags;

    if (alt == NULL || alt->Descriptor == NULL || alt->Length == 0)
    {
        return TRUE;
    }
    descFlags = alt->Descriptor->Flags;
    flags = RTL_RANGE_LIST_NULL_CONFLICT_OK;
    if (alt->Flags & ARBITER_ALTERNATIVE_FLAG_SHARED)
    {
        flags |= RTL_RANGE_LIST_SHARED_OK;
    }

    for (last = State->Start; UacpipGetNextAlias(descFlags, &alias, last); last = alias)
    {
        BOOLEAN available = FALSE;

        RtlIsRangeAvailable(Arbiter->PossibleAllocation,
                            alias, alias + alt->Length - 1,
                            flags, State->RangeAvailableAttributes,
                            Arbiter->ConflictCallbackContext,
                            Arbiter->ConflictCallback, &available);
        if (!available)
        {
            // Let OverrideConflict judge the mirror, on a copy of the state.
            ARBITER_ALLOCATION_STATE probe = *State;

            probe.CurrentMinimum = alias;
            probe.CurrentMaximum = alias + alt->Length - 1;
            if (Arbiter->OverrideConflict == NULL ||
                !Arbiter->OverrideConflict(Arbiter, &probe))
                {
                return FALSE;
            }
        }
    }
    return TRUE;
}

// Port search: skip candidates whose mirrors are taken; advance CurrentMinimum.
static BOOLEAN NTAPI
UacpiResPortFindSuitableRange(PARBITER_INSTANCE Arbiter,
                             PARBITER_ALLOCATION_STATE State)
{
    for (;;)
    {
        ULONGLONG next;

        if (!UacpiResFindSuitableRange(Arbiter, State))
        {
            return FALSE;
        }
        if (UacpipAliasesAvailable(Arbiter, State))
        {
            return TRUE;
        }
        if (State->CurrentAlternative == NULL ||
            State->CurrentAlternative->Length == 0)
            {
            return FALSE;
        }
        next = State->Start + State->CurrentAlternative->Length;
        if (next <= State->Start || next > State->CurrentMaximum)
        {
            return FALSE;       // wrapped, or nothing left in the window
        }
        State->CurrentMinimum = next;
    }
}

// Claim the placement through the engine, then every mirror, for the same PDO.
static VOID NTAPI
UacpiResPortAddAllocation(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    PARBITER_ALTERNATIVE alt = State->CurrentAlternative;
    PDEVICE_OBJECT owner = State->Entry ? State->Entry->PhysicalDeviceObject : NULL;
    ULONGLONG alias = 0, last;
    ULONG descFlags, flags;

    ArbiterLibAddAllocation(Arbiter, State);

    if (alt == NULL || alt->Descriptor == NULL || alt->Length == 0)
    {
        return;
    }
    descFlags = alt->Descriptor->Flags;
    flags = RTL_RANGE_LIST_ADD_IF_CONFLICT;
    if (alt->Flags & ARBITER_ALTERNATIVE_FLAG_SHARED)
    {
        flags |= RTL_RANGE_LIST_ADD_SHARED;
    }

    for (last = State->Start; UacpipGetNextAlias(descFlags, &alias, last); last = alias)
    {
        (void)RtlAddRange(Arbiter->PossibleAllocation,
                          alias, alias + alt->Length - 1,
                          (UCHAR)(State->RangeAttributes | UACPI_RANGE_ALIAS),
                          flags, NULL, owner);
    }
}

// Remove the mirrors, then let the engine undo the placement.
static VOID NTAPI
UacpiResPortBacktrackAllocation(PARBITER_INSTANCE Arbiter,
                               PARBITER_ALLOCATION_STATE State)
{
    PARBITER_ALTERNATIVE alt = State->CurrentAlternative;
    PDEVICE_OBJECT owner = State->Entry ? State->Entry->PhysicalDeviceObject : NULL;
    ULONGLONG alias = 0, last;

    if (alt != NULL && alt->Descriptor != NULL && alt->Length != 0)
    {
        for (last = State->Start;
             UacpipGetNextAlias(alt->Descriptor->Flags, &alias, last);
             last = alias)
             {
            (void)RtlDeleteRange(Arbiter->PossibleAllocation,
                                 alias, alias + alt->Length - 1, owner);
        }
    }
    ArbiterLibBacktrackAllocation(Arbiter, State);
}

static NTSTATUS
UacpipInitOneArbiter(PARBITER_INSTANCE Arbiter, PDEVICE_OBJECT Owner,
                    uacpi_namespace_node *node, CM_RESOURCE_TYPE cmType,
                    UCHAR wantRange, PCWSTR name, PCWSTR order, const char *label)
{
    NTSTATUS status;

    RtlZeroMemory(Arbiter, sizeof(*Arbiter));
    Arbiter->UnpackRequirement = UacpiResUnpackRequirement;
    Arbiter->PackResource      = UacpiResPackResource;
    Arbiter->UnpackResource    = UacpiResUnpackResource;
    Arbiter->ScoreRequirement  = UacpiResScoreRequirement;
    // Bus number keeps the default search; port uses the alias-aware wrapper.
    if (cmType == CmResourceTypeMemory)
    {
        Arbiter->FindSuitableRange = UacpiResFindSuitableRange;
    }
    else if (cmType == CmResourceTypePort)
    {
        Arbiter->FindSuitableRange   = UacpiResPortFindSuitableRange;
        Arbiter->AddAllocation       = UacpiResPortAddAllocation;
        Arbiter->BacktrackAllocation = UacpiResPortBacktrackAllocation;
    }
    // Set before ArbiterLibInitializeInstance, which only fills NULL slots.
    Arbiter->AllocateEntry      = UacpiResAllocateEntry;
    Arbiter->TestAllocation     = UacpiResTestAllocation;
    Arbiter->RollbackAllocation = UacpiResRollbackAllocation;
    Arbiter->CommitAllocation   = UacpiResCommitAllocation;

    status = ArbiterLibInitializeInstance(Arbiter, Owner, cmType, name, order, NULL);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] resarb: %s ArbiterLibInitializeInstance failed 0x%X\n",
                  label, status);
        return status;
    }
    UacpiTrace("[acpi] resarb: %s seeding\n", label);
    UacpipSeedArbiter(Arbiter, node, wantRange, label);

    // Memory only: block inaccessible ranges, boot-reserve the MMCONFIG window.
    if (cmType == CmResourceTypeMemory)
    {
        (VOID)ArbiterLibAddInaccessibleAllocationRange(Arbiter, order, Arbiter->Allocation);
        (VOID)ArbiterLibAddMmConfigRangeAsBootReserved(Arbiter, Arbiter->Allocation);
    }
    UacpiTrace("[acpi] resarb: %s seeded\n", label);

    // Dump the registry ordering and reserved lists once.
    if (UacpiResVerbose)
    {
        ULONG i;

        UacpiTrace("[acpi] resarb: %s ordering list: %u window(s), %u reserved\n",
                  label, Arbiter->OrderingList.Count, Arbiter->ReservedList.Count);
        for (i = 0; i < Arbiter->OrderingList.Count; i++)
        {
            UacpiTrace("[acpi] resarb:   order[%u] %010I64X..%010I64X\n", i,
                      Arbiter->OrderingList.Orderings[i].Start,
                      Arbiter->OrderingList.Orderings[i].End);
        }
        for (i = 0; i < Arbiter->ReservedList.Count; i++)
        {
            UacpiTrace("[acpi] resarb:   resv[%u]  %010I64X..%010I64X\n", i,
                      Arbiter->ReservedList.Orderings[i].Start,
                      Arbiter->ReservedList.Orderings[i].End);
        }
    }
    return STATUS_SUCCESS;
}

// Lazily create the root's three arbiters on first query.
static PUACPI_RES_ARBITERS
UacpipEnsureResArbiters(PUACPI_PDO Pdo)
{
    PUACPI_RES_ARBITERS a = (PUACPI_RES_ARBITERS)Pdo->ResArb;

    if (a != NULL)
    {
        return a;
    }
    UacpiTrace("[acpi] resarb: %s building arbiters\n", Pdo->Name);
    a = (PUACPI_RES_ARBITERS)ExAllocatePoolWithTag(NonPagedPool, sizeof(*a),
                                                  UACPI_POOL_TAG);
    if (a == NULL)
    {
        return NULL;
    }
    RtlZeroMemory(a, sizeof(*a));

    if (NT_SUCCESS(UacpipInitOneArbiter(&a->Mem, Pdo->Common.Self, Pdo->Node,
                                       CmResourceTypeMemory, UACPI_RANGE_MEMORY,
                                       L"ACPI_Memory", L"Root", "MEM")))
                                       {
        a->MemUp = TRUE;
    }
    if (NT_SUCCESS(UacpipInitOneArbiter(&a->Io, Pdo->Common.Self, Pdo->Node,
                                       CmResourceTypePort, UACPI_RANGE_IO,
                                       L"ACPI_Port", L"Root", "IO")))
                                       {
        a->IoUp = TRUE;
    }
    if (NT_SUCCESS(UacpipInitOneArbiter(&a->Bus, Pdo->Common.Self, Pdo->Node,
                                       CmResourceTypeBusNumber, UACPI_RANGE_BUS,
                                       L"ACPI_BusNumber", L"Root", "BUS")))
                                       {
        a->BusUp = TRUE;
    }
    // Motherboard and ECAM ranges arrive as PNP0C02/PNP0103 boot configurations.
    Pdo->ResArb = a;
    UacpiTrace("[acpi] resarb: %s arbiters up (mem=%u io=%u bus=%u)\n",
              Pdo->Name, a->MemUp, a->IoUp, a->BusUp);
    return a;
}

VOID
UacpiResArbiterTeardown(PUACPI_PDO Pdo)
{
    PUACPI_RES_ARBITERS a = (PUACPI_RES_ARBITERS)Pdo->ResArb;

    if (a == NULL)
    {
        return;
    }
    if (a->MemUp) { ArbiterLibDeleteInstance(&a->Mem); }
    if (a->IoUp)  { ArbiterLibDeleteInstance(&a->Io); }
    if (a->BusUp) { ArbiterLibDeleteInstance(&a->Bus); }
    ExFreePool(a);
    Pdo->ResArb = NULL;
}

// Hand out the root's arbiter for the queried type, or STATUS_NOT_SUPPORTED.
NTSTATUS
UacpiQueryResArbiter(PUACPI_PDO Pdo, PIO_STACK_LOCATION sp)
{
    PARBITER_INTERFACE ai = (PARBITER_INTERFACE)sp->Parameters.QueryInterface.Interface;
    ULONG_PTR type = (ULONG_PTR)sp->Parameters.QueryInterface.InterfaceSpecificData;
    PUACPI_RES_ARBITERS a;
    PARBITER_INSTANCE inst;
    const char *label;

    if (!UacpiResArbEnabled || !UacpiHidIsPciRoot(Pdo->Hid))
    {
        return STATUS_NOT_SUPPORTED;   // only a PCI root owns these pools
    }
    if (type != CmResourceTypeMemory && type != CmResourceTypePort &&
        type != CmResourceTypeBusNumber)
        {
        return STATUS_NOT_SUPPORTED;   // interrupt / other -> not this handler
    }
    if (sp->Parameters.QueryInterface.Size < sizeof(ARBITER_INTERFACE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    a = UacpipEnsureResArbiters(Pdo);
    if (a == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    switch (type)
    {
    case CmResourceTypeMemory:    inst = a->MemUp ? &a->Mem : NULL; label = "memory"; break;
    case CmResourceTypePort:      inst = a->IoUp  ? &a->Io  : NULL; label = "io";     break;
    default:                      inst = a->BusUp ? &a->Bus : NULL; label = "bus";    break;
    }
    if (inst == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ai->Size                 = sizeof(ARBITER_INTERFACE);
    ai->Version              = 1;
    ai->Context              = inst;
    ai->InterfaceReference   = UacpiResArbRef;
    ai->InterfaceDereference = UacpiResArbDeref;
    ai->ArbiterHandler       = ArbiterLibHandler;
    ai->Flags                = 0;      // full arbiter

    UacpiTrace("[acpi] resarb: provided ARBITER_INTERFACE (%s) to %s\n", label, Pdo->Name);
    return STATUS_SUCCESS;
}
