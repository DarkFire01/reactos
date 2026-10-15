/*
 * ReactOS MARTA provider
 * Copyright (C) 2005 - 2006 ReactOS Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
/*
 * PROJECT:         ReactOS MARTA provider
 * FILE:            lib/ntmarta/ntmarta.c
 * PURPOSE:         ReactOS MARTA provider
 * PROGRAMMER:      Thomas Weidenmueller <w3seek@reactos.com>
 *
 * UPDATE HISTORY:
 *      07/26/2005  Created
 */

#include "ntmarta.h"

#define NDEBUG
#include <debug.h>

HINSTANCE hDllInstance;

/* Inheritance flags a caller may put on an ACE it sets explicitly */
#define ACCP_EXPLICIT_INHERIT_FLAGS (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | \
                                     NO_PROPAGATE_INHERIT_ACE | INHERIT_ONLY_ACE)

/* FIXME: Vista+ API */
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

/* FIXME: Vista+ API */
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

static ACCESS_MODE
AccpGetAceAccessMode(IN PACE_HEADER AceHeader)
{
    ACCESS_MODE Mode = NOT_USED_ACCESS;

    switch (AceHeader->AceType)
    {
        case ACCESS_ALLOWED_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
            Mode = GRANT_ACCESS;
            break;

        case ACCESS_DENIED_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
            Mode = DENY_ACCESS;
            break;

        case SYSTEM_AUDIT_ACE_TYPE:
        case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
            if (AceHeader->AceFlags & FAILED_ACCESS_ACE_FLAG)
                Mode = SET_AUDIT_FAILURE;
            else if (AceHeader->AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG)
                Mode = SET_AUDIT_SUCCESS;
            break;
    }

    return Mode;
}

static UINT
AccpGetAceStructureSize(IN PACE_HEADER AceHeader)
{
    UINT Size = 0;

    switch (AceHeader->AceType)
    {
        case ACCESS_ALLOWED_ACE_TYPE:
        case ACCESS_DENIED_ACE_TYPE:
            Size = FIELD_OFFSET(ACCESS_ALLOWED_ACE,
                                SidStart);
            break;
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_ACE_TYPE:
            Size = FIELD_OFFSET(ACCESS_ALLOWED_CALLBACK_ACE,
                                SidStart);
            break;
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_CALLBACK_OBJECT_ACE Ace = (PACCESS_ALLOWED_CALLBACK_OBJECT_ACE)AceHeader;
            Size = FIELD_OFFSET(ACCESS_ALLOWED_CALLBACK_OBJECT_ACE,
                                ObjectType);
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->ObjectType);
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->InheritedObjectType);
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_OBJECT_ACE Ace = (PACCESS_ALLOWED_OBJECT_ACE)AceHeader;
            Size = FIELD_OFFSET(ACCESS_ALLOWED_OBJECT_ACE,
                                ObjectType);
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->ObjectType);
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->InheritedObjectType);
            break;
        }

        case SYSTEM_AUDIT_ACE_TYPE:
            Size = FIELD_OFFSET(SYSTEM_AUDIT_ACE,
                                SidStart);
            break;
        case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
            Size = FIELD_OFFSET(SYSTEM_AUDIT_CALLBACK_ACE,
                                SidStart);
            break;
        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE Ace = (PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE)AceHeader;
            Size = FIELD_OFFSET(SYSTEM_AUDIT_CALLBACK_OBJECT_ACE,
                                ObjectType);
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->ObjectType);
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->InheritedObjectType);
            break;
        }
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_OBJECT_ACE Ace = (PSYSTEM_AUDIT_OBJECT_ACE)AceHeader;
            Size = FIELD_OFFSET(SYSTEM_AUDIT_OBJECT_ACE,
                                ObjectType);
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->ObjectType);
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
                Size += sizeof(Ace->InheritedObjectType);
            break;
        }

        case SYSTEM_MANDATORY_LABEL_ACE_TYPE:
            Size = FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE,
                                SidStart);
            break;
    }

    return Size;
}

static PSID
AccpGetAceSid(IN PACE_HEADER AceHeader)
{
    return (PSID)((ULONG_PTR)AceHeader + AccpGetAceStructureSize(AceHeader));
}

static ACCESS_MASK
AccpGetAceAccessMask(IN PACE_HEADER AceHeader)
{
    return *((PACCESS_MASK)(AceHeader + 1));
}

static BOOL
AccpIsObjectAce(IN PACE_HEADER AceHeader)
{
    BOOL Ret;

    switch (AceHeader->AceType)
    {
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
            Ret = TRUE;
            break;

        default:
            Ret = FALSE;
            break;
    }

    return Ret;
}

static DWORD
AccpGetTrusteeObjects(IN PTRUSTEE_W Trustee,
                      OUT GUID *pObjectTypeGuid  OPTIONAL,
                      OUT GUID *pInheritedObjectTypeGuid  OPTIONAL)
{
    DWORD Ret;

    switch (Trustee->TrusteeForm)
    {
        case TRUSTEE_IS_OBJECTS_AND_NAME:
        {
            POBJECTS_AND_NAME_W pOan = (POBJECTS_AND_NAME_W)Trustee->ptstrName;

            /* pOan->ObjectsPresent should always be 0 here because a previous
               call to AccpGetTrusteeSid should have rejected these trustees
               already. */
            ASSERT(pOan->ObjectsPresent == 0);

            Ret = pOan->ObjectsPresent;
            break;
        }

        case TRUSTEE_IS_OBJECTS_AND_SID:
        {
            POBJECTS_AND_SID pOas = (POBJECTS_AND_SID)Trustee->ptstrName;

            if (pObjectTypeGuid != NULL && pOas->ObjectsPresent & ACE_OBJECT_TYPE_PRESENT)
                *pObjectTypeGuid = pOas->ObjectTypeGuid;

            if (pInheritedObjectTypeGuid != NULL && pOas->ObjectsPresent & ACE_INHERITED_OBJECT_TYPE_PRESENT)
                *pInheritedObjectTypeGuid = pOas->InheritedObjectTypeGuid;

            Ret = pOas->ObjectsPresent;
            break;
        }

        default:
            /* Any other trustee forms have no objects attached... */
            Ret = 0;
            break;
    }

    return Ret;
}

static DWORD
AccpCalcNeededAceSize(IN PSID Sid,
                      IN DWORD ObjectsPresent)
{
    DWORD Ret;

    Ret = sizeof(ACE) + GetLengthSid(Sid);

    /* This routine calculates the generic size of the ACE needed.
       If no objects are present it is assumed that only a standard
       ACE is to be created. */

    if (ObjectsPresent & ACE_OBJECT_TYPE_PRESENT)
        Ret += sizeof(GUID);
    if (ObjectsPresent & ACE_INHERITED_OBJECT_TYPE_PRESENT)
        Ret += sizeof(GUID);

    if (ObjectsPresent != 0)
        Ret += sizeof(DWORD); /* Include the Flags member to make it an object ACE */

    return Ret;
}

