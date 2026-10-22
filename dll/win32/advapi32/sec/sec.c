/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            lib/advapi32/sec/sec.c
 * PURPOSE:         Security descriptor functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 *                  Steven Edwards ( Steven_Ed4153@yahoo.com )
 *                  Andrew Greenwood ( silverblade_uk@hotmail.com )
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

#include <advapi32.h>
WINE_DEFAULT_DEBUG_CHANNEL(advapi);

/*
 * @implemented
 */
BOOL
WINAPI
GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                             PSECURITY_DESCRIPTOR_CONTROL pControl,
                             LPDWORD lpdwRevision)
{
    NTSTATUS Status;

    Status = RtlGetControlSecurityDescriptor(pSecurityDescriptor,
                                             pControl,
                                             (PULONG)lpdwRevision);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                          LPBOOL lpbDaclPresent,
                          PACL *pDacl,
                          LPBOOL lpbDaclDefaulted)
{
    BOOLEAN DaclPresent;
    BOOLEAN DaclDefaulted;
    NTSTATUS Status;

    Status = RtlGetDaclSecurityDescriptor(pSecurityDescriptor,
                                          &DaclPresent,
                                          pDacl,
                                          &DaclDefaulted);
    *lpbDaclPresent = (BOOL)DaclPresent;
    *lpbDaclDefaulted = (BOOL)DaclDefaulted;

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                           PSID *pGroup,
                           LPBOOL lpbGroupDefaulted)
{
    BOOLEAN GroupDefaulted;
    NTSTATUS Status;

    Status = RtlGetGroupSecurityDescriptor(pSecurityDescriptor,
                                           pGroup,
                                           &GroupDefaulted);
    *lpbGroupDefaulted = (BOOL)GroupDefaulted;

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                           PSID *pOwner,
                           LPBOOL lpbOwnerDefaulted)
{
    BOOLEAN OwnerDefaulted;
    NTSTATUS Status;

    Status = RtlGetOwnerSecurityDescriptor(pSecurityDescriptor,
                                           pOwner,
                                           &OwnerDefaulted);
    *lpbOwnerDefaulted = (BOOL)OwnerDefaulted;

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
DWORD
WINAPI
GetSecurityDescriptorRMControl(PSECURITY_DESCRIPTOR SecurityDescriptor,
                               PUCHAR RMControl)
{
    if (!RtlGetSecurityDescriptorRMControl(SecurityDescriptor,
                                           RMControl))
        return ERROR_INVALID_DATA;

    return ERROR_SUCCESS;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                          LPBOOL lpbSaclPresent,
                          PACL *pSacl,
                          LPBOOL lpbSaclDefaulted)
{
    BOOLEAN SaclPresent;
    BOOLEAN SaclDefaulted;
    NTSTATUS Status;

    Status = RtlGetSaclSecurityDescriptor(pSecurityDescriptor,
                                          &SaclPresent,
                                          pSacl,
                                          &SaclDefaulted);
    *lpbSaclPresent = (BOOL)SaclPresent;
    *lpbSaclDefaulted = (BOOL)SaclDefaulted;

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    BOOLEAN Result;

    Result = RtlValidSecurityDescriptor (pSecurityDescriptor);
    if (Result == FALSE)
        SetLastError(RtlNtStatusToDosError(STATUS_INVALID_SECURITY_DESCR));

    return (BOOL)Result;
}

/*
 * @implemented
 */
BOOL
WINAPI
MakeAbsoluteSD2(IN OUT PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor,
                OUT LPDWORD lpdwBufferSize)
{
    NTSTATUS Status;

    Status = RtlSelfRelativeToAbsoluteSD2(pSelfRelativeSecurityDescriptor,
                                          lpdwBufferSize);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
MakeSelfRelativeSD(PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor,
                   PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor,
                   LPDWORD lpdwBufferLength)
{
    NTSTATUS Status;

    Status = RtlAbsoluteToSelfRelativeSD(pAbsoluteSecurityDescriptor,
                                         pSelfRelativeSecurityDescriptor,
                                         (PULONG)lpdwBufferLength);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                             SECURITY_DESCRIPTOR_CONTROL ControlBitsOfInterest,
                             SECURITY_DESCRIPTOR_CONTROL ControlBitsToSet)
{
    NTSTATUS Status;

    Status = RtlSetControlSecurityDescriptor(pSecurityDescriptor,
                                             ControlBitsOfInterest,
                                             ControlBitsToSet);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                          BOOL bDaclPresent,
                          PACL pDacl,
                          BOOL bDaclDefaulted)
{
    NTSTATUS Status;

    Status = RtlSetDaclSecurityDescriptor(pSecurityDescriptor,
                                          bDaclPresent,
                                          pDacl,
                                          bDaclDefaulted);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                           PSID pGroup,
                           BOOL bGroupDefaulted)
{
    NTSTATUS Status;

    Status = RtlSetGroupSecurityDescriptor(pSecurityDescriptor,
                                           pGroup,
                                           bGroupDefaulted);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                           PSID pOwner,
                           BOOL bOwnerDefaulted)
{
    NTSTATUS Status;

    Status = RtlSetOwnerSecurityDescriptor(pSecurityDescriptor,
                                           pOwner,
                                           bOwnerDefaulted);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
DWORD
WINAPI
SetSecurityDescriptorRMControl(PSECURITY_DESCRIPTOR SecurityDescriptor,
                               PUCHAR RMControl)
{
    RtlSetSecurityDescriptorRMControl(SecurityDescriptor,
                                      RMControl);

    return ERROR_SUCCESS;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR pSecurityDescriptor,
                          BOOL bSaclPresent,
                          PACL pSacl,
                          BOOL bSaclDefaulted)
{
    NTSTATUS Status;

    Status = RtlSetSaclSecurityDescriptor(pSecurityDescriptor,
                                          bSaclPresent,
                                          pSacl,
                                          bSaclDefaulted);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented
 */
VOID
WINAPI
QuerySecurityAccessMask(IN SECURITY_INFORMATION SecurityInformation,
                        OUT LPDWORD DesiredAccess)
{
    *DesiredAccess = 0;

    if (SecurityInformation & (OWNER_SECURITY_INFORMATION |
                               GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION))
    {
        *DesiredAccess |= READ_CONTROL;
    }

    if (SecurityInformation & SACL_SECURITY_INFORMATION)
        *DesiredAccess |= ACCESS_SYSTEM_SECURITY;
}


/*
 * @implemented
 */
VOID
WINAPI
SetSecurityAccessMask(IN SECURITY_INFORMATION SecurityInformation,
                      OUT LPDWORD DesiredAccess)
{
    *DesiredAccess = 0;

    if (SecurityInformation & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION))
        *DesiredAccess |= WRITE_OWNER;

    if (SecurityInformation & DACL_SECURITY_INFORMATION)
        *DesiredAccess |= WRITE_DAC;

    if (SecurityInformation & SACL_SECURITY_INFORMATION)
        *DesiredAccess |= ACCESS_SYSTEM_SECURITY;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
ConvertToAutoInheritPrivateObjectSecurity(IN PSECURITY_DESCRIPTOR ParentDescriptor,
                                          IN PSECURITY_DESCRIPTOR CurrentSecurityDescriptor,
                                          OUT PSECURITY_DESCRIPTOR* NewSecurityDescriptor,
                                          IN GUID* ObjectType,
                                          IN BOOLEAN IsDirectoryObject,
                                          IN PGENERIC_MAPPING GenericMapping)
{
    UNIMPLEMENTED;
    return FALSE;
}


/* The SID of the user the caller runs as, from the thread token or else the process token */
static
DWORD
BasepGetCurrentUserSid(
    _Out_ PSID *Sid)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    HANDLE Token;
    DWORD Length;
    BOOL Success;

    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token) &&
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        return GetLastError();
    }

    Success = GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length);
    CloseHandle(Token);
    if (!Success)
        return GetLastError();

    Length = GetLengthSid(User->User.Sid);
    *Sid = LocalAlloc(LMEM_FIXED, Length);
    if (*Sid == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    CopySid(Length, *Sid, User->User.Sid);
    return ERROR_SUCCESS;
}

/* Looks a trustee name up; CURRENT_USER stands for the caller */
static
DWORD
BasepLookupTrusteeName(
    _In_ LPCWSTR Name,
    _Out_ PSID *Sid)
{
    DWORD SidLength = 0, DomainLength = 0;
    SID_NAME_USE Use;
    LPWSTR Domain;

    if (_wcsicmp(Name, L"CURRENT_USER") == 0)
        return BasepGetCurrentUserSid(Sid);

    LookupAccountNameW(NULL, Name, NULL, &SidLength, NULL, &DomainLength, &Use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return GetLastError();

    *Sid = LocalAlloc(LMEM_FIXED, SidLength);
    Domain = LocalAlloc(LMEM_FIXED, DomainLength * sizeof(WCHAR));
    if (*Sid == NULL || Domain == NULL)
    {
        LocalFree(*Sid);
        LocalFree(Domain);
        *Sid = NULL;
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    if (!LookupAccountNameW(NULL, Name, *Sid, &SidLength, Domain, &DomainLength, &Use))
    {
        LocalFree(*Sid);
        LocalFree(Domain);
        *Sid = NULL;
        return GetLastError();
    }

    LocalFree(Domain);
    return ERROR_SUCCESS;
}

/*
 * Gives the SID a trustee names. A SID taken from a name is allocated and
 * handed back in Allocated as well, so the caller can free it.
 */
static
DWORD
BasepTrusteeToSid(
    _In_ TRUSTEE_FORM Form,
    _In_ PVOID Value,
    _In_ BOOL Ansi,
    _Out_ PSID *Sid,
    _Out_ PSID *Allocated)
{
    UNICODE_STRING Unicode;
    ANSI_STRING String;
    DWORD Error;

    *Sid = NULL;
    *Allocated = NULL;

    switch (Form)
    {
        case TRUSTEE_IS_SID:
            *Sid = (PSID)Value;
            return ERROR_SUCCESS;

        case TRUSTEE_IS_OBJECTS_AND_SID:
            *Sid = ((POBJECTS_AND_SID)Value)->pSid;
            return ERROR_SUCCESS;

        case TRUSTEE_IS_OBJECTS_AND_NAME:
            Value = Ansi ? (PVOID)((POBJECTS_AND_NAME_A)Value)->ptstrName :
                           (PVOID)((POBJECTS_AND_NAME_W)Value)->ptstrName;
            break;

        case TRUSTEE_IS_NAME:
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    if (Value == NULL)
        return ERROR_INVALID_PARAMETER;

    if (!Ansi)
    {
        Error = BasepLookupTrusteeName((LPCWSTR)Value, Allocated);
    }
    else
    {
        RtlInitAnsiString(&String, (PCSZ)Value);
        if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&Unicode, &String, TRUE)))
            return ERROR_NOT_ENOUGH_MEMORY;
        Error = BasepLookupTrusteeName(Unicode.Buffer, Allocated);
        RtlFreeUnicodeString(&Unicode);
    }

    *Sid = *Allocated;
    return Error;
}

/*
 * Puts the new descriptor together in self relative form. Whatever the old
 * descriptor has is kept unless replaced, its DACL and SACL protection is
 * not, and both lists are always present, empty ones as NULL ACLs.
 */
static
DWORD
BasepBuildSecurityDescriptor(
    _In_opt_ PSID Owner,
    _In_opt_ PSID Group,
    _In_opt_ PACL Dacl,
    _In_opt_ PACL Sacl,
    _In_opt_ PSECURITY_DESCRIPTOR OldSD,
    _Out_ PULONG SizeNewSD,
    _Out_ PSECURITY_DESCRIPTOR *NewSD)
{
    SECURITY_DESCRIPTOR Absolute;
    PSECURITY_DESCRIPTOR Relative;
    BOOL Defaulted;
    DWORD Length = 0;

    if (OldSD != NULL)
    {
        if (Owner == NULL)
            GetSecurityDescriptorOwner(OldSD, &Owner, &Defaulted);
        if (Group == NULL)
            GetSecurityDescriptorGroup(OldSD, &Group, &Defaulted);
    }

    if (!InitializeSecurityDescriptor(&Absolute, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&Absolute, Owner, FALSE) ||
        !SetSecurityDescriptorGroup(&Absolute, Group, FALSE) ||
        !SetSecurityDescriptorDacl(&Absolute, TRUE, Dacl, FALSE) ||
        !SetSecurityDescriptorSacl(&Absolute, TRUE, Sacl, FALSE))
    {
        return GetLastError();
    }

    MakeSelfRelativeSD(&Absolute, NULL, &Length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return GetLastError();

    Relative = LocalAlloc(LMEM_FIXED, Length);
    if (Relative == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!MakeSelfRelativeSD(&Absolute, Relative, &Length))
    {
        LocalFree(Relative);
        return GetLastError();
    }

    *NewSD = Relative;
    *SizeNewSD = Length;
    return ERROR_SUCCESS;
}

/* The DACL and SACL of the old descriptor that new entries are merged into */
static
DWORD
BasepGetOldAcls(
    _In_opt_ PSECURITY_DESCRIPTOR OldSD,
    _Out_ PACL *Dacl,
    _Out_ PACL *Sacl)
{
    BOOL Present, Defaulted;

    *Dacl = NULL;
    *Sacl = NULL;
    if (OldSD == NULL)
        return ERROR_SUCCESS;

    if (!IsValidSecurityDescriptor(OldSD))
        return ERROR_INVALID_SECURITY_DESCR;

    if (!GetSecurityDescriptorDacl(OldSD, &Present, Dacl, &Defaulted) ||
        !GetSecurityDescriptorSacl(OldSD, &Present, Sacl, &Defaulted))
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
DWORD
WINAPI
BuildSecurityDescriptorW(
    _In_opt_ PTRUSTEE_W pOwner,
    _In_opt_ PTRUSTEE_W pGroup,
    _In_ ULONG cCountOfAccessEntries,
    _In_reads_opt_(cCountOfAccessEntries) PEXPLICIT_ACCESS_W pListOfAccessEntries,
    _In_ ULONG cCountOfAuditEntries,
    _In_reads_opt_(cCountOfAuditEntries) PEXPLICIT_ACCESS_W pListOfAuditEntries,
    _In_opt_ PSECURITY_DESCRIPTOR pOldSD,
    _Out_ PULONG pSizeNewSD,
    _Out_ PSECURITY_DESCRIPTOR *pNewSD)
{
    PSID Owner = NULL, Group = NULL, OwnerAllocated = NULL, GroupAllocated = NULL;
    PACL OldDacl, OldSacl, Dacl = NULL, Sacl = NULL;
    DWORD Error;

    if (pSizeNewSD == NULL || pNewSD == NULL ||
        (cCountOfAccessEntries != 0 && pListOfAccessEntries == NULL) ||
        (cCountOfAuditEntries != 0 && pListOfAuditEntries == NULL))
    {
        return ERROR_INVALID_PARAMETER;
    }

    Error = BasepGetOldAcls(pOldSD, &OldDacl, &OldSacl);
    if (Error == ERROR_SUCCESS && pOwner != NULL)
        Error = BasepTrusteeToSid(pOwner->TrusteeForm, pOwner->ptstrName, FALSE, &Owner, &OwnerAllocated);
    if (Error == ERROR_SUCCESS && pGroup != NULL)
        Error = BasepTrusteeToSid(pGroup->TrusteeForm, pGroup->ptstrName, FALSE, &Group, &GroupAllocated);
    if (Error == ERROR_SUCCESS && (cCountOfAccessEntries != 0 || OldDacl != NULL))
        Error = SetEntriesInAclW(cCountOfAccessEntries, pListOfAccessEntries, OldDacl, &Dacl);
    if (Error == ERROR_SUCCESS && (cCountOfAuditEntries != 0 || OldSacl != NULL))
        Error = SetEntriesInAclW(cCountOfAuditEntries, pListOfAuditEntries, OldSacl, &Sacl);
    if (Error == ERROR_SUCCESS)
        Error = BasepBuildSecurityDescriptor(Owner, Group, Dacl, Sacl, pOldSD, pSizeNewSD, pNewSD);

    LocalFree(OwnerAllocated);
    LocalFree(GroupAllocated);
    LocalFree(Dacl);
    LocalFree(Sacl);
    return Error;
}


/*
 * @implemented
 */
DWORD
WINAPI
BuildSecurityDescriptorA(
    _In_opt_ PTRUSTEE_A pOwner,
    _In_opt_ PTRUSTEE_A pGroup,
    _In_ ULONG cCountOfAccessEntries,
    _In_reads_opt_(cCountOfAccessEntries) PEXPLICIT_ACCESS_A pListOfAccessEntries,
    _In_ ULONG cCountOfAuditEntries,
    _In_reads_opt_(cCountOfAuditEntries) PEXPLICIT_ACCESS_A pListOfAuditEntries,
    _In_opt_ PSECURITY_DESCRIPTOR pOldSD,
    _Out_ PULONG pSizeNewSD,
    _Out_ PSECURITY_DESCRIPTOR *pNewSD)
{
    PSID Owner = NULL, Group = NULL, OwnerAllocated = NULL, GroupAllocated = NULL;
    PACL OldDacl, OldSacl, Dacl = NULL, Sacl = NULL;
    DWORD Error;

    if (pSizeNewSD == NULL || pNewSD == NULL ||
        (cCountOfAccessEntries != 0 && pListOfAccessEntries == NULL) ||
        (cCountOfAuditEntries != 0 && pListOfAuditEntries == NULL))
    {
        return ERROR_INVALID_PARAMETER;
    }

    Error = BasepGetOldAcls(pOldSD, &OldDacl, &OldSacl);
    if (Error == ERROR_SUCCESS && pOwner != NULL)
        Error = BasepTrusteeToSid(pOwner->TrusteeForm, pOwner->ptstrName, TRUE, &Owner, &OwnerAllocated);
    if (Error == ERROR_SUCCESS && pGroup != NULL)
        Error = BasepTrusteeToSid(pGroup->TrusteeForm, pGroup->ptstrName, TRUE, &Group, &GroupAllocated);
    if (Error == ERROR_SUCCESS && (cCountOfAccessEntries != 0 || OldDacl != NULL))
        Error = SetEntriesInAclA(cCountOfAccessEntries, pListOfAccessEntries, OldDacl, &Dacl);
    if (Error == ERROR_SUCCESS && (cCountOfAuditEntries != 0 || OldSacl != NULL))
        Error = SetEntriesInAclA(cCountOfAuditEntries, pListOfAuditEntries, OldSacl, &Sacl);
    if (Error == ERROR_SUCCESS)
        Error = BasepBuildSecurityDescriptor(Owner, Group, Dacl, Sacl, pOldSD, pSizeNewSD, pNewSD);

    LocalFree(OwnerAllocated);
    LocalFree(GroupAllocated);
    LocalFree(Dacl);
    LocalFree(Sacl);
    return Error;
}

/* EOF */
