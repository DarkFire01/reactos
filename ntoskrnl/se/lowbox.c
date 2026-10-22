/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     LowBox (AppContainer) tokens
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Number entries per session and package, handle entries per logon session and package */
static KGUARDED_MUTEX SepLowBoxLock;
static LIST_ENTRY SepLowBoxNumberList;
static LIST_ENTRY SepLowBoxHandlesList;

/* Leaf names a LowBox named object handle may carry besides the package itself */
static const UNICODE_STRING SepLowBoxObjectNames[] =
{
    RTL_CONSTANT_STRING(L"Global"),
    RTL_CONSTANT_STRING(L"Local"),
    RTL_CONSTANT_STRING(L"Session"),
    RTL_CONSTANT_STRING(L"RPC Control")
};

static const UNICODE_STRING SepNamedPipePrefix = RTL_CONSTANT_STRING(L"\\Device\\NamedPipe\\");

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Sets up the LowBox number and handle tables.
 */
CODE_SEG("INIT")
VOID
SepInitializeLowBox(VOID)
{
    KeInitializeGuardedMutex(&SepLowBoxLock);
    InitializeListHead(&SepLowBoxNumberList);
    InitializeListHead(&SepLowBoxHandlesList);
}

static
PSID
SepCopySidToPool(
    _In_ PSID Sid)
{
    ULONG Length = RtlLengthSid(Sid);
    PSID Copy;

    Copy = ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_LOWBOX);
    if (Copy)
        RtlCopySid(Length, Copy, Sid);

    return Copy;
}

