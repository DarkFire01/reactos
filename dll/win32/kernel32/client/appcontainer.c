/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AppContainer tokens, object directories and environment for new processes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <k32.h>
#include <ndk/sefuncs.h>
#include <appmodel.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Handles a LowBox token keeps: the package directory, its RPC Control and Global directories,
   the Local and Session links, the pipe directory and, for a child container, the parent's directory */
#define LOWBOX_HANDLE_COUNT 7

/* Rights a container directory is opened and created with */
#define LOWBOX_DIRECTORY_ACCESS (DIRECTORY_QUERY | DIRECTORY_TRAVERSE | DIRECTORY_CREATE_OBJECT | DIRECTORY_CREATE_SUBDIRECTORY)

/* Inherit-only ACE flags for objects created inside a container directory */
#define LOWBOX_INHERIT_ACE (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE)

/* Extra ACEs a container directory gets on top of the base named object directory's DACL */
#define LOWBOX_SD_ADMINISTRATORS 0x1
#define LOWBOX_SD_RESTRICTED     0x2
#define LOWBOX_SD_EVERYONE       0x4
#define LOWBOX_SD_SHARED         (LOWBOX_SD_ADMINISTRATORS | LOWBOX_SD_RESTRICTED | LOWBOX_SD_EVERYONE)

/* Where CreateAppContainerProfile and package registration record a container's folder name */
static const WCHAR AppContainerMappings[] =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings";

typedef struct _LOWBOX_HANDLES
{
    HANDLE Directory;
    HANDLE RpcControl;
    HANDLE Global;
    HANDLE Local;
    HANDLE Session;
    HANDLE Pipes;
    HANDLE ParentDirectory;
} LOWBOX_HANDLES, *PLOWBOX_HANDLES;

C_ASSERT(sizeof(LOWBOX_HANDLES) == LOWBOX_HANDLE_COUNT * sizeof(HANDLE));

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
BasepCloseLowBoxHandles(
    _Inout_ PLOWBOX_HANDLES Handles)
{
    PHANDLE Handle = (PHANDLE)Handles;
    ULONG i;

    for (i = 0; i < LOWBOX_HANDLE_COUNT; i++)
    {
        if (Handle[i])
            NtClose(Handle[i]);
        Handle[i] = NULL;
    }
}

static
NTSTATUS
BasepOpenEffectiveToken(
    _In_ ACCESS_MASK Access,
    _Out_ PHANDLE Token)
{
    NTSTATUS Status;

    Status = NtOpenThreadToken(NtCurrentThread(), Access, TRUE, Token);
    if (Status == STATUS_NO_TOKEN)
        Status = NtOpenProcessToken(NtCurrentProcess(), Access, Token);
    return Status;
}

static
NTSTATUS
BasepOpenDirectory(
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK Access,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&ObjectAttributes, &Name, 0, Root, NULL);
    return NtOpenDirectoryObject(Handle, Access, &ObjectAttributes);
}

