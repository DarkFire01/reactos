/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectComposition frames, frame statistics and the compositor clock
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dcompprotocol.h>
#include "../../gdi/ntgdi/dxgkfwd.h"

DBG_DEFAULT_CHANNEL(UserMisc);

#define TAG_DCOMP_FRAME 'fmcD'

/* Frames whose statistics stay readable, by id modulo this */
#define DCOMP_FRAME_HISTORY         300

/* Outputs a frame keeps statistics for */
#define DCOMP_FRAME_MAX_TARGETS     4

/* Most target ids a statistics call copies out, and most targets a confirm may carry */
#define DCOMP_MAX_TARGET_IDS        256

/* Handles WaitForCompositorClock waits on besides the clock */
#define DCOMP_MAX_CLOCK_HANDLES     32

/* A legacy token block never takes more of the section than this */
#define DCOMP_LEGACY_BLOCK_SIZE     2048

/*
 * The objects dxgkrnl's token manager hands out, called through their tables. Only the slots
 * win32k uses are named; dxgkrnl itself is x64 only.
 */
typedef struct _DCOMP_FRAME_OBJECT DCOMP_FRAME_OBJECT, *PDCOMP_FRAME_OBJECT;

typedef struct _DXGI_PRESENT_STATISTICS
{
    ULONG PresentCount;
    ULONG PresentRefreshCount;
    ULONG SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_PRESENT_STATISTICS;

typedef struct _DCOMP_FRAME_OBJECT_VTBL
{
    ULONG (NTAPI *AddRef)(_In_ PDCOMP_FRAME_OBJECT Frame);
    ULONG (NTAPI *Release)(_In_ PDCOMP_FRAME_OBJECT Frame);
    VOID (NTAPI *Confirm)(_In_ PDCOMP_FRAME_OBJECT Frame);
    VOID (NTAPI *Retire)(_In_ PDCOMP_FRAME_OBJECT Frame, _In_ const DXGI_PRESENT_STATISTICS* Virtual, _In_ const DXGI_PRESENT_STATISTICS* Physical);
    VOID (NTAPI *Discard)(_In_ PDCOMP_FRAME_OBJECT Frame);
    BOOLEAN (NTAPI *GetNextLegacyTokenBlock)(_In_ PDCOMP_FRAME_OBJECT Frame, _Out_ const UCHAR** Data, _Out_ PULONG Size, _Out_ PULONG Count);
    BOOLEAN (NTAPI *GetSurfaceUpdates)(_In_ PDCOMP_FRAME_OBJECT Frame, _Out_ PVOID Updates, _In_ ULONG MaxUpdates, _Out_ PULONG Written);
    PVOID SetBatches;
    PVOID GetBatches;
    ULONG64 (NTAPI *GetFrameId)(_In_ PDCOMP_FRAME_OBJECT Frame);
} DCOMP_FRAME_OBJECT_VTBL;

struct _DCOMP_FRAME_OBJECT
{
    const DCOMP_FRAME_OBJECT_VTBL* Vtbl;
};

typedef struct _DCOMP_FRAME_COLLECTION DCOMP_FRAME_COLLECTION, *PDCOMP_FRAME_COLLECTION;

typedef struct _DCOMP_FRAME_COLLECTION_VTBL
{
    ULONG (NTAPI *AddRef)(_In_ PDCOMP_FRAME_COLLECTION Collection);
    ULONG (NTAPI *Release)(_In_ PDCOMP_FRAME_COLLECTION Collection);
    VOID (NTAPI *Add)(_In_ PDCOMP_FRAME_COLLECTION Collection, _In_ PDCOMP_FRAME_OBJECT Frame);
    NTSTATUS (NTAPI *Remove)(_In_ PDCOMP_FRAME_COLLECTION Collection, _In_ ULONG64 FrameId);
    NTSTATUS (NTAPI *Find)(_In_ PDCOMP_FRAME_COLLECTION Collection, _In_ ULONG64 FrameId, _Out_ PDCOMP_FRAME_OBJECT* Frame);
    VOID (NTAPI *DiscardPrevious)(_In_ PDCOMP_FRAME_COLLECTION Collection, _In_ ULONG64 FrameId);
    VOID (NTAPI *DiscardAll)(_In_ PDCOMP_FRAME_COLLECTION Collection);
} DCOMP_FRAME_COLLECTION_VTBL;

struct _DCOMP_FRAME_COLLECTION
{
    const DCOMP_FRAME_COLLECTION_VTBL* Vtbl;
};

typedef struct _DCOMP_TOKEN_MANAGER DCOMP_TOKEN_MANAGER, *PDCOMP_TOKEN_MANAGER;

typedef struct _DCOMP_TOKEN_MANAGER_VTBL
{
    ULONG (NTAPI *AddRef)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    ULONG (NTAPI *Release)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    VOID (NTAPI *Lock)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    VOID (NTAPI *Unlock)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    PVOID Reserved4[4];
    NTSTATUS (NTAPI *GetMappedSection)(_In_ PDCOMP_TOKEN_MANAGER Manager, _Out_ PVOID* View, _Out_ PULONG64 Size);
    PVOID Reserved9;
    VOID (NTAPI *ReleaseToFrame)(_In_ PDCOMP_TOKEN_MANAGER Manager, _In_ PDCOMP_FRAME_OBJECT Frame);
    PVOID Reserved11[3];
    VOID (NTAPI *DeleteAllTokens)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    PVOID Reserved15[8];
    NTSTATUS (NTAPI *CreateFrameCollection)(_In_ PDCOMP_TOKEN_MANAGER Manager, _Out_ PDCOMP_FRAME_COLLECTION* Collection);
    NTSTATUS (NTAPI *CreateFrame)(_In_ PDCOMP_TOKEN_MANAGER Manager, _In_ const DCOMP_FRAME_INFO* Info, _Out_ PDCOMP_FRAME_OBJECT* Frame);
    PVOID Reserved25[2];
    VOID (NTAPI *ResetAdapterCollection)(_In_ PDCOMP_TOKEN_MANAGER Manager);
    PVOID Reserved28[5];
    VOID (NTAPI *ConfirmFrame)(_In_ PDCOMP_TOKEN_MANAGER Manager, _In_ const DCOMP_FRAME_CONFIRM* Confirm);
} DCOMP_TOKEN_MANAGER_VTBL;

struct _DCOMP_TOKEN_MANAGER
{
    const DCOMP_TOKEN_MANAGER_VTBL* Vtbl;
};

/* What is kept of a confirmed frame */
typedef struct _DCOMP_FRAME_RECORD
{
    ULONG64 FrameId;
    DCOMP_FRAME_STATS Stats;
    ULONG TargetCount;          /* as the compositor reported it */
    BOOLEAN Completed;          /* every output it presented to has shown it */
    BOOLEAN Retired;
    DCOMP_FRAME_TARGET Targets[DCOMP_FRAME_MAX_TARGETS];
} DCOMP_FRAME_RECORD, *PDCOMP_FRAME_RECORD;

static ERESOURCE gDcompFrameLock;
static PDCOMP_TOKEN_MANAGER gpTokenManager = NULL;
static PDCOMP_FRAME_COLLECTION gpFrameCollection = NULL;
static PDCOMP_FRAME_RECORD gpFrameHistory = NULL;
static ULONG64 gFrameIds[3];        /* created, confirmed, completed */
static ULONG64 gNextLocalFrameId;   /* when dxgkrnl numbers no frames */

/*
 * The compositor clock ticks on the grid of the clock output's vblanks. A waiter sleeps to the
 * next point of it in its own context; win32k lives in session space, so no timer or DPC may
 * run its code from whatever process happens to be current.
 */
static ULONG64 gClockBase;
static ULONG64 gClockPeriod;
static BOOLEAN gClockResolution = FALSE;
static ULONG gClockBoost;

static
VOID
DcompFrameLock(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&gDcompFrameLock, TRUE);
}

