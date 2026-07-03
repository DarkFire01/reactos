/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NT SID <-> POSIX uid/gid mapping. Faithful to the NT 4.0
 *              SidToPosixId (psxss.exe sub_1F46279): id = DOMAIN_OFFSET | RID.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ntsecapi.h>
#include <ndk/iofuncs.h>   // NtOpenFile / IO_STATUS_BLOCK (chown)

//
// POSIX domain offsets (high bits of a uid/gid); the RID is the low part.
//
#define PSX_UID_NOBODY              4095
#define PSX_OFFSET_ACCOUNT_DOMAIN   0x00030000  // local SAM account domain
#define PSX_OFFSET_PRIMARY_DOMAIN   0x00100000  // the machine's primary domain

//
// Map an NT SID to a POSIX uid/gid:  id = DOMAIN_POSIX_OFFSET | RID, where RID
// is the SID's last subauthority and the offset identifies the domain. Faithful
// to SidToPosixId; the {domainSID -> offset} cache and the trusted-domain offset
// (LsaQueryInfoTrustedDomain / TrustedPosixOffsetInformation) are still TODO.
//
ULONG
PsxSidToPosixId(IN PSID Sid)
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

    // S-1-5-X with exactly 3 subauthorities is a logon/special SID -> nobody.
    if ((SubAuthorityCount == 3) &&
        (RtlIdentifierAuthoritySid(Sid)->Value[5] == 5) &&
        (*RtlSubAuthoritySid(Sid, 0) == 5))
    {
        return PSX_UID_NOBODY;
    }

    // Build the domain SID = this SID with its last subauthority removed.
    DomainSidLength = RtlLengthSid(Sid);
    DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, DomainSidLength);
    if (DomainSid == NULL)
        return Rid;                 // degrade to the bare RID

    RtlCopySid(DomainSidLength, DomainSid, Sid);
    (*RtlSubAuthorityCountSid(DomainSid))--;

    Id = Rid;                       // fallback if every LSA step fails

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    Status = LsaOpenPolicy(NULL, &ObjectAttributes, POLICY_VIEW_LOCAL_INFORMATION,
                           &PolicyHandle);
    if (Status == STATUS_SUCCESS)
    {
        // (a) local SAM account domain -> 0x30000
        if ((LsaQueryInformationPolicy(PolicyHandle, PolicyAccountDomainInformation,
                                       (PVOID *)&AccountInfo) == STATUS_SUCCESS) &&
            (AccountInfo != NULL))
        {
            if (RtlEqualSid(AccountInfo->DomainSid, DomainSid))
                Id = Rid | PSX_OFFSET_ACCOUNT_DOMAIN;
            LsaFreeMemory(AccountInfo);
        }

        // (b) the machine's primary domain -> 0x100000
        if ((Id == Rid) &&
            (LsaQueryInformationPolicy(PolicyHandle, PolicyPrimaryDomainInformation,
                                       (PVOID *)&PrimaryInfo) == STATUS_SUCCESS) &&
            (PrimaryInfo != NULL))
        {
            if ((PrimaryInfo->Sid != NULL) && RtlEqualSid(PrimaryInfo->Sid, DomainSid))
                Id = Rid | PSX_OFFSET_PRIMARY_DOMAIN;
            LsaFreeMemory(PrimaryInfo);
        }

        // (c) TODO: a trusted domain -> NetGetAnyDCName + LsaOpenTrustedDomain +
        //     LsaQueryInfoTrustedDomain(TrustedPosixOffsetInformation).Offset.

        LsaClose(PolicyHandle);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, DomainSid);
    return Id;
}