/* Container directories carry a low integrity label so a container can write to them */
static
NTSTATUS
BasepSetLowIntegrityLabel(
    _In_ HANDLE Object)
{
    SID_IDENTIFIER_AUTHORITY LabelAuthority = {SECURITY_MANDATORY_LABEL_AUTHORITY};
    ULONG AclBuffer[(sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    SECURITY_DESCRIPTOR Descriptor;
    PACL Sacl = (PACL)AclBuffer;
    PSID LowSid = NULL;
    NTSTATUS Status;

    Status = RtlAllocateAndInitializeSid(&LabelAuthority, 1, SECURITY_MANDATORY_LOW_RID,
                                         0, 0, 0, 0, 0, 0, 0, &LowSid);
    if (NT_SUCCESS(Status))
        Status = RtlCreateSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION);
    if (NT_SUCCESS(Status))
        Status = RtlCreateAcl(Sacl, sizeof(AclBuffer), ACL_REVISION2);
    if (NT_SUCCESS(Status))
    {
        Status = RtlAddMandatoryAce(Sacl,
                                    ACL_REVISION2,
                                    0,
                                    LowSid,
                                    SYSTEM_MANDATORY_LABEL_ACE_TYPE,
                                    SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    }
    if (NT_SUCCESS(Status))
        Status = RtlSetSaclSecurityDescriptor(&Descriptor, TRUE, Sacl, FALSE);
    if (NT_SUCCESS(Status))
        Status = NtSetSecurityObject(Object, LABEL_SECURITY_INFORMATION, &Descriptor);

    if (LowSid)
        RtlFreeSid(LowSid);
    return Status;
}

static
BOOLEAN
BasepIsLogonSid(
    _In_ PSID Sid)
{
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};

    return RtlLengthSid(Sid) == RtlLengthRequiredSid(SECURITY_LOGON_IDS_RID_COUNT) &&
           RtlEqualMemory(RtlIdentifierAuthoritySid(Sid), &NtAuthority, sizeof(NtAuthority)) &&
           *RtlSubAuthoritySid(Sid, 0) == SECURITY_LOGON_IDS_RID;
}

static
NTSTATUS
BasepAddSidSize(
    _In_ BOOLEAN Wanted,
    _In_ PSID_IDENTIFIER_AUTHORITY Authority,
    _In_ UCHAR Count,
    _In_ ULONG First,
    _In_ ULONG Second,
    _Inout_ PULONG Size,
    _Out_ PSID *Sid)
{
    NTSTATUS Status;

    *Sid = NULL;
    if (!Wanted)
        return STATUS_SUCCESS;

    Status = RtlAllocateAndInitializeSid(Authority, Count, First, Second, 0, 0, 0, 0, 0, 0, Sid);
    if (NT_SUCCESS(Status))
        *Size += sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(*Sid);
    return Status;
}

/*
 * Builds the DACL of a container directory from the DACL of the named object directory it
 * shadows: the base entries, logon sessions widened to full directory access, then the user
 * and the package with full access, plus the entries Extras asks for, inherited by children.
 */
static
NTSTATUS
BasepBuildContainerDescriptor(
    _In_ PSECURITY_DESCRIPTOR BaseDescriptor,
    _In_ PSID PackageSid,
    _In_ PSID UserSid,
    _In_ ULONG Extras,
    _Out_ PSECURITY_DESCRIPTOR Descriptor,
    _Outptr_ PACL *Dacl)
{
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    ACL_SIZE_INFORMATION SizeInfo;
    ACL_REVISION_INFORMATION RevisionInfo;
    BOOLEAN Present, Defaulted, UserListed = FALSE;
    PACL BaseDacl, NewDacl = NULL;
    PSID Administrators = NULL, Restricted = NULL, Everyone = NULL;
    PACCESS_ALLOWED_ACE Ace;
    ULONG Size, Revision, i;
    NTSTATUS Status;

    *Dacl = NULL;

    Status = RtlCreateSecurityDescriptor(Descriptor, SECURITY_DESCRIPTOR_REVISION);
    if (NT_SUCCESS(Status))
        Status = RtlGetDaclSecurityDescriptor(BaseDescriptor, &Present, &BaseDacl, &Defaulted);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!Present || !BaseDacl)
        return STATUS_INVALID_SECURITY_DESCR;

    Status = RtlQueryInformationAcl(BaseDacl, &SizeInfo, sizeof(SizeInfo), AclSizeInformation);
    if (NT_SUCCESS(Status))
        Status = RtlQueryInformationAcl(BaseDacl, &RevisionInfo, sizeof(RevisionInfo), AclRevisionInformation);
    if (!NT_SUCCESS(Status))
        return Status;
    Revision = RevisionInfo.AclRevision;

    /* The base entries plus an access and an inherited entry for the package */
    Size = SizeInfo.AclBytesInUse + 2 * (sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(PackageSid));

    Status = BasepAddSidSize(!!(Extras & LOWBOX_SD_ADMINISTRATORS), &NtAuthority, 2,
                             SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, &Size, &Administrators);
    if (NT_SUCCESS(Status))
    {
        Status = BasepAddSidSize(!!(Extras & LOWBOX_SD_EVERYONE), &WorldAuthority, 1,
                                 SECURITY_WORLD_RID, 0, &Size, &Everyone);
    }
    if (NT_SUCCESS(Status))
    {
        Status = BasepAddSidSize(!!(Extras & LOWBOX_SD_RESTRICTED), &NtAuthority, 1,
                                 SECURITY_RESTRICTED_CODE_RID, 0, &Size, &Restricted);
    }
    if (!NT_SUCCESS(Status))
        goto Quit;

    for (i = 0; i < SizeInfo.AceCount; i++)
    {
        if (NT_SUCCESS(RtlGetAce(BaseDacl, i, (PVOID*)&Ace)) &&
            RtlEqualSid(UserSid, (PSID)&Ace->SidStart))
        {
            UserListed = TRUE;
            break;
        }
    }
    if (!UserListed)
        Size += 2 * (sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(UserSid));

    Size = ALIGN_UP_BY(Size, sizeof(ULONG));
    NewDacl = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!NewDacl)
    {
        Status = STATUS_NO_MEMORY;
        goto Quit;
    }

    Status = RtlCreateAcl(NewDacl, Size, Revision);
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* The base entries are contiguous, so they go over in one piece */
    if (SizeInfo.AceCount)
    {
        Status = RtlGetAce(BaseDacl, 0, (PVOID*)&Ace);
        if (NT_SUCCESS(Status))
            Status = RtlAddAce(NewDacl, Revision, 0, Ace, SizeInfo.AclBytesInUse - sizeof(ACL));
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    for (i = 0; i < SizeInfo.AceCount; i++)
    {
        if (!NT_SUCCESS(RtlGetAce(NewDacl, i, (PVOID*)&Ace)))
            break;
        if (BasepIsLogonSid((PSID)&Ace->SidStart) && !(Ace->Header.AceFlags & INHERIT_ONLY_ACE))
            Ace->Mask = DIRECTORY_ALL_ACCESS;
    }

    if (!UserListed)
    {
        Status = RtlAddAccessAllowedAce(NewDacl, Revision, DIRECTORY_ALL_ACCESS, UserSid);
        if (NT_SUCCESS(Status))
            Status = RtlAddAccessAllowedAceEx(NewDacl, Revision, LOWBOX_INHERIT_ACE, GENERIC_ALL, UserSid);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    Status = RtlAddAccessAllowedAce(NewDacl, Revision, DIRECTORY_ALL_ACCESS, PackageSid);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAceEx(NewDacl, Revision, LOWBOX_INHERIT_ACE, GENERIC_ALL, PackageSid);
    if (NT_SUCCESS(Status) && Administrators)
        Status = RtlAddAccessAllowedAceEx(NewDacl, Revision, LOWBOX_INHERIT_ACE, GENERIC_ALL, Administrators);
    if (NT_SUCCESS(Status) && Everyone)
        Status = RtlAddAccessAllowedAceEx(NewDacl, Revision, LOWBOX_INHERIT_ACE, GENERIC_READ | GENERIC_EXECUTE, Everyone);
    if (NT_SUCCESS(Status) && Restricted)
        Status = RtlAddAccessAllowedAceEx(NewDacl, Revision, LOWBOX_INHERIT_ACE, GENERIC_READ | GENERIC_EXECUTE, Restricted);
    if (NT_SUCCESS(Status))
        Status = RtlSetDaclSecurityDescriptor(Descriptor, TRUE, NewDacl, FALSE);

Quit:
    if (Administrators)
        RtlFreeSid(Administrators);
    if (Everyone)
        RtlFreeSid(Everyone);
    if (Restricted)
        RtlFreeSid(Restricted);

    if (NT_SUCCESS(Status))
        *Dacl = NewDacl;
    else if (NewDacl)
        RtlFreeHeap(RtlGetProcessHeap(), 0, NewDacl);
    return Status;
}

/* The pipe directory reuses the directory DACL with the rights translated to file rights */
static
VOID
BasepConvertDaclToPipeRights(
    _Inout_ PACL Dacl)
{
    PACCESS_ALLOWED_ACE Ace;
    ACCESS_MASK Directory;
    ULONG i;

    for (i = 0; NT_SUCCESS(RtlGetAce(Dacl, i, (PVOID*)&Ace)); i++)
    {
        Directory = Ace->Mask;
        Ace->Mask &= ~SPECIFIC_RIGHTS_ALL;
        if (Directory & DIRECTORY_CREATE_OBJECT)
            Ace->Mask |= SYNCHRONIZE | FILE_ADD_FILE;
        if (Directory & DIRECTORY_CREATE_SUBDIRECTORY)
            Ace->Mask |= SYNCHRONIZE | FILE_ADD_SUBDIRECTORY;
    }
}

static
NTSTATUS
BasepCreateContainerDirectory(
    _In_ HANDLE Parent,
    _In_ PCWSTR Name,
    _In_ PSECURITY_DESCRIPTOR Descriptor,
    _In_ HANDLE Shadow,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING NameString;
    HANDLE Labeled;
    NTSTATUS Status;

    *Handle = NULL;

    /* Labeling takes WRITE_OWNER; the handle kept afterwards has only the container rights */
    RtlInitUnicodeString(&NameString, Name);
    InitializeObjectAttributes(&ObjectAttributes, &NameString, OBJ_CASE_INSENSITIVE | OBJ_OPENIF,
                               Parent, Descriptor);
    Status = NtCreateDirectoryObjectEx(&Labeled, LOWBOX_DIRECTORY_ACCESS | WRITE_OWNER, &ObjectAttributes, Shadow, 1);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Creating directory %S failed: 0x%08lx\n", Name, Status);
        return Status;
    }

    Status = BasepSetLowIntegrityLabel(Labeled);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Labeling directory %S failed: 0x%08lx\n", Name, Status);
        NtClose(Labeled);
        return Status;
    }

    Status = NtDuplicateObject(NtCurrentProcess(),
                               Labeled,
                               NtCurrentProcess(),
                               Handle,
                               LOWBOX_DIRECTORY_ACCESS,
                               0,
                               DUPLICATE_CLOSE_SOURCE);
    if (!NT_SUCCESS(Status))
        DPRINT1("Reducing directory %S failed: 0x%08lx\n", Name, Status);
    return Status;
}

