/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt services
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * Interrupts that arrive through another driver rather than through the IDT.
 *
 * A GPIO or SPB controller raises one hardware line for many devices. The
 * controller's driver works out which of its pins asserted and hands the pin
 * back to the kernel, which then runs whatever ISR was connected to it. None of
 * this touches the IDT: a secondary interrupt is delivered by a call, from
 * inside the controller's own ISR or DPC, and the vector it carries is an index
 * into a table of connected service routines rather than a processor vector.
 *
 * Reconstructed from Win8 ntoskrnl.exe.c: KeInitializeSecondaryInterruptServices
 * (:827933), KiConnectSecondaryInterrupt (:345808), KeDispatchSecondaryInterrupt
 * (:345885) and the mask and unmask pair at :345619 and :345676.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/**
 * @brief
 * One secondary vector's connected service routines.
 *
 * The list is the same shared-vector chain a real vector uses, so an ISR
 * written for a line interrupt needs no changes to sit on a GPIO pin.
 */
typedef struct _KSECONDARY_IDT_ENTRY
{
    KSPIN_LOCK SpinLock;

    /* Serializes connect and disconnect against each other, not against the ISR */
    KEVENT ConnectLock;

    PKINTERRUPT InterruptList;

    /* Set while the controller has been asked to hold this line quiet */
    BOOLEAN LineMasked;
} KSECONDARY_IDT_ENTRY, *PKSECONDARY_IDT_ENTRY;

static PKSECONDARY_IDT_ENTRY KiGlobalSecondaryIDT;

/*
 * Set once the table exists. Everything here answers STATUS_UNSUCCESSFUL until
 * it does, which is what the reference does at :345820.
 */
BOOLEAN KiSecondaryInterruptServicesEnabled = FALSE;

/*
 * Which slots have been handed out. A slot is claimed for the lifetime of the
 * GSIV rather than of the connection, so a device that disconnects and
 * reconnects comes back on the vector its controller already knows about.
 */
static ULONG KiSecondaryVectorGsiv[MAXIMUM_SECONDARY_VECTORS];
static KSPIN_LOCK KiSecondaryVectorLock;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Builds the table of secondary vectors.
 *
 * Called by the HAL the first time a GSIV is minted for a secondary
 * controller, so a machine with no such controller never pays for the table.
 *
 * @param[in] HalExports
 * Reserved. The reference passes the HAL's own routine block here and ignores
 * it as well (:827933); mask and unmask are reached through the private
 * dispatch table instead.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