//
// Resolve the connecting client's identity and stamp it into its PSX_PROCESS.
// Opens the client process token and maps its user/primary-group SIDs.
// (The real psxss assigns identity at process-creation; we do it at connect
// until the SM/session/fork machinery exists -- TODO: use native NtOpen* and
// inherit across fork.)
//
VOID
PsxAssignIdentity(IN PCLIENT_ID ClientId, IN OUT PPSX_PROCESS Process)
{
    HANDLE ProcessHandle;
    HANDLE TokenHandle;
    UCHAR Buffer[256];
    DWORD ReturnLength;
    DWORD Pid = (DWORD)(ULONG_PTR)ClientId->UniqueProcess;

    // Default to "nobody" until the token is read successfully.
    Process->Uid = Process->EffectiveUid = PSX_UID_NOBODY;
    Process->Gid = Process->EffectiveGid = PSX_UID_NOBODY;

    ProcessHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, Pid);
    if (ProcessHandle == NULL)
        return;

    if (OpenProcessToken(ProcessHandle, TOKEN_QUERY, &TokenHandle))
    {
        if (GetTokenInformation(TokenHandle, TokenUser, Buffer, sizeof(Buffer), &ReturnLength))
        {
            ULONG Uid = PsxSidToPosixId(((PTOKEN_USER)Buffer)->User.Sid);
            Process->Uid = Process->EffectiveUid = Uid;
        }

        if (GetTokenInformation(TokenHandle, TokenPrimaryGroup, Buffer, sizeof(Buffer), &ReturnLength))
        {
            ULONG Gid = PsxSidToPosixId(((PTOKEN_PRIMARY_GROUP)Buffer)->PrimaryGroup);
            Process->Gid = Process->EffectiveGid = Gid;
        }

        CloseHandle(TokenHandle);
    }

    CloseHandle(ProcessHandle);
}

//
// getgroups(gidsetsize, grouplist) -- ApiNumber 0x0D. Reads the caller's NT token
// groups, maps each SID to a POSIX gid (dropping ones that map to 0), and, when a
// buffer is supplied, writes the gids into the client. gidsetsize at Data[0]; the
// client's *raw* grouplist pointer at Data[1] (not shared-section-biased). Returns
// the group count. Faithful to sub_1F4BA2D.
//
VOID
PsxSrvGetGroups(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG SetSize = ((PULONG)Message->Data.Raw)[0];         // +0x30
    ULONG_PTR ClientList = ((PULONG)Message->Data.Raw)[1];  // +0x34 (raw client ptr)
    HANDLE TokenHandle = NULL;
    UCHAR Buffer[2048];
    DWORD ReturnLength = 0;
    PTOKEN_GROUPS Groups;
    PULONG Gids;
    ULONG Count = 0;
    ULONG i;
    NTSTATUS Status;

    if ((Process->ProcessHandle == NULL) ||
        !OpenProcessToken(Process->ProcessHandle, TOKEN_QUERY, &TokenHandle))
    {
        Message->Errno = 13;            // EACCES
        Message->ReturnValue = -1;
        return;
    }

    if (!GetTokenInformation(TokenHandle, TokenGroups, Buffer, sizeof(Buffer), &ReturnLength))
    {
        CloseHandle(TokenHandle);
        Message->Errno = 13;
        Message->ReturnValue = -1;
        return;
    }
    CloseHandle(TokenHandle);
    Groups = (PTOKEN_GROUPS)Buffer;

    Gids = RtlAllocateHeap(RtlGetProcessHeap(), 0, (Groups->GroupCount + 1) * sizeof(ULONG));
    if (Gids == NULL)
    {
        Message->Errno = 12;            // ENOMEM
        Message->ReturnValue = -1;
        return;
    }
    for (i = 0; i < Groups->GroupCount; i++)
    {
        ULONG Gid = PsxSidToPosixId(Groups->Groups[i].Sid);
        if (Gid != 0)
            Gids[Count++] = Gid;
    }

    if (SetSize == 0)                   // query the count only
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
        Message->Errno = 0;
        Message->ReturnValue = (LONG)Count;
        return;
    }
    if (SetSize < Count)                // supplied buffer too small
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
        Message->Errno = 22;            // EINVAL
        Message->ReturnValue = -1;
        return;
    }

    Status = NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientList, Gids,
                                  Count * sizeof(ULONG), NULL);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Gids);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = 13;            // EACCES
        Message->ReturnValue = -1;
        return;
    }
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Count;
}