static
NTSTATUS
BasepCreateContainerLink(
    _In_ HANDLE Parent,
    _In_ PCWSTR Name,
    _In_ PSECURITY_DESCRIPTOR Descriptor,
    _In_ PUNICODE_STRING Target,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING NameString;

    RtlInitUnicodeString(&NameString, Name);
    InitializeObjectAttributes(&ObjectAttributes, &NameString, OBJ_CASE_INSENSITIVE | OBJ_OPENIF,
                               Parent, Descriptor);
    return NtCreateSymbolicLinkObject(Handle, SYMBOLIC_LINK_ALL_ACCESS, &ObjectAttributes, Target);
}

/*
 * Creates \Sessions\<n>\AppContainerNamedObjects\<package> with its RPC Control and Global
 * directories shadowing the session's ones, the Local and Session links back to itself, and the
 * matching named pipe directory. A child container nests inside its parent's directory.
 */
static
NTSTATUS
BasepCreateAppContainerDirectories(
    _In_ PSID PackageSid,
    _In_ HANDLE Token,
    _Out_ PLOWBOX_HANDLES Handles)
{
    ULONG UserBuffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_USER User = (PTOKEN_USER)UserBuffer;
    APPCONTAINER_SID_TYPE SidType;
    UNICODE_STRING SidString = {0}, Target, PipeName;
    WCHAR Leaf[128], RootPath[256], BasePath[256], ContainerPath[256], PipePath[300];
    SECURITY_DESCRIPTOR Descriptor, SharedDescriptor;
    PSECURITY_DESCRIPTOR BaseDescriptor = NULL;
    PACL Dacl = NULL, SharedDacl = NULL;
    HANDLE Base = NULL, Root = NULL, ShadowRpc = NULL, ShadowGlobal = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    ULONG SessionId, Length;
    PSID ParentSid = NULL;
    PCSTR Step = "naming";
    NTSTATUS Status;

    RtlZeroMemory(Handles, sizeof(*Handles));

    Status = NtQueryInformationToken(Token, TokenSessionId, &SessionId, sizeof(SessionId), &Length);
    if (NT_SUCCESS(Status))
        Status = NtQueryInformationToken(Token, TokenUser, User, sizeof(UserBuffer), &Length);
    if (NT_SUCCESS(Status))
        Status = RtlGetAppContainerSidType(PackageSid, &SidType);
    if (!NT_SUCCESS(Status))
        return Status;

    /* A parent container is named by its SID, a child by the last four sub-authorities of its SID */
    if (SidType == ParentAppContainerSidType)
    {
        Status = RtlConvertSidToUnicodeString(&SidString, PackageSid, TRUE);
        if (NT_SUCCESS(Status))
            Status = RtlStringCchPrintfW(Leaf, _countof(Leaf), L"%wZ", &SidString);
        if (NT_SUCCESS(Status))
        {
            Status = RtlStringCchPrintfW(RootPath, _countof(RootPath),
                                         L"\\Sessions\\%lu\\AppContainerNamedObjects", SessionId);
        }
        if (NT_SUCCESS(Status) && SessionId == 0)
            Status = RtlStringCchCopyW(BasePath, _countof(BasePath), L"\\BaseNamedObjects");
        else if (NT_SUCCESS(Status))
            Status = RtlStringCchPrintfW(BasePath, _countof(BasePath), L"\\Sessions\\%lu\\BaseNamedObjects", SessionId);
    }
    else
    {
        Status = RtlGetAppContainerParent(PackageSid, &ParentSid);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = RtlConvertSidToUnicodeString(&SidString, ParentSid, TRUE);
        RtlFreeHeap(RtlGetProcessHeap(), 0, ParentSid);
        if (NT_SUCCESS(Status))
        {
            Status = RtlStringCchPrintfW(Leaf, _countof(Leaf), L"%lu-%lu-%lu-%lu",
                                         *RtlSubAuthoritySid(PackageSid, 8),
                                         *RtlSubAuthoritySid(PackageSid, 9),
                                         *RtlSubAuthoritySid(PackageSid, 10),
                                         *RtlSubAuthoritySid(PackageSid, 11));
        }
        if (NT_SUCCESS(Status))
        {
            Status = RtlStringCchPrintfW(RootPath, _countof(RootPath),
                                         L"\\Sessions\\%lu\\AppContainerNamedObjects\\%wZ", SessionId, &SidString);
        }
        if (NT_SUCCESS(Status))
            Status = RtlStringCchCopyW(BasePath, _countof(BasePath), RootPath);
    }
    if (!NT_SUCCESS(Status))
        goto Quit;

    Step = "base directory";
    Status = BasepOpenDirectory(NULL, BasePath, READ_CONTROL | DIRECTORY_QUERY | DIRECTORY_TRAVERSE, &Base);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = NtQuerySecurityObject(Base, DACL_SECURITY_INFORMATION, NULL, 0, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        goto Quit;
    BaseDescriptor = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!BaseDescriptor)
    {
        Status = STATUS_NO_MEMORY;
        goto Quit;
    }
    Status = NtQuerySecurityObject(Base, DACL_SECURITY_INFORMATION, BaseDescriptor, Length, &Length);
    if (NT_SUCCESS(Status))
        Status = BasepBuildContainerDescriptor(BaseDescriptor, PackageSid, User->User.Sid, 0, &Descriptor, &Dacl);
    if (NT_SUCCESS(Status))
    {
        Step = "root directory";
        Status = BasepOpenDirectory(NULL, RootPath, LOWBOX_DIRECTORY_ACCESS, &Root);
    }
    if (NT_SUCCESS(Status))
    {
        Step = "container directory";
        Status = BasepCreateContainerDirectory(Root, Leaf, &Descriptor, Base, &Handles->Directory);
    }
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* RPC Control and Global fall through to the session's, or to the parent container's */
    Step = "RPC Control";
    if (SidType == ParentAppContainerSidType)
        Status = BasepOpenDirectory(NULL, L"\\RPC Control", DIRECTORY_QUERY | DIRECTORY_TRAVERSE, &ShadowRpc);
    else
        Status = BasepOpenDirectory(Root, L"RPC Control", DIRECTORY_QUERY | DIRECTORY_TRAVERSE, &ShadowRpc);
    if (NT_SUCCESS(Status))
    {
        Status = BasepBuildContainerDescriptor(BaseDescriptor, PackageSid, User->User.Sid, LOWBOX_SD_SHARED,
                                               &SharedDescriptor, &SharedDacl);
    }
    if (NT_SUCCESS(Status))
        Status = BasepCreateContainerDirectory(Handles->Directory, L"RPC Control", &SharedDescriptor, ShadowRpc, &Handles->RpcControl);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Step = "Global";
    if (SidType == ParentAppContainerSidType)
        Status = BasepOpenDirectory(NULL, L"\\BaseNamedObjects", DIRECTORY_QUERY | DIRECTORY_TRAVERSE, &ShadowGlobal);
    else
        Status = BasepOpenDirectory(Root, L"Global", DIRECTORY_QUERY | DIRECTORY_TRAVERSE, &ShadowGlobal);
    if (NT_SUCCESS(Status))
        Status = BasepCreateContainerDirectory(Handles->Directory, L"Global", &SharedDescriptor, ShadowGlobal, &Handles->Global);
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* Local and Session lead back to the container directory itself */
    Status = RtlStringCchPrintfW(ContainerPath, _countof(ContainerPath), L"%ws\\%ws", RootPath, Leaf);
    if (!NT_SUCCESS(Status))
        goto Quit;
    RtlInitUnicodeString(&Target, ContainerPath);

    Step = "links";
    Status = BasepCreateContainerLink(Handles->Directory, L"Local", &Descriptor, &Target, &Handles->Local);
    if (NT_SUCCESS(Status))
        Status = BasepCreateContainerLink(Handles->Directory, L"Session", &Descriptor, &Target, &Handles->Session);
    if (NT_SUCCESS(Status))
        Status = RtlStringCchPrintfW(PipePath, _countof(PipePath), L"\\Device\\NamedPipe%ws", ContainerPath);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Step = "pipe directory";
    BasepConvertDaclToPipeRights(Dacl);
    RtlInitUnicodeString(&PipeName, PipePath);
    InitializeObjectAttributes(&ObjectAttributes, &PipeName, OBJ_CASE_INSENSITIVE | OBJ_INHERIT, NULL, &Descriptor);
    Status = NtCreateFile(&Handles->Pipes,
                          STANDARD_RIGHTS_ALL | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          FILE_DIRECTORY_FILE,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* A child token also keeps its parent's directory alive */
    if (SidType == ChildAppContainerSidType)
    {
        Handles->ParentDirectory = Base;
        Base = NULL;
    }

Quit:
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("AppContainer %s failed for session %lu: 0x%08lx\n", Step, SessionId, Status);
        BasepCloseLowBoxHandles(Handles);
    }
    if (Base)
        NtClose(Base);
    if (Root)
        NtClose(Root);
    if (ShadowRpc)
        NtClose(ShadowRpc);
    if (ShadowGlobal)
        NtClose(ShadowGlobal);
    if (BaseDescriptor)
        RtlFreeHeap(RtlGetProcessHeap(), 0, BaseDescriptor);
    if (Dacl)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
    if (SharedDacl)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SharedDacl);
    RtlFreeUnicodeString(&SidString);
    return Status;
}