static
VOID
DcompFrameUnlock(VOID)
{
    ExReleaseResourceLite(&gDcompFrameLock);
    KeLeaveCriticalRegion();
}

static
BOOLEAN
DcompTargetIdEqual(
    _In_ const DCOMP_TARGET_ID* Left,
    _In_ const DCOMP_TARGET_ID* Right)
{
    return RtlEqualMemory(Left, Right, FIELD_OFFSET(DCOMP_TARGET_ID, UniqueId)) &&
           (Left->UniqueId == Right->UniqueId || !Left->UniqueId || !Right->UniqueId);
}

static
PDCOMP_FRAME_RECORD
DcompFindRecord(
    _In_ ULONG64 FrameId)
{
    PDCOMP_FRAME_RECORD Record;

    if (!gpFrameHistory || !FrameId)
        return NULL;
    Record = &gpFrameHistory[FrameId % DCOMP_FRAME_HISTORY];
    return Record->FrameId == FrameId ? Record : NULL;
}

static
ULONG
DcompStoredTargets(
    _In_ const DCOMP_FRAME_RECORD* Record)
{
    return min(Record->TargetCount, DCOMP_FRAME_MAX_TARGETS);
}

/* The second check is that the present count reached the screen at all */
static
BOOLEAN
DcompTargetPresented(
    _In_ const DCOMP_FRAME_TARGET* Target)
{
    return Target->PresentTime && Target->Presented.PresentCount;
}