static GUID*
AccpGetObjectAceObjectType(IN PACE_HEADER AceHeader)
{
    GUID *ObjectType = NULL;

    switch (AceHeader->AceType)
    {
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_CALLBACK_OBJECT_ACE Ace = (PACCESS_ALLOWED_CALLBACK_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                ObjectType = &Ace->ObjectType;
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_OBJECT_ACE Ace = (PACCESS_ALLOWED_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                ObjectType = &Ace->ObjectType;
            break;
        }

        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE Ace = (PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                ObjectType = &Ace->ObjectType;
            break;
        }
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_OBJECT_ACE Ace = (PSYSTEM_AUDIT_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                ObjectType = &Ace->ObjectType;
            break;
        }
    }

    return ObjectType;
}

static GUID*
AccpGetObjectAceInheritedObjectType(IN PACE_HEADER AceHeader)
{
    GUID *ObjectType = NULL;

    switch (AceHeader->AceType)
    {
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_CALLBACK_OBJECT_ACE Ace = (PACCESS_ALLOWED_CALLBACK_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
            {
                if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                    ObjectType = &Ace->InheritedObjectType;
                else
                    ObjectType = &Ace->ObjectType;
            }
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        {
            PACCESS_ALLOWED_OBJECT_ACE Ace = (PACCESS_ALLOWED_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
            {
                if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                    ObjectType = &Ace->InheritedObjectType;
                else
                    ObjectType = &Ace->ObjectType;
            }
            break;
        }

        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE Ace = (PSYSTEM_AUDIT_CALLBACK_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
            {
                if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                    ObjectType = &Ace->InheritedObjectType;
                else
                    ObjectType = &Ace->ObjectType;
            }
            break;
        }
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
        {
            PSYSTEM_AUDIT_OBJECT_ACE Ace = (PSYSTEM_AUDIT_OBJECT_ACE)AceHeader;
            if (Ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
            {
                if (Ace->Flags & ACE_OBJECT_TYPE_PRESENT)
                    ObjectType = &Ace->InheritedObjectType;
                else
                    ObjectType = &Ace->ObjectType;
            }
            break;
        }
    }

    return ObjectType;
}

static DWORD
AccpOpenLSAPolicyHandle(IN LPWSTR SystemName,
                        IN ACCESS_MASK DesiredAccess,
                        OUT PLSA_HANDLE pPolicyHandle)
{
    LSA_OBJECT_ATTRIBUTES LsaObjectAttributes = {0};
    LSA_UNICODE_STRING LsaSystemName, *psn;
    SIZE_T SystemNameLength;
    NTSTATUS Status;

    if (SystemName != NULL && SystemName[0] != L'\0')
    {
        SystemNameLength = wcslen(SystemName);
        if (SystemNameLength > UNICODE_STRING_MAX_CHARS)
        {
            return ERROR_INVALID_PARAMETER;
        }

        LsaSystemName.Buffer = SystemName;
        LsaSystemName.Length = (USHORT)SystemNameLength * sizeof(WCHAR);
        LsaSystemName.MaximumLength = LsaSystemName.Length + sizeof(WCHAR);
        psn = &LsaSystemName;
    }
    else
    {
        psn = NULL;
    }

    Status = LsaOpenPolicy(psn,
                           &LsaObjectAttributes,
                           DesiredAccess,
                           pPolicyHandle);
    if (!NT_SUCCESS(Status))
        return LsaNtStatusToWinError(Status);

    return ERROR_SUCCESS;
}

static LPWSTR
AccpGetTrusteeName(IN PTRUSTEE_W Trustee)
{
    switch (Trustee->TrusteeForm)
    {
        case TRUSTEE_IS_NAME:
            return Trustee->ptstrName;

        case TRUSTEE_IS_OBJECTS_AND_NAME:
            return ((POBJECTS_AND_NAME_W)Trustee->ptstrName)->ptstrName;

        default:
            return NULL;
    }
}

static DWORD
AccpLookupCurrentUser(OUT PSID *ppSid)
{
    DWORD Ret;
    CHAR Buffer[sizeof(TOKEN_USER) + sizeof(SID) + sizeof(DWORD)*SID_MAX_SUB_AUTHORITIES];
    DWORD Length;
    HANDLE Token;
    PSID pSid;

    *ppSid = NULL;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_READ, TRUE, &Token))
    {
        Ret = GetLastError();
        if (Ret != ERROR_NO_TOKEN)
        {
            return Ret;
        }

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &Token))
        {
            return GetLastError();
        }
    }

    Length = sizeof(Buffer);
    if (!GetTokenInformation(Token, TokenUser, Buffer, Length, &Length))
    {
        Ret = GetLastError();
        CloseHandle(Token);
        return Ret;
    }
    CloseHandle(Token);

    pSid = ((PTOKEN_USER)Buffer)->User.Sid;
    Length = GetLengthSid(pSid);
    *ppSid = LocalAlloc(LMEM_FIXED, Length);
    if (!*ppSid)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    CopyMemory(*ppSid, pSid, Length);

    return ERROR_SUCCESS;
}

static DWORD
AccpLookupSidByName(IN LSA_HANDLE PolicyHandle,
                    IN LPWSTR Name,
                    OUT PSID *pSid)
{
    NTSTATUS Status;
    LSA_UNICODE_STRING LsaNames[1];
    PLSA_REFERENCED_DOMAIN_LIST ReferencedDomains = NULL;
    PLSA_TRANSLATED_SID2 TranslatedSid = NULL;
    DWORD SidLen;
    SIZE_T NameLength;
    DWORD Ret = ERROR_SUCCESS;

    NameLength = wcslen(Name);
    if (NameLength > UNICODE_STRING_MAX_CHARS)
    {
        return ERROR_INVALID_PARAMETER;
    }

    LsaNames[0].Buffer = Name;
    LsaNames[0].Length = (USHORT)NameLength * sizeof(WCHAR);
    LsaNames[0].MaximumLength = LsaNames[0].Length + sizeof(WCHAR);

    Status = LsaLookupNames2(PolicyHandle,
                             0,
                             sizeof(LsaNames) / sizeof(LsaNames[0]),
                             LsaNames,
                             &ReferencedDomains,
                             &TranslatedSid);

    if (!NT_SUCCESS(Status))
        return LsaNtStatusToWinError(Status);

    if (TranslatedSid->Use == SidTypeUnknown || TranslatedSid->Use == SidTypeInvalid)
    {
        Ret = LsaNtStatusToWinError(STATUS_NONE_MAPPED); /* FIXME- what error code? */
        goto Cleanup;
    }

    SidLen = GetLengthSid(TranslatedSid->Sid);
    ASSERT(SidLen != 0);

    *pSid = LocalAlloc(LMEM_FIXED, (SIZE_T)SidLen);
    if (*pSid != NULL)
    {
        if (!CopySid(SidLen,
                     *pSid,
                     TranslatedSid->Sid))
        {
            Ret = GetLastError();

            LocalFree((HLOCAL)*pSid);
            *pSid = NULL;
        }
    }
    else
        Ret = ERROR_NOT_ENOUGH_MEMORY;

Cleanup:
    LsaFreeMemory(ReferencedDomains);
    LsaFreeMemory(TranslatedSid);

    return Ret;
}