/* An AppContainer caller may only pass on its own container with exactly its own capabilities */
static
NTSTATUS
BasepCopyAppContainerToken(
    _In_ HANDLE Token,
    _In_ PSECURITY_CAPABILITIES Capabilities,
    _Out_ PHANDLE NewToken)
{
    ULONG SidBuffer[(sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_APPCONTAINER_INFORMATION Own = (PTOKEN_APPCONTAINER_INFORMATION)SidBuffer;
    PTOKEN_GROUPS OwnCapabilities = NULL;
    APPCONTAINER_SID_TYPE SidType;
    ULONG Length, i, j;
    NTSTATUS Status;

    Status = NtQueryInformationToken(Token, TokenAppContainerSid, Own, sizeof(SidBuffer), &Length);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!RtlEqualSid(Own->TokenAppContainer, Capabilities->AppContainerSid))
        return STATUS_INVALID_PARAMETER;

    Status = RtlGetAppContainerSidType(Capabilities->AppContainerSid, &SidType);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SidType != ChildAppContainerSidType)
    {
        Status = NtQueryInformationToken(Token, TokenCapabilities, NULL, 0, &Length);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            return Status;
        OwnCapabilities = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
        if (!OwnCapabilities)
            return STATUS_NO_MEMORY;
        Status = NtQueryInformationToken(Token, TokenCapabilities, OwnCapabilities, Length, &Length);
        if (!NT_SUCCESS(Status))
            goto Quit;

        Status = STATUS_INVALID_PARAMETER;
        if (OwnCapabilities->GroupCount != Capabilities->CapabilityCount)
            goto Quit;
        for (i = 0; i < OwnCapabilities->GroupCount; i++)
        {
            for (j = 0; j < Capabilities->CapabilityCount; j++)
            {
                if (RtlEqualSid(OwnCapabilities->Groups[i].Sid, Capabilities->Capabilities[j].Sid) &&
                    OwnCapabilities->Groups[i].Attributes == Capabilities->Capabilities[j].Attributes)
                {
                    break;
                }
            }
            if (j == Capabilities->CapabilityCount)
                goto Quit;
        }
    }

    Status = NtDuplicateToken(Token, TOKEN_ALL_ACCESS, NULL, FALSE, TokenPrimary, NewToken);

Quit:
    if (OwnCapabilities)
        RtlFreeHeap(RtlGetProcessHeap(), 0, OwnCapabilities);
    return Status;
}

