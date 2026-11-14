/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KSE 'KeepRealTimePriority' shim implementation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The Qualcomm PEP (qcpep850.sys) drops the thread that starts it to priority 0
 * when it creates its workers. Done during its start, that lets its own data
 * worker run ahead of the rest of the start routine. Its driver reinitialization
 * routine later busy waits for those workers, which only works once the thread
 * runs below them. The drop is held back while the driver starts and applied
 * for the length of its reinitialization routine.
 */

#include <ntoskrnl.h>
#include <initguid.h>

#define NDEBUG
#include <debug.h>

DEFINE_GUID(KseKeepRealTimePriorityGuid, 0x4d598a44, 0x510c, 0x4d0f, 0x9d, 0x4d, 0xa2, 0x01, 0xd3, 0x34, 0x75, 0x10);

#define TAG_KSE_PRIORITY 'pKsK'

typedef struct _KSE_HELD_DROP
{
    PKTHREAD Thread;
    KPRIORITY Priority;
} KSE_HELD_DROP, *PKSE_HELD_DROP;

typedef struct _KSE_REINIT_CONTEXT
{
    PDRIVER_REINITIALIZE Routine;
    PVOID Context;
} KSE_REINIT_CONTEXT, *PKSE_REINIT_CONTEXT;

extern KSE_SHIM KseKeepRealTimePriorityShim;

static KSE_HELD_DROP KseHeldDrops[4];
static KSPIN_LOCK KseHeldDropLock;

typedef KPRIORITY (NTAPI *PKSE_SET_PRIORITY)(PKTHREAD, KPRIORITY);
typedef VOID (NTAPI *PKSE_REGISTER_REINIT)(PDRIVER_OBJECT, PDRIVER_REINITIALIZE, PVOID);

static
PKSE_SET_PRIORITY
KseOriginalSetPriority(VOID)
{
    return (PKSE_SET_PRIORITY)KseKeepRealTimePriorityShim.HookCollectionsArray->HookArray[0].OriginalFunction;
}

/* Remembers the last drop asked for each thread */
static
VOID
KseHoldDrop(
    _In_ PKTHREAD Thread,
    _In_ KPRIORITY Priority)
{
    PKSE_HELD_DROP Free = NULL;
    KIRQL OldIrql;
    ULONG Index;

    KeAcquireSpinLock(&KseHeldDropLock, &OldIrql);
    for (Index = 0; Index < RTL_NUMBER_OF(KseHeldDrops); Index++)
    {
        if (KseHeldDrops[Index].Thread == Thread)
        {
            Free = &KseHeldDrops[Index];
            break;
        }
        if (!Free && !KseHeldDrops[Index].Thread)
            Free = &KseHeldDrops[Index];
    }
    if (Free)
    {
        Free->Thread = Thread;
        Free->Priority = Priority;
    }
    KeReleaseSpinLock(&KseHeldDropLock, OldIrql);
}

static
BOOLEAN
KseTakeDrop(
    _In_ PKTHREAD Thread,
    _Out_ KPRIORITY *Priority)
{
    BOOLEAN Found = FALSE;
    KIRQL OldIrql;
    ULONG Index;

    KeAcquireSpinLock(&KseHeldDropLock, &OldIrql);
    for (Index = 0; Index < RTL_NUMBER_OF(KseHeldDrops); Index++)
    {
        if (KseHeldDrops[Index].Thread == Thread)
        {
            *Priority = KseHeldDrops[Index].Priority;
            KseHeldDrops[Index].Thread = NULL;
            Found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&KseHeldDropLock, OldIrql);

    return Found;
}

static
KPRIORITY
NTAPI
ShimKeSetPriorityThread(
    _Inout_ PKTHREAD Thread,
    _In_ KPRIORITY Priority)
{
    if (Thread->Priority >= LOW_REALTIME_PRIORITY && Priority < LOW_REALTIME_PRIORITY)
    {
        DPRINT1("KSE: holding back the drop of thread %p from %d to %ld\n", Thread, Thread->Priority, Priority);
        KseHoldDrop(Thread, Priority);
        return Thread->Priority;
    }

    return KseOriginalSetPriority()(Thread, Priority);
}

/* Runs the driver's routine at the priority it asked for earlier, then puts the thread back */
static
VOID
NTAPI
KseReinitializeAtHeldPriority(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count)
{
    PKSE_REINIT_CONTEXT Reinit = Context;
    PKTHREAD Thread = KeGetCurrentThread();
    KPRIORITY Held;
    KPRIORITY Saved = 0;
    BOOLEAN Lowered;

    Lowered = KseTakeDrop(Thread, &Held);
    if (Lowered)
        Saved = KseOriginalSetPriority()(Thread, Held);

    Reinit->Routine(DriverObject, Reinit->Context, Count);

    if (Lowered)
        KseOriginalSetPriority()(Thread, Saved);

    ExFreePoolWithTag(Reinit, TAG_KSE_PRIORITY);
}

static
VOID
NTAPI
ShimIoRegisterDriverReinitialization(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDRIVER_REINITIALIZE DriverReinitializationRoutine,
    _In_opt_ PVOID Context)
{
    PKSE_REGISTER_REINIT OriginalFunction;
    PKSE_REINIT_CONTEXT Reinit;

    OriginalFunction = (PKSE_REGISTER_REINIT)KseKeepRealTimePriorityShim.HookCollectionsArray->HookArray[1].OriginalFunction;

    Reinit = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Reinit), TAG_KSE_PRIORITY);
    if (!Reinit)
    {
        OriginalFunction(DriverObject, DriverReinitializationRoutine, Context);
        return;
    }

    Reinit->Routine = DriverReinitializationRoutine;
    Reinit->Context = Context;
    OriginalFunction(DriverObject, KseReinitializeAtHeldPriority, Reinit);
}

static KSE_HOOK KseKeepRealTimePriorityHooksNT[] = {
    { KseHookFunction, { "KeSetPriorityThread" }, (PVOID)(ULONG_PTR)ShimKeSetPriorityThread },
    { KseHookFunction, { "IoRegisterDriverReinitialization" }, (PVOID)(ULONG_PTR)ShimIoRegisterDriverReinitialization },
    { KseHookInvalid }
};

static KSE_HOOK_COLLECTION KseKeepRealTimePriorityCollections[] = {
    { KseCollectionNtExport, NULL, KseKeepRealTimePriorityHooksNT },
    { KseCollectionInvalid },
};

KSE_SHIM KseKeepRealTimePriorityShim = { sizeof(KSE_SHIM), &KseKeepRealTimePriorityGuid, L"KeepRealTimePriority", NULL, NULL, NULL, KseKeepRealTimePriorityCollections };

CODE_SEG("INIT")
NTSTATUS
NTAPI
KseKeepRealTimePriorityInitialize(VOID)
{
    NTSTATUS Status;

    KeInitializeSpinLock(&KseHeldDropLock);

    Status = KseRegisterShim(&KseKeepRealTimePriorityShim, NULL, 0);
    if (!NT_SUCCESS(Status))
        DPRINT1("KSE: Unable to register KeepRealTimePriority shim (0x%lx)\n", Status);

    return Status;
}
