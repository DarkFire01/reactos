/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Routines the virtualization stack expects of a Windows 10 kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Each of these is the newer face of something the kernel already does, or a
 * question about a machine feature ReactOS does not have. They live together
 * because they arrived together, brought in by the root side of the
 * virtualization stack.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

typedef struct _PS_PROTECTION
{
    union
    {
        UCHAR Level;
        struct
        {
            UCHAR Type:3;
            UCHAR Audit:1;
            UCHAR Signer:4;
        };
    };
} PS_PROTECTION, *PPS_PROTECTION;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Returns the NUMA node the running processor belongs to.
 *
 * @return
 * Zero. Memory here is one node.
 */
USHORT
NTAPI
KeGetCurrentNodeNumber(VOID)
{
    return 0;
}

/**
 * @brief
 * Describes how the logical processors of the machine relate to each other.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. Callers treat that as a machine with nothing worth
 * describing, which is what a flat single node looks like anyway.
 */
NTSTATUS
NTAPI
KeQueryLogicalProcessorRelationship(
    _In_opt_ PPROCESSOR_NUMBER ProcessorNumber,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_opt_(*Length) PVOID Information,
    _Inout_ PULONG Length)
{
    UNREFERENCED_PARAMETER(ProcessorNumber);
    UNREFERENCED_PARAMETER(RelationshipType);
    UNREFERENCED_PARAMETER(Information);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Puts a return that was diverted for a retpoline back on its way.
 *
 * @remarks
 * ReactOS does not divert returns, so there is never one to put back.
 */
VOID
NTAPI
KeReenterRetpolinedCode(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/**
 * @brief
 * Takes a DPC out of its queue, saying which processor's queue to look in.
 *
 * @return
 * TRUE when the DPC was queued and is now removed.
 */
BOOLEAN
NTAPI
KeRemoveQueueDpcEx(
    _Inout_ PRKDPC Dpc,
    _In_ BOOLEAN FlushQueue)
{
    UNREFERENCED_PARAMETER(FlushQueue);

    return KeRemoveQueueDpc(Dpc);
}

/**
 * @brief
 * Returns the kind of protection a process runs under.
 *
 * @remarks
 * ReactOS runs no process protected, so every process reads back as none.
 */
VOID
NTAPI
PsGetProcessProtection(
    _In_ PEPROCESS Process,
    _Out_ PPS_PROTECTION Protection)
{
    UNREFERENCED_PARAMETER(Process);

    Protection->Level = 0;
}

/**
 * @brief
 * Reads a firmware table.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED, as the kernel has no firmware table provider.
 */
NTSTATUS
NTAPI
ExGetSystemFirmwareTable(
    _In_ ULONG FirmwareTableProviderSignature,
    _In_ ULONG FirmwareTableId,
    _Out_writes_bytes_opt_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PULONG ReturnLength)
{
    UNREFERENCED_PARAMETER(FirmwareTableProviderSignature);
    UNREFERENCED_PARAMETER(FirmwareTableId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferLength);

    if (ReturnLength != NULL)
        *ReturnLength = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Reads the mailbox the secure kernel leaves for the normal one.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. There is no secure kernel here to leave one.
 */
NTSTATUS
NTAPI
VslRetrieveMailbox(
    _In_ ULONG MailboxKey,
    _Inout_ PULONG Size,
    _Out_writes_bytes_opt_(*Size) PVOID Buffer)
{
    UNREFERENCED_PARAMETER(MailboxKey);
    UNREFERENCED_PARAMETER(Buffer);

    if (Size != NULL)
        *Size = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