/* Reads the folder name recorded for a container, under its parent's key for a child */
static
NTSTATUS
BasepLookupAppContainerMoniker(
    _In_ PSID UserSid,
    _In_ PSID Sid,
    _Out_writes_(Count) PWSTR Moniker,
    _In_ SIZE_T Count)
{
    ULONG ValueBuffer[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 256 * sizeof(WCHAR)) / sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)ValueBuffer;
    UNICODE_STRING SidString = {0}, ParentString = {0}, UserString = {0}, KeyName, ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    APPCONTAINER_SID_TYPE SidType;
    HANDLE UserKey = NULL, Key = NULL;
    PSID ParentSid = NULL;
    WCHAR KeyPath[400], UserPath[128];
    ULONG Length;
    NTSTATUS Status;

    Status = RtlGetAppContainerSidType(Sid, &SidType);
    if (NT_SUCCESS(Status))
        Status = RtlConvertSidToUnicodeString(&SidString, Sid, TRUE);
    if (NT_SUCCESS(Status))
        Status = RtlConvertSidToUnicodeString(&UserString, UserSid, TRUE);
    if (!NT_SUCCESS(Status))
        goto Quit;

    if (SidType == ChildAppContainerSidType)
    {
        Status = RtlGetAppContainerParent(Sid, &ParentSid);
        if (NT_SUCCESS(Status))
            Status = RtlConvertSidToUnicodeString(&ParentString, ParentSid, TRUE);
        if (NT_SUCCESS(Status))
        {
            Status = RtlStringCchPrintfW(KeyPath, _countof(KeyPath), L"%ws\\%wZ\\Children\\%wZ",
                                         AppContainerMappings, &ParentString, &SidString);
        }
    }
    else
    {
        Status = RtlStringCchPrintfW(KeyPath, _countof(KeyPath), L"%ws\\%wZ", AppContainerMappings, &SidString);
    }
    /* The mapping lives in the hive of the user the process is made for, not necessarily the caller's */
    if (NT_SUCCESS(Status))
        Status = RtlStringCchPrintfW(UserPath, _countof(UserPath), L"\\Registry\\User\\%wZ", &UserString);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&KeyName, UserPath);
        InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = NtOpenKey(&UserKey, KEY_READ, &ObjectAttributes);
    }
    if (!NT_SUCCESS(Status))
        goto Quit;

    RtlInitUnicodeString(&KeyName, KeyPath);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_CASE_INSENSITIVE, UserKey, NULL);
    Status = NtOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&ValueName, L"Moniker");
        Status = NtQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(ValueBuffer), &Length);
    }
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND)
        Status = STATUS_NO_SUCH_FILE;
    if (!NT_SUCCESS(Status))
        goto Quit;

    if (Value->Type != REG_SZ || Value->DataLength < sizeof(WCHAR))
        Status = STATUS_UNSUCCESSFUL;
    else
        Status = RtlStringCchCopyNW(Moniker, Count, (PCWSTR)Value->Data, Value->DataLength / sizeof(WCHAR));

Quit:
    if (Key)
        NtClose(Key);
    if (UserKey)
        NtClose(UserKey);
    if (ParentSid)
        RtlFreeHeap(RtlGetProcessHeap(), 0, ParentSid);
    RtlFreeUnicodeString(&SidString);
    RtlFreeUnicodeString(&ParentString);
    RtlFreeUnicodeString(&UserString);
    return Status;
}

/* Copies an environment block into memory the Rtl environment routines can grow */
static
NTSTATUS
BasepCopyEnvironment(
    _In_opt_ PWSTR Source,
    _Out_ PWSTR *Copy)
{
    SIZE_T Size;
    PWSTR Scan;
    NTSTATUS Status;

    *Copy = NULL;
    if (!Source)
        return RtlCreateEnvironment(TRUE, Copy);

    for (Scan = Source; *Scan; Scan += wcslen(Scan) + 1);
    Size = (Scan + 1 - Source) * sizeof(WCHAR);

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID*)Copy, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    if (NT_SUCCESS(Status))
        RtlCopyMemory(*Copy, Source, (Scan + 1 - Source) * sizeof(WCHAR));
    return Status;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * Builds the security a process and its first thread get from the token they run
 * with: its owner, its primary group and its default DACL. Free it with RtlFreeHeap.
 */
