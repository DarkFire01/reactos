/*
 * PROJECT:     ReactOS Named Pipe FileSystem
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * FILE:        drivers/filesystems/npfs/prefxsup.c
 * PURPOSE:     Pipes Prefixes Support
 * PROGRAMMERS: ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include "npfs.h"

// File ID number for NPFS bugchecking support
#define NPFS_BUGCHECK_FILE_ID   (NPFS_BUGCHECK_PREFXSUP)

/* FUNCTIONS ******************************************************************/

PNP_FCB
NTAPI
NpFindPrefix(IN PUNICODE_STRING Name,
             IN ULONG CaseInsensitiveIndex,
             IN PUNICODE_STRING Prefix)
{
    PUNICODE_PREFIX_TABLE_ENTRY Entry;
    PNP_FCB Fcb;
    PAGED_CODE();

    Entry = RtlFindUnicodePrefix(&NpVcb->PrefixTable,
                                 Name,
                                 CaseInsensitiveIndex);
    if (!Entry) NpBugCheck(0, 0, 0);

    Fcb = CONTAINING_RECORD(Entry, NP_FCB, PrefixTableEntry);

    Prefix->Length = Name->Length - Fcb->FullName.Length;
    Prefix->MaximumLength = Prefix->Length;
    Prefix->Buffer = &Name->Buffer[Fcb->FullName.Length / sizeof(WCHAR)];

    if ((Prefix->Length) && (Prefix->Buffer[0] == OBJ_NAME_PATH_SEPARATOR))
    {
        Prefix->Length -= sizeof(WCHAR);
        Prefix->MaximumLength -= sizeof(WCHAR);
        ++Prefix->Buffer;
    }

    return Fcb;
}