static
BOOLEAN
DcompRecordComplete(
    _In_ const DCOMP_FRAME_RECORD* Record)
{
    ULONG i;

    for (i = 0; i < DcompStoredTargets(Record); i++)
    {
        if (DcompTargetPresented(&Record->Targets[i]) && !Record->Targets[i].Completed.PresentCount)
            return FALSE;
    }
    return TRUE;
}

/* Hands the presents of a frame that reached the screen back to their producers with its statistics */
static
VOID
DcompRetireFrame(
    _In_ ULONG64 FrameId,
    _In_ const DCOMP_PRESENT_STATS* Completed)
{
    DXGI_PRESENT_STATISTICS Virtual, Physical;
    PDCOMP_FRAME_OBJECT Frame;

    if (!gpFrameCollection || !NT_SUCCESS(gpFrameCollection->Vtbl->Find(gpFrameCollection, FrameId, &Frame)))
        return;

    RtlZeroMemory(&Virtual, sizeof(Virtual));
    Virtual.PresentCount = Completed->PresentCount;
    Virtual.PresentRefreshCount = Virtual.SyncRefreshCount = Completed->VirtualRefreshCount;
    Virtual.SyncQPCTime.QuadPart = Completed->VirtualTime;

    RtlZeroMemory(&Physical, sizeof(Physical));
    Physical.PresentCount = Completed->PresentCount;
    Physical.PresentRefreshCount = Physical.SyncRefreshCount = Completed->RefreshCount;
    Physical.SyncQPCTime.QuadPart = Completed->Time;

    Frame->Vtbl->Retire(Frame, &Virtual, &Physical);
    Frame->Vtbl->Release(Frame);
    gpFrameCollection->Vtbl->Remove(gpFrameCollection, FrameId);
}

/* Gives up a frame that never reached the screen; its presents count as skipped */
static
VOID
DcompDiscardFrame(
    _In_ ULONG64 FrameId)
{
    PDCOMP_FRAME_OBJECT Frame;

    if (!gpFrameCollection || !NT_SUCCESS(gpFrameCollection->Vtbl->Find(gpFrameCollection, FrameId, &Frame)))
        return;

    Frame->Vtbl->Discard(Frame);
    Frame->Vtbl->Release(Frame);
    gpFrameCollection->Vtbl->Remove(gpFrameCollection, FrameId);
}

/* Takes completion an older frame learns from a newer confirm; target 0 decides its retirement */
static
VOID
DcompUpdateCompleted(
    _Inout_ PDCOMP_FRAME_RECORD Record,
    _In_reads_(Count) const DCOMP_FRAME_TARGET* Targets,
    _In_ ULONG Count)
{
    ULONG i, j;

    for (i = 0; i < DcompStoredTargets(Record); i++)
    {
        PDCOMP_FRAME_TARGET Old = &Record->Targets[i];
        const DCOMP_FRAME_TARGET* New = NULL;

        if (!DcompTargetPresented(Old) || Old->Completed.PresentCount)
            continue;

        for (j = 0; j < Count && !New; j++)
        {
            if (DcompTargetIdEqual(&Old->Id, &Targets[j].Id))
                New = &Targets[j];
        }

        if (!New)
        {
            if (i == 0 && !Record->Retired)
            {
                DcompDiscardFrame(Record->FrameId);
                Record->Retired = TRUE;
            }
            continue;
        }

        if (Old->Presented.PresentCount <= New->Completed.PresentCount)
        {
            Old->Completed = New->Completed;
            if (i == 0 && !Record->Retired)
            {
                DcompRetireFrame(Record->FrameId, &Old->Completed);
                Record->Retired = TRUE;
            }
        }
    }
    Record->Completed = DcompRecordComplete(Record);
}

