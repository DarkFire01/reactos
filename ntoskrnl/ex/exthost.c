/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extension hosts, which let a driver take over part of the kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The kernel publishes a host for a piece of work it is willing to hand over,
 * and a driver binds its own table of routines into that host. Each side then
 * calls the other through a table rather than by name, so the driver can be
 * loaded and unloaded around a kernel that was built without it.
 *
 * Verified against Reference/Reference_Win10 for what a registration carries
 * and what a caller gets back.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static LIST_ENTRY ExpHostListHead;
static KSPIN_LOCK ExpHostListLock;

/*
 * Second level address translation, which is the host the virtualization stack
 * binds itself into. Nothing in the kernel drives it, so every routine it
 * publishes says so and the driver takes its own path.
 */
#define EXP_HOST_SLAT_ID      8
#define EXP_HOST_SLAT_VERSION 1

static EXP_EXTENSION_HOST ExpSlatHost;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Stands in for a host routine the kernel does not have.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED, whatever it was asked for.
 *
 * @remarks
 * Every argument arrives in a register on this architecture and the caller
 * cleans up after itself, so one routine can answer for any of the slots
 * without disturbing the stack.
 */
static
NTSTATUS
NTAPI
ExpHostRoutineNotImplemented(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * The host in the order it publishes its routines, because the driver reaches
 * for them by slot. A slot the kernel has nothing behind still has to answer,
 * so it gets the stub and the driver takes its own path from the status.
 */
static PVOID ExpSlatHostTable[] =
{
    VmCreateMemoryRange,
    VmDeleteMemoryRange,
    VmSplitMemoryRange,
    VmMergeMemoryRanges,
    VmPreallocateForRangeCreate,
    VmFreePreallocationForRangeCreate,
    VmAccessFault,
    ExpHostRoutineNotImplemented,       /* VmPauseResumeNotify */
    ExpHostRoutineNotImplemented,       /* VmColdPagesHint */
    ExpHostRoutineNotImplemented,       /* VmCreateMemoryProcess */
    ExpHostRoutineNotImplemented,       /* VmSetThreadSchedulerAssist */
    ExpHostRoutineNotImplemented,       /* VmProbeAndLockPages */
    ExpHostRoutineNotImplemented,       /* VmUnlockPages */
    ExpHostRoutineNotImplemented,       /* VmSecureBackingMemory */
    ExpHostRoutineNotImplemented,       /* VmUnsecureBackingMemory */
    ExpHostRoutineNotImplemented,       /* VmCallSkSvc */
    ExpHostRoutineNotImplemented,       /* VmTerminateMemoryProcess */
    ExpHostRoutineNotImplemented,       /* VmSetVpHostProcess */
    ExpHostRoutineNotImplemented,       /* KeAbPreAcquire */
    ExpHostRoutineNotImplemented,       /* KeAbPreWait */
    ExpHostRoutineNotImplemented,       /* KeAbPostReleaseEx */
    ExpHostRoutineNotImplemented,       /* KeAbPostAcquire */
    ExpHostRoutineNotImplemented,       /* KeAbPostRelease */
    ExpHostRoutineNotImplemented,       /* MmIsMdlPageDanging */
    VmPinMemoryRange,
    VmUnpinMemoryRange,
    ExpHostRoutineNotImplemented,       /* VmSetTestMode */
    ExpHostRoutineNotImplemented,       /* VmUpdateCommitMemoryRange */
    PsGetJobMemoryPartition,
    PsReferencePartitionSystemProcess,
    PsGetProcessPartition,
    VmMapSectionExecuteNoAcg,
    ExpHostRoutineNotImplemented,       /* EtwpWriteProcessorTrace */
};

/**
 * @brief
 * Finds the host a registration names.
 *
 * @remarks
 * Called with the list lock held.
 */
static
PEXP_EXTENSION_HOST
NTAPI
ExpFindHost(
    _In_ USHORT ExtensionId,
    _In_ USHORT ExtensionVersion)
{
    PLIST_ENTRY Entry;
    PEXP_EXTENSION_HOST Host;

    for (Entry = ExpHostListHead.Flink;
         Entry != &ExpHostListHead;
         Entry = Entry->Flink)
    {
        Host = CONTAINING_RECORD(Entry, EXP_EXTENSION_HOST, ListEntry);

        if ((Host->ExtensionId == ExtensionId) &&
            (Host->ExtensionVersion == ExtensionVersion))
        {
            return Host;
        }
    }

    return NULL;
}

/**
 * @brief
 * Publishes the hosts the kernel offers.
 */
CODE_SEG("INIT")
BOOLEAN
NTAPI
ExpInitializeExtensions(VOID)
{
    InitializeListHead(&ExpHostListHead);
    KeInitializeSpinLock(&ExpHostListLock);

    ExpSlatHost.ExtensionId = EXP_HOST_SLAT_ID;
    ExpSlatHost.ExtensionVersion = EXP_HOST_SLAT_VERSION;
    ExpSlatHost.FunctionCount = 0;
    ExpSlatHost.HostTable = ExpSlatHostTable;
    ExpSlatHost.FunctionTable = NULL;

    InsertTailList(&ExpHostListHead, &ExpSlatHost.ListEntry);

    return TRUE;
}

/**
 * @brief
 * Binds a driver's table of routines into the host that asked for them.
 *
 * @param[out] Extension
 * Receives the binding, which is what unregistering takes back.
 *
 * @param[in] Version
 * The layout of @p Registration. Only the first is defined.
 *
 * @param[in,out] Registration
 * What the driver offers, and where it wants the host's own table written.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for a registration that does not
 * fit, STATUS_NOT_FOUND when no host offers that interface, or
 * STATUS_OBJECT_NAME_COLLISION when one is already bound.
 */
NTSTATUS
NTAPI
ExRegisterExtension(
    _Out_ PEX_EXTENSION *Extension,
    _In_ ULONG Version,
    _Inout_ PEX_EXTENSION_REGISTRATION_1 Registration)
{
    PEXP_EXTENSION_HOST Host;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (((Version & 0xFFFF0000) != 0x00010000) ||
        (Registration == NULL) ||
        (Registration->FunctionTable == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&ExpHostListLock, &OldIrql);

    Host = ExpFindHost(Registration->ExtensionId, Registration->ExtensionVersion);
    if (Host == NULL)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (Registration->FunctionCount < Host->FunctionCount)
    {
        /* The host calls more routines than the driver brought */
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Host->FunctionTable != NULL)
    {
        Status = STATUS_OBJECT_NAME_COLLISION;
    }
    else
    {
        Host->FunctionTable = Registration->FunctionTable;

        if (Registration->HostInterface != NULL)
            *Registration->HostInterface = Host->HostTable;

        *Extension = (PEX_EXTENSION)Host;
        Status = STATUS_SUCCESS;
    }

    KeReleaseSpinLock(&ExpHostListLock, OldIrql);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Ex: extension %u version %u was refused, status %lx\n",
                Registration->ExtensionId,
                Registration->ExtensionVersion,
                Status);
    }

    return Status;
}

/**
 * @brief
 * Takes a driver's routines back out of its host.
 */
VOID
NTAPI
ExUnregisterExtension(
    _In_ PEX_EXTENSION Extension)
{
    PEXP_EXTENSION_HOST Host = (PEXP_EXTENSION_HOST)Extension;
    KIRQL OldIrql;

    if (Host == NULL)
        return;

    KeAcquireSpinLock(&ExpHostListLock, &OldIrql);
    Host->FunctionTable = NULL;
    KeReleaseSpinLock(&ExpHostListLock, OldIrql);
}

/* EOF */