static
BOOLEAN
SepTokenIsNotLow(
    _In_ PTOKEN Token)
{
    PSID_AND_ATTRIBUTES Integrity = SepGetIntegrityLevelFromToken(Token);
    PSID Sid;

    if (Integrity == NULL)
        return TRUE;

    Sid = Integrity->Sid;
    return *RtlSubAuthoritySid(Sid, *RtlSubAuthorityCountSid(Sid) - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
}

/*
 * Only a caller above low integrity may create a LowBox token, or a LowBox
 * caller creating a child of its own package.
 */
static
NTSTATUS
SepCheckCreateLowBox(
    _In_ PSID PackageSid)
{
    SECURITY_SUBJECT_CONTEXT Subject;
    PTOKEN Token;
    BOOLEAN Allowed = FALSE;

    SeCaptureSubjectContext(&Subject);

    if (Subject.ClientToken == NULL || Subject.ImpersonationLevel >= SecurityImpersonation)
    {
        Token = Subject.ClientToken ? Subject.ClientToken : Subject.PrimaryToken;

        SepAcquireTokenLockShared(Token);
        Allowed = SepTokenIsNotLow(Token);
        if (!Allowed && (Token->TokenFlags & TOKEN_LOWBOX) && Token->Package)
            Allowed = RtlIsParentOfChildAppContainer(Token->Package, PackageSid);
        SepReleaseTokenLock(Token);
    }

    SeReleaseSubjectContext(&Subject);
    return Allowed ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}

/*
 * A child AppContainer may only hold capabilities its parent token holds with the
 * same attributes, or capabilities derived from its own package.
 */
static
BOOLEAN
SepCapabilitiesAreSubset(
    _In_ PTOKEN Token,
    _In_ ULONG CapabilityCount,
    _In_reads_(CapabilityCount) PSID_AND_ATTRIBUTES Capabilities)
{
    ULONG Index, Held;
    BOOLEAN Found;
    PSID Sid;

    for (Index = 0; Index < CapabilityCount; Index++)
    {
        Found = FALSE;
        for (Held = 0; Held < Token->CapabilityCount && !Found; Held++)
        {
            Found = RtlEqualSid(Token->Capabilities[Held].Sid, Capabilities[Index].Sid) &&
                    Token->Capabilities[Held].Attributes == Capabilities[Index].Attributes;
        }

        if (Found)
            continue;

        /* S-1-15-3-<the seven package sub-authorities> is the package's own capability */
        Sid = Capabilities[Index].Sid;
        if (*RtlSubAuthorityCountSid(Sid) != SECURITY_APP_PACKAGE_RID_COUNT ||
            *RtlSubAuthoritySid(Sid, 0) != SECURITY_CAPABILITY_BASE_RID ||
            Token->Package == NULL ||
            *RtlSubAuthorityCountSid(Token->Package) < SECURITY_APP_PACKAGE_RID_COUNT ||
            !RtlEqualMemory(RtlSubAuthoritySid(Sid, 1),
                            RtlSubAuthoritySid(Token->Package, 1),
                            (SECURITY_APP_PACKAGE_RID_COUNT - 1) * sizeof(ULONG)))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static
VOID
SepDereferenceLowBoxNumberEntry(
    _In_ PSEP_LOWBOX_NUMBER_ENTRY Entry)
{
    KeAcquireGuardedMutex(&SepLowBoxLock);
    if (--Entry->ReferenceCount == 0)
    {
        RemoveEntryList(&Entry->ListEntry);
        KeReleaseGuardedMutex(&SepLowBoxLock);
        ExFreePoolWithTag(Entry, TAG_SE_LOWBOX);
        return;
    }
    KeReleaseGuardedMutex(&SepLowBoxLock);
}

/* Tokens of one package in one session share the lowest number free in that session */
static
NTSTATUS
SepReferenceLowBoxNumberEntry(
    _In_ ULONG SessionId,
    _In_ PSID PackageSid,
    _Out_ PSEP_LOWBOX_NUMBER_ENTRY *Result)
{
    PSEP_LOWBOX_NUMBER_ENTRY Entry, New;
    PLIST_ENTRY Link;
    ULONG Number;
    BOOLEAN Taken;
    ULONG SidLength = RtlLengthSid(PackageSid);

    New = ExAllocatePoolWithTag(PagedPool, sizeof(*New) + SidLength, TAG_SE_LOWBOX);
    if (New == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireGuardedMutex(&SepLowBoxLock);

    for (Link = SepLowBoxNumberList.Flink; Link != &SepLowBoxNumberList; Link = Link->Flink)
    {
        Entry = CONTAINING_RECORD(Link, SEP_LOWBOX_NUMBER_ENTRY, ListEntry);
        if (Entry->SessionId == SessionId && RtlEqualSid(Entry->PackageSid, PackageSid))
        {
            Entry->ReferenceCount++;
            KeReleaseGuardedMutex(&SepLowBoxLock);
            ExFreePoolWithTag(New, TAG_SE_LOWBOX);
            *Result = Entry;
            return STATUS_SUCCESS;
        }
    }

    for (Number = 1; ; Number++)
    {
        Taken = FALSE;
        for (Link = SepLowBoxNumberList.Flink; Link != &SepLowBoxNumberList && !Taken; Link = Link->Flink)
        {
            Entry = CONTAINING_RECORD(Link, SEP_LOWBOX_NUMBER_ENTRY, ListEntry);
            Taken = (Entry->SessionId == SessionId && Entry->LowboxNumber == Number);
        }

        if (!Taken)
            break;
    }

    New->ReferenceCount = 1;
    New->SessionId = SessionId;
    New->LowboxNumber = Number;
    New->PackageSid = (PSID)(New + 1);
    RtlCopySid(SidLength, New->PackageSid, PackageSid);
    InsertTailList(&SepLowBoxNumberList, &New->ListEntry);

    KeReleaseGuardedMutex(&SepLowBoxLock);

    *Result = New;
    return STATUS_SUCCESS;
}

static
VOID
SepCloseLowBoxHandles(
    _In_ ULONG HandleCount,
    _In_reads_(HandleCount) PHANDLE Handles)
{
    ULONG Index;

    for (Index = 0; Index < HandleCount; Index++)
    {
        if (Handles[Index])
            ObCloseHandle(Handles[Index], KernelMode);
    }
}

static
VOID
SepDereferenceLowBoxHandlesEntry(
    _In_ PSEP_LOWBOX_HANDLES_ENTRY Entry)
{
    KeAcquireGuardedMutex(&SepLowBoxLock);
    if (--Entry->ReferenceCount != 0)
    {
        KeReleaseGuardedMutex(&SepLowBoxLock);
        return;
    }

    RemoveEntryList(&Entry->ListEntry);
    KeReleaseGuardedMutex(&SepLowBoxLock);

    if (Entry->Handles)
    {
        SepCloseLowBoxHandles(Entry->HandleCount, Entry->Handles);
        ExFreePoolWithTag(Entry->Handles, TAG_SE_LOWBOX);
    }

    ExFreePoolWithTag(Entry, TAG_SE_LOWBOX);
}

/*
 * Tokens of one package in one logon session share one handle entry. The first
 * token that brings handles hands them over; later ones close theirs.
 */
static
NTSTATUS
SepReferenceLowBoxHandlesEntry(
    _In_ PLUID AuthenticationId,
    _In_ PSID PackageSid,
    _In_ ULONG HandleCount,
    _Inout_ PHANDLE *Handles,
    _Out_ PSEP_LOWBOX_HANDLES_ENTRY *Result)
{
    PSEP_LOWBOX_HANDLES_ENTRY Entry, New;
    PLIST_ENTRY Link;
    ULONG SidLength = RtlLengthSid(PackageSid);

    New = ExAllocatePoolWithTag(PagedPool, sizeof(*New) + SidLength, TAG_SE_LOWBOX);
    if (New == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireGuardedMutex(&SepLowBoxLock);

    for (Link = SepLowBoxHandlesList.Flink; Link != &SepLowBoxHandlesList; Link = Link->Flink)
    {
        Entry = CONTAINING_RECORD(Link, SEP_LOWBOX_HANDLES_ENTRY, ListEntry);
        if (RtlEqualLuid(&Entry->AuthenticationId, AuthenticationId) &&
            RtlEqualSid(Entry->PackageSid, PackageSid))
        {
            Entry->ReferenceCount++;
            if (Entry->HandleCount == 0 && HandleCount != 0)
            {
                Entry->HandleCount = HandleCount;
                Entry->Handles = *Handles;
                *Handles = NULL;
            }

            KeReleaseGuardedMutex(&SepLowBoxLock);
            ExFreePoolWithTag(New, TAG_SE_LOWBOX);
            *Result = Entry;
            return STATUS_SUCCESS;
        }
    }

    New->ReferenceCount = 1;
    New->AuthenticationId = *AuthenticationId;
    New->PackageSid = (PSID)(New + 1);
    RtlCopySid(SidLength, New->PackageSid, PackageSid);
    New->HandleCount = HandleCount;
    New->Handles = HandleCount ? *Handles : NULL;
    if (HandleCount)
        *Handles = NULL;
    InsertTailList(&SepLowBoxHandlesList, &New->ListEntry);

    KeReleaseGuardedMutex(&SepLowBoxLock);

    *Result = New;
    return STATUS_SUCCESS;
}

/*
 * Each handle must be a directory, a symbolic link or a named pipe file of the
 * package's named object tree: under \Device\NamedPipe\, or under \Sessions\<n>\
 * with the package (or child) name or one of the well known leaf names.
 */
static
NTSTATUS
SepValidateLowBoxHandles(
    _In_ ULONG SessionId,
    _In_ PSID PackageSid,
    _In_ ULONG HandleCount,
    _In_reads_(HandleCount) PHANDLE Handles)
{
    APPCONTAINER_SID_TYPE SidType;
    UNICODE_STRING PackageName = {0};
    UNICODE_STRING SessionPrefix, Leaf;
    WCHAR SessionBuffer[32];
    WCHAR ChildBuffer[64];
    POBJECT_NAME_INFORMATION NameInfo = NULL;
    POBJECT_TYPE Type;
    PVOID Object;
    ULONG Index, Name, Length;
    BOOLEAN FreePackageName = FALSE;
    BOOLEAN Matches;
    NTSTATUS Status;

    Status = RtlGetAppContainerSidType(PackageSid, &SidType);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SidType == ParentAppContainerSidType)
    {
        Status = RtlConvertSidToUnicodeString(&PackageName, PackageSid, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
        FreePackageName = TRUE;
    }
    else
    {
        /* A child's directory is named after its last four sub-authorities */
        Status = RtlStringCbPrintfW(ChildBuffer, sizeof(ChildBuffer), L"%u-%u-%u-%u",
                                    *RtlSubAuthoritySid(PackageSid, 8),
                                    *RtlSubAuthoritySid(PackageSid, 9),
                                    *RtlSubAuthoritySid(PackageSid, 10),
                                    *RtlSubAuthoritySid(PackageSid, 11));
        if (!NT_SUCCESS(Status))
            return Status;
        RtlInitUnicodeString(&PackageName, ChildBuffer);
    }

    Status = RtlStringCbPrintfW(SessionBuffer, sizeof(SessionBuffer), L"\\Sessions\\%lu", SessionId);
    if (!NT_SUCCESS(Status))
        goto Quit;
    RtlInitUnicodeString(&SessionPrefix, SessionBuffer);

    for (Index = 0; Index < HandleCount && NT_SUCCESS(Status); Index++)
    {
        Status = ObReferenceObjectByHandle(Handles[Index], 0, NULL, KernelMode, &Object, NULL);
        if (!NT_SUCCESS(Status))
            break;

        Type = OBJECT_TO_OBJECT_HEADER(Object)->Type;
        if (Type != ObpDirectoryObjectType &&
            Type != ObpSymbolicLinkObjectType &&
            (Type != IoFileObjectType ||
             ((PFILE_OBJECT)Object)->DeviceObject->DeviceType != FILE_DEVICE_NAMED_PIPE))
        {
            ObDereferenceObject(Object);
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        /* Ask for the size first, then the name */
        Length = 0;
        ObQueryNameString(Object, NULL, 0, &Length);
        NameInfo = Length ? ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_LOWBOX) : NULL;
        Status = NameInfo ? ObQueryNameString(Object, NameInfo, Length, &Length) : STATUS_INVALID_PARAMETER;
        ObDereferenceObject(Object);

        if (!NT_SUCCESS(Status) || NameInfo->Name.Length == 0)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        Matches = RtlPrefixUnicodeString(&SepNamedPipePrefix, &NameInfo->Name, TRUE);
        if (!Matches && RtlPrefixUnicodeString(&SessionPrefix, &NameInfo->Name, TRUE))
        {
            /* The leaf follows the last separator */
            Leaf = NameInfo->Name;
            while (Leaf.Length && Leaf.Buffer[Leaf.Length / sizeof(WCHAR) - 1] != OBJ_NAME_PATH_SEPARATOR)
                Leaf.Length -= sizeof(WCHAR);
            Leaf.Buffer += Leaf.Length / sizeof(WCHAR);
            Leaf.Length = NameInfo->Name.Length - Leaf.Length;
            Leaf.MaximumLength = Leaf.Length;

            Matches = RtlEqualUnicodeString(&Leaf, &PackageName, TRUE);
            for (Name = 0; !Matches && Name < RTL_NUMBER_OF(SepLowBoxObjectNames); Name++)
                Matches = RtlEqualUnicodeString(&Leaf, &SepLowBoxObjectNames[Name], TRUE);
        }

        ExFreePoolWithTag(NameInfo, TAG_SE_LOWBOX);
        NameInfo = NULL;

        if (!Matches)
            Status = STATUS_INVALID_PARAMETER;
    }

Quit:
    if (FreePackageName)
        RtlFreeUnicodeString(&PackageName);

    return Status;
}

/* Builds a copy of an ACL with one allow ACE appended */
static
NTSTATUS
SepAppendAllowedAce(
    _In_opt_ PACL Acl,
    _In_ ACCESS_MASK Mask,
    _In_ PSID Sid,
    _Out_ PACL *NewAcl)
{
    ULONG Length;
    PACL Result;
    NTSTATUS Status;

    Length = (Acl ? Acl->AclSize : sizeof(ACL)) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG) + RtlLengthSid(Sid);
    Result = ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_LOWBOX);
    if (Result == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Acl)
    {
        RtlCopyMemory(Result, Acl, Acl->AclSize);
        Result->AclSize = (USHORT)Length;
    }
    else
    {
        RtlCreateAcl(Result, Length, ACL_REVISION);
    }

    /* Appending to a revision 4 ACL keeps its revision */
    Status = RtlAddAccessAllowedAce(Result, max(Result->AclRevision, ACL_REVISION), Mask, Sid);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Result, TAG_SE_LOWBOX);
        return Status;
    }

    *NewAcl = Result;
    return STATUS_SUCCESS;
}

/* The package may do anything with its own token, administrators may query it */
static
NTSTATUS
SepAppendLowBoxTokenObjectAces(
    _In_ PTOKEN Token,
    _In_ PSID PackageSid)
{
    PSECURITY_DESCRIPTOR Descriptor;
    SECURITY_DESCRIPTOR NewDescriptor;
    BOOLEAN MemoryAllocated, Present, Defaulted;
    PACL Dacl = NULL, WithPackage = NULL, WithAdmins = NULL;
    NTSTATUS Status;

    Status = ObGetObjectSecurity(Token, &Descriptor, &MemoryAllocated);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Descriptor)
        RtlGetDaclSecurityDescriptor(Descriptor, &Present, &Dacl, &Defaulted);

    Status = SepAppendAllowedAce(Dacl, TOKEN_ALL_ACCESS, PackageSid, &WithPackage);
    if (NT_SUCCESS(Status))
        Status = SepAppendAllowedAce(WithPackage, TOKEN_QUERY, SeAliasAdminsSid, &WithAdmins);

    if (Descriptor)
        ObReleaseObjectSecurity(Descriptor, MemoryAllocated);

    if (NT_SUCCESS(Status))
    {
        RtlCreateSecurityDescriptor(&NewDescriptor, SECURITY_DESCRIPTOR_REVISION);
        RtlSetDaclSecurityDescriptor(&NewDescriptor, TRUE, WithAdmins, FALSE);
        Status = ObSetSecurityObjectByPointer(Token, DACL_SECURITY_INFORMATION, &NewDescriptor);
    }

    if (WithPackage)
        ExFreePoolWithTag(WithPackage, TAG_SE_LOWBOX);
    if (WithAdmins)
        ExFreePoolWithTag(WithAdmins, TAG_SE_LOWBOX);

    return Status;
}