//
// Reverse of PsxSidToPosixId: reconstruct an NT SID for a POSIX uid/gid by
// appending RID = (id & 0xFFFF) to the domain SID selected by the offset bits.
// Returns a heap SID the caller frees, or NULL. Faithful to sub_1F465FA +
// sub_1F4CDD3.
//
static PSID
PsxBuildSidFromId(IN ULONG Id)
{
    ULONG Offset = Id & 0xFFFF0000;
    ULONG Rid = Id & 0x0000FFFF;
    LSA_HANDLE PolicyHandle = NULL;
    LSA_OBJECT_ATTRIBUTES ObjectAttributes;
    PSID DomainSid = NULL;
    ULONG Length;

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    if (LsaOpenPolicy(NULL, &ObjectAttributes, POLICY_VIEW_LOCAL_INFORMATION,
                      &PolicyHandle) != STATUS_SUCCESS)
        return NULL;

    if (Offset == PSX_OFFSET_ACCOUNT_DOMAIN)
    {
        PPOLICY_ACCOUNT_DOMAIN_INFO Info = NULL;
        if ((LsaQueryInformationPolicy(PolicyHandle, PolicyAccountDomainInformation,
                                       (PVOID *)&Info) == STATUS_SUCCESS) && Info)
        {
            Length = RtlLengthSid(Info->DomainSid);
            DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + sizeof(ULONG));
            if (DomainSid != NULL)
                RtlCopySid(Length, DomainSid, Info->DomainSid);
            LsaFreeMemory(Info);
        }
    }
    else if (Offset == PSX_OFFSET_PRIMARY_DOMAIN)
    {
        PPOLICY_PRIMARY_DOMAIN_INFO Info = NULL;
        if ((LsaQueryInformationPolicy(PolicyHandle, PolicyPrimaryDomainInformation,
                                       (PVOID *)&Info) == STATUS_SUCCESS) && Info)
        {
            if (Info->Sid != NULL)
            {
                Length = RtlLengthSid(Info->Sid);
                DomainSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + sizeof(ULONG));
                if (DomainSid != NULL)
                    RtlCopySid(Length, DomainSid, Info->Sid);
            }
            LsaFreeMemory(Info);
        }
    }
    LsaClose(PolicyHandle);

    if (DomainSid == NULL)
        return NULL;

    (*RtlSubAuthorityCountSid(DomainSid))++;
    *RtlSubAuthoritySid(DomainSid, *RtlSubAuthorityCountSid(DomainSid) - 1) = Rid;
    return DomainSid;
}

//
// Resolve an account name for a uid/gid: reconstruct the SID and LookupAccountSid.
//
static BOOLEAN
PsxResolveIdName(IN ULONG Id, OUT PSTR Name, IN ULONG NameLen)
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

//
// Resolve the calling process's own login name from its token (accurate path for
// getpwuid(getuid())).
//
static BOOLEAN
PsxResolveCallerName(IN PPSX_PROCESS Process, OUT PSTR Name, IN ULONG NameLen)
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
            LookupAccountSidA(NULL, ((PTOKEN_USER)Buffer)->User.Sid, Name, &NameSize,
                              Domain, &DomainSize, &Use))
        {
            Ok = TRUE;
        }
        CloseHandle(TokenHandle);
    }
    return Ok;
}

//
// Fill a POSIX `struct passwd` into a client-view buffer: the 20-byte header with
// the char* fields stored as buffer-relative offsets the client relocates, then
// the name/dir/shell strings. Returns the total byte length.
//
static ULONG
PsxFillPasswd(IN PVOID Buffer, IN ULONG Uid, IN ULONG Gid,
              IN PCSTR Name, IN PCSTR Dir, IN PCSTR Shell)
{
    PULONG Fields = (PULONG)Buffer;             // pw_name,pw_uid,pw_gid,pw_dir,pw_shell
    ULONG Offset = 20;                          // strings start after the header
    ULONG Length;

    Fields[1] = Uid;                            // pw_uid  @+4
    Fields[2] = Gid;                            // pw_gid  @+8

    Fields[0] = Offset;                         // pw_name @+0 -> offset
    Length = (ULONG)strlen(Name) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Name, Length);
    Offset += Length;

    Fields[3] = Offset;                         // pw_dir  @+12
    Length = (ULONG)strlen(Dir) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Dir, Length);
    Offset += Length;

    Fields[4] = Offset;                         // pw_shell @+16
    Length = (ULONG)strlen(Shell) + 1;
    RtlCopyMemory((PCHAR)Buffer + Offset, Shell, Length);
    Offset += Length;

    return Offset;
}