NTAPI
KeInitializeSecondaryInterruptServices(
    _In_opt_ PVOID HalExports)
{
    PKSECONDARY_IDT_ENTRY Table;
    ULONG Index;

    UNREFERENCED_PARAMETER(HalExports);

    if (KiSecondaryInterruptServicesEnabled)
    {
        return STATUS_SUCCESS;
    }

    Table = ExAllocatePoolZero(NonPagedPool,
                               MAXIMUM_SECONDARY_VECTORS * sizeof(KSECONDARY_IDT_ENTRY),
                               TAG_KERNEL_SECONDARY);
    if (Table == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < MAXIMUM_SECONDARY_VECTORS; Index++)
    {
        KeInitializeSpinLock(&Table[Index].SpinLock);
        KeInitializeEvent(&Table[Index].ConnectLock, SynchronizationEvent, TRUE);
        KiSecondaryVectorGsiv[Index] = KI_SECONDARY_VECTOR_FREE;
    }

    KeInitializeSpinLock(&KiSecondaryVectorLock);

    KiGlobalSecondaryIDT = Table;
    KiSecondaryInterruptServicesEnabled = TRUE;

    DPRINT1("Secondary interrupt services enabled, %u vectors from 0x%x\n",
            MAXIMUM_SECONDARY_VECTORS, SECONDARY_VECTOR_BASE);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives a GSIV the vector its interrupts will be delivered on.
 *
 * The vector is not a processor vector and never reaches the IDT: it is the
 * name the HAL records for the line, so that a controller reporting a pin can
 * be turned back into the routines connected to it. The reference has the ACPI
 * arbiter allocate it out of the same pool it uses for real IDT entries, based
 * at 256 (acpi.sys.c:49602 sets the base to 256 for a secondary interrupt);
 * this keeps the allocation with the table it indexes, which is the only place
 * that can say whether a slot is free.
 *
 * @param[in] Gsiv
 * The secondary GSIV.
 *
 * @param[out] Vector
 * Receives its vector.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES once the table is full.
 */
NTSTATUS
NTAPI
KeAllocateSecondaryVector(
    _In_ ULONG Gsiv,
    _Out_ PULONG Vector)
{
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    ULONG Free = MAXIMUM_SECONDARY_VECTORS;
    KIRQL OldIrql;
    ULONG Index;

    *Vector = 0;

    if (!KiSecondaryInterruptServicesEnabled)
    {
        return STATUS_UNSUCCESSFUL;
    }

    KeAcquireSpinLock(&KiSecondaryVectorLock, &OldIrql);

    for (Index = 0; Index < MAXIMUM_SECONDARY_VECTORS; Index++)
    {
        if (KiSecondaryVectorGsiv[Index] == Gsiv)
        {
            /* Already named; the same line keeps the same vector */
            *Vector = SECONDARY_VECTOR_BASE + Index;
            Status = STATUS_SUCCESS;
            break;
        }

        if ((Free == MAXIMUM_SECONDARY_VECTORS) &&
            (KiSecondaryVectorGsiv[Index] == KI_SECONDARY_VECTOR_FREE))
        {
            Free = Index;
        }
    }

    if (!NT_SUCCESS(Status) && (Free != MAXIMUM_SECONDARY_VECTORS))
    {
        KiSecondaryVectorGsiv[Free] = Gsiv;
        *Vector = SECONDARY_VECTOR_BASE + Free;
        Status = STATUS_SUCCESS;
    }

    KeReleaseSpinLock(&KiSecondaryVectorLock, OldIrql);

    return Status;
}

/**
 * @brief
 * The table entry a secondary vector names.
 *
 * @param[in] Vector
 * The vector.
 *
 * @return
 * Its entry, or NULL for anything that is not a secondary vector.
 */
static
PKSECONDARY_IDT_ENTRY
KiSecondaryIdtEntry(
    _In_ ULONG Vector)
{
    ULONG Index;

    if (!KiSecondaryInterruptServicesEnabled)
    {
        return NULL;
    }

    Index = Vector - SECONDARY_VECTOR_BASE;
    if (Index >= MAXIMUM_SECONDARY_VECTORS)
    {
        return NULL;
    }

    return &KiGlobalSecondaryIDT[Index];
}

/**
 * @brief
 * Says whether a connection describes an interrupt another driver delivers.
 *
 * @param[in] ConnectionData
 * The device's published connection data.
 *
 * @return
 * TRUE for a single controller input whose GSIV belongs to a secondary
 * controller.
 */
BOOLEAN
NTAPI
KiIsInterruptTypeSecondary(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    if (!KiSecondaryInterruptServicesEnabled || (ConnectionData->Count != 1))
    {
        return FALSE;
    }

    if (HalIsInterruptTypeSecondary == NULL)
    {
        return FALSE;
    }

    return HalIsInterruptTypeSecondary(
               ConnectionData->Vectors[0].Type,
               ConnectionData->Vectors[0].ControllerInput.Gsiv);
}

/**
 * @brief
 * Puts an interrupt object on a secondary vector.
 *
 * This is the secondary counterpart of KeConnectInterrupt, and shares its
 * rules: a vector already carrying an interrupt takes another only when both
 * sides agreed to share and both describe the same trigger.
 *
 * @param[in] Interrupt
 * The interrupt object, already initialized.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER when the vector cannot take it.
 */
NTSTATUS
NTAPI
KiConnectSecondaryInterrupt(
    _In_ PKINTERRUPT Interrupt)
{
    PKSECONDARY_IDT_ENTRY Entry;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;

    if (!KiSecondaryInterruptServicesEnabled)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Entry = KiSecondaryIdtEntry(Interrupt->Vector);
    if ((Entry == NULL) ||
        (Interrupt->Irql > HIGH_LEVEL) ||
        (Interrupt->SynchronizeIrql < Interrupt->Irql) ||
        (Interrupt->FloatingSave))
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeWaitForSingleObject(&Entry->ConnectLock, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&Entry->SpinLock, &OldIrql);

    if (!Interrupt->Connected)
    {
        PKINTERRUPT Head = Entry->InterruptList;

        if (Head == NULL)
        {
            InitializeListHead(&Interrupt->InterruptListEntry);
            Entry->InterruptList = Interrupt;
            Interrupt->Connected = TRUE;
            Status = STATUS_SUCCESS;
        }
        else if (Interrupt->ShareVector && Head->ShareVector &&
                 (Head->Mode == Interrupt->Mode))
        {
            InsertTailList(&Head->InterruptListEntry, &Interrupt->InterruptListEntry);
            Interrupt->Connected = TRUE;
            Status = STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&Entry->SpinLock, OldIrql);
    KeSetEvent(&Entry->ConnectLock, IO_NO_INCREMENT, FALSE);

    return Status;
}

/**
 * @brief
 * Takes an interrupt object off its secondary vector.
 *
 * @param[in] Interrupt
 * The interrupt object.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for one that was never on it.
 */
NTSTATUS
NTAPI
KiDisconnectSecondaryInterrupt(
    _In_ PKINTERRUPT Interrupt)
{
    PKSECONDARY_IDT_ENTRY Entry;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;

    Entry = KiSecondaryIdtEntry(Interrupt->Vector);
    if (Entry == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeWaitForSingleObject(&Entry->ConnectLock, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&Entry->SpinLock, &OldIrql);

    if (Interrupt->Connected)
    {
        if (Entry->InterruptList == Interrupt)
        {
            PLIST_ENTRY Next = Interrupt->InterruptListEntry.Flink;

            /*
             * The head is the entry the rest of the chain is threaded onto, so
             * handing the vector over to whoever is next has to happen before
             * this one is unlinked.
             */
            Entry->InterruptList = (Next == &Interrupt->InterruptListEntry)
                                       ? NULL
                                       : CONTAINING_RECORD(Next, KINTERRUPT, InterruptListEntry);
        }

        RemoveEntryList(&Interrupt->InterruptListEntry);
        Interrupt->Connected = FALSE;
        Status = STATUS_SUCCESS;
    }

    KeReleaseSpinLock(&Entry->SpinLock, OldIrql);
    KeSetEvent(&Entry->ConnectLock, IO_NO_INCREMENT, FALSE);

    return Status;
}

/**
 * @brief
 * Runs the service routines connected to one secondary vector.
 *
 * Called by the HAL on behalf of the controller that demultiplexed the line,
 * so this already runs at the controller's own IRQL or below. Each routine is
 * called at the IRQL it asked to be synchronized to, under its own lock, which
 * is what makes KeSynchronizeExecution mean the same thing for a pin as it
 * does for a line.
 *
 * @param[in] Vector
 * The secondary vector that fired.
 *
 * @param[in] Flags
 * Reserved. The reference carries a wake indication in bit 20 (:345894).
 *
 * @return
 * TRUE if one of the routines claimed the interrupt.
 */
BOOLEAN
NTAPI
KeDispatchSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Flags)
{
    PKSECONDARY_IDT_ENTRY Entry;
    PKINTERRUPT Interrupt;
    PLIST_ENTRY ListHead, NextEntry;
    BOOLEAN Handled = FALSE;
    KIRQL OldIrql, OldInterruptIrql;

    UNREFERENCED_PARAMETER(Flags);

    Entry = KiSecondaryIdtEntry(Vector);
    if (Entry == NULL)
    {
        return FALSE;
    }

    KeAcquireSpinLock(&Entry->SpinLock, &OldIrql);

    Interrupt = Entry->InterruptList;
    if (Interrupt == NULL)
    {
        KeReleaseSpinLock(&Entry->SpinLock, OldIrql);
        return FALSE;
    }

    ListHead = &Interrupt->InterruptListEntry;
    NextEntry = ListHead;

    while (TRUE)
    {
        if (Interrupt->SynchronizeIrql > Interrupt->Irql)
        {
            KeRaiseIrql(Interrupt->SynchronizeIrql, &OldInterruptIrql);
        }
        else
        {
            OldInterruptIrql = Interrupt->Irql;
        }

        KxAcquireSpinLock(Interrupt->ActualLock);

        Handled = Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext);

        KxReleaseSpinLock(Interrupt->ActualLock);

        if (Interrupt->SynchronizeIrql > Interrupt->Irql)
        {
            KeLowerIrql(OldInterruptIrql);
        }

        /* A level source stays asserted until someone services it */
        if (Handled && (Interrupt->Mode == LevelSensitive))
        {
            break;
        }

        NextEntry = NextEntry->Flink;
        if (NextEntry == ListHead)
        {
            break;
        }

        Interrupt = CONTAINING_RECORD(NextEntry, KINTERRUPT, InterruptListEntry);
    }

    KeReleaseSpinLock(&Entry->SpinLock, OldIrql);

    return Handled;
}

/**
 * @brief
 * Asks the controller to stop or resume raising one line.
 *
 * @param[in] Vector
 * The secondary vector the line was connected on.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @param[in] Mask
 * TRUE to mask, FALSE to unmask.
 *
 * @return
 * STATUS_SUCCESS, or the HAL's failure.
 */
static
NTSTATUS
KiMaskUnmaskSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Gsiv,
    _In_ BOOLEAN Mask)
{
    PKSECONDARY_IDT_ENTRY Entry;
    NTSTATUS Status;

    Entry = KiSecondaryIdtEntry(Vector);
    if (Entry == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Mask)
    {
        if (HalMaskInterrupt == NULL)
        {
            return STATUS_NOT_SUPPORTED;
        }

        Status = HalMaskInterrupt(Gsiv, 0);
        if (NT_SUCCESS(Status))
        {
            Entry->LineMasked = TRUE;
        }
    }
    else
    {
        if (HalUnmaskInterrupt == NULL)
        {
            return STATUS_NOT_SUPPORTED;
        }

        Status = HalUnmaskInterrupt(Gsiv, 0);
        if (NT_SUCCESS(Status))
        {
            Entry->LineMasked = FALSE;
        }
    }

    return Status;
}

NTSTATUS
NTAPI
KiMaskSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Gsiv)
{
    return KiMaskUnmaskSecondaryInterrupt(Vector, Gsiv, TRUE);
}

NTSTATUS
NTAPI
KiUnmaskSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Gsiv)
{
    return KiMaskUnmaskSecondaryInterrupt(Vector, Gsiv, FALSE);
}

/* EOF */