/*
 * Turns a fresh duplicate into a LowBox token: low integrity with no write up,
 * only the change notify and working set privileges, the capabilities, the
 * package, the shared number and handle entries, and the package in the
 * default DACL.
 */
static
NTSTATUS
SepMakeLowBoxToken(
    _Inout_ PTOKEN Token,
    _In_ PSID PackageSid,
    _In_ ULONG CapabilityCount,
    _In_reads_opt_(CapabilityCount) PSID_AND_ATTRIBUTES Capabilities,
    _In_ ULONG HandleCount,
    _Inout_ PHANDLE *Handles)
{
    PSID_AND_ATTRIBUTES Integrity, NewCapabilities = NULL;
    PSID Sid, Package, End;
    PACL Dacl;
    ULONG Index, Length, Remaining;
    NTSTATUS Status;

    Token->MandatoryPolicy.Policy = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;

    Integrity = SepGetIntegrityLevelFromToken(Token);
    if (Integrity == NULL)
        return STATUS_INVALID_PARAMETER;

    Sid = Integrity->Sid;
    *RtlSubAuthoritySid(Sid, *RtlSubAuthorityCountSid(Sid) - 1) = SECURITY_MANDATORY_LOW_RID;

    for (Index = 0; Index < Token->PrivilegeCount; Index++)
    {
        if (!RtlEqualLuid(&Token->Privileges[Index].Luid, &SeChangeNotifyPrivilege) &&
            !RtlEqualLuid(&Token->Privileges[Index].Luid, &SeIncreaseWorkingSetPrivilege))
        {
            SepRemovePrivilegeToken(Token, Index);
            Index--;
        }
    }
    SepUpdatePrivilegeFlagsToken(Token);

    Token->TokenFlags = (Token->TokenFlags & ~TOKEN_NOT_LOW) | TOKEN_LOWBOX;

    /* A token that already is a LowBox may only become a child of its package */
    if (Token->Package && !RtlIsParentOfChildAppContainer(Token->Package, PackageSid))
        return STATUS_ACCESS_DENIED;

    if (CapabilityCount)
    {
        Length = CapabilityCount * sizeof(SID_AND_ATTRIBUTES) +
                 RtlLengthSidAndAttributes(CapabilityCount, Capabilities);
        NewCapabilities = ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_LOWBOX);
        if (NewCapabilities == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = RtlCopySidAndAttributesArray(CapabilityCount,
                                              Capabilities,
                                              Length - CapabilityCount * sizeof(SID_AND_ATTRIBUTES),
                                              NewCapabilities,
                                              &NewCapabilities[CapabilityCount],
                                              &End,
                                              &Remaining);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(NewCapabilities, TAG_SE_LOWBOX);
            return Status;
        }
    }

    if (Token->Capabilities)
        ExFreePoolWithTag(Token->Capabilities, TAG_SE_LOWBOX);
    Token->Capabilities = NewCapabilities;
    Token->CapabilityCount = CapabilityCount;

    /* The duplicate shared the entries of the token it came from */
    if (Token->LowboxNumberEntry)
    {
        SepDereferenceLowBoxNumberEntry(Token->LowboxNumberEntry);
        Token->LowboxNumberEntry = NULL;
    }
    if (Token->LowboxHandlesEntry)
    {
        SepDereferenceLowBoxHandlesEntry(Token->LowboxHandlesEntry);
        Token->LowboxHandlesEntry = NULL;
    }

    Status = SepReferenceLowBoxNumberEntry(Token->SessionId, PackageSid, &Token->LowboxNumberEntry);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SepReferenceLowBoxHandlesEntry(&Token->AuthenticationId,
                                            PackageSid,
                                            HandleCount,
                                            Handles,
                                            &Token->LowboxHandlesEntry);
    if (!NT_SUCCESS(Status))
        return Status;

    Package = SepCopySidToPool(PackageSid);
    if (Package == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Token->Package)
        ExFreePoolWithTag(Token->Package, TAG_SE_LOWBOX);
    Token->Package = Package;

    Status = SepAppendAllowedAce(Token->DefaultDacl, GENERIC_ALL, PackageSid, &Dacl);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SepAssignDefaultDaclToken(Token, Dacl);
    ExFreePoolWithTag(Dacl, TAG_SE_LOWBOX);
    return Status;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Gives a new token the AppContainer data of the token it is copied from.
 * Package and capabilities are copied, the number and handle entries shared.
 */
