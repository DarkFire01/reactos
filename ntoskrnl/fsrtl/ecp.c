/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extra parameters a create can carry beyond its own fields
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * A create carries these when what it has to say does not fit in the fields it
 * already has. Each one is named by a GUID and holds whatever its own owner
 * puts in it, and the caller only ever sees that part: the bookkeeping sits
 * immediately in front of what it was handed, so a context pointer is enough
 * to find everything else about the parameter.
 */

#define TAG_ECP_LIST 'LpcE'

/* TYPES **********************************************************************/

typedef struct _ECP_HEADER
{
    LIST_ENTRY Link;
    GUID EcpType;
    PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK CleanupCallback;
    ULONG PoolTag;
    ULONG ContextSize;
    /* What the caller was handed starts here */
    ULONG64 Context[ANYSIZE_ARRAY];
} ECP_HEADER, *PECP_HEADER;

typedef struct _ECP_LIST
{
    LIST_ENTRY Parameters;
    FSRTL_ALLOCATE_ECPLIST_FLAGS Flags;
} ECP_LIST, *PECP_LIST;

/* The bookkeeping in front of a context, and the context in front of it */
#define ECP_HEADER_FROM_CONTEXT(c) \
    CONTAINING_RECORD((c), ECP_HEADER, Context)

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlAllocateExtraCreateParameterList(
    _In_ FSRTL_ALLOCATE_ECPLIST_FLAGS Flags,
    _Outptr_ PECP_LIST *EcpList)
{
    PECP_LIST List;

    if (EcpList == NULL)
        return STATUS_INVALID_PARAMETER;

    List = ExAllocatePoolZero(PagedPool, sizeof(*List), TAG_ECP_LIST);
    if (List == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    InitializeListHead(&List->Parameters);
    List->Flags = Flags;

    *EcpList = List;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
FsRtlFreeExtraCreateParameterList(
    _In_ PECP_LIST EcpList)
{
    if (EcpList == NULL)
        return;

    /* Whatever is still on the list goes with it */
    while (!IsListEmpty(&EcpList->Parameters))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&EcpList->Parameters);
        PECP_HEADER Header = CONTAINING_RECORD(Entry, ECP_HEADER, Link);

        if (Header->CleanupCallback != NULL)
            Header->CleanupCallback(&Header->Context, &Header->EcpType);

        ExFreePoolWithTag(Header, Header->PoolTag);
    }

    ExFreePoolWithTag(EcpList, TAG_ECP_LIST);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlAllocateExtraCreateParameter(
    _In_ LPCGUID EcpType,
    _In_ ULONG SizeOfContext,
    _In_ FSRTL_ALLOCATE_ECP_FLAGS Flags,
    _In_opt_ PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK CleanupCallback,
    _In_ ULONG PoolTag,
    _Outptr_result_bytebuffer_(SizeOfContext) PVOID *EcpContext)
{
    PECP_HEADER Header;
    SIZE_T Total;

    UNREFERENCED_PARAMETER(Flags);

    if ((EcpType == NULL) || (EcpContext == NULL))
        return STATUS_INVALID_PARAMETER;

    Total = FIELD_OFFSET(ECP_HEADER, Context) + SizeOfContext;

    Header = ExAllocatePoolZero(PagedPool, Total, PoolTag);
    if (Header == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    InitializeListHead(&Header->Link);
    Header->EcpType = *EcpType;
    Header->CleanupCallback = CleanupCallback;
    Header->PoolTag = PoolTag;
    Header->ContextSize = SizeOfContext;

    /* The caller is given its own part, not the whole of it */
    *EcpContext = &Header->Context;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
FsRtlFreeExtraCreateParameter(
    _In_ PVOID EcpContext)
{
    PECP_HEADER Header;

    if (EcpContext == NULL)
        return;

    Header = ECP_HEADER_FROM_CONTEXT(EcpContext);

    if (Header->CleanupCallback != NULL)
        Header->CleanupCallback(EcpContext, &Header->EcpType);

    ExFreePoolWithTag(Header, Header->PoolTag);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlInsertExtraCreateParameter(
    _Inout_ PECP_LIST EcpList,
    _Inout_ PVOID EcpContext)
{
    PECP_HEADER Header;

    if ((EcpList == NULL) || (EcpContext == NULL))
        return STATUS_INVALID_PARAMETER;

    Header = ECP_HEADER_FROM_CONTEXT(EcpContext);

    /* The list owns it from here, and frees it when the list goes */
    InsertTailList(&EcpList->Parameters, &Header->Link);
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlFindExtraCreateParameter(
    _In_ PECP_LIST EcpList,
    _In_ LPCGUID EcpType,
    _Outptr_opt_ PVOID *EcpContext,
    _Out_opt_ ULONG *EcpContextSize)
{
    PLIST_ENTRY Entry;

    if ((EcpList == NULL) || (EcpType == NULL))
        return STATUS_INVALID_PARAMETER;

    for (Entry = EcpList->Parameters.Flink;
         Entry != &EcpList->Parameters;
         Entry = Entry->Flink)
    {
        PECP_HEADER Header = CONTAINING_RECORD(Entry, ECP_HEADER, Link);

        if (!IsEqualGUID(&Header->EcpType, EcpType))
            continue;

        if (EcpContext != NULL)
            *EcpContext = &Header->Context;

        if (EcpContextSize != NULL)
            *EcpContextSize = Header->ContextSize;

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlRemoveExtraCreateParameter(
    _Inout_ PECP_LIST EcpList,
    _In_ LPCGUID EcpType,
    _Outptr_ PVOID *EcpContext,
    _Out_opt_ ULONG *EcpContextSize)
{
    PLIST_ENTRY Entry;

    if ((EcpList == NULL) || (EcpType == NULL) || (EcpContext == NULL))
        return STATUS_INVALID_PARAMETER;

    for (Entry = EcpList->Parameters.Flink;
         Entry != &EcpList->Parameters;
         Entry = Entry->Flink)
    {
        PECP_HEADER Header = CONTAINING_RECORD(Entry, ECP_HEADER, Link);

        if (!IsEqualGUID(&Header->EcpType, EcpType))
            continue;

        RemoveEntryList(&Header->Link);
        InitializeListHead(&Header->Link);

        /* Taking it off the list hands it back, so the caller frees it now */
        *EcpContext = &Header->Context;

        if (EcpContextSize != NULL)
            *EcpContextSize = Header->ContextSize;

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlIsNonEmptyDirectoryReparsePointAllowed(
    _In_ ULONG ReparseTag)
{
    /*
     * A tag says so about itself with a bit of its own. Container isolation is
     * the one that predates the bit and is allowed by name instead.
     */
    return (((ReparseTag & IO_REPARSE_TAG_DIRECTORY_MASK) != 0) ||
            (ReparseTag == IO_REPARSE_TAG_WCI));
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlGetEcpListFromIrp(
    _In_ PIRP Irp,
    _Outptr_result_maybenull_ PECP_LIST *EcpList)
{
    /* Only a create carries one, and only a create leaves the room for it free */
    if (!(Irp->Flags & IRP_CREATE_OPERATION))
        return STATUS_INVALID_PARAMETER;

    *EcpList = Irp->UserBuffer;

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlSetEcpListIntoIrp(
    _Inout_ PIRP Irp,
    _In_ PECP_LIST EcpList)
{
    if (!(Irp->Flags & IRP_CREATE_OPERATION))
        return STATUS_INVALID_PARAMETER;

    Irp->UserBuffer = EcpList;

    return STATUS_SUCCESS;
}

/* EOF */