static DWORD
AccpGetTrusteeSid(IN PTRUSTEE_W Trustee,
                  IN OUT PLSA_HANDLE pPolicyHandle,
                  OUT PSID *ppSid,
                  OUT BOOL *Allocated)
{
    DWORD Ret = ERROR_SUCCESS;
    LPWSTR TrusteeName;

    *ppSid = NULL;
    *Allocated = FALSE;

    /* Windows ignores this */
#if 0
    if (Trustee->pMultipleTrustee || Trustee->MultipleTrusteeOperation != NO_MULTIPLE_TRUSTEE)
    {
        /* This is currently not supported */
        return ERROR_INVALID_PARAMETER;
    }
#endif

    switch (Trustee->TrusteeForm)
    {
        case TRUSTEE_IS_OBJECTS_AND_NAME:
            if (((POBJECTS_AND_NAME_W)Trustee->ptstrName)->ObjectsPresent != 0)
            {
                /* This is not supported as there is no way to interpret the
                   strings provided, and we need GUIDs for the ACEs... */
                Ret = ERROR_INVALID_PARAMETER;
                break;
            }
            /* fall through */

        case TRUSTEE_IS_NAME:
            TrusteeName = AccpGetTrusteeName(Trustee);
            if (!wcscmp(TrusteeName, L"CURRENT_USER"))
            {
                Ret = AccpLookupCurrentUser(ppSid);
                if (Ret == ERROR_SUCCESS)
                {
                    ASSERT(*ppSid != NULL);
                    *Allocated = TRUE;
                }
                break;
            }

            if (*pPolicyHandle == NULL)
            {
                Ret = AccpOpenLSAPolicyHandle(NULL, /* FIXME - always local? */
                                              POLICY_LOOKUP_NAMES,
                                              pPolicyHandle);
                if (Ret != ERROR_SUCCESS)
                    return Ret;

                ASSERT(*pPolicyHandle != NULL);
            }

            Ret = AccpLookupSidByName(*pPolicyHandle,
                                      TrusteeName,
                                      ppSid);
            if (Ret == ERROR_SUCCESS)
            {
                ASSERT(*ppSid != NULL);
                *Allocated = TRUE;
            }
            break;

        case TRUSTEE_IS_OBJECTS_AND_SID:
            *ppSid = ((POBJECTS_AND_SID)Trustee->ptstrName)->pSid;
            break;

        case TRUSTEE_IS_SID:
            *ppSid = (PSID)Trustee->ptstrName;
            break;

        default:
            Ret = ERROR_INVALID_PARAMETER;
            break;
    }

    return Ret;
}


/**********************************************************************
 * AccRewriteGetHandleRights				EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccRewriteGetHandleRights(HANDLE handle,
                          SE_OBJECT_TYPE ObjectType,
                          SECURITY_INFORMATION SecurityInfo,
                          PSID* ppsidOwner,
                          PSID* ppsidGroup,
                          PACL* ppDacl,
                          PACL* ppSacl,
                          PSECURITY_DESCRIPTOR* ppSecurityDescriptor)
{
    PSECURITY_DESCRIPTOR pSD = NULL;
    ULONG SDSize = 0;
    NTSTATUS Status;
    DWORD LastErr;
    DWORD Ret;

    /* save the last error code */
    LastErr = GetLastError();

    do
    {
        Ret = ERROR_SUCCESS;

        /* allocate a buffer large enough to hold the
           security descriptor we need to return */
        SDSize += 0x100;
        if (pSD == NULL)
        {
            pSD = LocalAlloc(LMEM_FIXED,
                             (SIZE_T)SDSize);
        }
        else
        {
            PSECURITY_DESCRIPTOR newSD;

            newSD = LocalReAlloc((HLOCAL)pSD,
                                 (SIZE_T)SDSize,
                                 LMEM_MOVEABLE);
            if (newSD != NULL)
                pSD = newSD;
        }

        if (pSD == NULL)
        {
            Ret = GetLastError();
            break;
        }

        /* perform the actual query depending on the object type */
        switch (ObjectType)
        {
            case SE_REGISTRY_KEY:
            {
                Ret = (DWORD)RegGetKeySecurity((HKEY)handle,
                                               SecurityInfo,
                                               pSD,
                                               &SDSize);
                break;
            }

            case SE_FILE_OBJECT:
                /* FIXME - handle console handles? */
            case SE_KERNEL_OBJECT:
            {
                Status = NtQuerySecurityObject(handle,
                                               SecurityInfo,
                                               pSD,
                                               SDSize,
                                               &SDSize);
                if (!NT_SUCCESS(Status))
                {
                    Ret = RtlNtStatusToDosError(Status);
                }
                break;
            }

            case SE_SERVICE:
            {
                if (!QueryServiceObjectSecurity((SC_HANDLE)handle,
                                                SecurityInfo,
                                                pSD,
                                                SDSize,
                                                &SDSize))
                {
                    Ret = GetLastError();
                }
                break;
            }

            case SE_WINDOW_OBJECT:
            {
                if (!GetUserObjectSecurity(handle,
                                           &SecurityInfo,
                                           pSD,
                                           SDSize,
                                           &SDSize))
                {
                    Ret = GetLastError();
                }
                break;
            }

            default:
            {
                UNIMPLEMENTED;
                Ret = ERROR_CALL_NOT_IMPLEMENTED;
                break;
            }
        }

    } while (Ret == ERROR_INSUFFICIENT_BUFFER);

    if (Ret == ERROR_SUCCESS)
    {
        BOOL Present, Defaulted;

        if (SecurityInfo & OWNER_SECURITY_INFORMATION && ppsidOwner != NULL)
        {
            *ppsidOwner = NULL;
            if (!GetSecurityDescriptorOwner(pSD,
                                            ppsidOwner,
                                            &Defaulted))
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }

        if (SecurityInfo & GROUP_SECURITY_INFORMATION && ppsidGroup != NULL)
        {
            *ppsidGroup = NULL;
            if (!GetSecurityDescriptorGroup(pSD,
                                            ppsidGroup,
                                            &Defaulted))
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }

        if (SecurityInfo & DACL_SECURITY_INFORMATION && ppDacl != NULL)
        {
            *ppDacl = NULL;
            if (!GetSecurityDescriptorDacl(pSD,
                                           &Present,
                                           ppDacl,
                                           &Defaulted))
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }

        if (SecurityInfo & SACL_SECURITY_INFORMATION && ppSacl != NULL)
        {
            *ppSacl = NULL;
            if (!GetSecurityDescriptorSacl(pSD,
                                           &Present,
                                           ppSacl,
                                           &Defaulted))
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }

        *ppSecurityDescriptor = pSD;
    }
    else
    {
Cleanup:
        if (pSD != NULL)
        {
            LocalFree((HLOCAL)pSD);
        }
    }

    /* restore the last error code */
    SetLastError(LastErr);

    return Ret;
}


/**********************************************************************
 * AccRewriteSetHandleRights				EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccRewriteSetHandleRights(HANDLE handle,
                          SE_OBJECT_TYPE ObjectType,
                          SECURITY_INFORMATION SecurityInfo,
                          PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    NTSTATUS Status;
    DWORD LastErr;
    DWORD Ret = ERROR_SUCCESS;

    /* save the last error code */
    LastErr = GetLastError();

    /* set the security according to the object type */
    switch (ObjectType)
    {
        case SE_REGISTRY_KEY:
        {
            Ret = (DWORD)RegSetKeySecurity((HKEY)handle,
                                           SecurityInfo,
                                           pSecurityDescriptor);
            break;
        }

        case SE_FILE_OBJECT:
            /* FIXME - handle console handles? */
        case SE_KERNEL_OBJECT:
        {
            Status = NtSetSecurityObject(handle,
                                         SecurityInfo,
                                         pSecurityDescriptor);
            if (!NT_SUCCESS(Status))
            {
                Ret = RtlNtStatusToDosError(Status);
            }
            break;
        }

        case SE_SERVICE:
        {
            if (!SetServiceObjectSecurity((SC_HANDLE)handle,
                                          SecurityInfo,
                                          pSecurityDescriptor))
            {
                Ret = GetLastError();
            }
            break;
        }

        case SE_WINDOW_OBJECT:
        {
            if (!SetUserObjectSecurity(handle,
                                       &SecurityInfo,
                                       pSecurityDescriptor))
            {
                Ret = GetLastError();
            }
            break;
        }

        default:
        {
            UNIMPLEMENTED;
            Ret = ERROR_CALL_NOT_IMPLEMENTED;
            break;
        }
    }


    /* restore the last error code */
    SetLastError(LastErr);

    return Ret;
}


