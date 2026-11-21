/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Directed power transitions of PoFx devices in connected standby
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* Without a timeout of its own a device gets this long per transition */
#define POP_FX_DIRECTED_DOWN_TIMEOUT_S  10
#define POP_FX_DIRECTED_UP_TIMEOUT_S    5

typedef struct _POP_FX_DIRECTED_ENTRY
{
    PPOP_FX_DEVICE Device;
    ULONG Level;
} POP_FX_DIRECTED_ENTRY, *PPOP_FX_DIRECTED_ENTRY;

/* GLOBALS *******************************************************************/

/* Held across a whole pass, unregistering waits for it so devices stay valid */
KGUARDED_MUTEX PopFxDirectedLock;

static KTIMER PopFxDirectedTimer;
static KDPC PopFxDirectedDpc;
static WORK_QUEUE_ITEM PopFxDirectedWorkItem;
static volatile LONG PopFxDirectedQueued;
static volatile LONG PopFxDirectedAbort;

/* PRIVATE FUNCTIONS *********************************************************/

static
ULONG
PopFxDeviceLevel(
    _In_ PPOP_FX_DEVICE Device)
{
    PDEVICE_NODE Node = Device->Pdo ? IopGetDeviceNode(Device->Pdo) : NULL;

    return Node ? Node->Level : 0;
}

/**
 * @brief Collects the devices that take directed transitions, sorted by devnode depth.
 *
 * @param[in] Deepest
 * TRUE puts children before their parents, FALSE parents first.
 */
static
PPOP_FX_DIRECTED_ENTRY
PopFxCollectDirected(
    _In_ BOOLEAN Deepest,
    _Out_ PULONG Count)
{
    PPOP_FX_DIRECTED_ENTRY Entries, Key;
    KLOCK_QUEUE_HANDLE LockHandle;
    PLIST_ENTRY Link;
    ULONG Capacity = 0, Found = 0, i, j;
    POP_FX_DIRECTED_ENTRY Swap;

    *Count = 0;

    KeAcquireInStackQueuedSpinLock(&PopFxDeviceLock, &LockHandle);
    for (Link = PopFxDeviceList.Flink; Link != &PopFxDeviceList; Link = Link->Flink)
        Capacity++;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    /* Devices registered meanwhile wait for the next pass */
    if (!Capacity)
        return NULL;

    Entries = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(*Entries), TAG_PO_FX_WORK_ORDER);
    if (!Entries)
        return NULL;

    KeAcquireInStackQueuedSpinLock(&PopFxDeviceLock, &LockHandle);
    for (Link = PopFxDeviceList.Flink; Link != &PopFxDeviceList && Found < Capacity; Link = Link->Flink)
    {
        PPOP_FX_DEVICE Device = CONTAINING_RECORD(Link, POP_FX_DEVICE, Link);

        if (!Device->DirectedPowerDownCallback || !Device->DirectedPowerUpCallback)
            continue;

        Entries[Found].Device = Device;
        Found++;
    }
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    for (i = 0; i < Found; i++)
        Entries[i].Level = PopFxDeviceLevel(Entries[i].Device);

    /* Few devices take part, a plain insertion sort is enough */
    for (i = 1; i < Found; i++)
    {
        Swap = Entries[i];
        for (j = i; j > 0; j--)
        {
            Key = &Entries[j - 1];
            if (Deepest ? (Key->Level >= Swap.Level) : (Key->Level <= Swap.Level))
                break;
            Entries[j] = *Key;
        }
        Entries[j] = Swap;
    }

    *Count = Found;
    return Entries;
}

