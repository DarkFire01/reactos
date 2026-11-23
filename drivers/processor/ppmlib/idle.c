/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI C-states, idle handlers and _CSD idle domains
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* PM1 control and status bits used around C3 */
#define PPMLIB_PM1_BM_RLD               0x0002
#define PPMLIB_PM1_BM_STS               0x0010
#define PPMLIB_PM2_ARB_DIS              0x0001

/* Every ACPI state keeps caches coherent and the thread context */
#define PPMLIB_IDLE_BASE_FLAGS          (PPM_IDLE_STATE_INTERRUPTIBLE | \
                                         PPM_IDLE_STATE_CACHE_COHERENT | \
                                         PPM_IDLE_STATE_CONTEXT_RETAINED)

/* _CST latencies are in microseconds, the kernel takes 100ns units */
#define PPMLIB_LATENCY_SCALE            10

/* HANDLERS *******************************************************************/

static
VOID
NTAPI
PpmLibHaltIdle(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);

    _enable();
    __halt();
}

/* Some C1 entries ask for an I/O read before halting */
static
VOID
NTAPI
PpmLibIoHaltIdle(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    PpmLibReadRegister(&Context->Register);
    _enable();
    __halt();
}

static
VOID
NTAPI
PpmLibClearBusMasterReload(VOID)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;

    PpmLibWriteRegister(&Fadt->Pm1aControl, PpmLibReadRegister(&Fadt->Pm1aControl) & ~PPMLIB_PM1_BM_RLD);
    if (Fadt->Pm1bControl.Address != 0)
        PpmLibWriteRegister(&Fadt->Pm1bControl, PpmLibReadRegister(&Fadt->Pm1bControl) & ~PPMLIB_PM1_BM_RLD);
}

/* Reading P_LVL2 enters C2, a C3 before it may have left BM_RLD set */
static
VOID
NTAPI
PpmLibC2Idle(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    if (InterlockedExchange(&PpmLib.BusMasterReload, 0) == 1)
        PpmLibClearBusMasterReload();

    PpmLibReadRegister(&Context->Register);
}

/* C3 with bus master wakeup and arbitration disabled around the P_LVL3 read */
static
VOID
NTAPI
PpmLibC3Idle(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;
    ULONG64 Pm2;

    InterlockedExchange(&PpmLib.BusMasterReload, 1);

    PpmLibWriteRegister(&Fadt->Pm1aControl, PpmLibReadRegister(&Fadt->Pm1aControl) | PPMLIB_PM1_BM_RLD);
    if (Fadt->Pm1bControl.Address != 0)
        PpmLibWriteRegister(&Fadt->Pm1bControl, PpmLibReadRegister(&Fadt->Pm1bControl) | PPMLIB_PM1_BM_RLD);

    Pm2 = PpmLibReadRegister(&Fadt->Pm2Control);
    PpmLibWriteRegister(&Fadt->Pm2Control, Pm2 | PPMLIB_PM2_ARB_DIS);
    PpmLibReadRegister(&Context->Register);
    PpmLibWriteRegister(&Fadt->Pm2Control, Pm2);
}

/**
 * @brief Refuses a bus master avoiding state while bus master traffic was seen.
 */
NTSTATUS
NTAPI
PpmLibBusMasterCheck(VOID)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;
    UCHAR Status;

    Status = (UCHAR)PpmLibReadRegister(&Fadt->Pm1aEvent);
    if (Fadt->Pm1bEvent.Address != 0)
        Status |= (UCHAR)PpmLibReadRegister(&Fadt->Pm1bEvent);

    if (!(Status & PPMLIB_PM1_BM_STS))
        return STATUS_SUCCESS;

    PpmLibWriteRegister(&Fadt->Pm1aEvent, PPMLIB_PM1_BM_STS);
    if (Fadt->Pm1bEvent.Address != 0)
        PpmLibWriteRegister(&Fadt->Pm1bEvent, PPMLIB_PM1_BM_STS);

    return STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
PpmLibC3Check(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    return PpmLibBusMasterCheck();
}

/* KERNEL CALLBACKS ***********************************************************/

/**
 * @brief Picks the next C-state: an override, the deepest state when parked,
 * or one step of promotion or demotion per time check window.
 */