static DWORD
AccpOpenNamedObject(LPWSTR pObjectName,
                    SE_OBJECT_TYPE ObjectType,
                    SECURITY_INFORMATION SecurityInfo,
                    PHANDLE Handle,
                    PHANDLE Handle2,
                    BOOL Write)
{
    LPWSTR lpPath;
    NTSTATUS Status;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)0;
    DWORD Ret = ERROR_SUCCESS;

    /* determine the required access rights */
    switch (ObjectType)
    {
        case SE_REGISTRY_KEY:
        case SE_FILE_OBJECT:
        case SE_KERNEL_OBJECT:
        case SE_SERVICE:
        case SE_WINDOW_OBJECT:
            if (Write)
            {
                SetSecurityAccessMask(SecurityInfo,
                                      (PDWORD)&DesiredAccess);
            }
            else
            {
                QuerySecurityAccessMask(SecurityInfo,
                                        (PDWORD)&DesiredAccess);
            }
            break;

        default:
            break;
    }

    /* make a copy of the path if we're modifying the string */
    switch (ObjectType)
    {
        case SE_REGISTRY_KEY:
        case SE_SERVICE:
            lpPath = (LPWSTR)LocalAlloc(LMEM_FIXED,
                                        (wcslen(pObjectName) + 1) * sizeof(WCHAR));
            if (lpPath == NULL)
            {
                Ret = GetLastError();
                goto Cleanup;
            }

            wcscpy(lpPath,
                   pObjectName);
            break;

        default:
            lpPath = pObjectName;
            break;
    }

    /* open a handle to the path depending on the object type */
    switch (ObjectType)
    {
        case SE_FILE_OBJECT:
        {
            IO_STATUS_BLOCK IoStatusBlock;
            OBJECT_ATTRIBUTES ObjectAttributes;
            UNICODE_STRING FileName;

            if (!RtlDosPathNameToNtPathName_U(pObjectName,
                                              &FileName,
                                              NULL,
                                              NULL))
            {
                Ret = ERROR_INVALID_NAME;
                goto Cleanup;
            }

            InitializeObjectAttributes(&ObjectAttributes,
                                       &FileName,
                                       OBJ_CASE_INSENSITIVE,
                                       NULL,
                                       NULL);

            Status = NtOpenFile(Handle,
                                DesiredAccess | SYNCHRONIZE,
                                &ObjectAttributes,
                                &IoStatusBlock,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_SYNCHRONOUS_IO_NONALERT);

            RtlFreeHeap(RtlGetProcessHeap(),
                        0,
                        FileName.Buffer);

            if (!NT_SUCCESS(Status))
            {
                Ret = RtlNtStatusToDosError(Status);
            }
            break;
        }

        case SE_REGISTRY_KEY:
        {
            static const struct
            {
                HKEY hRootKey;
                LPCWSTR szRootKey;
            } AccRegRootKeys[] =
            {
                {HKEY_CLASSES_ROOT, L"CLASSES_ROOT"},
                {HKEY_CURRENT_USER, L"CURRENT_USER"},
                {HKEY_LOCAL_MACHINE, L"MACHINE"},
                {HKEY_USERS, L"USERS"},
                {HKEY_CURRENT_CONFIG, L"CONFIG"},
            };
            LPWSTR lpMachineName, lpRootKeyName, lpKeyName;
            HKEY hRootKey = NULL;
            UINT i;

            /* parse the registry path */
            if (lpPath[0] == L'\\' && lpPath[1] == L'\\')
            {
                lpMachineName = lpPath;

                lpRootKeyName = wcschr(lpPath + 2,
                                       L'\\');
                if (lpRootKeyName == NULL)
                    goto ParseRegErr;
                else
                    *(lpRootKeyName++) = L'\0';
            }
            else
            {
                lpMachineName = NULL;
                lpRootKeyName = lpPath;
            }

            lpKeyName = wcschr(lpRootKeyName,
                               L'\\');
            if (lpKeyName != NULL)
            {
                *(lpKeyName++) = L'\0';
            }

            for (i = 0;
                 i != sizeof(AccRegRootKeys) / sizeof(AccRegRootKeys[0]);
                 i++)
            {
                if (!_wcsicmp(lpRootKeyName,
                             AccRegRootKeys[i].szRootKey))
                {
                    hRootKey = AccRegRootKeys[i].hRootKey;
                    break;
                }
            }

            if (hRootKey == NULL)
            {
ParseRegErr:
                /* FIXME - right error code? */
                Ret = ERROR_INVALID_PARAMETER;
                goto Cleanup;
            }

            /* open the registry key */
            if (lpMachineName != NULL)
            {
                Ret = RegConnectRegistry(lpMachineName,
                                         hRootKey,
                                         (PHKEY)Handle2);

                if (Ret != ERROR_SUCCESS)
                    goto Cleanup;

                hRootKey = (HKEY)(*Handle2);
            }

            Ret = RegOpenKeyEx(hRootKey,
                               lpKeyName,
                               0,
                               (REGSAM)DesiredAccess,
                               (PHKEY)Handle);
            if (Ret != ERROR_SUCCESS)
            {
                if (*Handle2 != NULL)
                {
                    RegCloseKey((HKEY)(*Handle2));
                }

                goto Cleanup;
            }
            break;
        }

        case SE_SERVICE:
        {
            LPWSTR lpServiceName, lpMachineName;

            /* parse the service path */
            if (lpPath[0] == L'\\' && lpPath[1] == L'\\')
            {
                DesiredAccess |= SC_MANAGER_CONNECT;

                lpMachineName = lpPath;

                lpServiceName = wcschr(lpPath + 2,
                                       L'\\');
                if (lpServiceName == NULL)
                {
                    /* FIXME - right error code? */
                    Ret = ERROR_INVALID_PARAMETER;
                    goto Cleanup;
                }
                else
                    *(lpServiceName++) = L'\0';
            }
            else
            {
                lpMachineName = NULL;
                lpServiceName = lpPath;
            }

            /* open the service */
            *Handle2 = (HANDLE)OpenSCManager(lpMachineName,
                                             NULL,
                                             (DWORD)DesiredAccess);
            if (*Handle2 == NULL)
            {
                Ret = GetLastError();
                ASSERT(Ret != ERROR_SUCCESS);
                goto Cleanup;
            }

            DesiredAccess &= ~SC_MANAGER_CONNECT;
            *Handle = (HANDLE)OpenService((SC_HANDLE)(*Handle2),
                                          lpServiceName,
                                          (DWORD)DesiredAccess);
            if (*Handle == NULL)
            {
                Ret = GetLastError();
                ASSERT(Ret != ERROR_SUCCESS);
                ASSERT(*Handle2 != NULL);
                CloseServiceHandle((SC_HANDLE)(*Handle2));

                goto Cleanup;
            }
            break;
        }

        default:
        {
            UNIMPLEMENTED;
            Ret = ERROR_CALL_NOT_IMPLEMENTED;
            break;
        }
    }

Cleanup:
    if (lpPath != NULL && lpPath != pObjectName)
    {
        LocalFree((HLOCAL)lpPath);
    }

    return Ret;
}


static VOID
AccpCloseObjectHandle(SE_OBJECT_TYPE ObjectType,
                      HANDLE Handle,
                      HANDLE Handle2)
{
    ASSERT(Handle != NULL);

    /* close allocated handles depending on the object type */
    switch (ObjectType)
    {
        case SE_REGISTRY_KEY:
            RegCloseKey((HKEY)Handle);
            if (Handle2 != NULL)
                RegCloseKey((HKEY)Handle2);
            break;

        case SE_FILE_OBJECT:
            NtClose(Handle);
            break;

        case SE_KERNEL_OBJECT:
        case SE_WINDOW_OBJECT:
            CloseHandle(Handle);
            break;

        case SE_SERVICE:
            CloseServiceHandle((SC_HANDLE)Handle);
            ASSERT(Handle2 != NULL);
            CloseServiceHandle((SC_HANDLE)Handle2);
            break;

        default:
            break;
    }
}


