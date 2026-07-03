/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NT SID to POSIX uid/gid mapping, passwd/group lookups and chown
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ntsecapi.h>
#include <ndk/iofuncs.h>

/* POSIX ids are DOMAIN_OFFSET | RID; the offset selects the domain */
#define PSX_UID_NOBODY              4095
#define PSX_OFFSET_ACCOUNT_DOMAIN   0x00030000  /* Local SAM account domain */
#define PSX_OFFSET_PRIMARY_DOMAIN   0x00100000  /* Machine's primary domain */

/**
 * @brief Map an NT SID to a POSIX uid/gid.
 *
 * TODO: cache domain offsets and handle trusted domains (TrustedPosixOffsetInformation).
 */
ULONG
PsxSidToPosixId(
    _In_opt_ PSID Sid)
{
    UCHAR SubAuthorityCount;
    ULONG Rid;
    ULONG Id;
    PSID DomainSid;
    ULONG DomainSidLength;
    LSA_HANDLE PolicyHandle = NULL;
    LSA_OBJECT_ATTRIBUTES ObjectAttributes;
    PPOLICY_ACCOUNT_DOMAIN_INFO AccountInfo = NULL;
    PPOLICY_PRIMARY_DOMAIN_INFO PrimaryInfo = NULL;
    NTSTATUS Status;

    if ((Sid == NULL) || !RtlValidSid(Sid))
        return PSX_UID_NOBODY;

    SubAuthorityCount = *RtlSubAuthorityCountSid(Sid);
    if (SubAuthorityCount == 0)
        return PSX_UID_NOBODY;

    Rid = *RtlSubAuthoritySid(Sid, SubAuthorityCount - 1);

    /* S-1-5-5-X-Y logon SIDs map to nobody */
    if ((SubAuthorityCount == 3) &&
        (RtlIdentifierAuthoritySid(Sid)->Value[5] == 5) &&
        (*RtlSubAuthoritySid(Sid, 0) == 5))
    {
        return PSX_UID_NOBODY;
    }

    /* The domain SID is this SID without its last subauthority */
    DomainSidLength = RtlLengthSid(Sid);
    DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, DomainSidLength);
    if (DomainSid == NULL)
        return Rid;

    RtlCopySid(DomainSidLength, DomainSid, Sid);
    (*RtlSubAuthorityCountSid(DomainSid))--;

    /* Bare RID if no domain matches */
    Id = Rid;

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    Status = LsaOpenPolicy(NULL, &ObjectAttributes, POLICY_VIEW_LOCAL_INFORMATION, &PolicyHandle);
    if (Status == STATUS_SUCCESS)
    {
        if ((LsaQueryInformationPolicy(PolicyHandle,
                                       PolicyAccountDomainInformation,
                                       (PVOID *)&AccountInfo) == STATUS_SUCCESS) &&
            (AccountInfo != NULL))
        {
            if (RtlEqualSid(AccountInfo->DomainSid, DomainSid))
                Id = Rid | PSX_OFFSET_ACCOUNT_DOMAIN;
            LsaFreeMemory(AccountInfo);
        }

        if ((Id == Rid) &&
            (LsaQueryInformationPolicy(PolicyHandle,
                                       PolicyPrimaryDomainInformation,
                                       (PVOID *)&PrimaryInfo) == STATUS_SUCCESS) &&
            (PrimaryInfo != NULL))
        {
            if ((PrimaryInfo->Sid != NULL) && RtlEqualSid(PrimaryInfo->Sid, DomainSid))
                Id = Rid | PSX_OFFSET_PRIMARY_DOMAIN;
            LsaFreeMemory(PrimaryInfo);
        }

        LsaClose(PolicyHandle);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, DomainSid);
    return Id;
}

/**
 * @brief Set a process's uid/gid from its token's user and primary group SIDs.
 */