/* Records a confirmed frame and works out which earlier frames are now on screen */
static
VOID
DcompUpdateStats(
    _In_ const DCOMP_FRAME_CONFIRM* Confirm,
    _In_reads_(Count) const DCOMP_FRAME_TARGET* Targets,
    _In_ ULONG Count)
{
    PDCOMP_FRAME_RECORD Record;
    ULONG64 Oldest = 0;
    ULONG64 Id;
    ULONG i;

    gFrameIds[DCOMP_FRAME_ID_CONFIRMED] = Confirm->FrameId;

    for (Id = Confirm->FrameId - 1; Id > gFrameIds[DCOMP_FRAME_ID_COMPLETED] && Confirm->FrameId - Id < DCOMP_FRAME_HISTORY; Id--)
    {
        Record = DcompFindRecord(Id);
        if (!Record)
            continue;
        if (!Record->Completed)
            DcompUpdateCompleted(Record, Targets, Count);
        if (!Record->Completed)
            Oldest = Id;
    }

    Record = &gpFrameHistory[Confirm->FrameId % DCOMP_FRAME_HISTORY];
    if (Record->FrameId && Record->FrameId != Confirm->FrameId && !Record->Retired)
        DcompDiscardFrame(Record->FrameId);

    RtlZeroMemory(Record, sizeof(*Record));
    Record->FrameId = Confirm->FrameId;
    Record->Stats.StartTime = Confirm->StartTime;
    Record->Stats.TargetTime = Confirm->TargetTime;
    Record->Stats.FramePeriod = Confirm->FramePeriod;
    Record->TargetCount = Count;
    RtlCopyMemory(Record->Targets, Targets, min(Count, DCOMP_FRAME_MAX_TARGETS) * sizeof(*Targets));

    for (i = 0; i < DcompStoredTargets(Record); i++)
    {
        PDCOMP_FRAME_TARGET Target = &Record->Targets[i];

        if (DcompTargetPresented(Target) && Target->Completed.PresentCount >= Target->Presented.PresentCount)
        {
            if (i == 0)
            {
                DcompRetireFrame(Record->FrameId, &Target->Completed);
                Record->Retired = TRUE;
            }
        }
        else
        {
            RtlZeroMemory(&Target->Completed, sizeof(Target->Completed));
        }
    }

    /* A frame that showed nothing is over at once */
    if (!Count || !Targets[0].PresentTime)
    {
        if (!Record->Retired)
            DcompDiscardFrame(Record->FrameId);
        Record->Retired = TRUE;
    }

    Record->Completed = DcompRecordComplete(Record);
    if (Oldest)
        gFrameIds[DCOMP_FRAME_ID_COMPLETED] = Oldest - 1;
    else
        gFrameIds[DCOMP_FRAME_ID_COMPLETED] = Record->Completed ? Confirm->FrameId : Confirm->FrameId - 1;

    if (gpFrameCollection)
        gpFrameCollection->Vtbl->DiscardPrevious(gpFrameCollection, gFrameIds[DCOMP_FRAME_ID_COMPLETED]);
}

/* How long until the next point on the clock grid, in 100 ns units; 0 without a grid */
static
LONGLONG
DcompClockDelay(VOID)
{
    LARGE_INTEGER Now, Frequency;
    ULONG64 Base, Period, Next, Ticks;

    DcompFrameLock();
    Base = gClockBase;
    Period = gClockPeriod;
    if (Period && !gClockResolution)
    {
        /* Vblanks come more often than the default clock tick */
        ExSetTimerResolution(10000, TRUE);
        gClockResolution = TRUE;
    }
    DcompFrameUnlock();

    if (!Period)
        return 0;

    Now = KeQueryPerformanceCounter(&Frequency);
    if ((ULONG64)Now.QuadPart < Base)
        Next = Base;
    else
        Next = Base + (((ULONG64)Now.QuadPart - Base) / Period + 1) * Period;

    Ticks = (Next - (ULONG64)Now.QuadPart) * 10000000ULL / (ULONG64)Frequency.QuadPart;
    return (LONGLONG)max(Ticks, 1);
}

/**
 * @brief Sets up frame state when win32k starts.
 */
NTSTATUS
FASTCALL
IntDcompFrameInitialize(VOID)
{
    return ExInitializeResourceLite(&gDcompFrameLock);
}

/**
 * @brief Lets go of every frame and token when the compositor goes away.
 */
VOID
FASTCALL
IntDcompFrameCompositorGone(VOID)
{
    DcompFrameLock();
    if (gpFrameCollection)
    {
        gpFrameCollection->Vtbl->DiscardAll(gpFrameCollection);
        gpFrameCollection->Vtbl->Release(gpFrameCollection);
        gpFrameCollection = NULL;
    }
    if (gpTokenManager)
    {
        gpTokenManager->Vtbl->DeleteAllTokens(gpTokenManager);
        gpTokenManager->Vtbl->ResetAdapterCollection(gpTokenManager);
        gpTokenManager->Vtbl->Release(gpTokenManager);
        gpTokenManager = NULL;
    }
    gClockPeriod = 0;
    if (gClockResolution)
    {
        ExSetTimerResolution(0, FALSE);
        gClockResolution = FALSE;
    }
    DcompFrameUnlock();
}

/**
 * @brief Drops what a process asked of the compositor clock.
 */
VOID
FASTCALL
IntDcompFrameProcessCleanup(
    _In_ PPROCESSINFO ProcessInfo)
{
    DcompFrameLock();
    if (ProcessInfo->DcompClockBoost)
    {
        gClockBoost -= min(gClockBoost, 1);
        ProcessInfo->DcompClockBoost = 0;
    }
    DcompFrameUnlock();
}