NTSTATUS
WINAPI
BasepCreateTokenObjectSecurity(
    _In_ HANDLE Token,
    _Out_ PSECURITY_DESCRIPTOR *SecurityDescriptor)
{
    static const TOKEN_INFORMATION_CLASS Classes[] = { TokenOwner, TokenPrimaryGroup, TokenDefaultDacl };
    PVOID Info[RTL_NUMBER_OF(Classes)] = {NULL};
    SECURITY_DESCRIPTOR Absolute;
    PSECURITY_DESCRIPTOR Relative = NULL;
    ULONG Length, i;
    NTSTATUS Status = STATUS_SUCCESS;

    *SecurityDescriptor = NULL;

    for (i = 0; i < RTL_NUMBER_OF(Classes) && NT_SUCCESS(Status); i++)
    {
        Status = NtQueryInformationToken(Token, Classes[i], NULL, 0, &Length);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            break;

        Info[i] = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
        if (!Info[i])
        {
            Status = STATUS_NO_MEMORY;
            break;
        }
        Status = NtQueryInformationToken(Token, Classes[i], Info[i], Length, &Length);
    }

    if (NT_SUCCESS(Status))
    {
        RtlCreateSecurityDescriptor(&Absolute, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&Absolute, ((PTOKEN_OWNER)Info[0])->Owner, FALSE);
        RtlSetGroupSecurityDescriptor(&Absolute, ((PTOKEN_PRIMARY_GROUP)Info[1])->PrimaryGroup, FALSE);
        RtlSetDaclSecurityDescriptor(&Absolute, TRUE, ((PTOKEN_DEFAULT_DACL)Info[2])->DefaultDacl, FALSE);

        Length = 0;
        RtlAbsoluteToSelfRelativeSD(&Absolute, NULL, &Length);
        Relative = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
        if (!Relative)
            Status = STATUS_NO_MEMORY;
        else
            Status = RtlAbsoluteToSelfRelativeSD(&Absolute, Relative, &Length);
    }

    for (i = 0; i < RTL_NUMBER_OF(Info); i++)
    {
        if (Info[i])
            RtlFreeHeap(RtlGetProcessHeap(), 0, Info[i]);
    }

    if (!NT_SUCCESS(Status))
    {
        if (Relative)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Relative);
        return Status;
    }

    *SecurityDescriptor = Relative;
    return STATUS_SUCCESS;
}

/*
 * Makes the AppContainer token a new process runs with, from BaseToken or, when there is none,
 * from the caller's own token.
 */