VOID
PsxAssignIdentity(
    _In_ PCLIENT_ID ClientId,
    _Inout_ PPSX_PROCESS Process)
{
    HANDLE ProcessHandle;
    HANDLE TokenHandle;
    UCHAR Buffer[256];
    DWORD ReturnLength;
    DWORD Pid = (DWORD)(ULONG_PTR)ClientId->UniqueProcess;
    ULONG Id;

    /* Default to nobody until the token is read */
    Process->Uid = Process->EffectiveUid = PSX_UID_NOBODY;
    Process->Gid = Process->EffectiveGid = PSX_UID_NOBODY;

    ProcessHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, Pid);
    if (ProcessHandle == NULL)
        return;

    if (OpenProcessToken(ProcessHandle, TOKEN_QUERY, &TokenHandle))
    {
        if (GetTokenInformation(TokenHandle, TokenUser, Buffer, sizeof(Buffer), &ReturnLength))
        {
            Id = PsxSidToPosixId(((PTOKEN_USER)Buffer)->User.Sid);
            Process->Uid = Process->EffectiveUid = Id;
        }

        if (GetTokenInformation(TokenHandle, TokenPrimaryGroup, Buffer, sizeof(Buffer), &ReturnLength))
        {
            Id = PsxSidToPosixId(((PTOKEN_PRIMARY_GROUP)Buffer)->PrimaryGroup);
            Process->Gid = Process->EffectiveGid = Id;
        }

        CloseHandle(TokenHandle);
    }

    CloseHandle(ProcessHandle);
}

/**
 * @brief getgroups(gidsetsize, grouplist) (ApiNumber 0x0D).
 *
 * Data[0] is gidsetsize and Data[1] is the client's own grouplist address (not a
 * shared section pointer). Groups that map to gid 0 are skipped.
 */
VOID
PsxSrvGetGroups(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG SetSize = ((PULONG)Message->Data.Raw)[0];
    ULONG_PTR ClientList = ((PULONG)Message->Data.Raw)[1];
    HANDLE TokenHandle = NULL;
    UCHAR Buffer[2048];
    DWORD ReturnLength = 0;
    PTOKEN_GROUPS Groups;
    PULONG Gids;
    ULONG Gid;
    ULONG Count = 0;
    ULONG i;
    NTSTATUS Status;

    if ((Process->ProcessHandle == NULL) ||
        !OpenProcessToken(Process->ProcessHandle, TOKEN_QUERY, &TokenHandle))
    {
        Message->Errno = 13; /* EACCES */
        Message->ReturnValue = -1;
        return;
    }

    if (!GetTokenInformation(TokenHandle, TokenGroups, Buffer, sizeof(Buffer), &ReturnLength))
    {
        CloseHandle(TokenHandle);
        Message->Errno = 13; /* EACCES */
        Message->ReturnValue = -1;
        return;
    }
    CloseHandle(TokenHandle);
    Groups = (PTOKEN_GROUPS)Buffer;

    Gids = RtlAllocateHeap(RtlGetProcessHeap(), 0, (Groups->GroupCount + 1) * sizeof(*Gids));
    if (Gids == NULL)
    {
        Message->Errno = 12; /* ENOMEM */
        Message->ReturnValue = -1;
        return;
    }
    for (i = 0; i < Groups->GroupCount; i++)
    {
        Gid = PsxSidToPosixId(Groups->Groups[i].Sid);
        if (Gid != 0)
            Gids[Count++] = Gid;
    }

    /* A zero size only queries the count */
    if (SetSize == 0)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
        Message->Errno = 0;
        Message->ReturnValue = (LONG)Count;
        return;
    }
    if (SetSize < Count)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                  (PVOID)ClientList,
                                  Gids,
                                  Count * sizeof(*Gids),
                                  NULL);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = 13; /* EACCES */
        Message->ReturnValue = -1;
        return;
    }
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Count;
}

/**
 * @brief Build an NT SID for a POSIX uid/gid (the reverse of PsxSidToPosixId).
 *
 * @return A heap SID the caller frees, or NULL.
 */
