/*
 * PROJECT:     ReactOS Shell Account Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SAM and LSA helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define USER_PICTURES_FOLDER L"Microsoft\\User Account Pictures"

/* Unknown names and ids come back as ERROR_NOT_FOUND, other failures as NT facility codes */
HRESULT
WINAPI
SamHResultFromStatus(
    _In_ NTSTATUS Status)
{
    if (NT_SUCCESS(Status))
        return S_OK;

    if (Status == STATUS_NONE_MAPPED || Status == STATUS_NO_SUCH_USER)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    return HRESULT_FROM_NT(Status);
}

HRESULT
WINAPI
CopySidToTaskMem(
    _In_ PSID psid,
    _Outptr_ PSID *ppsidCopy)
{
    DWORD cbSid = GetLengthSid(psid);
    PSID psidCopy;

    *ppsidCopy = NULL;

    psidCopy = CoTaskMemAlloc(cbSid);
    if (!psidCopy)
        return E_OUTOFMEMORY;

    if (!CopySid(cbSid, psidCopy, psid))
    {
        DWORD dwError = GetLastError();

        CoTaskMemFree(psidCopy);
        return HRESULT_FROM_WIN32(dwError);
    }

    *ppsidCopy = psidCopy;
    return S_OK;
}

HRESULT
WINAPI
GetAccountDomainSid(
    _Outptr_ PSID *ppsidDomain)
{
    LSA_OBJECT_ATTRIBUTES ObjectAttributes;
    PPOLICY_ACCOUNT_DOMAIN_INFO pDomainInfo = NULL;
    LSA_HANDLE hPolicy = NULL;
    NTSTATUS Status;
    HRESULT hr;

    *ppsidDomain = NULL;

    ZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    Status = LsaOpenPolicy(NULL, &ObjectAttributes, POLICY_VIEW_LOCAL_INFORMATION, &hPolicy);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);

    Status = LsaQueryInformationPolicy(hPolicy,
                                       PolicyAccountDomainInformation,
                                       (PVOID *)&pDomainInfo);
    if (NT_SUCCESS(Status))
    {
        if (pDomainInfo->DomainSid)
            hr = CopySidToTaskMem(pDomainInfo->DomainSid, ppsidDomain);
        else
            hr = E_FAIL;
        LsaFreeMemory(pDomainInfo);
    }
    else
    {
        hr = HRESULT_FROM_NT(Status);
    }

    LsaClose(hPolicy);
    return hr;
}

HRESULT
WINAPI
GetBuiltinDomainSid(
    _Outptr_ PSID *ppsidDomain)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID psidBuiltin;
    HRESULT hr;

    *ppsidDomain = NULL;

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  1,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  &psidBuiltin))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    hr = CopySidToTaskMem(psidBuiltin, ppsidDomain);
    FreeSid(psidBuiltin);
    return hr;
}

HRESULT
WINAPI
OpenSamDomain(
    _In_ PSID psidDomain,
    _Out_ SAM_HANDLE *phServer,
    _Out_ SAM_HANDLE *phDomain)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    SAM_HANDLE hServer = NULL;
    SAM_HANDLE hDomain = NULL;
    NTSTATUS Status;

    *phServer = NULL;
    *phDomain = NULL;

    ZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    ObjectAttributes.Length = sizeof(ObjectAttributes);

    Status = SamConnect(NULL,
                        &hServer,
                        SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                        &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);

    Status = SamOpenDomain(hServer, DOMAIN_LOOKUP | DOMAIN_LIST_ACCOUNTS, psidDomain, &hDomain);
    if (!NT_SUCCESS(Status))
    {
        SamCloseHandle(hServer);
        return HRESULT_FROM_NT(Status);
    }

    *phServer = hServer;
    *phDomain = hDomain;
    return S_OK;
}

VOID
WINAPI
CloseSamDomain(
    _In_opt_ SAM_HANDLE hServer,
    _In_opt_ SAM_HANDLE hDomain)
{
    if (hDomain)
        SamCloseHandle(hDomain);

    if (hServer)
        SamCloseHandle(hServer);
}

/* Fails when the name exists in the domain but is not a user account */
HRESULT
WINAPI
LookupUserRid(
    _In_ PSID psidDomain,
    _In_ PCWSTR pszUserName,
    _Out_ ULONG *pulRid)
{
    UNICODE_STRING UserName;
    SAM_HANDLE hServer;
    SAM_HANDLE hDomain;
    PULONG pRids = NULL;
    PSID_NAME_USE pUse = NULL;
    NTSTATUS Status;
    HRESULT hr;

    *pulRid = 0;

    if (!pszUserName)
        return E_POINTER;

    hr = OpenSamDomain(psidDomain, &hServer, &hDomain);
    if (FAILED(hr))
        return hr;

    RtlInitUnicodeString(&UserName, pszUserName);
    Status = SamLookupNamesInDomain(hDomain, 1, &UserName, &pRids, &pUse);
    hr = SamHResultFromStatus(Status);
    if (NT_SUCCESS(Status))
    {
        if (pUse[0] == SidTypeUser)
            *pulRid = pRids[0];
        else
            hr = HRESULT_FROM_NT(STATUS_RESOURCE_TYPE_NOT_FOUND);

        SamFreeMemory(pRids);
        SamFreeMemory(pUse);
    }

    CloseSamDomain(hServer, hDomain);
    return hr;
}

/* The returned buffer is freed with SamFreeMemory */
HRESULT
WINAPI
EnumerateDomainUsers(
    _In_ PSID psidDomain,
    _Outptr_result_buffer_maybenull_(*pcUsers) PSAM_RID_ENUMERATION *pprgUsers,
    _Out_ ULONG *pcUsers)
{
    SAM_ENUMERATE_HANDLE EnumContext = 0;
    SAM_HANDLE hServer;
    SAM_HANDLE hDomain;
    PVOID pvBuffer = NULL;
    ULONG cReturned = 0;
    NTSTATUS Status;
    HRESULT hr;

    *pprgUsers = NULL;
    *pcUsers = 0;

    hr = OpenSamDomain(psidDomain, &hServer, &hDomain);
    if (FAILED(hr))
        return hr;

    Status = SamEnumerateUsersInDomain(hDomain, &EnumContext, 0, &pvBuffer, MAXULONG, &cReturned);
    hr = SamHResultFromStatus(Status);
    if (NT_SUCCESS(Status))
    {
        *pprgUsers = (PSAM_RID_ENUMERATION)pvBuffer;
        *pcUsers = cReturned;
    }

    CloseSamDomain(hServer, hDomain);
    return hr;
}

/* <ProgramData>\Microsoft\User Account Pictures, the folder explorer and logon read */
HRESULT
WINAPI
GetUserPicturesDirectory(
    _Out_writes_(cchDir) PWSTR pszDir,
    _In_ UINT cchDir)
{
    WCHAR szProgramData[MAX_PATH];
    DWORD cchNeeded;

    cchNeeded = ExpandEnvironmentStringsW(L"%ProgramData%",
                                          szProgramData,
                                          ARRAYSIZE(szProgramData));
    if (!cchNeeded)
        return HRESULT_FROM_WIN32(GetLastError());

    if (cchNeeded > ARRAYSIZE(szProgramData))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    return StringCchPrintfW(pszDir, cchDir, L"%s\\%s", szProgramData, USER_PICTURES_FOLDER);
}