static
VOID
NTAPI
PpmLibIdlePrepare(
    _Inout_ PPROCESSOR_IDLE_PREPARE_INFO Info)
{
    PPPMLIB_IDLE_RUNTIME Runtime = Info->Context;
    PPM_IDLE_CONSTRAINTS *Constraints = &Info->Constraints;
    BOOLEAN Snapshot = FALSE;
    ULONG64 Elapsed;
    ULONG64 Idle;
    ULONG Preferred;
    ULONG State;
    ULONG Index;

    if (Constraints->OverrideState != MAXULONG)
    {
        State = min(Constraints->OverrideState, Runtime->Count - 1);
        Preferred = 0;
        for (Index = 1; Index < Runtime->PreferredCount; Index++)
        {
            if (Runtime->PreferredStates[Index] <= State)
                Preferred = Index;
        }
        Snapshot = TRUE;
    }
    else if (Constraints->Parked)
    {
        Preferred = Runtime->PreferredCount - 1;
        State = Runtime->PreferredStates[Preferred];
        Snapshot = TRUE;
    }
    else
    {
        Preferred = Runtime->Preferred;
        Elapsed = Constraints->TotalTime - Runtime->LastTotal;
        if (Elapsed >= Constraints->TimeCheck)
        {
            Idle = Constraints->IdleTime - Runtime->LastIdle;
            if (Idle < (ULONG)(Constraints->DemotePercent * Elapsed / 100) && Preferred > 0)
                Preferred -= 1;
            else if (Idle >= (ULONG)(Constraints->PromotePercent * Elapsed / 100) &&
                     Preferred + 1 < Runtime->PreferredCount)
                Preferred++;

            Snapshot = TRUE;
        }

        State = Runtime->PreferredStates[Preferred];
    }

    Runtime->Target = State;
    Runtime->Preferred = Preferred;
    if (Snapshot)
    {
        Runtime->LastTotal = Constraints->TotalTime;
        Runtime->LastIdle = Constraints->IdleTime;
    }

    /* A state whose precondition fails falls back one preferred step */
    while (State != 0 && Runtime->State[State].Check != NULL)
    {
        if (NT_SUCCESS(Runtime->State[State].Check(&Runtime->State[State].Context)))
            break;

        Info->IdleSelectFailureMask |= 1UL << State;
        Preferred -= 1;
        State = Runtime->PreferredStates[Preferred];
    }

    Runtime->Selected = State;
    Info->ProcessorIdleStateIndex = State;
    Info->DependencyUsed = 0;
    Info->PlatformIdleStateIndex = PPM_IDLE_INDEX_NONE;
}

static
NTSTATUS
NTAPI
PpmLibIdleExecute(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState)
{
    PPPMLIB_IDLE_RUNTIME Runtime = Context;

    UNREFERENCED_PARAMETER(PlatformState);

    Runtime->State[ProcessorState].Handler(&Runtime->State[ProcessorState].Context);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
PpmLibIdleComplete(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ProcessorState);
    UNREFERENCED_PARAMETER(PlatformState);
}

static
VOID
NTAPI
PpmLibIdleCancel(
    _In_ PVOID Context,
    _In_ ULONG CancelCode)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(CancelCode);
}

static
BOOLEAN
NTAPI
PpmLibIdleNever(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return FALSE;
}

/* STATE TABLES ***************************************************************/