static
PSID
PsxBuildSidFromId(
    _In_ ULONG Id)
{
    ULONG Offset = Id & 0xFFFF0000;
    ULONG Rid = Id & 0x0000FFFF;
    LSA_HANDLE PolicyHandle = NULL;
    LSA_OBJECT_ATTRIBUTES ObjectAttributes;
    PPOLICY_ACCOUNT_DOMAIN_INFO AccountInfo = NULL;
    PPOLICY_PRIMARY_DOMAIN_INFO PrimaryInfo = NULL;
    PSID DomainSid = NULL;
    ULONG Length;

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    if (LsaOpenPolicy(NULL,
                      &ObjectAttributes,
                      POLICY_VIEW_LOCAL_INFORMATION,
                      &PolicyHandle) != STATUS_SUCCESS)
    {
        return NULL;
    }

    if (Offset == PSX_OFFSET_ACCOUNT_DOMAIN)
    {
        if ((LsaQueryInformationPolicy(PolicyHandle,
                                       PolicyAccountDomainInformation,
                                       (PVOID *)&AccountInfo) == STATUS_SUCCESS) &&
            (AccountInfo != NULL))
        {
            Length = RtlLengthSid(AccountInfo->DomainSid);
            DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + sizeof(ULONG));
            if (DomainSid != NULL)
                RtlCopySid(Length, DomainSid, AccountInfo->DomainSid);
            LsaFreeMemory(AccountInfo);
        }
    }
    else if (Offset == PSX_OFFSET_PRIMARY_DOMAIN)
    {
        if ((LsaQueryInformationPolicy(PolicyHandle,
                                       PolicyPrimaryDomainInformation,
                                       (PVOID *)&PrimaryInfo) == STATUS_SUCCESS) &&
            (PrimaryInfo != NULL))
        {
            if (PrimaryInfo->Sid != NULL)
            {
                Length = RtlLengthSid(PrimaryInfo->Sid);
                DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + sizeof(ULONG));
                if (DomainSid != NULL)
                    RtlCopySid(Length, DomainSid, PrimaryInfo->Sid);
            }
            LsaFreeMemory(PrimaryInfo);
        }
    }
    LsaClose(PolicyHandle);

    if (DomainSid == NULL)
        return NULL;

    /* Append the RID as a new last subauthority */
    (*RtlSubAuthorityCountSid(DomainSid))++;
    *RtlSubAuthoritySid(DomainSid, *RtlSubAuthorityCountSid(DomainSid) - 1) = Rid;
    return DomainSid;
}

/**
 * @brief Look up the account name of a uid/gid.
 */
static
BOOLEAN
PsxResolveIdName(
    _In_ ULONG Id,
    _Out_writes_(NameLen) PSTR Name,
    _In_ ULONG NameLen)
{
    PSID Sid = PsxBuildSidFromId(Id);
    CHAR Domain[128];
    DWORD NameSize = NameLen;
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    BOOLEAN Ok = FALSE;

    if (Sid != NULL)
    {
        if (LookupAccountSidA(NULL, Sid, Name, &NameSize, Domain, &DomainSize, &Use))
            Ok = TRUE;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sid);
    }
    return Ok;
}

/**
 * @brief Look up the calling process's own account name from its token.
 */
static
BOOLEAN
PsxResolveCallerName(
    _In_ PPSX_PROCESS Process,
    _Out_writes_(NameLen) PSTR Name,
    _In_ ULONG NameLen)
{
    HANDLE TokenHandle = NULL;
    UCHAR Buffer[256];
    DWORD ReturnLength;
    CHAR Domain[128];
    DWORD NameSize = NameLen;
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    BOOLEAN Ok = FALSE;

    if ((Process->ProcessHandle != NULL) &&
        OpenProcessToken(Process->ProcessHandle, TOKEN_QUERY, &TokenHandle))
    {
        if (GetTokenInformation(TokenHandle, TokenUser, Buffer, sizeof(Buffer), &ReturnLength) &&
            LookupAccountSidA(NULL,
                              ((PTOKEN_USER)Buffer)->User.Sid,
                              Name,
                              &NameSize,
                              Domain,
                              &DomainSize,
                              &Use))
        {
            Ok = TRUE;
        }
        CloseHandle(TokenHandle);
    }
    return Ok;
}

/**
 * @brief Write a struct passwd into a client buffer.
 *
 * The 20-byte header holds pw_name, pw_uid, pw_gid, pw_dir, pw_shell. String fields
 * are stored as offsets from the buffer start, which the client relocates.
 *
 * @return Total bytes written.
 */