/**********************************************************************
 * AccRewriteGetNamedRights				EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccRewriteGetNamedRights(LPWSTR pObjectName,
                         SE_OBJECT_TYPE ObjectType,
                         SECURITY_INFORMATION SecurityInfo,
                         PSID* ppsidOwner,
                         PSID* ppsidGroup,
                         PACL* ppDacl,
                         PACL* ppSacl,
                         PSECURITY_DESCRIPTOR* ppSecurityDescriptor)
{
    HANDLE Handle = NULL;
    HANDLE Handle2 = NULL;
    DWORD LastErr;
    DWORD Ret;

    /* save the last error code */
    LastErr = GetLastError();

    /* create the handle */
    Ret = AccpOpenNamedObject(pObjectName,
                              ObjectType,
                              SecurityInfo,
                              &Handle,
                              &Handle2,
                              FALSE);

    if (Ret == ERROR_SUCCESS)
    {
        ASSERT(Handle != NULL);

        /* perform the operation */
        Ret = AccRewriteGetHandleRights(Handle,
                                        ObjectType,
                                        SecurityInfo,
                                        ppsidOwner,
                                        ppsidGroup,
                                        ppDacl,
                                        ppSacl,
                                        ppSecurityDescriptor);

        /* close opened handles */
        AccpCloseObjectHandle(ObjectType,
                              Handle,
                              Handle2);
    }

    /* restore the last error code */
    SetLastError(LastErr);

    return Ret;
}


typedef enum _ACCP_ACE_GROUP
{
    AccpExplicitDenyAce,
    AccpExplicitOtherAce,
    AccpInheritedAce
} ACCP_ACE_GROUP;

static
ACCP_ACE_GROUP
AccpGetAceGroup(
    _In_ PACE_HEADER AceHeader)
{
    if (AceHeader->AceFlags & INHERITED_ACE)
        return AccpInheritedAce;

    if (AccpGetAceAccessMode(AceHeader) == DENY_ACCESS)
        return AccpExplicitDenyAce;

    return AccpExplicitOtherAce;
}

/** @brief Appends the ACEs of OldAcl that are still kept and belong to Group. */
static
BOOL
AccpCopyKeptAces(
    _Inout_ PACL NewAcl,
    _In_ PACL OldAcl,
    _In_reads_(AceCount) const BOOLEAN *KeepAce,
    _In_ ULONG AceCount,
    _In_ ACCP_ACE_GROUP Group)
{
    PACE_HEADER AceHeader;
    ULONG Index;

    for (Index = 0; Index < AceCount; Index++)
    {
        if (!KeepAce[Index])
            continue;

        if (!GetAce(OldAcl, Index, (PVOID*)&AceHeader))
            return FALSE;

        if (AccpGetAceGroup(AceHeader) != Group)
            continue;

        if (!AddAce(NewAcl, OldAcl->AclRevision, MAXDWORD, AceHeader, AceHeader->AceSize))
            return FALSE;
    }

    return TRUE;
}

static
BOOL
AccpAddKnownAce(
    _Inout_ PACL Acl,
    _In_ UCHAR AceType,
    _In_ UCHAR AceFlags,
    _In_ ACCESS_MASK Mask,
    _In_ PSID Sid)
{
    if (AceType == ACCESS_DENIED_ACE_TYPE)
        return AddAccessDeniedAceEx(Acl, Acl->AclRevision, AceFlags, Mask, Sid);

    return AddAccessAllowedAceEx(Acl, Acl->AclRevision, AceFlags, Mask, Sid);
}

/**
 * @brief
 * Appends to Acl the ACEs that an object below a directory with ParentDacl
 * inherits from it.
 */