NTSTATUS
SepCopyTokenAppContainerData(
    _In_ PTOKEN Source,
    _Inout_ PTOKEN Copy)
{
    ULONG Length, Remaining;
    PSID End;
    NTSTATUS Status;

    Copy->MandatoryPolicy = Source->MandatoryPolicy;

    Status = SepDuplicateSecurityAttributes(Source->pSecurityAttributes, FALSE, &Copy->pSecurityAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Source->Package)
    {
        Copy->Package = SepCopySidToPool(Source->Package);
        if (Copy->Package == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Source->CapabilityCount)
    {
        Length = Source->CapabilityCount * sizeof(SID_AND_ATTRIBUTES) +
                 RtlLengthSidAndAttributes(Source->CapabilityCount, Source->Capabilities);
        Copy->Capabilities = ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_LOWBOX);
        if (Copy->Capabilities == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = RtlCopySidAndAttributesArray(Source->CapabilityCount,
                                              Source->Capabilities,
                                              Length - Source->CapabilityCount * sizeof(SID_AND_ATTRIBUTES),
                                              Copy->Capabilities,
                                              &Copy->Capabilities[Source->CapabilityCount],
                                              &End,
                                              &Remaining);
        if (!NT_SUCCESS(Status))
            return Status;

        Copy->CapabilityCount = Source->CapabilityCount;
    }

    KeAcquireGuardedMutex(&SepLowBoxLock);
    Copy->LowboxNumberEntry = Source->LowboxNumberEntry;
    if (Copy->LowboxNumberEntry)
        Copy->LowboxNumberEntry->ReferenceCount++;
    Copy->LowboxHandlesEntry = Source->LowboxHandlesEntry;
    if (Copy->LowboxHandlesEntry)
        Copy->LowboxHandlesEntry->ReferenceCount++;
    KeReleaseGuardedMutex(&SepLowBoxLock);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Releases the AppContainer data and security attributes of a token being deleted.
 */
VOID
SepFreeTokenAppContainerData(
    _Inout_ PTOKEN Token)
{
    if (Token->LowboxNumberEntry)
    {
        SepDereferenceLowBoxNumberEntry(Token->LowboxNumberEntry);
        Token->LowboxNumberEntry = NULL;
    }

    if (Token->LowboxHandlesEntry)
    {
        SepDereferenceLowBoxHandlesEntry(Token->LowboxHandlesEntry);
        Token->LowboxHandlesEntry = NULL;
    }

    if (Token->Capabilities)
    {
        ExFreePoolWithTag(Token->Capabilities, TAG_SE_LOWBOX);
        Token->Capabilities = NULL;
        Token->CapabilityCount = 0;
    }

    if (Token->Package)
    {
        ExFreePoolWithTag(Token->Package, TAG_SE_LOWBOX);
        Token->Package = NULL;
    }

    SepFreeSecurityAttributes(Token->pSecurityAttributes);
    Token->pSecurityAttributes = NULL;
}

/* SYSTEM CALLS ***************************************************************/

/**
 * @brief
 * Creates a LowBox (AppContainer) primary token from an existing token.
 *
 * @param[out] TokenHandle
 * Receives the new token handle.
 *
 * @param[in] ExistingTokenHandle
 * Token to derive from, opened with TOKEN_DUPLICATE. A primary token or an
 * impersonation token at SecurityImpersonation or above.
 *
 * @param[in] DesiredAccess
 * Access for the new handle. Zero takes the access of the existing handle.
 *
 * @param[in] ObjectAttributes
 * Attributes for the new token object.
 *
 * @param[in] PackageSid
 * Parent (S-1-15-2 and 7 sub-authorities) or child (11) AppContainer SID.
 *
 * @param[in] CapabilityCount
 * Number of capabilities.
 *
 * @param[in] Capabilities
 * Capability (S-1-15-3) SIDs, each at most once.
 *
 * @param[in] HandleCount
 * Number of named object handles.
 *
 * @param[in] Handles
 * Handles of the package's named object directory, its links and its named pipe
 * directory, kept open as long as a token of the package lives in the logon session.
 *
 * @return
 * STATUS_SUCCESS or a failure code.
 */
NTSTATUS
NTAPI
NtCreateLowBoxToken(
    _Out_ PHANDLE TokenHandle,
    _In_ HANDLE ExistingTokenHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ PSID PackageSid,
    _In_ ULONG CapabilityCount,
    _In_reads_opt_(CapabilityCount) PSID_AND_ATTRIBUTES Capabilities,
    _In_ ULONG HandleCount,
    _In_reads_opt_(HandleCount) HANDLE *Handles)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    OBJECT_HANDLE_INFORMATION HandleInformation;
    APPCONTAINER_SID_TYPE SidType;
    PSID_AND_ATTRIBUTES CapturedCapabilities = NULL;
    ULONG CapturedCapabilitiesLength = 0;
    PHANDLE KernelHandles = NULL;
    PSID CapturedPackage = NULL;
    PTOKEN Existing, Token = NULL;
    HANDLE NewHandle = NULL;
    ULONG Index, Other;
    NTSTATUS Status;

    PAGED_CODE();

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteHandle(TokenHandle);
            if (HandleCount)
                ProbeForRead(Handles, HandleCount * sizeof(HANDLE), sizeof(HANDLE));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    if (PackageSid == NULL)
        return STATUS_INVALID_PARAMETER;

    if ((HandleCount != 0) != (Handles != NULL))
        return STATUS_INVALID_PARAMETER_MIX;

    Status = ObReferenceObjectByHandle(ExistingTokenHandle,
                                       TOKEN_DUPLICATE,
                                       SeTokenObjectType,
                                       PreviousMode,
                                       (PVOID*)&Existing,
                                       &HandleInformation);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SepCaptureSid(PackageSid, PreviousMode, PagedPool, TRUE, &CapturedPackage);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = SepCheckCreateLowBox(CapturedPackage);
    if (!NT_SUCCESS(Status))
        goto Quit;

    if (Existing->TokenType != TokenPrimary && Existing->ImpersonationLevel < SecurityImpersonation)
    {
        Status = STATUS_BAD_IMPERSONATION_LEVEL;
        goto Quit;
    }

    if (DesiredAccess == 0)
        DesiredAccess = HandleInformation.GrantedAccess;

    if (CapabilityCount)
    {
        if (Capabilities == NULL)
        {
            Status = STATUS_INVALID_PARAMETER_MIX;
            goto Quit;
        }

        Status = SeCaptureSidAndAttributesArray(Capabilities,
                                                CapabilityCount,
                                                PreviousMode,
                                                NULL,
                                                0,
                                                PagedPool,
                                                TRUE,
                                                &CapturedCapabilities,
                                                &CapturedCapabilitiesLength);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    /* Keep the named object handles open with kernel handles of our own */
    if (HandleCount)
    {
        KernelHandles = ExAllocatePoolZero(PagedPool, HandleCount * sizeof(HANDLE), TAG_SE_LOWBOX);
        if (KernelHandles == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Quit;
        }

        for (Index = 0; Index < HandleCount; Index++)
        {
            HANDLE Source;

            _SEH2_TRY
            {
                Source = Handles[Index];
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
                _SEH2_YIELD(goto Quit);
            }
            _SEH2_END;

            Status = ZwDuplicateObject(NtCurrentProcess(),
                                       Source,
                                       NtCurrentProcess(),
                                       &KernelHandles[Index],
                                       0,
                                       OBJ_KERNEL_HANDLE,
                                       DUPLICATE_SAME_ACCESS);
            if (!NT_SUCCESS(Status))
                goto Quit;
        }
    }

    if (!RtlIsPackageSid(CapturedPackage))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Quit;
    }

    if (!NT_SUCCESS(RtlGetAppContainerSidType(CapturedPackage, &SidType)))
    {
        Status = STATUS_INVALID_PACKAGE_SID_LENGTH;
        goto Quit;
    }

    for (Index = 0; Index < CapabilityCount; Index++)
    {
        if (!RtlIsCapabilitySid(CapturedCapabilities[Index].Sid) ||
            (CapturedCapabilities[Index].Attributes & ~SE_GROUP_VALID_ATTRIBUTES))
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Quit;
        }

        for (Other = 0; Other < Index; Other++)
        {
            if (RtlEqualSid(CapturedCapabilities[Other].Sid, CapturedCapabilities[Index].Sid))
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Quit;
            }
        }
    }

    if (SidType == ChildAppContainerSidType)
    {
        SepAcquireTokenLockShared(Existing);
        if (!SepCapabilitiesAreSubset(Existing, CapabilityCount, CapturedCapabilities))
            Status = STATUS_ACCESS_DENIED;
        SepReleaseTokenLock(Existing);

        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    if (HandleCount)
    {
        Status = SepValidateLowBoxHandles(Existing->SessionId, CapturedPackage, HandleCount, KernelHandles);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    Status = SepDuplicateToken(Existing,
                               ObjectAttributes,
                               FALSE,
                               TokenPrimary,
                               SecurityAnonymous,
                               PreviousMode,
                               &Token);
    if (!NT_SUCCESS(Status))
        goto Quit;

    SepAcquireTokenLockExclusive(Token);
    Status = SepMakeLowBoxToken(Token,
                                CapturedPackage,
                                CapabilityCount,
                                CapturedCapabilities,
                                HandleCount,
                                &KernelHandles);
    if (NT_SUCCESS(Status))
        ExAllocateLocallyUniqueId(&Token->ModifiedId);
    SepReleaseTokenLock(Token);

    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Token);
        goto Quit;
    }

    /* ObInsertObject consumes the reference even when it fails */
    ObReferenceObject(Token);
    Status = ObInsertObject(Token, NULL, DesiredAccess, 0, NULL, &NewHandle);
    if (NT_SUCCESS(Status))
    {
        Status = SepAppendLowBoxTokenObjectAces(Token, CapturedPackage);
        if (!NT_SUCCESS(Status))
        {
            ObCloseHandle(NewHandle, PreviousMode);
            NewHandle = NULL;
        }
    }
    ObDereferenceObject(Token);

    if (NT_SUCCESS(Status))
    {
        _SEH2_TRY
        {
            *TokenHandle = NewHandle;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
            ObCloseHandle(NewHandle, PreviousMode);
        }
        _SEH2_END;
    }

Quit:
    if (KernelHandles)
    {
        SepCloseLowBoxHandles(HandleCount, KernelHandles);
        ExFreePoolWithTag(KernelHandles, TAG_SE_LOWBOX);
    }

    if (CapturedCapabilities)
        SeReleaseSidAndAttributesArray(CapturedCapabilities, PreviousMode, TRUE);

    if (CapturedPackage)
        SepReleaseSid(CapturedPackage, PreviousMode, TRUE);

    ObDereferenceObject(Existing);
    return Status;
}

/* EOF */