NTSTATUS
APIENTRY
NtDCompositionBeginFrame(
    _In_ HANDLE Connection,
    _In_ const DCOMP_FRAME_INFO* FrameInfo,
    _Out_ PULONG64 FrameId)
{
    DCOMP_FRAME_INFO Info;
    PDCOMP_FRAME_OBJECT Frame = NULL;
    ULONG64 Id = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!IntDcompIsCompositor(Connection))
        return STATUS_ACCESS_DENIED;
    if (!FrameInfo || !FrameId)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        ProbeForRead(FrameInfo, sizeof(Info), sizeof(ULONG));
        Info = *FrameInfo;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    /* dxgkrnl divides by these periods when it places presents on the frame's vblank grid */
    if (!Info.FramePeriod || !Info.VblankPeriod || !Info.VirtualVblankPeriod)
        return STATUS_INVALID_PARAMETER;

    DcompFrameLock();
    gClockBase = Info.TargetTime;
    gClockPeriod = Info.FramePeriod;

    if (!gpFrameHistory)
    {
        gpFrameHistory = ExAllocatePoolWithTag(PagedPool, DCOMP_FRAME_HISTORY * sizeof(*gpFrameHistory), TAG_DCOMP_FRAME);
        if (!gpFrameHistory)
        {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }
        RtlZeroMemory(gpFrameHistory, DCOMP_FRAME_HISTORY * sizeof(*gpFrameHistory));
    }

    if (!gpTokenManager && !NT_SUCCESS(DlGetSessionTokenManager((PVOID*)&gpTokenManager)))
        gpTokenManager = NULL;

    if (gpTokenManager)
    {
        if (!gpFrameCollection)
        {
            Status = gpTokenManager->Vtbl->CreateFrameCollection(gpTokenManager, &gpFrameCollection);
            if (!NT_SUCCESS(Status))
                goto Exit;
        }

        Status = gpTokenManager->Vtbl->CreateFrame(gpTokenManager, &Info, &Frame);
        if (!NT_SUCCESS(Status))
            goto Exit;

        /* Presents due by the frame's deadline join it, which lets their producers go on */
        gpTokenManager->Vtbl->Lock(gpTokenManager);
        gpTokenManager->Vtbl->ReleaseToFrame(gpTokenManager, Frame);
        gpFrameCollection->Vtbl->Add(gpFrameCollection, Frame);
        gpTokenManager->Vtbl->Unlock(gpTokenManager);

        Id = Frame->Vtbl->GetFrameId(Frame);
        Frame->Vtbl->Release(Frame);
    }
    else
    {
        if (!++gNextLocalFrameId)
            gNextLocalFrameId = 1;
        Id = gNextLocalFrameId;
    }
    gFrameIds[DCOMP_FRAME_ID_CREATED] = Id;

Exit:
    DcompFrameUnlock();

    _SEH2_TRY
    {
        ProbeForWrite(FrameId, sizeof(*FrameId), sizeof(ULONG));
        *FrameId = Id;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionConfirmFrame(
    _In_ HANDLE Connection,
    _In_ const DCOMP_FRAME_CONFIRM* FrameConfirm)
{
    DCOMP_FRAME_CONFIRM Confirm;
    DCOMP_FRAME_TARGET LocalTargets[DCOMP_FRAME_MAX_TARGETS];
    PDCOMP_FRAME_TARGET Targets = LocalTargets;
    PDCOMP_FRAME_OBJECT Frame;
    ULONG Count;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!IntDcompIsCompositor(Connection))
        return STATUS_ACCESS_DENIED;
    if (!FrameConfirm)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        ProbeForRead(FrameConfirm, sizeof(Confirm), sizeof(ULONG));
        Confirm = *FrameConfirm;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (!Confirm.FrameId)
        return STATUS_INVALID_PARAMETER;

    Count = min(Confirm.TargetCount, DCOMP_MAX_TARGET_IDS);
    if (Count > DCOMP_FRAME_MAX_TARGETS)
    {
        Targets = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Targets), TAG_DCOMP_FRAME);
        if (!Targets)
            return STATUS_NO_MEMORY;
    }

    _SEH2_TRY
    {
        ProbeForRead((PVOID)(ULONG_PTR)Confirm.Targets, Count * sizeof(*Targets), sizeof(ULONG));
        RtlCopyMemory(Targets, (PVOID)(ULONG_PTR)Confirm.Targets, Count * sizeof(*Targets));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Exit;

    DcompFrameLock();
    if (gpTokenManager)
        gpTokenManager->Vtbl->ConfirmFrame(gpTokenManager, &Confirm);
    if (gpFrameCollection)
    {
        Status = gpFrameCollection->Vtbl->Find(gpFrameCollection, Confirm.FrameId, &Frame);
        if (NT_SUCCESS(Status))
        {
            Frame->Vtbl->Confirm(Frame);
            Frame->Vtbl->Release(Frame);
        }
    }
    if (gpFrameHistory)
        DcompUpdateStats(&Confirm, Targets, Count);
    DcompFrameUnlock();

Exit:
    if (Targets != LocalTargets)
        ExFreePoolWithTag(Targets, TAG_DCOMP_FRAME);
    return Status;
}