static
BOOL
AccpAddInheritedAces(
    _Inout_ PACL Acl,
    _In_ PACL ParentDacl,
    _In_ BOOL IsDirectory,
    _In_opt_ PSID Owner,
    _In_opt_ PSID Group)
{
    static GENERIC_MAPPING FileMapping = {FILE_GENERIC_READ,
                                          FILE_GENERIC_WRITE,
                                          FILE_GENERIC_EXECUTE,
                                          FILE_ALL_ACCESS};
    SID CreatorOwner = {SID_REVISION, 1, {SECURITY_CREATOR_SID_AUTHORITY}, {SECURITY_CREATOR_OWNER_RID}};
    SID CreatorGroup = {SID_REVISION, 1, {SECURITY_CREATOR_SID_AUTHORITY}, {SECURITY_CREATOR_GROUP_RID}};
    PACCESS_ALLOWED_ACE Ace;
    ACCESS_MASK Mask;
    PSID AceSid;
    PSID EffectiveSid;
    UCHAR InheritBits;
    BOOL Effective;
    BOOL Inheritable;
    ULONG Index;

    for (Index = 0; Index < ParentDacl->AceCount; Index++)
    {
        if (!GetAce(ParentDacl, Index, (PVOID*)&Ace))
            return FALSE;

        if (Ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE &&
            Ace->Header.AceType != ACCESS_DENIED_ACE_TYPE)
        {
            continue;
        }

        InheritBits = Ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);

        if (IsDirectory)
        {
            /* A subdirectory uses container ACEs and passes object ACEs further down */
            Effective = (InheritBits & CONTAINER_INHERIT_ACE) != 0;
            Inheritable = InheritBits && !(Ace->Header.AceFlags & NO_PROPAGATE_INHERIT_ACE);
        }
        else
        {
            Effective = (InheritBits & OBJECT_INHERIT_ACE) != 0;
            Inheritable = FALSE;
        }

        if (!Effective && !Inheritable)
            continue;

        AceSid = (PSID)&Ace->SidStart;
        EffectiveSid = AceSid;
        if (Owner && EqualSid(AceSid, &CreatorOwner))
            EffectiveSid = Owner;
        else if (Group && EqualSid(AceSid, &CreatorGroup))
            EffectiveSid = Group;

        Mask = Ace->Mask;
        MapGenericMask(&Mask, &FileMapping);

        /* An ACE that changes when applied here also needs an untouched copy for the children */
        if (Effective && Inheritable && Mask == Ace->Mask && EffectiveSid == AceSid)
        {
            if (!AccpAddKnownAce(Acl, Ace->Header.AceType, INHERITED_ACE | InheritBits, Mask, AceSid))
                return FALSE;

            continue;
        }

        if (Effective &&
            !AccpAddKnownAce(Acl, Ace->Header.AceType, INHERITED_ACE, Mask, EffectiveSid))
        {
            return FALSE;
        }

        if (Inheritable &&
            !AccpAddKnownAce(Acl,
                             Ace->Header.AceType,
                             INHERITED_ACE | INHERIT_ONLY_ACE | InheritBits,
                             Ace->Mask,
                             AceSid))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief
 * Builds the DACL a file really gets when its DACL is set without protection:
 * the explicit ACEs from the caller followed by what the parent directory hands down.
 *
 * @return ERROR_SUCCESS with *MergedDacl set to NULL when nothing has to be merged.
 */
static
DWORD
AccpMergeParentDacl(
    _In_ LPWSTR pObjectName,
    _In_ SECURITY_INFORMATION SecurityInfo,
    _In_ PSECURITY_DESCRIPTOR pSecurityDescriptor,
    _Outptr_result_maybenull_ PACL *MergedDacl)
{
    PSECURITY_DESCRIPTOR ObjectSd = NULL;
    PSECURITY_DESCRIPTOR ParentSd = NULL;
    SECURITY_DESCRIPTOR_CONTROL Control;
    ACL_SIZE_INFORMATION SizeInfo;
    PACE_HEADER AceHeader;
    LPWSTR ParentName = NULL;
    LPWSTR Separator;
    PACL Dacl;
    PACL ParentDacl;
    PACL NewDacl = NULL;
    PSID Owner = NULL;
    PSID Group = NULL;
    PSID SdOwner;
    PSID SdGroup;
    BOOL Present;
    BOOL Defaulted;
    DWORD Revision;
    DWORD Attributes;
    DWORD AclSize;
    SIZE_T Length;
    ULONG Index;
    DWORD Ret = ERROR_SUCCESS;

    *MergedDacl = NULL;

    if (!(SecurityInfo & DACL_SECURITY_INFORMATION) ||
        (SecurityInfo & PROTECTED_DACL_SECURITY_INFORMATION))
    {
        return ERROR_SUCCESS;
    }

    /* A NULL DACL already grants everything */
    if (!GetSecurityDescriptorDacl(pSecurityDescriptor, &Present, &Dacl, &Defaulted) ||
        !Present || !Dacl)
    {
        return ERROR_SUCCESS;
    }

    if (AccRewriteGetNamedRights(pObjectName,
                                 SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                 &Owner,
                                 &Group,
                                 NULL,
                                 NULL,
                                 &ObjectSd) != ERROR_SUCCESS)
    {
        return ERROR_SUCCESS;
    }

    /* Without an explicit request the object keeps its current protection */
    if (!(SecurityInfo & UNPROTECTED_DACL_SECURITY_INFORMATION) &&
        GetSecurityDescriptorControl(ObjectSd, &Control, &Revision) &&
        (Control & SE_DACL_PROTECTED))
    {
        goto Cleanup;
    }

    if ((SecurityInfo & OWNER_SECURITY_INFORMATION) &&
        GetSecurityDescriptorOwner(pSecurityDescriptor, &SdOwner, &Defaulted) && SdOwner)
    {
        Owner = SdOwner;
    }

    if ((SecurityInfo & GROUP_SECURITY_INFORMATION) &&
        GetSecurityDescriptorGroup(pSecurityDescriptor, &SdGroup, &Defaulted) && SdGroup)
    {
        Group = SdGroup;
    }

    /* Find the parent directory, a volume root has none */
    Length = wcslen(pObjectName);
    ParentName = LocalAlloc(LMEM_FIXED, (Length + 1) * sizeof(WCHAR));
    if (!ParentName)
    {
        Ret = ERROR_NOT_ENOUGH_MEMORY;
        goto Cleanup;
    }
    wcscpy(ParentName, pObjectName);

    while (Length > 0 && (ParentName[Length - 1] == L'\\' || ParentName[Length - 1] == L'/'))
        ParentName[--Length] = UNICODE_NULL;

    Separator = max(wcsrchr(ParentName, L'\\'), wcsrchr(ParentName, L'/'));
    if (!Separator)
        goto Cleanup;

    /* Keep the separator when the parent is a root such as C:\ */
    if (Separator == ParentName || Separator[-1] == L':')
        Separator[1] = UNICODE_NULL;
    else
        Separator[0] = UNICODE_NULL;

    if (AccRewriteGetNamedRights(ParentName,
                                 SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION,
                                 NULL,
                                 NULL,
                                 &ParentDacl,
                                 NULL,
                                 &ParentSd) != ERROR_SUCCESS ||
        !ParentDacl)
    {
        goto Cleanup;
    }

    Attributes = GetFileAttributesW(pObjectName);

    /* Every parent ACE can split in two and pick up an owner or group SID */
    AclSize = Dacl->AclSize + 2 * ParentDacl->AclSize +
              2 * ParentDacl->AceCount * SECURITY_MAX_SID_SIZE;

    NewDacl = LocalAlloc(LMEM_FIXED, AclSize);
    if (!NewDacl)
    {
        Ret = ERROR_NOT_ENOUGH_MEMORY;
        goto Cleanup;
    }

    if (!InitializeAcl(NewDacl, AclSize, max(Dacl->AclRevision, ACL_REVISION)) ||
        !GetAclInformation(Dacl, &SizeInfo, sizeof(SizeInfo), AclSizeInformation))
    {
        Ret = GetLastError();
        goto Cleanup;
    }

    /* Keep what the caller set explicitly, stale inherited entries get rebuilt below */
    for (Index = 0; Index < SizeInfo.AceCount; Index++)
    {
        if (!GetAce(Dacl, Index, (PVOID*)&AceHeader))
        {
            Ret = GetLastError();
            goto Cleanup;
        }

        if (AceHeader->AceFlags & INHERITED_ACE)
            continue;

        if (!AddAce(NewDacl, Dacl->AclRevision, MAXDWORD, AceHeader, AceHeader->AceSize))
        {
            Ret = GetLastError();
            goto Cleanup;
        }
    }

    if (!AccpAddInheritedAces(NewDacl,
                              ParentDacl,
                              Attributes != INVALID_FILE_ATTRIBUTES &&
                                  (Attributes & FILE_ATTRIBUTE_DIRECTORY),
                              Owner,
                              Group))
    {
        Ret = GetLastError();
        goto Cleanup;
    }

    *MergedDacl = NewDacl;
    NewDacl = NULL;

Cleanup:
    if (NewDacl)
        LocalFree(NewDacl);

    if (ParentSd)
        LocalFree(ParentSd);

    if (ParentName)
        LocalFree(ParentName);

    LocalFree(ObjectSd);

    return Ret;
}

/**********************************************************************
 * AccRewriteSetNamedRights				EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccRewriteSetNamedRights(LPWSTR pObjectName,
                         SE_OBJECT_TYPE ObjectType,
                         SECURITY_INFORMATION SecurityInfo,
                         PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    HANDLE Handle = NULL;
    HANDLE Handle2 = NULL;
    SECURITY_DESCRIPTOR MergedSd;
    PACL MergedDacl = NULL;
    DWORD LastErr;
    DWORD Ret;

    /* save the last error code */
    LastErr = GetLastError();

    /* an unprotected file DACL also carries what the parent directory hands down */
    if (ObjectType == SE_FILE_OBJECT &&
        !(((PISECURITY_DESCRIPTOR)pSecurityDescriptor)->Control & SE_SELF_RELATIVE))
    {
        Ret = AccpMergeParentDacl(pObjectName,
                                  SecurityInfo,
                                  pSecurityDescriptor,
                                  &MergedDacl);
        if (Ret != ERROR_SUCCESS)
            goto Exit;

        if (MergedDacl)
        {
            MergedSd = *(PISECURITY_DESCRIPTOR)pSecurityDescriptor;
            MergedSd.Dacl = MergedDacl;
            MergedSd.Control |= SE_DACL_AUTO_INHERITED;
            pSecurityDescriptor = &MergedSd;
        }
    }

    /* create the handle */
    Ret = AccpOpenNamedObject(pObjectName,
                              ObjectType,
                              SecurityInfo,
                              &Handle,
                              &Handle2,
                              TRUE);

    if (Ret == ERROR_SUCCESS)
    {
        ASSERT(Handle != NULL);

        /* perform the operation */
        Ret = AccRewriteSetHandleRights(Handle,
                                        ObjectType,
                                        SecurityInfo,
                                        pSecurityDescriptor);

        /* close opened handles */
        AccpCloseObjectHandle(ObjectType,
                              Handle,
                              Handle2);
    }

Exit:
    if (MergedDacl)
        LocalFree(MergedDacl);

    /* restore the last error code */
    SetLastError(LastErr);

    return Ret;
}