static
NTSTATUS
NTAPI
PpmLibValidateCst(
    _In_opt_ PPPMLIB_CST Cst)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;
    PPPMLIB_CSTATE Entry;
    ULONG Index;

    if (Cst == NULL)
        return STATUS_UNSUCCESSFUL;

    for (Index = 1; Index < Cst->Count; Index++)
    {
        if (Cst->State[Index].Type < Cst->State[Index - 1].Type ||
            Cst->State[Index].Latency < Cst->State[Index - 1].Latency ||
            Cst->State[Index].Power > Cst->State[Index - 1].Power)
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    /* I/O states need the PM blocks to enter and to watch bus masters */
    for (Index = 0; Index < Cst->Count; Index++)
    {
        Entry = &Cst->State[Index];
        if (Entry->Register.SpaceId == PPMLIB_SPACE_FFH)
            continue;

        if (Entry->Type <= 1)
            return STATUS_UNSUCCESSFUL;

        if (Entry->Type != 2 &&
            (!PpmLibIsValidRegister(&Fadt->Pm2Control) || !PpmLibIsValidRegister(&Fadt->Pm1aEvent)))
        {
            return STATUS_UNSUCCESSFUL;
        }

        if (!PpmLibIsValidRegister(&Fadt->Pm1aControl))
            return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Reads and checks _CST.
 */
NTSTATUS
NTAPI
PpmLibInitCstAndValidate(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CST *Cst)
{
    PPPMLIB_CST Table;
    NTSTATUS Status;

    Status = PpmLibEvaluateCst(Device, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = PpmLibValidateCst(Table);
    if (!NT_SUCCESS(Status))
    {
        Device->Invalidated |= PPMLIB_ERROR_CST;
        ExFreePoolWithTag(Table, PPMLIB_TAG);
        return Status;
    }

    *Cst = Table;
    return STATUS_SUCCESS;
}

/* C1 always, C2 and C3 from P_BLK and the FADT on a single processor system */
static
NTSTATUS
NTAPI
PpmLibInitFadtCStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;
    BOOLEAN C2 = FALSE;
    BOOLEAN C3 = FALSE;
    PPPMLIB_CSTATE Entry;
    PPPMLIB_CST Cst;
    ULONG Count = 1;

    Device->Found |= Device->Enabled & PPMLIB_ACPI1_IDLE;

    if ((Device->Enabled & PPMLIB_ACPI1_C2) &&
        Fadt->C2Latency <= 100 &&
        PpmLibIsValidRegister(&Fadt->Pm1aControl) &&
        PpmLibIsValidRegister(&Device->PBlk) &&
        PpmLib.ProcessorCount == 1)
    {
        C2 = TRUE;
        Count++;
    }

    if ((Device->Enabled & PPMLIB_ACPI1_C3) &&
        Fadt->C3Latency <= 1000 &&
        PpmLibIsValidRegister(&Fadt->Pm2Control) &&
        PpmLibIsValidRegister(&Fadt->Pm1aEvent) &&
        PpmLibIsValidRegister(&Fadt->Pm1aControl) &&
        PpmLibIsValidRegister(&Device->PBlk) &&
        PpmLib.ProcessorCount == 1)
    {
        C3 = TRUE;
        Count++;
    }

    Cst = ExAllocatePoolWithTag(NonPagedPoolNx, FIELD_OFFSET(PPMLIB_CST, State[Count]), PPMLIB_TAG);
    if (Cst == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Cst, FIELD_OFFSET(PPMLIB_CST, State[Count]));

    /* An FFH register with no width means a plain halt */
    Entry = &Cst->State[Cst->Count++];
    Entry->Register.SpaceId = PPMLIB_SPACE_FFH;
    Entry->Type = 1;

    if (C2)
    {
        Entry = &Cst->State[Cst->Count++];
        Entry->Type = 2;
        Entry->Latency = Fadt->C2Latency;
        Entry->Register.SpaceId = PPMLIB_SPACE_IO;
        Entry->Register.BitWidth = 8;
        Entry->Register.AccessBits = 8;
        Entry->Register.Address = (ULONG)(Device->PBlk.Address + 4);
    }

    if (C3)
    {
        Entry = &Cst->State[Cst->Count++];
        Entry->Type = 3;
        Entry->Latency = Fadt->C3Latency;
        Entry->Register.SpaceId = PPMLIB_SPACE_IO;
        Entry->Register.BitWidth = 8;
        Entry->Register.AccessBits = 8;
        Entry->Register.Address = (ULONG)(Device->PBlk.Address + 5);
    }

    Device->Cst = Cst;
    return STATUS_SUCCESS;
}

/**
 * @brief Builds the C-state table from _CST, or from the FADT when _CST is
 * missing or unusable.
 */
NTSTATUS
NTAPI
PpmLibInitCStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PpmLibAcquireAcpi(Device);

    if (Device->Enabled & PPMLIB_CST_IDLE)
    {
        Status = PpmLibInitCstAndValidate(Device, &Device->Cst);
        if (NT_SUCCESS(Status))
            goto Exit;

        Device->Enabled &= ~PPMLIB_CST_IDLE;
    }

    if (Device->Enabled & PPMLIB_ACPI1_IDLE)
    {
        Status = PpmLibInitFadtCStates(Device);
        if (!NT_SUCCESS(Status))
            Device->Enabled &= ~PPMLIB_ACPI1_IDLE;
    }

Exit:
    PpmLibReleaseAcpi(Device);
    return Status;
}

/**
 * @brief Turns one _CST entry into a handler. The vendor gets the first try.
 */
static
NTSTATUS
NTAPI
PpmLibDecodeCState(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_CSTATE CState,
    _Out_ PBOOLEAN InterruptsEnabled,
    _Inout_ PPPMLIB_IDLE_STATE State)
{
    PPMLIB_GAS Register = CState->Register;
    PPPMLIB_IDLE_HANDLER Handler;
    PPPMLIB_IDLE_CHECK Check = NULL;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    if (PpmLib.Vendor.DecodeCState)
    {
        Status = PpmLib.Vendor.DecodeCState(Device, CState, InterruptsEnabled, State);
        if (NT_SUCCESS(Status))
            return Status;
    }

    if (Register.SpaceId == PPMLIB_SPACE_FFH)
    {
        if (Register.BitWidth != 1)
        {
            /* Halt is the only generic FFH state */
            if (CState->Type != 1)
                return Status;

            Handler = PpmLibHaltIdle;
        }
        else if (Register.BitOffset == 1)
        {
            /* Read the I/O port in the address, then halt */
            if ((ULONG)Register.Address != 0)
            {
                Register.Address = (ULONG)Register.Address;
                Register.SpaceId = PPMLIB_SPACE_IO;
                Register.BitWidth = 8;
                Register.BitOffset = 0;
                Register.AccessBits = 8;
            }

            Handler = PpmLibIoHaltIdle;
        }
        else
        {
            return Status;
        }

        *InterruptsEnabled = TRUE;
    }
    else if (Register.SpaceId == PPMLIB_SPACE_IO && CState->Type > 1)
    {
        PpmLibTranslateAccessSize(&Register);
        if (CState->Type == 2)
        {
            Handler = PpmLibC2Idle;
        }
        else
        {
            Handler = PpmLibC3Idle;
            Check = PpmLibC3Check;
        }

        *InterruptsEnabled = FALSE;
    }
    else
    {
        return Status;
    }

    State->Check = Check;
    State->Handler = Handler;
    State->Context.Register = Register;
    State->Native = TRUE;
    return STATUS_SUCCESS;
}

static
ULONG
NTAPI
PpmLibCStateClass(
    _In_ PPPMLIB_CSTATE CState)
{
    return min(CState->Type, 3);
}

/**
 * @brief Fills at most one kernel state per C1, C2 and C3 class from _CST.
 */
static
NTSTATUS
NTAPI
PpmLibBuildCStates(
    _Inout_ PPPMLIB_DEVICE Device,
    _Inout_ PPPM_IDLE_STATES_REGISTRATION Registration,
    _Inout_ PPPMLIB_IDLE_RUNTIME Runtime)
{
    PPPMLIB_CST Cst = Device->Cst;
    PPMLIB_CSTATE Halt;
    PPPMLIB_CSTATE Next;
    BOOLEAN InterruptsEnabled = FALSE;
    ULONG Target = 1;
    ULONG Cursor = 0;
    ULONG Slot;

    if (Cst == NULL || Cst->Count == 0)
        return STATUS_UNSUCCESSFUL;

    Registration->IdlePrepare = PpmLibIdlePrepare;
    Registration->IdleExecute = PpmLibIdleExecute;
    Registration->IdleComplete = PpmLibIdleComplete;
    Registration->IdleCancel = PpmLibIdleCancel;
    Registration->IdleIsHalted = PpmLibIdleNever;
    Registration->IdleInitiateWake = PpmLibIdleNever;
    Registration->MaximumDependencies = 0;
    Registration->EstimateIdleDuration = FALSE;
    Registration->ProcessorIdleCount = 0;
    Runtime->Count = 0;

    /* Firmware without a C1 entry still gets halt as the first state */
    if (Cst->State[0].Type != 1)
    {
        RtlZeroMemory(&Halt, sizeof(Halt));
        Halt.Register.SpaceId = PPMLIB_SPACE_FFH;
        Halt.Type = 1;
        PpmLibDecodeCState(Device, &Halt, &InterruptsEnabled, &Runtime->State[0]);

        Registration->State[0].StateFlags = PPMLIB_IDLE_BASE_FLAGS | (1 << PPM_IDLE_STATE_CSTATE_SHIFT);
        if (InterruptsEnabled)
            Registration->State[0].StateFlags |= PPM_IDLE_STATE_INTERRUPTS_ENABLED;

        Registration->ProcessorIdleCount = 1;
        Runtime->Count = 1;
        Target = 2;
    }

    for (; Cursor < Cst->Count && Target <= 3; Target++)
    {
        if (PpmLibCStateClass(&Cst->State[Cursor]) != Target)
            continue;

        /* Of adjacent entries in one class take the one with the lowest latency */
        while (Cursor + 1 < Cst->Count)
        {
            Next = &Cst->State[Cursor + 1];
            if (PpmLibCStateClass(Next) != Target)
                break;
            if (Next->Latency > Cst->State[Cursor].Latency ||
                (Next->Latency == Cst->State[Cursor].Latency && Next->Type >= Cst->State[Cursor].Type))
            {
                break;
            }
            Cursor++;
        }

        Slot = Registration->ProcessorIdleCount;
        Runtime->State[Slot].CState = &Cst->State[Cursor];
        if (NT_SUCCESS(PpmLibDecodeCState(Device, &Cst->State[Cursor], &InterruptsEnabled, &Runtime->State[Slot])))
        {
            Registration->State[Slot].StateFlags = PPMLIB_IDLE_BASE_FLAGS | (Target << PPM_IDLE_STATE_CSTATE_SHIFT);
            if (InterruptsEnabled)
                Registration->State[Slot].StateFlags |= PPM_IDLE_STATE_INTERRUPTS_ENABLED;

            Registration->State[Slot].Latency = PPMLIB_LATENCY_SCALE * Cst->State[Cursor].Latency;
            Registration->State[Slot].Power = Cst->State[Cursor].Power;
            Registration->ProcessorIdleCount++;
            Runtime->Count++;
        }
        else
        {
            Device->Invalidated |= PPMLIB_ERROR_CST;
        }

        Cursor++;
    }

    Runtime->PreferredCount = (UCHAR)Runtime->Count;
    for (Slot = 0; Slot < Runtime->PreferredCount; Slot++)
        Runtime->PreferredStates[Slot] = (UCHAR)Slot;

    return STATUS_SUCCESS;
}

/* IDLE DOMAINS ***************************************************************/

static
NTSTATUS
NTAPI
PpmLibValidateCsd(
    _In_ PPPMLIB_CSD Csd,
    _In_ PPPMLIB_CST Cst)
{
    PPPMLIB_IDLE_DEPENDENCY Entry;
    ULONG Index;
    ULONG Other;

    for (Index = 0; Index < Csd->Count; Index++)
    {
        Entry = &Csd->Entry[Index];
        if (Entry->Entries != 6)
            return STATUS_UNSUCCESSFUL;

        /* Only hardware coordinated idle is understood */
        if (Entry->Coordination != PPM_COORDINATION_HW_ALL)
            return STATUS_NOT_SUPPORTED;

        if (Entry->Processors == 0 || Entry->Processors > 32 || Entry->Index >= Cst->Count)
            return STATUS_UNSUCCESSFUL;

        for (Other = 0; Other < Csd->Count; Other++)
        {
            if (Other != Index && Csd->Entry[Other].Index == Entry->Index)
                return STATUS_NOT_SUPPORTED;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Reads and checks _CSD against the given _CST.
 */
NTSTATUS
NTAPI
PpmLibInitIdleDomain(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_CST Cst,
    _Outptr_result_maybenull_ PPPMLIB_CSD *Csd)
{
    PPPMLIB_CSD Table;
    NTSTATUS Status;

    Status = PpmLibEvaluateCsd(Device, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = PpmLibValidateCsd(Table, Cst);
    if (!NT_SUCCESS(Status))
    {
        Device->Invalidated |= PPMLIB_ERROR_CSD;
        ExFreePoolWithTag(Table, PPMLIB_TAG);
        return Status;
    }

    *Csd = Table;
    return STATUS_SUCCESS;
}

/* Tells the kernel which processors share each of this processor's states */
static
NTSTATUS
NTAPI
PpmLibUpdateIdleDomains(
    _In_opt_ PPPMLIB_DEVICE Device)
{
    PPPM_IDLE_DOMAINS_REGISTRATION Registration;
    PPPMLIB_IDLE_DOMAIN Domain;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    if (Device == NULL || Device->IdleRuntime == NULL)
        return STATUS_SUCCESS;

    Count = Device->IdleRuntime->Count;
    Registration = ExAllocatePoolWithTag(NonPagedPoolNx,
                                         FIELD_OFFSET(PPM_IDLE_DOMAINS_REGISTRATION, State[Count]),
                                         PPMLIB_TAG);
    if (Registration == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Registration, FIELD_OFFSET(PPM_IDLE_DOMAINS_REGISTRATION, State[Count]));
    Registration->Version = PPM_DRIVER_INTERFACE_VERSION;
    Registration->Count = Count;
    Registration->TargetProcessors = AFFINITY_MASK(Device->NtNumber);

    for (Index = 0; Index < Count; Index++)
    {
        if (Device->IdleRuntime->State[Index].Entry == NULL)
            continue;

        /* A domain is only reported once all its processors arrived */
        Domain = Device->IdleRuntime->State[Index].Entry->Domain;
        if (Domain && Domain->Count >= Domain->Total)
            Registration->State[Index].Members = Domain->Mask;
    }

    Status = PpmLib.Kernel.RegisterIdleDomains(Registration);
    ExFreePoolWithTag(Registration, PPMLIB_TAG);
    return Status;
}

static
VOID
NTAPI
PpmLibUpdateIdleDomainSet(
    _In_ KAFFINITY Processors)
{
    ULONG Index;

    for (Index = 0; Index < PPMLIB_MAX_PROCESSORS; Index++)
    {
        if (Processors & AFFINITY_MASK(Index))
            PpmLibUpdateIdleDomains(PpmLib.Devices[Index]);
    }
}

/* Adds this processor's state to a _CSD domain, TRUE when the domain is complete */
static
BOOLEAN
NTAPI
PpmLibJoinIdleDomain(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_IDLE_DEPENDENCY Dependency)
{
    PPPMLIB_IDLE_DOMAIN Domain = NULL;
    PPPMLIB_IDLE_LINK Link;
    PLIST_ENTRY Entry;

    for (Entry = PpmLib.IdleDomainList.Flink; Entry != &PpmLib.IdleDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPMLIB_IDLE_DOMAIN, Link);
        if (Domain->Domain == Dependency->Domain)
            break;
        Domain = NULL;
    }

    if (Domain == NULL)
    {
        Domain = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Domain), PPMLIB_TAG);
        if (Domain == NULL)
            return FALSE;

        RtlZeroMemory(Domain, sizeof(*Domain));
        InitializeListHead(&Domain->Entries);
        Domain->Domain = Dependency->Domain;
        Domain->Coordination = Dependency->Coordination;
        Domain->Total = Dependency->Processors;
        InsertTailList(&PpmLib.IdleDomainList, &Domain->Link);
    }

    Link = &Device->IdleLinks[Dependency->Index];
    Link->Domain = Domain;
    InsertTailList(&Domain->Entries, &Link->Link);

    if (!(Domain->Mask & AFFINITY_MASK(Device->NtNumber)))
    {
        Domain->Mask |= AFFINITY_MASK(Device->NtNumber);
        Domain->Count++;
    }

    return (Domain->Count == Domain->Total);
}

/* Takes one state out of its domain, TRUE when that broke a complete domain */
static
BOOLEAN
NTAPI
PpmLibLeaveIdleDomain(
    _Inout_ PPPMLIB_DEVICE Device,
    _Inout_ PPPMLIB_IDLE_LINK Link)
{
    PPPMLIB_IDLE_DOMAIN Domain = Link->Domain;
    PPPMLIB_IDLE_LINK Other;
    BOOLEAN LastOfDevice = TRUE;
    BOOLEAN WasComplete = FALSE;
    PLIST_ENTRY Entry;

    for (Entry = Domain->Entries.Flink; Entry != &Domain->Entries; Entry = Entry->Flink)
    {
        Other = CONTAINING_RECORD(Entry, PPMLIB_IDLE_LINK, Link);
        if (Other != Link &&
            Other >= Device->IdleLinks &&
            Other < Device->IdleLinks + Device->IdleLinkCount)
        {
            LastOfDevice = FALSE;
        }
    }

    if (LastOfDevice)
    {
        WasComplete = (Domain->Count == Domain->Total);
        Domain->Mask &= ~AFFINITY_MASK(Device->NtNumber);
        Domain->Count -= 1;
    }

    RemoveEntryList(&Link->Link);
    if (IsListEmpty(&Domain->Entries))
    {
        RemoveEntryList(&Domain->Link);
        ExFreePoolWithTag(Domain, PPMLIB_TAG);
    }

    Link->Domain = NULL;
    return WasComplete;
}

/* Global lock held */
static
VOID
NTAPI
PpmLibDeregisterIdleDomains(
    _Inout_ PPPMLIB_DEVICE Device)
{
    KAFFINITY Update = 0;
    KAFFINITY Mask;
    ULONG Index;

    if (Device->IdleLinks == NULL)
        return;

    for (Index = 0; Index < Device->IdleLinkCount; Index++)
    {
        if (Device->IdleLinks[Index].Domain == NULL)
            continue;

        Mask = Device->IdleLinks[Index].Domain->Mask;
        if (PpmLibLeaveIdleDomain(Device, &Device->IdleLinks[Index]))
            Update |= Mask;
    }

    PpmLibUpdateIdleDomainSet(Update);

    if (Device->IdleRuntime)
    {
        for (Index = 0; Index < Device->IdleRuntime->Count; Index++)
            Device->IdleRuntime->State[Index].Entry = NULL;
    }

    ExFreePoolWithTag(Device->IdleLinks, PPMLIB_TAG);
    Device->IdleLinks = NULL;
    Device->IdleLinkCount = 0;
}

/* Global lock held */
static
NTSTATUS
NTAPI
PpmLibRegisterIdleDomains(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_CST Cst = Device->Cst;
    PPPMLIB_IDLE_LINK Links;
    KAFFINITY Update = 0;
    ULONG Index;
    ULONG Link;

    Links = ExAllocatePoolWithTag(NonPagedPoolNx, Cst->Count * sizeof(*Links), PPMLIB_TAG);
    if (Links == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Links, Cst->Count * sizeof(*Links));
    for (Index = 0; Index < Cst->Count; Index++)
    {
        Links[Index].CState = &Cst->State[Index];
        InitializeListHead(&Links[Index].Link);
    }

    Device->IdleLinks = Links;
    Device->IdleLinkCount = Cst->Count;

    for (Index = 0; Index < Device->IdleRuntime->Count; Index++)
    {
        for (Link = 0; Link < Cst->Count; Link++)
        {
            if (Links[Link].CState == Device->IdleRuntime->State[Index].CState)
                Device->IdleRuntime->State[Index].Entry = &Links[Link];
        }
    }

    if (Device->Csd == NULL)
        return STATUS_SUCCESS;

    for (Index = 0; Index < Device->Csd->Count; Index++)
    {
        if (PpmLibJoinIdleDomain(Device, &Device->Csd->Entry[Index]) &&
            Device->Csd->Entry[Index].Index < Cst->Count)
        {
            Update |= Links[Device->Csd->Entry[Index].Index].Domain->Mask;
        }
    }

    PpmLibUpdateIdleDomainSet(Update);
    return STATUS_SUCCESS;
}

/* REGISTRATION ***************************************************************/

/**
 * @brief Registers the processor's C-states with the kernel. ACPI lock held.
 */
NTSTATUS
NTAPI
PpmLibRegisterIdleStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPM_IDLE_STATES_REGISTRATION Registration = NULL;
    PPPMLIB_IDLE_RUNTIME Runtime = NULL;
    PPPMLIB_IDLE_RUNTIME Previous = NULL;
    SIZE_T Size;
    NTSTATUS Status;

    PpmLibAcquireGlobal();
    if (!Device->IdleInitialized)
    {
        if (PpmLib.Vendor.InitializeCStates)
            PpmLib.Vendor.InitializeCStates(Device);
        Device->IdleInitialized = TRUE;
    }
    PpmLibDeregisterIdleDomains(Device);
    PpmLibReleaseGlobal();

    if (!(Device->Enabled & PPMLIB_ALL_IDLE))
        return STATUS_NOT_SUPPORTED;

    Runtime = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Runtime), PPMLIB_TAG);
    if (Runtime == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Runtime, sizeof(*Runtime));

    Size = FIELD_OFFSET(PPM_IDLE_STATES_REGISTRATION, State[PPMLIB_MAX_IDLE_STATES]);
    Registration = ExAllocatePoolWithTag(NonPagedPoolNx, Size, PPMLIB_TAG);
    if (Registration == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Registration, Size);
    Status = KeGetProcessorNumberFromIndex(Device->NtNumber, &Registration->Processor);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Registration->Version = PPM_DRIVER_INTERFACE_VERSION;
    Registration->Context = Runtime;
    Registration->ProcessorIdleCount = PPMLIB_MAX_IDLE_STATES;
    Runtime->Count = PPMLIB_MAX_IDLE_STATES;

    PpmLibBuildCStates(Device, Registration, Runtime);
    if (Runtime->PreferredCount == 0)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    PpmLibAcquireGlobal();
    Status = PpmLib.Kernel.RegisterIdleStates(Registration);
    if (NT_SUCCESS(Status))
    {
        Previous = Device->IdleRuntime;
        Device->IdleRuntime = Runtime;
        Runtime = NULL;

        if (Device->Enabled & PPMLIB_CST_IDLE)
            PpmLibRegisterIdleDomains(Device);
    }
    PpmLibReleaseGlobal();