//
// Fill a POSIX `struct group` (gr_name,gr_gid,gr_mem) with an empty member vector.
//
static ULONG
PsxFillGroup(IN PVOID Buffer, IN ULONG Gid, IN PCSTR Name)
{
    PULONG Fields = (PULONG)Buffer;
    ULONG Length = (ULONG)strlen(Name) + 1;

    Fields[1] = Gid;                            // gr_gid @+4
    Fields[2] = 12;                             // gr_mem @+8 -> the vector at +12
    Fields[3] = 0;                              // @+12: member vector = { NULL } (empty)
    Fields[0] = 16;                             // gr_name @+0 -> name at +16
    RtlCopyMemory((PCHAR)Buffer + 16, Name, Length);
    return 16 + Length;
}

//
// getpwuid(uid) -- ApiNumber 0x37. uid at Data[0]; the client's shared-section
// result buffer at Data[1]; the byte length used is reported back in Data[2].
// Faithful to sub_1F4C07B (SAM/LSA reverse map); we resolve the name via the
// caller's token when it is the caller's own uid, else by SID reconstruction.
//
VOID
PsxSrvGetpwuid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG Uid = ((PULONG)Message->Data.Raw)[0];             // +0x30
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];      // +0x34 (server-relative)
    CHAR Name[64];
    ULONG Gid;
    ULONG Length;

    if (!PsxValidateClientPointer(Process, BufPtr, 20))
    {
        Message->Errno = 22;                    // EINVAL
        Message->ReturnValue = -1;
        return;
    }

    if ((Uid == Process->Uid) && PsxResolveCallerName(Process, Name, sizeof(Name)))
    {
        Gid = Process->Gid;
    }
    else if (PsxResolveIdName(Uid, Name, sizeof(Name)))
    {
        Gid = Uid;                              // primary gid unknown without SAM
    }
    else
    {
        Message->Errno = 1;                     // EPERM: cannot resolve
        Message->ReturnValue = -1;
        return;
    }

    Length = PsxFillPasswd((PVOID)BufPtr, Uid, Gid, Name, "/", "/bin/sh");
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// getpwnam(name) -- ApiNumber 0x38. ANSI name at Data[0] (client view); result
// buffer at Data[1]; length reported in Data[2]. Faithful to sub_1F4C241.
//
VOID
PsxSrvGetpwnam(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG_PTR NamePtr = ((PULONG)Message->Data.Raw)[0];     // +0x30 (client view)
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];      // +0x34
    PSTR Name = (PSTR)NamePtr;
    UCHAR SidBuffer[SECURITY_MAX_SID_SIZE];
    CHAR Domain[128];
    DWORD SidSize = sizeof(SidBuffer);
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    ULONG Uid, Gid, Length;

    if (!PsxValidateClientPointer(Process, NamePtr, 1) ||
        !PsxValidateClientPointer(Process, BufPtr, 20))
    {
        Message->Errno = 22;                    // EINVAL
        Message->ReturnValue = -1;
        return;
    }

    if (!LookupAccountNameA(NULL, Name, SidBuffer, &SidSize, Domain, &DomainSize, &Use))
    {
        Message->Errno = 1;                     // EPERM: no such user
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

//
// getgrgid(gid) -- ApiNumber 0x39. gid at Data[0]; result buffer at Data[1].
// Faithful to sub_1F4C612 (gid 4095 -> "nobody").
//
VOID
PsxSrvGetgrgid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG Gid = ((PULONG)Message->Data.Raw)[0];             // +0x30
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];      // +0x34
    CHAR Name[64];
    ULONG Length;

    if (!PsxValidateClientPointer(Process, BufPtr, 16))
    {
        Message->Errno = 22;
        Message->ReturnValue = -1;
        return;
    }

    if (Gid == PSX_UID_NOBODY)
        RtlCopyMemory(Name, "nobody", sizeof("nobody"));
    else if (!PsxResolveIdName(Gid, Name, sizeof(Name)))
    {
        Message->Errno = 1;                     // EPERM
        Message->ReturnValue = -1;
        return;
    }

    Length = PsxFillGroup((PVOID)BufPtr, Gid, Name);
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// getgrnam(name) -- ApiNumber 0x3A. ANSI name at Data[0]; result buffer at Data[1].
// Faithful to sub_1F4C83A.
//
VOID
PsxSrvGetgrnam(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG_PTR NamePtr = ((PULONG)Message->Data.Raw)[0];     // +0x30
    ULONG_PTR BufPtr = ((PULONG)Message->Data.Raw)[1];      // +0x34
    PSTR Name = (PSTR)NamePtr;
    UCHAR SidBuffer[SECURITY_MAX_SID_SIZE];
    CHAR Domain[128];
    DWORD SidSize = sizeof(SidBuffer);
    DWORD DomainSize = sizeof(Domain);
    SID_NAME_USE Use;
    ULONG Gid, Length;

    if (!PsxValidateClientPointer(Process, NamePtr, 1) ||
        !PsxValidateClientPointer(Process, BufPtr, 16))
    {
        Message->Errno = 22;
        Message->ReturnValue = -1;
        return;
    }

    if (!LookupAccountNameA(NULL, Name, SidBuffer, &SidSize, Domain, &DomainSize, &Use))
    {
        Message->Errno = 1;                     // EPERM
        Message->ReturnValue = -1;
        return;
    }

    Gid = PsxSidToPosixId(SidBuffer);
    Length = PsxFillGroup((PVOID)BufPtr, Gid, Name);
    ((PULONG)Message->Data.Raw)[2] = Length;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// chown(path, uid, gid) -- ApiNumber 0x23. Sets the file's NT owner/group SIDs
// (reconstructed from the POSIX ids). uid at Data[2], gid at Data[3]. Faithful to
// sub_1F49805 (owner/group SIDs from ids, NtSetSecurityObject with OWNER|GROUP).
//
VOID
PsxSrvChown(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = *(PUNICODE_STRING)Message->Data.Raw;  // Data[0]/Data[1]
    ULONG Uid = ((PULONG)Message->Data.Raw)[2];                 // +0x38
    ULONG Gid = ((PULONG)Message->Data.Raw)[3];                 // +0x3C
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    SECURITY_DESCRIPTOR SecurityDescriptor;
    PSID OwnerSid = NULL;
    PSID GroupSid = NULL;
    HANDLE Handle = NULL;
    NTSTATUS Status;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length))
    {
        Message->Errno = 22;
        Message->ReturnValue = -1;
        return;
    }

    OwnerSid = PsxBuildSidFromId(Uid);
    GroupSid = PsxBuildSidFromId(Gid);
    if ((OwnerSid == NULL) || (GroupSid == NULL))
    {
        if (OwnerSid) RtlFreeHeap(RtlGetProcessHeap(), 0, OwnerSid);
        if (GroupSid) RtlFreeHeap(RtlGetProcessHeap(), 0, GroupSid);
        Message->Errno = 1;                     // EPERM
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, WRITE_OWNER | WRITE_DAC | READ_CONTROL | SYNCHRONIZE,
                        &ObjectAttributes, &IoStatusBlock,
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

//
// Register the SAM/LSA-backed identity handlers + chown into the dispatch table.
//
VOID
PsxInitIdentityOps(VOID)
{
    extern PPSX_API_HANDLER g_OpHandlers[];
    g_OpHandlers[PsxApiChown]    = PsxSrvChown;      // 0x23
    g_OpHandlers[PsxApiGetpwuid] = PsxSrvGetpwuid;   // 0x37
    g_OpHandlers[PsxApiGetpwnam] = PsxSrvGetpwnam;   // 0x38
    g_OpHandlers[PsxApiGetgrgid] = PsxSrvGetgrgid;   // 0x39
    g_OpHandlers[PsxApiGetgrnam] = PsxSrvGetgrnam;   // 0x3A
}