/**********************************************************************
 * AccRewriteSetEntriesInAcl				EXPORTED
 *
 * @implemented
 */
DWORD WINAPI
AccRewriteSetEntriesInAcl(ULONG cCountOfExplicitEntries,
                          PEXPLICIT_ACCESS_W pListOfExplicitEntries,
                          PACL OldAcl,
                          PACL* NewAcl)
{
    PACL pNew = NULL;
    ACL_SIZE_INFORMATION SizeInformation;
    PACE_HEADER pAce;
    BOOLEAN KeepAceBuf[8];
    BOOLEAN *pKeepAce = NULL;
    GUID ObjectTypeGuid, InheritedObjectTypeGuid;
    DWORD ObjectsPresent;
    BOOL needToClean;
    PSID pSid1, pSid2;
    ULONG i, j;
    LSA_HANDLE PolicyHandle = NULL;
    BOOL bRet;
    DWORD LastErr;
    DWORD Ret = ERROR_SUCCESS;

    /* save the last error code */
    LastErr = GetLastError();

    *NewAcl = NULL;

    /* Get information about previous ACL */
    if (OldAcl)
    {
        if (!GetAclInformation(OldAcl, &SizeInformation, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation))
        {
            Ret = GetLastError();
            goto Cleanup;
        }

        if (SizeInformation.AceCount > sizeof(KeepAceBuf) / sizeof(KeepAceBuf[0]))
        {
            pKeepAce = (BOOLEAN *)LocalAlloc(LMEM_FIXED, SizeInformation.AceCount * sizeof(*pKeepAce));
            if (!pKeepAce)
            {
                Ret = ERROR_NOT_ENOUGH_MEMORY;
                goto Cleanup;
            }
        }
        else
            pKeepAce = KeepAceBuf;

        memset(pKeepAce, TRUE, SizeInformation.AceCount * sizeof(*pKeepAce));
    }
    else
    {
        ZeroMemory(&SizeInformation, sizeof(ACL_SIZE_INFORMATION));
        SizeInformation.AclBytesInUse = sizeof(ACL);
    }

    /* Get size required for new entries */
    for (i = 0; i < cCountOfExplicitEntries; i++)
    {
        Ret = AccpGetTrusteeSid(&pListOfExplicitEntries[i].Trustee,
                                &PolicyHandle,
                                &pSid1,
                                &needToClean);
        if (Ret != ERROR_SUCCESS)
            goto Cleanup;

        ObjectsPresent = AccpGetTrusteeObjects(&pListOfExplicitEntries[i].Trustee,
                                               NULL,
                                               NULL);

        switch (pListOfExplicitEntries[i].grfAccessMode)
        {
            case REVOKE_ACCESS:
            case SET_ACCESS:
                /* Discard all accesses for the trustee... */
                for (j = 0; j < SizeInformation.AceCount; j++)
                {
                    if (!pKeepAce[j])
                        continue;
                    if (!GetAce(OldAcl, j, (PVOID*)&pAce))
                    {
                        Ret = GetLastError();
                        goto Cleanup;
                    }

                    pSid2 = AccpGetAceSid(pAce);
                    if (RtlEqualSid(pSid1, pSid2))
                    {
                        pKeepAce[j] = FALSE;
                        SizeInformation.AclBytesInUse -= pAce->AceSize;
                    }
                }
                if (pListOfExplicitEntries[i].grfAccessMode == REVOKE_ACCESS)
                    break;
                /* ...and replace by the current access */
            case GRANT_ACCESS:
            case DENY_ACCESS:
                /* Add to ACL */
                SizeInformation.AclBytesInUse += AccpCalcNeededAceSize(pSid1, ObjectsPresent);
                break;
            case SET_AUDIT_SUCCESS:
            case SET_AUDIT_FAILURE:
                /* FIXME */
                DPRINT1("Case not implemented!\n");
                break;
            default:
                DPRINT1("Unknown access mode 0x%x. Ignoring it\n", pListOfExplicitEntries[i].grfAccessMode);
                break;
        }

        if (needToClean)
            LocalFree((HLOCAL)pSid1);
    }

    /* Succeed, if no ACL needs to be allocated */
    if (SizeInformation.AclBytesInUse == 0)
        goto Cleanup;

    /* OK, now create the new ACL */
    DPRINT("Allocating %u bytes for the new ACL\n", SizeInformation.AclBytesInUse);
    pNew = (PACL)LocalAlloc(LMEM_FIXED, SizeInformation.AclBytesInUse);
    if (!pNew)
    {
        Ret = ERROR_NOT_ENOUGH_MEMORY;
        goto Cleanup;
    }
    if (!InitializeAcl(pNew, SizeInformation.AclBytesInUse, ACL_REVISION))
    {
        Ret = GetLastError();
        goto Cleanup;
    }

    /* Fill it */
    /* 1a) New audit entries (SET_AUDIT_SUCCESS, SET_AUDIT_FAILURE) */
    /* FIXME */

    /* 1b) Existing audit entries */
    /* FIXME */

    /* 2a) New denied entries (DENY_ACCESS) */
    for (i = 0; i < cCountOfExplicitEntries; i++)
    {
        if (pListOfExplicitEntries[i].grfAccessMode == DENY_ACCESS)
        {
            Ret = AccpGetTrusteeSid(&pListOfExplicitEntries[i].Trustee,
                                    &PolicyHandle,
                                    &pSid1,
                                    &needToClean);
            if (Ret != ERROR_SUCCESS)
                goto Cleanup;

            ObjectsPresent = AccpGetTrusteeObjects(&pListOfExplicitEntries[i].Trustee,
                                                   &ObjectTypeGuid,
                                                   &InheritedObjectTypeGuid);

            if (ObjectsPresent == 0)
            {
                bRet = AddAccessDeniedAceEx(pNew,
                                            ACL_REVISION,
                                            pListOfExplicitEntries[i].grfInheritance & ACCP_EXPLICIT_INHERIT_FLAGS,
                                            pListOfExplicitEntries[i].grfAccessPermissions,
                                            pSid1);
            }
            else
            {
                /* FIXME: Call AddAccessDeniedObjectAce */
                DPRINT1("Object ACEs not yet supported!\n");
                SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
                bRet = FALSE;
            }

            if (needToClean) LocalFree((HLOCAL)pSid1);
            if (!bRet)
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }
    }

    /* 2b) Existing denied entries */
    if (OldAcl &&
        !AccpCopyKeptAces(pNew, OldAcl, pKeepAce, SizeInformation.AceCount, AccpExplicitDenyAce))
    {
        Ret = GetLastError();
        goto Cleanup;
    }

    /* 3a) New allow entries (GRANT_ACCESS, SET_ACCESS) */
    for (i = 0; i < cCountOfExplicitEntries; i++)
    {
        if (pListOfExplicitEntries[i].grfAccessMode == SET_ACCESS ||
            pListOfExplicitEntries[i].grfAccessMode == GRANT_ACCESS)
        {
            Ret = AccpGetTrusteeSid(&pListOfExplicitEntries[i].Trustee,
                                    &PolicyHandle,
                                    &pSid1,
                                    &needToClean);
            if (Ret != ERROR_SUCCESS)
                goto Cleanup;

            ObjectsPresent = AccpGetTrusteeObjects(&pListOfExplicitEntries[i].Trustee,
                                                   &ObjectTypeGuid,
                                                   &InheritedObjectTypeGuid);

            if (ObjectsPresent == 0)
            {
                bRet = AddAccessAllowedAceEx(pNew,
                                             ACL_REVISION,
                                             pListOfExplicitEntries[i].grfInheritance & ACCP_EXPLICIT_INHERIT_FLAGS,
                                             pListOfExplicitEntries[i].grfAccessPermissions,
                                             pSid1);
            }
            else
            {
                /* FIXME: Call AddAccessAllowedObjectAce */
                DPRINT1("Object ACEs not yet supported!\n");
                SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
                bRet = FALSE;
            }

            if (needToClean) LocalFree((HLOCAL)pSid1);
            if (!bRet)
            {
                Ret = GetLastError();
                goto Cleanup;
            }
        }
    }

    /* 3b) Existing allow entries, then everything that was inherited */
    if (OldAcl &&
        (!AccpCopyKeptAces(pNew, OldAcl, pKeepAce, SizeInformation.AceCount, AccpExplicitOtherAce) ||
         !AccpCopyKeptAces(pNew, OldAcl, pKeepAce, SizeInformation.AceCount, AccpInheritedAce)))
    {
        Ret = GetLastError();
        goto Cleanup;
    }

    *NewAcl = pNew;

Cleanup:
    if (pKeepAce && pKeepAce != KeepAceBuf)
        LocalFree((HLOCAL)pKeepAce);

    if (pNew && Ret != ERROR_SUCCESS)
        LocalFree((HLOCAL)pNew);

    if (PolicyHandle)
        LsaClose(PolicyHandle);

    /* restore the last error code */
    SetLastError(LastErr);

    return Ret;
}