static
NTSTATUS
DcompCaptureFrameId(
    _In_ PULONG64 UserFrameId,
    _Out_ PULONG64 FrameId)
{
    NTSTATUS Status = STATUS_SUCCESS;

    *FrameId = 0;
    if (!UserFrameId)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        ProbeForRead(UserFrameId, sizeof(*UserFrameId), sizeof(ULONG));
        *FrameId = *UserFrameId;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (NT_SUCCESS(Status) && !*FrameId)
        Status = STATUS_INVALID_PARAMETER;
    return Status;
}

static
NTSTATUS
DcompWriteCounts(
    _Out_ PULONG UserCount,
    _Out_ PULONG UserMore,
    _In_ ULONG Count,
    _In_ BOOLEAN More)
{
    NTSTATUS Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        ProbeForWrite(UserCount, sizeof(*UserCount), sizeof(ULONG));
        ProbeForWrite(UserMore, sizeof(*UserMore), sizeof(ULONG));
        *UserCount = Count;
        *UserMore = More;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

/**
 * @brief Has dxgkrnl write the frame's surface updates into the token manager section the
 *        compositor mapped, ten at a time.
 */
NTSTATUS
APIENTRY
NtDCompositionGetFrameSurfaceUpdates(
    _In_ PULONG64 UserFrameId,
    _Out_ PULONG Count,
    _Out_ PULONG More)
{
    PDCOMP_FRAME_OBJECT Frame;
    ULONG64 FrameId;
    ULONG64 Size;
    PVOID View;
    ULONG Written = 0;
    BOOLEAN Remaining = FALSE;
    NTSTATUS Status;

    if (!IntDwmIsCurrentProcess())
        return STATUS_ACCESS_DENIED;

    Status = DcompCaptureFrameId(UserFrameId, &FrameId);
    if (NT_SUCCESS(Status))
    {
        DcompFrameLock();
        if (!gpTokenManager || !gpFrameCollection)
            Status = STATUS_NOT_FOUND;
        else
            Status = gpFrameCollection->Vtbl->Find(gpFrameCollection, FrameId, &Frame);
        if (NT_SUCCESS(Status))
        {
            Status = gpTokenManager->Vtbl->GetMappedSection(gpTokenManager, &View, &Size);
            if (NT_SUCCESS(Status))
                Remaining = Frame->Vtbl->GetSurfaceUpdates(Frame, View, (ULONG)(Size / DCOMP_SURFACE_UPDATE_SIZE), &Written);
            Frame->Vtbl->Release(Frame);
        }
        DcompFrameUnlock();
    }

    if (!NT_SUCCESS(Status))
    {
        Written = 0;
        Remaining = FALSE;
    }
    DcompWriteCounts(Count, More, Written, Remaining);
    return Status;
}

/**
 * @brief Copies the frame's legacy present tokens into the token manager section.
 */
NTSTATUS
APIENTRY
NtDCompositionGetFrameLegacyTokens(
    _In_ PULONG64 UserFrameId,
    _Out_ PULONG Count,
    _Out_ PULONG More)
{
    PDCOMP_FRAME_OBJECT Frame;
    ULONG64 FrameId;
    ULONG64 Size;
    PUCHAR View;
    ULONG Used = 0;
    ULONG Tokens = 0;
    BOOLEAN Remaining = FALSE;
    NTSTATUS Status;

    if (!IntDwmIsCurrentProcess())
        return STATUS_ACCESS_DENIED;

    Status = DcompCaptureFrameId(UserFrameId, &FrameId);
    if (NT_SUCCESS(Status))
    {
        DcompFrameLock();
        if (!gpTokenManager || !gpFrameCollection)
            Status = STATUS_NOT_FOUND;
        else
            Status = gpFrameCollection->Vtbl->Find(gpFrameCollection, FrameId, &Frame);
        if (NT_SUCCESS(Status))
        {
            Status = gpTokenManager->Vtbl->GetMappedSection(gpTokenManager, (PVOID*)&View, &Size);
            while (NT_SUCCESS(Status) && Size - Used >= DCOMP_LEGACY_BLOCK_SIZE)
            {
                const UCHAR* Block;
                ULONG BlockSize, BlockTokens;

                Remaining = Frame->Vtbl->GetNextLegacyTokenBlock(Frame, &Block, &BlockSize, &BlockTokens);
                if (BlockSize)
                {
                    RtlMoveMemory(View + Used, Block, min(BlockSize, DCOMP_LEGACY_BLOCK_SIZE));
                    Used += min(BlockSize, DCOMP_LEGACY_BLOCK_SIZE);
                    Tokens += BlockTokens;
                }
                if (!Remaining)
                    break;
            }
            Frame->Vtbl->Release(Frame);
        }
        DcompFrameUnlock();
    }

    if (!NT_SUCCESS(Status))
    {
        Tokens = 0;
        Remaining = FALSE;
    }
    DcompWriteCounts(Count, More, Tokens, Remaining);
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionGetFrameId(
    _In_ ULONG Type,
    _Out_ PULONG64 FrameId)
{
    ULONG64 Id;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Type > DCOMP_FRAME_ID_COMPLETED || !FrameId)
        return STATUS_INVALID_PARAMETER;
    if (!IntDcompHasCompositor())
        return STATUS_ACCESS_DENIED;

    DcompFrameLock();
    Id = gFrameIds[Type];
    DcompFrameUnlock();

    /* Nothing was composed yet */
    if (!Id)
        return STATUS_UNSUCCESSFUL;

    _SEH2_TRY
    {
        ProbeForWrite(FrameId, sizeof(*FrameId), sizeof(ULONG));
        *FrameId = Id;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

/**
 * @brief Reads a confirmed frame's times and the outputs it was shown on. Fewer ids than the
 *        frame has are filled in order; ActualTargetIdCount always tells the frame's count.
 */
NTSTATUS
APIENTRY
NtDCompositionGetStatistics(
    _In_ PULONG64 UserFrameId,
    _Out_ PDCOMP_FRAME_STATS FrameStats,
    _In_ ULONG TargetIdCount,
    _Out_writes_opt_(TargetIdCount) PDCOMP_TARGET_ID TargetIds,
    _Out_opt_ PULONG ActualTargetIdCount)
{
    DCOMP_TARGET_ID LocalIds[DCOMP_FRAME_MAX_TARGETS];
    PDCOMP_FRAME_RECORD Record;
    DCOMP_FRAME_STATS Stats;
    ULONG64 FrameId;
    ULONG Actual = 0;
    ULONG Copied = 0;
    ULONG i;
    NTSTATUS Status;

    if (!FrameStats || (TargetIdCount && !TargetIds))
        return STATUS_INVALID_PARAMETER;
    if (!IntDcompHasCompositor())
        return STATUS_ACCESS_DENIED;

    Status = DcompCaptureFrameId(UserFrameId, &FrameId);
    if (!NT_SUCCESS(Status))
        return Status;
    TargetIdCount = min(TargetIdCount, DCOMP_MAX_TARGET_IDS);

    DcompFrameLock();
    Record = DcompFindRecord(FrameId);
    if (Record)
    {
        Stats = Record->Stats;
        Actual = Record->TargetCount;
        Copied = min(min(TargetIdCount, DcompStoredTargets(Record)), DCOMP_FRAME_MAX_TARGETS);
        for (i = 0; i < Copied; i++)
            LocalIds[i] = Record->Targets[i].Id;
    }
    DcompFrameUnlock();

    if (!Record)
        return STATUS_NOT_FOUND;

    _SEH2_TRY
    {
        ProbeForWrite(FrameStats, sizeof(*FrameStats), sizeof(ULONG));
        *FrameStats = Stats;
        if (TargetIdCount)
        {
            ProbeForWrite(TargetIds, TargetIdCount * sizeof(*TargetIds), sizeof(ULONG));
            RtlCopyMemory(TargetIds, LocalIds, Copied * sizeof(*TargetIds));
            RtlZeroMemory(TargetIds + Copied, (TargetIdCount - Copied) * sizeof(*TargetIds));
        }
        if (ActualTargetIdCount)
        {
            ProbeForWrite(ActualTargetIdCount, sizeof(*ActualTargetIdCount), sizeof(ULONG));
            *ActualTargetIdCount = Actual;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionGetTargetStatistics(
    _In_ PULONG64 UserFrameId,
    _In_ const DCOMP_TARGET_ID* UserTargetId,
    _Out_ PDCOMP_TARGET_STATS TargetStats)
{
    PDCOMP_FRAME_RECORD Record;
    DCOMP_TARGET_STATS Stats;
    DCOMP_TARGET_ID TargetId;
    ULONG64 FrameId;
    BOOLEAN Found = FALSE;
    ULONG i;
    NTSTATUS Status;

    if (!UserTargetId)
        return STATUS_INVALID_PARAMETER;
    if (!IntDcompHasCompositor())
        return STATUS_ACCESS_DENIED;

    Status = DcompCaptureFrameId(UserFrameId, &FrameId);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        ProbeForRead(UserTargetId, sizeof(*UserTargetId), sizeof(ULONG));
        TargetId = *UserTargetId;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    RtlZeroMemory(&Stats, sizeof(Stats));
    DcompFrameLock();
    Record = DcompFindRecord(FrameId);
    for (i = 0; Record && !Found && i < DcompStoredTargets(Record); i++)
    {
        const DCOMP_FRAME_TARGET* Target = &Record->Targets[i];

        if (!DcompTargetIdEqual(&Target->Id, &TargetId))
            continue;

        Found = TRUE;
        Stats.OutstandingPresents = Target->OutstandingPresents;
        Stats.PresentTime = Target->PresentTime;
        Stats.VblankDuration = Target->VblankDuration;
        Stats.PresentedStats.PresentCount = Target->Presented.PresentCount;
        Stats.PresentedStats.RefreshCount = Target->Presented.RefreshCount;
        Stats.PresentedStats.VirtualRefreshCount = Target->Presented.VirtualRefreshCount;
        Stats.PresentedStats.Time = Target->Presented.Time;
        Stats.CompletedStats.PresentCount = Target->Completed.PresentCount;
        Stats.CompletedStats.RefreshCount = Target->Completed.RefreshCount;
        Stats.CompletedStats.VirtualRefreshCount = Target->Completed.VirtualRefreshCount;
        Stats.CompletedStats.Time = Target->Completed.Time;
    }
    DcompFrameUnlock();

    if (!Found)
        return STATUS_NOT_FOUND;

    _SEH2_TRY
    {
        ProbeForWrite(TargetStats, sizeof(*TargetStats), sizeof(ULONG));
        *TargetStats = Stats;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

/**
 * @brief Waits for any of the handles or the next compositor clock tick, which is reported
 *        as the index one past the handles.
 */
NTSTATUS
APIENTRY
NtDCompositionWaitForCompositorClock(
    _In_ ULONG Count,
    _In_reads_opt_(Count) const HANDLE* UserHandles,
    _In_ ULONG TimeoutMs)
{
    HANDLE Handles[DCOMP_MAX_CLOCK_HANDLES];
    LARGE_INTEGER Timeout;
    LONGLONG Delay;
    BOOLEAN Tick;
    NTSTATUS Status;

    if (Count > DCOMP_MAX_CLOCK_HANDLES || (Count && !UserHandles))
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (Count)
        {
            ProbeForRead(UserHandles, Count * sizeof(HANDLE), sizeof(HANDLE));
            RtlCopyMemory(Handles, UserHandles, Count * sizeof(HANDLE));
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (!IntDcompHasCompositor())
        return STATUS_ACCESS_DENIED;
    Delay = DcompClockDelay();
    if (!Delay)
        return STATUS_UNSUCCESSFUL;

    /* Whichever comes first, the caller's timeout or the tick, ends the wait */
    Tick = TimeoutMs == MAXULONG || Delay <= 10000LL * TimeoutMs;
    Timeout.QuadPart = Tick ? -Delay : -10000LL * TimeoutMs;

    if (Count)
        Status = ZwWaitForMultipleObjects(Count, Handles, WaitAny, FALSE, &Timeout);
    else
        Status = KeDelayExecutionThread(UserMode, FALSE, &Timeout);

    if (Status == STATUS_TIMEOUT || (!Count && Status == STATUS_SUCCESS))
        return Tick ? STATUS_WAIT_0 + Count : STATUS_TIMEOUT;
    return Status;
}

/**
 * @brief Counts a process asking for the compositor clock to keep its full rate.
 */
NTSTATUS
APIENTRY
NtDCompositionBoostCompositorClock(
    _In_ BOOL Enable)
{
    PPROCESSINFO ProcessInfo = PsGetCurrentProcessWin32Process();
    NTSTATUS Status = STATUS_SUCCESS;

    if (!IntDcompHasCompositor())
        return STATUS_ACCESS_DENIED;
    if (!ProcessInfo)
        return STATUS_UNSUCCESSFUL;

    DcompFrameLock();
    if (Enable)
    {
        if (ProcessInfo->DcompClockBoost == MAXULONG)
            Status = STATUS_UNSUCCESSFUL;
        else if (!ProcessInfo->DcompClockBoost++)
            gClockBoost++;
    }
    else
    {
        if (!ProcessInfo->DcompClockBoost)
            Status = STATUS_UNSUCCESSFUL;
        else if (!--ProcessInfo->DcompClockBoost)
            gClockBoost--;
    }
    DcompFrameUnlock();
    return Status;
}

/* EOF */