static
ULONG
PsxFillPasswd(
    _Out_ PVOID Buffer,
    _In_ ULONG Uid,
    _In_ ULONG Gid,
    _In_z_ PCSTR Name,
    _In_z_ PCSTR Dir,
    _In_z_ PCSTR Shell)
{
    PULONG Fields = (PULONG)Buffer;
    ULONG Offset = 20;
    ULONG Length;

    Fields[1] = Uid;
    Fields[2] = Gid;

    Fields[0] = Offset;
    Length = (ULONG)strlen(Name) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Name, Length);
    Offset += Length;

    Fields[3] = Offset;
    Length = (ULONG)strlen(Dir) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Dir, Length);
    Offset += Length;

    Fields[4] = Offset;
    Length = (ULONG)strlen(Shell) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Shell, Length);
    Offset += Length;

    return Offset;
}

/**
 * @brief Write a struct group with an empty member list into a client buffer.
 *
 * Layout: gr_name offset, gr_gid, gr_mem offset (12), NULL member terminator, name at 16.
 */
static
ULONG
PsxFillGroup(
    _Out_ PVOID Buffer,
    _In_ ULONG Gid,
    _In_z_ PCSTR Name)
{
    PULONG Fields = (PULONG)Buffer;
    ULONG Length = (ULONG)strlen(Name) + 1;

    Fields[1] = Gid;
    Fields[2] = 12;
    Fields[3] = 0;
    Fields[0] = 16;
    RtlCopyMemory((PCHAR)Buffer + 16, Name, Length);
    return 16 + Length;
}

/**
 * @brief getpwuid(uid) (ApiNumber 0x37).
 *
 * Data[0] is the uid, Data[1] the shared section result buffer; the used length
 * is returned in Data[2].
 */