static
BOOLEAN
PopFxWaitDirected(
    _In_ PPOP_FX_DEVICE Device,
    _In_ ULONG DefaultSeconds)
{
    LARGE_INTEGER Timeout;
    ULONG Seconds = Device->DirectedFxTimeout ? Device->DirectedFxTimeout : DefaultSeconds;

    Timeout.QuadPart = -10000000LL * Seconds;
    return KeWaitForSingleObject(&Device->DirectedEvent, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

/**
 * @brief Asks every device that supports it to power down, children before parents.
 */
static
VOID
PopFxDirectedPowerDown(VOID)
{
    PPOP_FX_DIRECTED_ENTRY Entries;
    PPOP_FX_DEVICE Device;
    ULONG Count, Index, Down = 0;
    KIRQL OldIrql;

    PAGED_CODE();

    KeAcquireGuardedMutex(&PopFxDirectedLock);

    Entries = PopFxCollectDirected(TRUE, &Count);
    DPRINT1("PoFx directing %lu device(s) to power down\n", Count);

    for (Index = 0; Index < Count; Index++)
    {
        /* Leaving standby stops the pass, what is down comes back up */
        if (PopFxDirectedAbort)
            break;

        Device = Entries[Index].Device;

        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        if (!Device->Started || Device->DirectedState != POP_FX_DIRECTED_NONE)
        {
            KeReleaseSpinLock(&Device->Lock, OldIrql);
            continue;
        }
        Device->DirectedState = POP_FX_DIRECTED_DOWN_PENDING;
        KeClearEvent(&Device->DirectedEvent);
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        Device->DirectedPowerDownCallback(Device->DriverContext, 0);

        if (PopFxWaitDirected(Device, POP_FX_DIRECTED_DOWN_TIMEOUT_S))
        {
            Down++;
            DPRINT1("PoFx %wZ (level %lu) is directed down\n", &Device->DeviceId, Entries[Index].Level);
        }
        else
        {
            DPRINT1("PoFx %wZ did not finish its directed power down\n", &Device->DeviceId);
        }
    }

    DPRINT1("PoFx directed power down done, %lu of %lu device(s) down\n", Down, Count);

    if (Entries)
        ExFreePoolWithTag(Entries, TAG_PO_FX_WORK_ORDER);
    KeReleaseGuardedMutex(&PopFxDirectedLock);
}

static
VOID
NTAPI
PopFxDirectedWorker(
    _In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    InterlockedExchange(&PopFxDirectedQueued, 0);
    if (!PopFxDirectedAbort)
        PopFxDirectedPowerDown();
}

static
VOID
NTAPI
PopFxDirectedDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (!InterlockedExchange(&PopFxDirectedQueued, 1))
        ExQueueWorkItem(&PopFxDirectedWorkItem, DelayedWorkQueue);
}

/* PUBLIC FUNCTIONS **********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PopFxInitializeDirected(VOID)
{
    KeInitializeGuardedMutex(&PopFxDirectedLock);
    KeInitializeTimer(&PopFxDirectedTimer);
    KeInitializeDpc(&PopFxDirectedDpc, PopFxDirectedDpcRoutine, NULL);
    ExInitializeWorkItem(&PopFxDirectedWorkItem, PopFxDirectedWorker, NULL);
}

/**
 * @brief Powers the directed devices down once connected standby has lasted a while.
 *
 * @param[in] DelayMs
 * How long the display has to stay off first.
 */
VOID
NTAPI
PopFxScheduleDirectedPowerDown(
    _In_ ULONG DelayMs)
{
    LARGE_INTEGER DueTime;

    InterlockedExchange(&PopFxDirectedAbort, 0);
    DueTime.QuadPart = -10000LL * DelayMs;
    KeSetTimer(&PopFxDirectedTimer, DueTime, &PopFxDirectedDpc);
}

/**
 * @brief Brings every directed device back, parents before children.
 */
VOID
NTAPI
PopFxDirectedPowerUp(VOID)
{
    PPOP_FX_DIRECTED_ENTRY Entries;
    PPOP_FX_DEVICE Device;
    ULONG Count, Index, Up = 0;
    BOOLEAN WasDown;
    KIRQL OldIrql;

    PAGED_CODE();

    /* A pass that has not started yet never will, one in progress stops early */
    InterlockedExchange(&PopFxDirectedAbort, 1);
    KeCancelTimer(&PopFxDirectedTimer);

    KeAcquireGuardedMutex(&PopFxDirectedLock);

    Entries = PopFxCollectDirected(FALSE, &Count);
    for (Index = 0; Index < Count; Index++)
    {
        Device = Entries[Index].Device;

        /* A late power down has to finish before the device may be asked up again */
        if (Device->DirectedState == POP_FX_DIRECTED_DOWN_PENDING &&
            !PopFxWaitDirected(Device, POP_FX_DIRECTED_UP_TIMEOUT_S))
        {
            DPRINT1("PoFx %wZ is still powering down, it stays down\n", &Device->DeviceId);
            continue;
        }

        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        WasDown = (Device->DirectedState == POP_FX_DIRECTED_DOWN);
        if (WasDown)
        {
            Device->DirectedState = POP_FX_DIRECTED_UP_PENDING;
            KeClearEvent(&Device->DirectedEvent);
        }
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        if (!WasDown)
            continue;

        Device->DirectedPowerUpCallback(Device->DriverContext, 0);

        if (PopFxWaitDirected(Device, POP_FX_DIRECTED_UP_TIMEOUT_S))
            Up++;
        else
            DPRINT1("PoFx %wZ did not report itself powered on after its directed power up\n", &Device->DeviceId);
    }

    if (Up)
        DPRINT1("PoFx directed %lu device(s) back up\n", Up);

    if (Entries)
        ExFreePoolWithTag(Entries, TAG_PO_FX_WORK_ORDER);
    KeReleaseGuardedMutex(&PopFxDirectedLock);
}