NTSTATUS
NTAPI
NpFindRelativePrefix(IN PNP_DCB Dcb,
                     IN PUNICODE_STRING Name,
                     IN ULONG CaseInsensitiveIndex,
                     IN PUNICODE_STRING Prefix,
                     OUT PNP_FCB *FoundFcb)
{
    PWCHAR Buffer;
    PNP_FCB Fcb;
    UNICODE_STRING RootName;
    USHORT Length, MaximumLength;
    PAGED_CODE();

    Length = Name->Length;
    MaximumLength = Length + sizeof(OBJ_NAME_PATH_SEPARATOR) + sizeof(UNICODE_NULL);
    if (MaximumLength < Length) return STATUS_INVALID_PARAMETER;

    ASSERT(Dcb->NodeType == NPFS_NTC_ROOT_DCB);

    Buffer = ExAllocatePoolWithTag(PagedPool, MaximumLength, NPFS_NAME_BLOCK_TAG);
    if (!Buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Buffer = OBJ_NAME_PATH_SEPARATOR;
    RtlCopyMemory(Buffer + 1, Name->Buffer, Length);
    Buffer[(Length / sizeof(WCHAR)) + 1] = UNICODE_NULL;

    RootName.Length = Length + sizeof(OBJ_NAME_PATH_SEPARATOR);
    RootName.MaximumLength = MaximumLength;
    RootName.Buffer = Buffer;

    Fcb = NpFindPrefix(&RootName, CaseInsensitiveIndex, Prefix);

    ExFreePool(Buffer);

    Prefix->Buffer = &Name->Buffer[(Length - Prefix->Length) / sizeof(WCHAR)];
    *FoundFcb = Fcb;

    return STATUS_SUCCESS;
}

/* Access check against a prefix, as the caller of a create */
static
NTSTATUS
NpAccessCheckPrefix(
    _In_ PNP_PREFIX Prefix,
    _In_ PACCESS_STATE AccessState,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ BOOLEAN UpdateAccessState,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    PPRIVILEGE_SET Privileges = NULL;
    ACCESS_MASK GrantedAccess;
    NTSTATUS Status;
    BOOLEAN Granted;

    SeLockSubjectContext(&AccessState->SubjectSecurityContext);
    Granted = SeAccessCheck(Prefix->SecurityDescriptor,
                            &AccessState->SubjectSecurityContext,
                            TRUE,
                            DesiredAccess,
                            0,
                            &Privileges,
                            IoGetFileObjectGenericMapping(),
                            PreviousMode,
                            &GrantedAccess,
                            &Status);
    if (Privileges)
    {
        if (UpdateAccessState)
            SeAppendPrivileges(AccessState, Privileges);
        SeFreePrivileges(Privileges);
    }

    if (Granted && UpdateAccessState)
    {
        AccessState->PreviouslyGrantedAccess |= GrantedAccess;
        AccessState->RemainingDesiredAccess &= ~(GrantedAccess | MAXIMUM_ALLOWED);
    }
    SeUnlockSubjectContext(&AccessState->SubjectSecurityContext);

    return Granted ? STATUS_SUCCESS : Status;
}

/**
 * @brief
 * Opens an existing protected prefix by its exact name.
 */
IO_STATUS_BLOCK
NTAPI
NpOpenNamedPipePrefix(
    _In_ PFILE_OBJECT FileObject,
    _In_ PACCESS_STATE AccessState,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    IO_STATUS_BLOCK IoStatus = {{0}};
    UNICODE_STRING Remaining;
    PNP_PREFIX Prefix;

    PAGED_CODE();

    Prefix = (PNP_PREFIX)NpFindPrefix(&FileObject->FileName, 1, &Remaining);
    if (Prefix->NodeType != NPFS_NTC_PREFIX || Remaining.Length)
    {
        IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        return IoStatus;
    }

    IoStatus.Status = NpAccessCheckPrefix(Prefix, AccessState, DesiredAccess, TRUE, PreviousMode);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus;

    ++Prefix->CurrentInstances;
    NpSetFileObject(FileObject, Prefix, NULL, FALSE);
    IoStatus.Information = FILE_OPENED;
    return IoStatus;
}

/**
 * @brief
 * Creates a protected prefix, or opens it for FILE_OPEN_IF. The caller needs
 * SeTcbPrivilege or FILE_ADD_SUBDIRECTORY on the prefix it is created under.
 */
IO_STATUS_BLOCK
NTAPI
NpCreateNamedPipePrefix(
    _In_ PFILE_OBJECT FileObject,
    _In_ PACCESS_STATE AccessState,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ KPROCESSOR_MODE PreviousMode,
    _In_ ULONG Disposition)
{
    IO_STATUS_BLOCK IoStatus = {{0}};
    PUNICODE_STRING Name = &FileObject->FileName;
    UNICODE_STRING Remaining;
    PSECURITY_DESCRIPTOR SecurityDescriptor, CachedSecurityDescriptor;
    PNP_PREFIX Enclosing, Prefix;
    PNP_FCB Node;
    PWCHAR NameBuffer;

    PAGED_CODE();

    if (Name->Length <= sizeof(WCHAR) || Name->Buffer[0] != OBJ_NAME_PATH_SEPARATOR)
    {
        IoStatus.Status = STATUS_INVALID_PARAMETER;
        return IoStatus;
    }

    /* An existing name is opened, but only when it is a prefix */
    Node = NpFindPrefix(Name, 1, &Remaining);
    if (!Remaining.Length)
    {
        if (Disposition == FILE_CREATE)
            IoStatus.Status = STATUS_OBJECT_NAME_COLLISION;
        else if (Node->NodeType != NPFS_NTC_PREFIX)
            IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        else
            return NpOpenNamedPipePrefix(FileObject, AccessState, DesiredAccess, PreviousMode);

        return IoStatus;
    }

    Enclosing = (Node->NodeType == NPFS_NTC_PREFIX) ? (PNP_PREFIX)Node : NULL;
    if (!SeSinglePrivilegeCheck(SeExports->SeTcbPrivilege, PreviousMode))
    {
        if (!Enclosing)
        {
            IoStatus.Status = STATUS_PRIVILEGE_NOT_HELD;
            return IoStatus;
        }

        IoStatus.Status = NpAccessCheckPrefix(Enclosing, AccessState, FILE_ADD_SUBDIRECTORY, FALSE, PreviousMode);
        if (!NT_SUCCESS(IoStatus.Status))
            return IoStatus;
    }

    SeLockSubjectContext(&AccessState->SubjectSecurityContext);
    IoStatus.Status = SeAssignSecurity(NULL,
                                       AccessState->SecurityDescriptor,
                                       &SecurityDescriptor,
                                       TRUE,
                                       &AccessState->SubjectSecurityContext,
                                       IoGetFileObjectGenericMapping(),
                                       PagedPool);
    SeUnlockSubjectContext(&AccessState->SubjectSecurityContext);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus;

    IoStatus.Status = ObLogSecurityDescriptor(SecurityDescriptor, &CachedSecurityDescriptor, 1);
    ExFreePoolWithTag(SecurityDescriptor, 0);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus;

    Prefix = ExAllocatePoolZero(PagedPool, sizeof(*Prefix), NPFS_PREFIX_TAG);
    NameBuffer = ExAllocatePoolWithTag(PagedPool, Name->Length + sizeof(UNICODE_NULL), NPFS_NAME_BLOCK_TAG);
    if (!Prefix || !NameBuffer)
    {
        if (Prefix)
            ExFreePoolWithTag(Prefix, NPFS_PREFIX_TAG);
        if (NameBuffer)
            ExFreePoolWithTag(NameBuffer, NPFS_NAME_BLOCK_TAG);
        ObDereferenceSecurityDescriptor(CachedSecurityDescriptor, 1);
        IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        return IoStatus;
    }

    RtlCopyMemory(NameBuffer, Name->Buffer, Name->Length);
    NameBuffer[Name->Length / sizeof(WCHAR)] = UNICODE_NULL;

    Prefix->NodeType = NPFS_NTC_PREFIX;
    Prefix->CurrentInstances = 1;
    Prefix->SecurityDescriptor = CachedSecurityDescriptor;
    InitializeListHead(&Prefix->DcbEntry);
    Prefix->FullName.Buffer = NameBuffer;
    Prefix->FullName.Length = Name->Length;
    Prefix->FullName.MaximumLength = Name->Length + sizeof(UNICODE_NULL);
    Prefix->ShortName.Buffer = NameBuffer + 1;
    Prefix->ShortName.Length = Name->Length - sizeof(WCHAR);
    Prefix->ShortName.MaximumLength = Prefix->ShortName.Length;

    if (!RtlInsertUnicodePrefix(&NpVcb->PrefixTable, &Prefix->FullName, &Prefix->PrefixTableEntry))
    {
        NpBugCheck(0, 0, 0);
    }

    NpSetFileObject(FileObject, Prefix, NULL, FALSE);
    IoStatus.Information = FILE_CREATED;
    return IoStatus;
}

/**
 * @brief
 * Drops a handle reference of a prefix; the last one removes it from the namespace.
 */
VOID
NTAPI
NpDereferencePrefix(
    _In_ PNP_PREFIX Prefix)
{
    PAGED_CODE();

    if (--Prefix->CurrentInstances)
        return;

    RtlRemoveUnicodePrefix(&NpVcb->PrefixTable, &Prefix->PrefixTableEntry);
    ObDereferenceSecurityDescriptor(Prefix->SecurityDescriptor, 1);
    ExFreePoolWithTag(Prefix->FullName.Buffer, NPFS_NAME_BLOCK_TAG);
    ExFreePoolWithTag(Prefix, NPFS_PREFIX_TAG);
}

/**
 * @brief
 * A pipe created under a prefix needs FILE_ADD_FILE on it.
 *
 * @param[in] Node
 * The longest registered name the new pipe name starts with.
 */
NTSTATUS
NTAPI
NpCheckAddToPrefix(
    _In_ PNP_FCB Node,
    _In_ PACCESS_STATE AccessState,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    PAGED_CODE();

    if (Node->NodeType != NPFS_NTC_PREFIX)
        return STATUS_SUCCESS;

    return NpAccessCheckPrefix((PNP_PREFIX)Node, AccessState, FILE_ADD_FILE, FALSE, PreviousMode);
}

/* EOF */