NTSTATUS
WINAPI
BasepCreateAppContainerToken(
    _In_opt_ HANDLE BaseToken,
    _In_ PSECURITY_CAPABILITIES Capabilities,
    _Out_ PHANDLE NewToken)
{
    ULONG SidBuffer[(sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_APPCONTAINER_INFORMATION Own = (PTOKEN_APPCONTAINER_INFORMATION)SidBuffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    LOWBOX_HANDLES Handles = {0};
    APPCONTAINER_SID_TYPE SidType;
    HANDLE Token = BaseToken, OwnToken = NULL;
    ULONG IsAppContainer = 0, Length, HandleCount;
    NTSTATUS Status;

    *NewToken = NULL;

    if (!Capabilities->AppContainerSid)
        return STATUS_INVALID_PARAMETER;

    if (!Token)
    {
        Status = BasepOpenEffectiveToken(TOKEN_ALL_ACCESS, &OwnToken);
        if (!NT_SUCCESS(Status))
            return Status;
        Token = OwnToken;
    }

    if (Capabilities->CapabilityCount && !Capabilities->Capabilities)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Quit;
    }

    Status = NtQueryInformationToken(Token, TokenIsAppContainer, &IsAppContainer, sizeof(IsAppContainer), &Length);
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* Inside a container, a process can start its own children or more of itself, nothing else */
    if (IsAppContainer)
    {
        Status = NtQueryInformationToken(Token, TokenAppContainerSid, Own, sizeof(SidBuffer), &Length);
        if (!NT_SUCCESS(Status))
            goto Quit;

        if (!RtlIsParentOfChildAppContainer(Own->TokenAppContainer, Capabilities->AppContainerSid))
        {
            Status = BasepCopyAppContainerToken(Token, Capabilities, NewToken);
            goto Quit;
        }
    }

    Status = BasepCreateAppContainerDirectories(Capabilities->AppContainerSid, Token, &Handles);
    if (NT_SUCCESS(Status))
        Status = RtlGetAppContainerSidType(Capabilities->AppContainerSid, &SidType);
    if (!NT_SUCCESS(Status))
        goto Quit;
    HandleCount = (SidType == ChildAppContainerSidType) ? LOWBOX_HANDLE_COUNT : LOWBOX_HANDLE_COUNT - 1;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    Status = NtCreateLowBoxToken(NewToken,
                                 Token,
                                 TOKEN_ALL_ACCESS,
                                 &ObjectAttributes,
                                 Capabilities->AppContainerSid,
                                 Capabilities->CapabilityCount,
                                 Capabilities->Capabilities,
                                 HandleCount,
                                 (PHANDLE)&Handles);
    if (!NT_SUCCESS(Status))
        DPRINT1("NtCreateLowBoxToken failed: 0x%08lx\n", Status);

Quit:
    BasepCloseLowBoxHandles(&Handles);
    if (OwnToken)
        NtClose(OwnToken);
    if (!NT_SUCCESS(Status))
        *NewToken = NULL;
    return Status;
}

/*
 * Points LOCALAPPDATA, TEMP and TMP of a new container process at the container's AC folder,
 * <LOCALAPPDATA>\Packages\<moniker>\AC, with a child under its parent's folder as
 * AC\#!<child moniker>. Returns no environment when the caller already runs in that container.
 */
NTSTATUS
WINAPI
BasepCreateAppContainerEnvironment(
    _In_opt_ HANDLE UserToken,
    _In_ PSID PackageSid,
    _In_opt_ PWSTR Environment,
    _Out_ PWSTR *NewEnvironment)
{
    ULONG SidBuffer[(sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_APPCONTAINER_INFORMATION Own = (PTOKEN_APPCONTAINER_INFORMATION)SidBuffer;
    ULONG UserBuffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_USER User = (PTOKEN_USER)UserBuffer;
    UNICODE_STRING Name, Value;
    APPCONTAINER_SID_TYPE SidType, OwnType = NotAppContainerSidType;
    WCHAR Moniker[256], ParentMoniker[256], Folder[MAX_PATH * 2];
    PWSTR Created = NULL, Found;
    PSID ParentSid = NULL;
    HANDLE Token = NULL;
    ULONG IsAppContainer = 0, Length;
    SIZE_T Used;
    NTSTATUS Status;

    *NewEnvironment = NULL;

    Status = BasepOpenEffectiveToken(TOKEN_QUERY, &Token);
    if (NT_SUCCESS(Status))
        Status = NtQueryInformationToken(Token, TokenIsAppContainer, &IsAppContainer, sizeof(IsAppContainer), &Length);
    if (NT_SUCCESS(Status) && IsAppContainer)
    {
        Status = NtQueryInformationToken(Token, TokenAppContainerSid, Own, sizeof(SidBuffer), &Length);
        if (NT_SUCCESS(Status))
            Status = RtlGetAppContainerSidType(Own->TokenAppContainer, &OwnType);
        if (NT_SUCCESS(Status) && OwnType == ParentAppContainerSidType && RtlEqualSid(Own->TokenAppContainer, PackageSid))
            goto Quit;
    }
    if (NT_SUCCESS(Status))
        Status = NtQueryInformationToken(UserToken ? UserToken : Token, TokenUser, User, sizeof(UserBuffer), &Length);
    if (NT_SUCCESS(Status))
        Status = BasepLookupAppContainerMoniker(User->User.Sid, PackageSid, Moniker, _countof(Moniker));
    if (!NT_SUCCESS(Status))
        DPRINT1("AppContainer moniker lookup failed: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
        Status = RtlGetAppContainerSidType(PackageSid, &SidType);
    if (NT_SUCCESS(Status))
        Status = BasepCopyEnvironment(Environment, &Created);
    if (!NT_SUCCESS(Status))
        goto Quit;

    RtlInitUnicodeString(&Name, L"LOCALAPPDATA");
    Value.Buffer = Folder;
    Value.Length = 0;
    Value.MaximumLength = sizeof(Folder);
    Status = RtlQueryEnvironmentVariable_U(Created, &Name, &Value);
    if (!NT_SUCCESS(Status))
        goto Quit;
    Used = Value.Length / sizeof(WCHAR);
    Folder[Used] = UNICODE_NULL;

    if (SidType == ParentAppContainerSidType)
    {
        Status = RtlStringCchPrintfW(&Folder[Used], _countof(Folder) - Used, L"\\Packages\\%ws\\AC", Moniker);
    }
    else
    {
        Status = RtlGetAppContainerParent(PackageSid, &ParentSid);
        if (NT_SUCCESS(Status))
            Status = BasepLookupAppContainerMoniker(User->User.Sid, ParentSid, ParentMoniker, _countof(ParentMoniker));
        if (!NT_SUCCESS(Status))
            goto Quit;

        /* A caller inside the parent container already has the parent's AC folder */
        Found = wcsstr(Folder, ParentMoniker);
        if (OwnType == ParentAppContainerSidType)
        {
            if (!Found || _wcsicmp(Found + wcslen(ParentMoniker), L"\\AC"))
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Quit;
            }
        }
        if (!Found)
            Status = RtlStringCchPrintfW(&Folder[Used], _countof(Folder) - Used, L"\\Packages\\%ws\\AC", ParentMoniker);
        if (NT_SUCCESS(Status))
            Status = RtlStringCchCatW(Folder, _countof(Folder), L"\\#!");
        if (NT_SUCCESS(Status))
            Status = RtlStringCchCatW(Folder, _countof(Folder), Moniker);
    }
    if (!NT_SUCCESS(Status))
        goto Quit;

    RtlInitUnicodeString(&Value, Folder);
    Status = RtlSetEnvironmentVariable(&Created, &Name, &Value);
    if (NT_SUCCESS(Status))
        Status = RtlStringCchCatW(Folder, _countof(Folder), L"\\Temp");
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&Value, Folder);
        RtlInitUnicodeString(&Name, L"TEMP");
        Status = RtlSetEnvironmentVariable(&Created, &Name, &Value);
    }
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&Name, L"TMP");
        Status = RtlSetEnvironmentVariable(&Created, &Name, &Value);
    }
    if (NT_SUCCESS(Status))
    {
        *NewEnvironment = Created;
        Created = NULL;
    }

Quit:
    if (Created)
        RtlDestroyEnvironment(Created);
    if (ParentSid)
        RtlFreeHeap(RtlGetProcessHeap(), 0, ParentSid);
    if (Token)
        NtClose(Token);
    return Status;
}

/* kernel.appcore keeps the package store */
typedef LONG (WINAPI *PBASEP_OPEN_PACKAGE_INFO)(PSID, PCWSTR, const UINT32, PACKAGE_INFO_REFERENCE *);
typedef LONG (WINAPI *PBASEP_GET_PACKAGE_INFO)(PACKAGE_INFO_REFERENCE, const UINT32, UINT32 *, BYTE *, UINT32 *);
typedef LONG (WINAPI *PBASEP_CLOSE_PACKAGE_INFO)(PACKAGE_INFO_REFERENCE);
typedef LONG (WINAPI *PBASEP_PACKAGE_NAME_FROM_TOKEN)(HANDLE, UINT32 *, PWSTR);

/* WIN://PKG claim bit of a full trust packaged application */
#define BASEP_PACKAGE_CLAIM_FULL_TRUST 0x4