VOID
PsxSrvGetpwuid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG Uid = ((PULONG)Message->Data.Raw)[0];
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];
    CHAR Name[64];
    ULONG Gid;
    ULONG Length;

    if (!PsxValidateClientPointer(Process, BufPtr, 20))
    {
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    if ((Uid == Process->Uid) && PsxResolveCallerName(Process, Name, sizeof(Name)))
    {
        Gid = Process->Gid;
    }
    else if (PsxResolveIdName(Uid, Name, sizeof(Name)))
    {
        /* The primary gid is unknown without SAM */
        Gid = Uid;
    }
    else
    {
        Message->Errno = 1; /* EPERM */
        Message->ReturnValue = -1;
        return;
    }

    Length = PsxFillPasswd((PVOID)BufPtr, Uid, Gid, Name, "/", "/bin/sh");
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief getpwnam(name) (ApiNumber 0x38). Same buffer layout as getpwuid, name at Data[0].
 */
VOID
PsxSrvGetpwnam(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG_PTR NamePtr = ((PULONG)Message->Data.Raw)[0];
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];
    PSTR Name = (PSTR)NamePtr;
    UCHAR SidBuffer[SECURITY_MAX_SID_SIZE];
    CHAR Domain[128];
    DWORD SidSize = sizeof(SidBuffer);
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    ULONG Uid;
    ULONG Gid;
    ULONG Length;

    if (!PsxValidateClientPointer(Process, NamePtr, 1) ||
        !PsxValidateClientPointer(Process, BufPtr, 20))
    {
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    if (!LookupAccountNameA(NULL, Name, SidBuffer, &SidSize, Domain, &DomainSize, &Use))
    {
        Message->Errno = 1; /* EPERM */
        Message->ReturnValue = -1;
        return;
    }

    Uid = PsxSidToPosixId(SidBuffer);
    Gid = (Uid == Process->Uid) ? Process->Gid : Uid;
    Length = PsxFillPasswd((PVOID)BufPtr, Uid, Gid, Name, "/", "/bin/sh");
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief getgrgid(gid) (ApiNumber 0x39). Gid 4095 resolves to "nobody".
 */
VOID
PsxSrvGetgrgid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG Gid = ((PULONG)Message->Data.Raw)[0];
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];
    CHAR Name[64];
    ULONG Length;

    if (!PsxValidateClientPointer(Process, BufPtr, 16))
    {
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    if (Gid == PSX_UID_NOBODY)
    {
        RtlCopyMemory(Name, "nobody", sizeof("nobody"));
    }
    else if (!PsxResolveIdName(Gid, Name, sizeof(Name)))
    {
        Message->Errno = 1; /* EPERM */
        Message->ReturnValue = -1;
        return;
    }

    Length = PsxFillGroup((PVOID)BufPtr, Gid, Name);
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief getgrnam(name) (ApiNumber 0x3A). Name at Data[0], result buffer at Data[1].
 */
VOID
PsxSrvGetgrnam(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG_PTR NamePtr = ((PULONG)Message->Data.Raw)[0];
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];
    PSTR Name = (PSTR)NamePtr;
    UCHAR SidBuffer[SECURITY_MAX_SID_SIZE];
    CHAR Domain[128];
    DWORD SidSize = sizeof(SidBuffer);
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    ULONG Gid;
    ULONG Length;

    if (!PsxValidateClientPointer(Process, NamePtr, 1) ||
        !PsxValidateClientPointer(Process, BufPtr, 16))
    {
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    if (!LookupAccountNameA(NULL, Name, SidBuffer, &SidSize, Domain, &DomainSize, &Use))
    {
        Message->Errno = 1; /* EPERM */
        Message->ReturnValue = -1;
        return;
    }

    Gid = PsxSidToPosixId(SidBuffer);
    Length = PsxFillGroup((PVOID)BufPtr, Gid, Name);
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief chown(path, uid, gid) (ApiNumber 0x23). Sets the file's owner and group SIDs.
 *
 * The path UNICODE_STRING is at Data[0..1], uid at Data[2], gid at Data[3].
 */
VOID
PsxSrvChown(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = *(PUNICODE_STRING)Message->Data.Raw;
    ULONG Uid = ((PULONG)Message->Data.Raw)[2];
    ULONG Gid = ((PULONG)Message->Data.Raw)[3];
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    SECURITY_DESCRIPTOR SecurityDescriptor;
    PSID OwnerSid = NULL;
    PSID GroupSid = NULL;
    HANDLE Handle = NULL;
    NTSTATUS Status;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length))
    {
        Message->Errno = 22; /* EINVAL */
        Message->ReturnValue = -1;
        return;
    }

    OwnerSid = PsxBuildSidFromId(Uid);
    GroupSid = PsxBuildSidFromId(Gid);
    if ((OwnerSid == NULL) || (GroupSid == NULL))
    {
        if (OwnerSid != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, OwnerSid);
        if (GroupSid != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, GroupSid);
        Message->Errno = 1; /* EPERM */
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle,
                        WRITE_OWNER | WRITE_DAC | READ_CONTROL | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, OwnerSid);
        RtlFreeHeap(RtlGetProcessHeap(), 0, GroupSid);
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    RtlCreateSecurityDescriptor(&SecurityDescriptor, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&SecurityDescriptor, OwnerSid, FALSE);
    RtlSetGroupSecurityDescriptor(&SecurityDescriptor, GroupSid, FALSE);

    PsxImpersonateClient(Process, Message);
    Status = NtSetSecurityObject(Handle,
                                 OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
                                 &SecurityDescriptor);
    PsxRevertToSelf();

    NtClose(Handle);
    RtlFreeHeap(RtlGetProcessHeap(), 0, OwnerSid);
    RtlFreeHeap(RtlGetProcessHeap(), 0, GroupSid);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief Register the identity and chown handlers in the dispatch table.
 */
VOID
PsxInitIdentityOps(VOID)
{
    g_OpHandlers[PsxApiChown] = PsxSrvChown;
    g_OpHandlers[PsxApiGetpwuid] = PsxSrvGetpwuid;
    g_OpHandlers[PsxApiGetpwnam] = PsxSrvGetpwnam;
    g_OpHandlers[PsxApiGetgrgid] = PsxSrvGetgrgid;
    g_OpHandlers[PsxApiGetgrnam] = PsxSrvGetgrnam;
}