Exit:
    if (Registration)
        ExFreePoolWithTag(Registration, PPMLIB_TAG);
    if (Runtime)
        ExFreePoolWithTag(Runtime, PPMLIB_TAG);
    if (Previous)
        ExFreePoolWithTag(Previous, PPMLIB_TAG);

    return Status;
}

/**
 * @brief _CST changed, notify 0x81. A _CST that no longer parses leaves halt only.
 */
VOID
NTAPI
PpmLibCStateChange(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_CST OldCst;
    PPPMLIB_CSD OldCsd;
    PPPMLIB_CST NewCst;
    PPPMLIB_CSD NewCsd = NULL;

    if (!(Device->Enabled & PPMLIB_CST_IDLE))
        return;

    PpmLibAcquireAcpi(Device);

    if (NT_SUCCESS(PpmLibInitCstAndValidate(Device, &NewCst)))
    {
        if (!NT_SUCCESS(PpmLibInitIdleDomain(Device, NewCst, &NewCsd)))
            NewCsd = NULL;
    }
    else
    {
        NewCst = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*NewCst), PPMLIB_TAG);
        if (NewCst == NULL)
        {
            PpmLibReleaseAcpi(Device);
            return;
        }

        RtlZeroMemory(NewCst, sizeof(*NewCst));
        NewCst->Count = 1;
        NewCst->State[0].Register.SpaceId = PPMLIB_SPACE_FFH;
        NewCst->State[0].Type = 1;
    }

    OldCst = Device->Cst;
    OldCsd = Device->Csd;
    Device->Cst = NewCst;
    Device->Csd = NewCsd;

    PpmLibRegisterIdleStates(Device);
    PpmLibReleaseAcpi(Device);

    if (OldCst)
        ExFreePoolWithTag(OldCst, PPMLIB_TAG);
    if (OldCsd)
        ExFreePoolWithTag(OldCsd, PPMLIB_TAG);
}

/* EOF */