/**********************************************************************
 * AccGetInheritanceSource				EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccGetInheritanceSource(LPWSTR pObjectName,
                        SE_OBJECT_TYPE ObjectType,
                        SECURITY_INFORMATION SecurityInfo,
                        BOOL Container,
                        GUID** pObjectClassGuids,
                        DWORD GuidCount,
                        PACL pAcl,
                        PFN_OBJECT_MGR_FUNCTS pfnArray,
                        PGENERIC_MAPPING pGenericMapping,
                        PINHERITED_FROMW pInheritArray)
{
    UNIMPLEMENTED;
    return ERROR_CALL_NOT_IMPLEMENTED;
}


/**********************************************************************
 * AccFreeIndexArray					EXPORTED
 *
 * @implemented
 */
DWORD WINAPI
AccFreeIndexArray(PINHERITED_FROMW pInheritArray,
                  USHORT AceCnt,
                  PFN_OBJECT_MGR_FUNCTS pfnArray  OPTIONAL)
{
    PINHERITED_FROMW pLast;

    UNREFERENCED_PARAMETER(pfnArray);

    pLast = pInheritArray + AceCnt;
    while (pInheritArray != pLast)
    {
        if (pInheritArray->AncestorName != NULL)
        {
            LocalFree((HLOCAL)pInheritArray->AncestorName);
            pInheritArray->AncestorName = NULL;
        }

        pInheritArray++;
    }

    return ERROR_SUCCESS;
}


/**********************************************************************
 * AccRewriteGetExplicitEntriesFromAcl			EXPORTED
 *
 * @implemented
 */
DWORD WINAPI
AccRewriteGetExplicitEntriesFromAcl(PACL pacl,
                                    PULONG pcCountOfExplicitEntries,
                                    PEXPLICIT_ACCESS_W* pListOfExplicitEntries)
{
    PACE_HEADER AceHeader;
    PSID Sid, SidTarget;
    ULONG ObjectAceCount = 0;
    POBJECTS_AND_SID ObjSid;
    SIZE_T Size;
    PEXPLICIT_ACCESS_W peaw;
    DWORD LastErr, SidLen;
    DWORD AceIndex = 0;
    DWORD ErrorCode = ERROR_SUCCESS;

    /* save the last error code */
    LastErr = GetLastError();

    if (pacl != NULL)
    {
        if (pacl->AceCount != 0)
        {
            Size = (SIZE_T)pacl->AceCount * sizeof(EXPLICIT_ACCESS_W);

            /* calculate the space needed */
            while (GetAce(pacl,
                          AceIndex,
                          (LPVOID*)&AceHeader))
            {
                Sid = AccpGetAceSid(AceHeader);
                Size += GetLengthSid(Sid);

                if (AccpIsObjectAce(AceHeader))
                    ObjectAceCount++;

                AceIndex++;
            }

            Size += ObjectAceCount * sizeof(OBJECTS_AND_SID);

            ASSERT(pacl->AceCount == AceIndex);

            /* allocate the array */
            peaw = (PEXPLICIT_ACCESS_W)LocalAlloc(LMEM_FIXED,
                                                  Size);
            if (peaw != NULL)
            {
                AceIndex = 0;
                ObjSid = (POBJECTS_AND_SID)(peaw + pacl->AceCount);
                SidTarget = (PSID)(ObjSid + ObjectAceCount);

                /* initialize the array */
                while (GetAce(pacl,
                              AceIndex,
                              (LPVOID*)&AceHeader))
                {
                    Sid = AccpGetAceSid(AceHeader);
                    SidLen = GetLengthSid(Sid);

                    peaw[AceIndex].grfAccessPermissions = AccpGetAceAccessMask(AceHeader);
                    peaw[AceIndex].grfAccessMode = AccpGetAceAccessMode(AceHeader);
                    peaw[AceIndex].grfInheritance = AceHeader->AceFlags & VALID_INHERIT_FLAGS;

                    if (CopySid(SidLen,
                                SidTarget,
                                Sid))
                    {
                        if (AccpIsObjectAce(AceHeader))
                        {
                            BuildTrusteeWithObjectsAndSid(&peaw[AceIndex].Trustee,
                                                          ObjSid++,
                                                          AccpGetObjectAceObjectType(AceHeader),
                                                          AccpGetObjectAceInheritedObjectType(AceHeader),
                                                          SidTarget);
                        }
                        else
                        {
                            BuildTrusteeWithSid(&peaw[AceIndex].Trustee,
                                                SidTarget);
                        }

                        SidTarget = (PSID)((ULONG_PTR)SidTarget + SidLen);
                    }
                    else
                    {
                        /* copying the SID failed, treat it as an fatal error... */
                        ErrorCode = GetLastError();

                        /* free allocated resources */
                        LocalFree(peaw);
                        peaw = NULL;
                        AceIndex = 0;
                        break;
                    }

                    AceIndex++;
                }

                *pcCountOfExplicitEntries = AceIndex;
                *pListOfExplicitEntries = peaw;
            }
            else
                ErrorCode = ERROR_NOT_ENOUGH_MEMORY;
        }
        else
        {
            goto EmptyACL;
        }
    }
    else
    {
EmptyACL:
        *pcCountOfExplicitEntries = 0;
        *pListOfExplicitEntries = NULL;
    }

    /* restore the last error code */
    SetLastError(LastErr);

    return ErrorCode;
}


/**********************************************************************
 * AccTreeResetNamedSecurityInfo			EXPORTED
 *
 * @unimplemented
 */
DWORD WINAPI
AccTreeResetNamedSecurityInfo(LPWSTR pObjectName,
                              SE_OBJECT_TYPE ObjectType,
                              SECURITY_INFORMATION SecurityInfo,
                              PSID pOwner,
                              PSID pGroup,
                              PACL pDacl,
                              PACL pSacl,
                              BOOL KeepExplicit,
                              FN_PROGRESSW fnProgress,
                              PROG_INVOKE_SETTING ProgressInvokeSetting,
                              PVOID Args)
{
    UNIMPLEMENTED;
    return ERROR_CALL_NOT_IMPLEMENTED;
}


BOOL WINAPI
DllMain(IN HINSTANCE hinstDLL,
        IN DWORD dwReason,
        IN LPVOID lpvReserved)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            hDllInstance = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            break;

        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}