static
BOOLEAN
BasepQueryPackageClaim(
    _Out_ PULONG64 Claim)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"WIN://PKG");
    PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Information;
    PTOKEN_SECURITY_ATTRIBUTE_V1 Attribute;
    ULONG64 Buffer[32];
    HANDLE Token;
    ULONG Length;
    NTSTATUS Status;

    *Claim = 0;
    Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Token);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = NtQuerySecurityAttributesToken(Token, &Name, 1, Buffer, sizeof(Buffer), &Length);
    NtClose(Token);
    Information = (PTOKEN_SECURITY_ATTRIBUTES_INFORMATION)Buffer;
    if (!NT_SUCCESS(Status) || !Information->AttributeCount)
        return FALSE;

    Attribute = Information->Attribute.pAttributeV1;
    if (Attribute->ValueType != TOKEN_SECURITY_ATTRIBUTE_TYPE_UINT64 || !Attribute->ValueCount)
        return FALSE;
    *Claim = Attribute->Values.pUint64[0];
    return TRUE;
}

/*
 * The loader marks a packaged process when it starts, from the package claim of its token
 */
BOOLEAN
WINAPI
BasepIsPackagedProcess(VOID)
{
    return (NtCurrentPeb()->BitField & BASEP_PEB_PACKAGED_PROCESS) != 0;
}

/*
 * Like on Windows, a packaged process only gets SetDllDirectory and AddDllDirectory
 * when it runs with full trust
 */
BOOLEAN
WINAPI
BasepPackageAllowsUserDirectories(VOID)
{
    static LONG Allowed = -1;
    ULONG64 Claim;

    if (!BasepIsPackagedProcess())
        return TRUE;

    /* The claims of a running process do not change */
    if (Allowed < 0)
        Allowed = BasepQueryPackageClaim(&Claim) && (Claim & BASEP_PACKAGE_CLAIM_FULL_TRUST);
    return Allowed != 0;
}

/*
 * The DllPath of a packaged process: the folders of its main package and of its frameworks,
 * in dependency order, each ending in a semicolon. Resource packages hold no code and stay out.
 * Without a package name the process inherits the caller's package, if it has one.
 */
DWORD
WINAPI
BasepGetPackageDllPath(
    _In_opt_ HANDLE UserToken,
    _In_opt_ PCWSTR PackageFullName,
    _Outptr_result_maybenull_ PWSTR *DllPath)
{
    ULONG UserBuffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PTOKEN_USER User = (PTOKEN_USER)UserBuffer;
    WCHAR Inherited[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    PBASEP_PACKAGE_NAME_FROM_TOKEN NameFromToken = NULL;
    PBASEP_OPEN_PACKAGE_INFO OpenInfo = NULL;
    PBASEP_GET_PACKAGE_INFO GetInfo = NULL;
    PBASEP_CLOSE_PACKAGE_INFO CloseInfo = NULL;
    PACKAGE_INFO_REFERENCE Reference = NULL;
    PACKAGE_INFO *Packages = NULL;
    HANDLE Token = UserToken;
    HMODULE AppCore = NULL;
    UINT32 Length = 0;
    UINT32 Count = 0;
    UINT32 Index;
    SIZE_T Size;
    PWSTR Path;
    ULONG Returned;
    NTSTATUS Status;
    DWORD Error;

    *DllPath = NULL;
    if (!Token)
    {
        Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Token);
        if (!NT_SUCCESS(Status))
            return RtlNtStatusToDosError(Status);
    }

    AppCore = LoadLibraryW(L"kernel.appcore.dll");
    if (!AppCore)
    {
        Error = GetLastError();
        goto Quit;
    }
    NameFromToken = (PBASEP_PACKAGE_NAME_FROM_TOKEN)GetProcAddress(AppCore, "GetPackageFullNameFromToken");
    OpenInfo = (PBASEP_OPEN_PACKAGE_INFO)GetProcAddress(AppCore, "OpenPackageInfoByFullNameForUser");
    GetInfo = (PBASEP_GET_PACKAGE_INFO)GetProcAddress(AppCore, "GetPackageInfo");
    CloseInfo = (PBASEP_CLOSE_PACKAGE_INFO)GetProcAddress(AppCore, "ClosePackageInfo");
    if (!NameFromToken || !OpenInfo || !GetInfo || !CloseInfo)
    {
        Error = ERROR_PROC_NOT_FOUND;
        goto Quit;
    }

    if (!PackageFullName)
    {
        Length = _countof(Inherited);
        Error = NameFromToken(Token, &Length, Inherited);
        if (Error == APPMODEL_ERROR_NO_PACKAGE)
            Error = ERROR_SUCCESS;
        if (Error != ERROR_SUCCESS || !Length)
            goto Quit;
        PackageFullName = Inherited;
    }

    /* The graph of the user the process runs as */
    Status = NtQueryInformationToken(Token, TokenUser, User, sizeof(UserBuffer), &Returned);
    if (!NT_SUCCESS(Status))
    {
        Error = RtlNtStatusToDosError(Status);
        goto Quit;
    }
    Error = OpenInfo(User->User.Sid, PackageFullName, 0, &Reference);
    if (Error == ERROR_SUCCESS)
    {
        Length = 0;
        Error = GetInfo(Reference, PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &Length, NULL, &Count);
    }
    if (Error == ERROR_INSUFFICIENT_BUFFER)
    {
        Packages = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
        Error = Packages ? GetInfo(Reference, PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &Length, (BYTE *)Packages, &Count)
                         : ERROR_NOT_ENOUGH_MEMORY;
    }
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Size = sizeof(UNICODE_NULL);
    for (Index = 0; Index < Count; Index++)
    {
        if (Packages[Index].path && !(Packages[Index].flags & PACKAGE_PROPERTY_RESOURCE))
            Size += (wcslen(Packages[Index].path) + 1) * sizeof(WCHAR);
    }
    Path = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
    if (!Path)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Quit;
    }

    Path[0] = UNICODE_NULL;
    for (Index = 0; Index < Count; Index++)
    {
        if (!Packages[Index].path || (Packages[Index].flags & PACKAGE_PROPERTY_RESOURCE))
            continue;
        RtlStringCbCatW(Path, Size, Packages[Index].path);
        RtlStringCbCatW(Path, Size, L";");
    }
    *DllPath = Path;

Quit:
    if (Packages)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Packages);
    if (Reference)
        CloseInfo(Reference);
    if (AppCore)
        FreeLibrary(AppCore);
    if (Token != UserToken)
        NtClose(Token);
    return Error;
}
