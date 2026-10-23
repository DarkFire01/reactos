/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AppContainer profiles
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#include <sddl.h>
#include <aclapi.h>
#include <wincrypt.h>

#define NDEBUG
#include <debug.h>

#ifndef CRITICAL_ACE_FLAG
#define CRITICAL_ACE_FLAG 0x20
#endif

/* Longest names, in characters without the terminator */
#define AC_NAME_MAX         64
#define AC_CHILD_NAME_MAX   50
#define AC_DISPLAY_MAX      511
#define AC_DESCRIPTION_MAX  2047

/* Room for a moniker read back from a mapping, and for an HKEY_USERS relative key path */
#define AC_MONIKER_CHARS    128
#define AC_KEY_CHARS        512

/* Hash words that follow SECURITY_APP_PACKAGE_BASE_RID in a parent SID */
#define AC_HASH_RIDS        (SECURITY_PARENT_PACKAGE_RID_COUNT - 1)

/* Hash words a child SID appends to its parent's */
#define AC_CHILD_RIDS       (SECURITY_CHILD_PACKAGE_RID_COUNT - SECURITY_PARENT_PACKAGE_RID_COUNT)

/* Rights asked for on a storage key, which also has its DACL replaced */
#define AC_STORAGE_ACCESS   (KEY_READ | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | WRITE_DAC)

static const WCHAR AcKeyRoot[] =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer";

/* Security of the Storage key when it is first created; the capability SID gets read access */
static const WCHAR AcStorageRootSddl[] =
    L"D:(A;OICI;KA;;;CO)(A;OICI;KA;;;SY)(A;OICI;KA;;;%s)"
    L"(A;OICI;KR;;;S-1-15-3-1024-3635283841-2530182609-996808640-1887759898-3848208603-3313616867-983405619-2501854204)"
    L"(A;OICI;KA;;;BA)";

typedef struct _AC_FOLDER
{
    PCWSTR Name;
    DWORD Attributes;
} AC_FOLDER;

/* Folders made under a container's AC folder */
static const AC_FOLDER AcKnownFolders[] =
{
    {L"Temp", FILE_ATTRIBUTE_NORMAL},
    {L"INetCache", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED},
    {L"INetHistory", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED},
    {L"INetCookies", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED}
};

/* Extra folders a child container gets next to the known ones */
static const PCWSTR AcChildFolders[] =
{
    L"LocalState",
    L"TempState"
};

/* Everything known about one container: who asks, its SIDs, and where its state lives */
typedef struct _AC_PROFILE
{
    HANDLE Token;
    PSID UserSid;
    PWSTR UserSidString;
    PSID ParentSid;
    PWSTR ParentSidString;
    PSID Sid;
    PWSTR SidString;
    WCHAR Moniker[AC_MONIKER_CHARS];
    WCHAR ParentMoniker[AC_MONIKER_CHARS];
    WCHAR Packages[MAX_PATH];
    WCHAR Mapping[AC_KEY_CHARS];
    WCHAR Storage[AC_KEY_CHARS];
    WCHAR Root[MAX_PATH];
    WCHAR Folder[MAX_PATH];
} AC_PROFILE, *PAC_PROFILE;

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOL
AcpValidLength(
    _In_opt_ PCWSTR String,
    _In_ SIZE_T Max)
{
    SIZE_T Length;

    if (!String || FAILED(StringCchLengthW(String, Max + 1, &Length)))
        return FALSE;
    return Length != 0;
}

static
HRESULT
AcpHrFromStatus(
    _In_ NTSTATUS Status)
{
    return HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
}

/**
 * @brief
 * Hashes the lower case form of a name with SHA-256 and returns the first
 * AC_HASH_RIDS words, which is what AppContainer SIDs are made of.
 */
static
HRESULT
AcpHashName(
    _In_ PCWSTR Name,
    _Out_writes_(AC_HASH_RIDS) PULONG Rids)
{
    UNICODE_STRING Source, Lower;
    ULONG Digest[8];
    DWORD DigestSize = sizeof(Digest);
    HCRYPTPROV Provider;
    HCRYPTHASH Hash;
    NTSTATUS Status;
    HRESULT hr = S_OK;

    RtlInitUnicodeString(&Source, Name);
    Status = RtlDowncaseUnicodeString(&Lower, &Source, TRUE);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);

    if (!CryptAcquireContextW(&Provider, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto Quit;
    }

    if (!CryptCreateHash(Provider, CALG_SHA_256, 0, 0, &Hash))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        if (!CryptHashData(Hash, (PBYTE)Lower.Buffer, Lower.Length, 0) ||
            !CryptGetHashParam(Hash, HP_HASHVAL, (PBYTE)Digest, &DigestSize, 0))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
        CryptDestroyHash(Hash);
    }
    CryptReleaseContext(Provider, 0);

    if (SUCCEEDED(hr))
        RtlCopyMemory(Rids, Digest, AC_HASH_RIDS * sizeof(*Rids));

Quit:
    RtlFreeUnicodeString(&Lower);
    return hr;
}

/* Builds an APPLICATION PACKAGE AUTHORITY SID that FreeSid can release */
static
HRESULT
AcpAllocateSid(
    _In_ UCHAR Count,
    _In_reads_(Count) const ULONG *Rids,
    _Outptr_ PSID *Sid)
{
    SID_IDENTIFIER_AUTHORITY PackageAuthority = {SECURITY_APP_PACKAGE_AUTHORITY};
    PSID NewSid;
    UCHAR Index;

    NewSid = RtlAllocateHeap(RtlGetProcessHeap(), 0, RtlLengthRequiredSid(Count));
    if (!NewSid)
        return E_OUTOFMEMORY;

    RtlInitializeSid(NewSid, &PackageAuthority, Count);
    for (Index = 0; Index < Count; Index++)
        *RtlSubAuthoritySid(NewSid, Index) = Rids[Index];

    *Sid = NewSid;
    return S_OK;
}

static
HRESULT
AcpSidFromName(
    _In_ PCWSTR Name,
    _Outptr_ PSID *Sid)
{
    ULONG Rids[SECURITY_PARENT_PACKAGE_RID_COUNT];
    HRESULT hr;

    Rids[0] = SECURITY_APP_PACKAGE_BASE_RID;
    hr = AcpHashName(Name, &Rids[1]);
    if (FAILED(hr))
        return hr;

    return AcpAllocateSid(SECURITY_PARENT_PACKAGE_RID_COUNT, Rids, Sid);
}

/**
 * @brief
 * A child SID is the parent SID followed by the last AC_CHILD_RIDS hash
 * words of the child name.
 */
static
HRESULT
AcpChildSidFromName(
    _In_ PSID ParentSid,
    _In_ PCWSTR Name,
    _Outptr_ PSID *Sid)
{
    ULONG Rids[SECURITY_CHILD_PACKAGE_RID_COUNT];
    ULONG Hash[AC_HASH_RIDS];
    APPCONTAINER_SID_TYPE Type;
    NTSTATUS Status;
    HRESULT hr;
    ULONG Index;

    Status = RtlGetAppContainerSidType(ParentSid, &Type);
    if (!NT_SUCCESS(Status))
        return AcpHrFromStatus(Status);
    if (Type != ParentAppContainerSidType)
        return E_INVALIDARG;

    hr = AcpHashName(Name, Hash);
    if (FAILED(hr))
        return hr;

    for (Index = 0; Index < SECURITY_PARENT_PACKAGE_RID_COUNT; Index++)
        Rids[Index] = *RtlSubAuthoritySid(ParentSid, Index);
    RtlCopyMemory(&Rids[SECURITY_PARENT_PACKAGE_RID_COUNT],
                  &Hash[AC_HASH_RIDS - AC_CHILD_RIDS],
                  AC_CHILD_RIDS * sizeof(*Rids));

    return AcpAllocateSid(SECURITY_CHILD_PACKAGE_RID_COUNT, Rids, Sid);
}

/* The impersonation token when there is one, else the process token */
static
HRESULT
AcpOpenCallerToken(
    _Out_ PHANDLE Token)
{
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, Token))
        return S_OK;
    if (GetLastError() == ERROR_NO_TOKEN && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, Token))
        return S_OK;
    return HRESULT_FROM_WIN32(GetLastError());
}

/**
 * @brief
 * Copies the SID a token reports for TokenUser or TokenAppContainerSid.
 * Both classes return a structure that starts with the SID pointer.
 */
static
HRESULT
AcpCopyTokenSid(
    _In_ HANDLE Token,
    _In_ TOKEN_INFORMATION_CLASS Class,
    _Outptr_ PSID *Sid)
{
    ULONG Buffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PSID Found;
    DWORD Length;

    if (!GetTokenInformation(Token, Class, Buffer, sizeof(Buffer), &Length))
        return HRESULT_FROM_WIN32(GetLastError());

    Found = *(PSID *)Buffer;
    Length = RtlLengthSid(Found);
    *Sid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!*Sid)
        return E_OUTOFMEMORY;

    RtlCopySid(Length, *Sid, Found);
    return S_OK;
}

/**
 * @brief
 * Opens the caller's token and gathers its user, the user's Packages folder
 * and, when the caller runs in a container, that container's SID.
 */
static
HRESULT
AcpOpenCaller(
    _Out_ PAC_PROFILE Profile,
    _Outptr_opt_result_maybenull_ PSID *ContainerSid)
{
    DWORD IsAppContainer = 0, Length, Size;
    HRESULT hr;

    RtlZeroMemory(Profile, sizeof(*Profile));
    if (ContainerSid)
        *ContainerSid = NULL;

    hr = AcpOpenCallerToken(&Profile->Token);
    if (SUCCEEDED(hr))
        hr = AcpCopyTokenSid(Profile->Token, TokenUser, &Profile->UserSid);
    if (FAILED(hr))
        return hr;

    if (!ConvertSidToStringSidW(Profile->UserSid, &Profile->UserSidString))
        return HRESULT_FROM_WIN32(GetLastError());

    Size = _countof(Profile->Packages);
    if (!GetUserProfileDirectoryW(Profile->Token, Profile->Packages, &Size))
        return HRESULT_FROM_WIN32(GetLastError());
    hr = StringCchCatW(Profile->Packages, _countof(Profile->Packages), L"\\AppData\\Local\\Packages");
    if (FAILED(hr) || !ContainerSid)
        return hr;

    if (!GetTokenInformation(Profile->Token, TokenIsAppContainer, &IsAppContainer, sizeof(IsAppContainer), &Length))
        return HRESULT_FROM_WIN32(GetLastError());
    if (IsAppContainer)
        hr = AcpCopyTokenSid(Profile->Token, TokenAppContainerSid, ContainerSid);
    return hr;
}

static
VOID
AcpCloseProfile(
    _Inout_ PAC_PROFILE Profile)
{
    if (Profile->Sid)
        RtlFreeSid(Profile->Sid);
    if (Profile->ParentSid)
        RtlFreeSid(Profile->ParentSid);
    if (Profile->UserSid)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Profile->UserSid);
    if (Profile->SidString)
        LocalFree(Profile->SidString);
    if (Profile->ParentSidString)
        LocalFree(Profile->ParentSidString);
    if (Profile->UserSidString)
        LocalFree(Profile->UserSidString);
    if (Profile->Token)
        CloseHandle(Profile->Token);
}

static
HRESULT
AcpReadString(
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName,
    _Out_writes_z_(Count) PWSTR Buffer,
    _In_ DWORD Count)
{
    DWORD Type, Size = (Count - 1) * sizeof(WCHAR);
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_USERS, KeyPath, 0, KEY_QUERY_VALUE, &Key);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegQueryValueExW(Key, ValueName, NULL, &Type, (PBYTE)Buffer, &Size);
        RegCloseKey(Key);
    }
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);
    if (Type != REG_SZ)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

    Buffer[Size / sizeof(WCHAR)] = UNICODE_NULL;
    return S_OK;
}

/**
 * @brief
 * Fills in the key and folder paths of the container in Profile->Sid, which
 * is a child of Profile->ParentSid when that is set. A moniker that was not
 * given is read back from the mappings.
 */
static
HRESULT
AcpDescribe(
    _Inout_ PAC_PROFILE Profile)
{
    WCHAR Base[AC_KEY_CHARS];
    HRESULT hr;

    if (!ConvertSidToStringSidW(Profile->Sid, &Profile->SidString))
        return HRESULT_FROM_WIN32(GetLastError());

    hr = StringCchPrintfW(Base, _countof(Base), L"%s\\%s", Profile->UserSidString, AcKeyRoot);
    if (FAILED(hr))
        return hr;

    if (!Profile->ParentSid)
    {
        hr = StringCchPrintfW(Profile->Mapping, _countof(Profile->Mapping), L"%s\\Mappings\\%s",
                              Base, Profile->SidString);
        if (SUCCEEDED(hr) && !Profile->Moniker[0])
            hr = AcpReadString(Profile->Mapping, L"Moniker", Profile->Moniker, _countof(Profile->Moniker));
        if (SUCCEEDED(hr))
        {
            hr = StringCchPrintfW(Profile->Storage, _countof(Profile->Storage), L"%s\\Storage\\%s",
                                  Base, Profile->Moniker);
        }
        if (SUCCEEDED(hr))
            hr = StringCchPrintfW(Profile->Root, _countof(Profile->Root), L"%s\\%s", Profile->Packages, Profile->Moniker);
        if (SUCCEEDED(hr))
            hr = StringCchPrintfW(Profile->Folder, _countof(Profile->Folder), L"%s\\AC", Profile->Root);
        return hr;
    }

    /* A child lives under its parent's mapping, storage key and AC folder */
    if (!ConvertSidToStringSidW(Profile->ParentSid, &Profile->ParentSidString))
        return HRESULT_FROM_WIN32(GetLastError());

    hr = StringCchPrintfW(Profile->Mapping, _countof(Profile->Mapping), L"%s\\Mappings\\%s",
                          Base, Profile->ParentSidString);
    if (SUCCEEDED(hr))
        hr = AcpReadString(Profile->Mapping, L"Moniker", Profile->ParentMoniker, _countof(Profile->ParentMoniker));
    if (SUCCEEDED(hr))
    {
        hr = StringCchPrintfW(Profile->Mapping, _countof(Profile->Mapping), L"%s\\Mappings\\%s\\Children\\%s",
                              Base, Profile->ParentSidString, Profile->SidString);
    }
    if (SUCCEEDED(hr) && !Profile->Moniker[0])
        hr = AcpReadString(Profile->Mapping, L"Moniker", Profile->Moniker, _countof(Profile->Moniker));
    if (SUCCEEDED(hr))
    {
        hr = StringCchPrintfW(Profile->Storage, _countof(Profile->Storage), L"%s\\Storage\\%s\\Children\\%s",
                              Base, Profile->ParentMoniker, Profile->Moniker);
    }
    if (SUCCEEDED(hr))
    {
        hr = StringCchPrintfW(Profile->Root, _countof(Profile->Root), L"%s\\%s\\AC\\#!%s",
                              Profile->Packages, Profile->ParentMoniker, Profile->Moniker);
    }
    if (SUCCEEDED(hr))
        hr = StringCchCopyW(Profile->Folder, _countof(Profile->Folder), Profile->Root);
    return hr;
}

/**
 * @brief
 * Prepares the profile of a container named by the caller. A caller that
 * runs in a container names one of its children.
 */
static
HRESULT
AcpOpenProfile(
    _Out_ PAC_PROFILE Profile,
    _In_ PCWSTR Name,
    _In_ BOOL Creating)
{
    UNICODE_STRING Source, Lower;
    NTSTATUS Status;
    HRESULT hr;

    hr = AcpOpenCaller(Profile, &Profile->ParentSid);
    if (FAILED(hr))
        return hr;

    if (Creating && Profile->ParentSid && !AcpValidLength(Name, AC_CHILD_NAME_MAX))
        return E_INVALIDARG;

    RtlInitUnicodeString(&Source, Name);
    RtlInitEmptyUnicodeString(&Lower, Profile->Moniker, sizeof(Profile->Moniker) - sizeof(WCHAR));
    Status = RtlDowncaseUnicodeString(&Lower, &Source, FALSE);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);
    Profile->Moniker[Lower.Length / sizeof(WCHAR)] = UNICODE_NULL;

    if (Profile->ParentSid)
        hr = AcpChildSidFromName(Profile->ParentSid, Profile->Moniker, &Profile->Sid);
    else
        hr = AcpSidFromName(Profile->Moniker, &Profile->Sid);
    if (FAILED(hr))
        return hr;

    return AcpDescribe(Profile);
}

/* S_OK when the file or folder exists, S_FALSE when it does not */
static
HRESULT
AcpPathExists(
    _In_ PCWSTR Path)
{
    HANDLE File;
    DWORD Error;

    File = CreateFileW(Path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS,
                       NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        return S_OK;
    }

    Error = GetLastError();
    if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
        return S_FALSE;
    return HRESULT_FROM_WIN32(Error);
}

/* S_OK when the key, or the value when one is named, exists, S_FALSE when it does not */
static
HRESULT
AcpKeyExists(
    _In_ PCWSTR KeyPath,
    _In_opt_ PCWSTR ValueName)
{
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_USERS, KeyPath, 0, KEY_READ, &Key);
    if (Error == ERROR_SUCCESS)
    {
        if (ValueName)
            Error = RegQueryValueExW(Key, ValueName, NULL, NULL, NULL, NULL);
        RegCloseKey(Key);
    }

    if (Error == ERROR_SUCCESS)
        return S_OK;
    if (Error == ERROR_FILE_NOT_FOUND)
        return S_FALSE;
    return HRESULT_FROM_WIN32(Error);
}

/**
 * @brief
 * Counts which parts of a profile exist: its folders, its storage key and
 * the moniker and display name of its mapping.
 */
static
HRESULT
AcpCountState(
    _In_ PAC_PROFILE Profile,
    _Out_ PULONG Present,
    _Out_ PULONG Missing)
{
    WCHAR Path[MAX_PATH];
    /* Root and AC folders, the known folders, the storage key and two mapping values */
    HRESULT Checks[2 + _countof(AcKnownFolders) + 3];
    ULONG Count = 0, Index;
    HRESULT hr;

    *Present = 0;
    *Missing = 0;

    hr = AcpPathExists(Profile->Root);
    Checks[Count++] = hr;
    if (hr == S_OK)
    {
        if (!Profile->ParentSid)
            Checks[Count++] = AcpPathExists(Profile->Folder);

        for (Index = 0; Index < _countof(AcKnownFolders); Index++)
        {
            hr = StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Profile->Folder, AcKnownFolders[Index].Name);
            Checks[Count++] = SUCCEEDED(hr) ? AcpPathExists(Path) : hr;
        }
    }
    Checks[Count++] = AcpKeyExists(Profile->Storage, NULL);
    Checks[Count++] = AcpKeyExists(Profile->Mapping, L"Moniker");
    Checks[Count++] = AcpKeyExists(Profile->Mapping, L"DisplayName");

    for (Index = 0; Index < Count; Index++)
    {
        if (FAILED(Checks[Index]))
            return Checks[Index];
        if (Checks[Index] == S_OK)
            (*Present)++;
        else
            (*Missing)++;
    }
    return S_OK;
}

/**
 * @brief
 * Builds the security of a mapping key: full control for SYSTEM, the
 * administrators, the user and the parent container, read for the creator
 * owner, and PackageAccess for the container itself. Free with HeapFree.
 */
static
HRESULT
AcpCreateMappingSecurity(
    _In_ PSID UserSid,
    _In_opt_ PSID ParentSid,
    _In_opt_ PSID PackageSid,
    _In_ ACCESS_MASK PackageAccess,
    _Out_ PSECURITY_DESCRIPTOR *Descriptor)
{
    ULONG System[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
    ULONG Creator[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
    ULONG Admins[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
    PSID Sids[6];
    ACCESS_MASK Access[6];
    PSECURITY_DESCRIPTOR NewDescriptor;
    DWORD Size, AclSize = sizeof(ACL);
    ULONG Count = 0, Index;
    PACL Acl;

    Size = sizeof(System);
    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, System, &Size))
        return HRESULT_FROM_WIN32(GetLastError());
    Size = sizeof(Creator);
    if (!CreateWellKnownSid(WinCreatorOwnerSid, NULL, Creator, &Size))
        return HRESULT_FROM_WIN32(GetLastError());
    Size = sizeof(Admins);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, Admins, &Size))
        return HRESULT_FROM_WIN32(GetLastError());

    Sids[Count] = System;
    Access[Count++] = KEY_ALL_ACCESS;
    Sids[Count] = Creator;
    Access[Count++] = KEY_READ;
    Sids[Count] = Admins;
    Access[Count++] = KEY_ALL_ACCESS;
    Sids[Count] = UserSid;
    Access[Count++] = KEY_ALL_ACCESS;
    if (ParentSid)
    {
        Sids[Count] = ParentSid;
        Access[Count++] = KEY_ALL_ACCESS;
    }
    if (PackageSid)
    {
        Sids[Count] = PackageSid;
        Access[Count++] = PackageAccess;
    }

    for (Index = 0; Index < Count; Index++)
        AclSize += FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(Sids[Index]);

    NewDescriptor = HeapAlloc(GetProcessHeap(), 0, sizeof(SECURITY_DESCRIPTOR) + AclSize);
    if (!NewDescriptor)
        return E_OUTOFMEMORY;
    Acl = (PACL)((PBYTE)NewDescriptor + sizeof(SECURITY_DESCRIPTOR));

    if (!InitializeAcl(Acl, AclSize, ACL_REVISION))
        goto Fail;
    for (Index = 0; Index < Count; Index++)
    {
        if (!AddAccessAllowedAceEx(Acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, Access[Index], Sids[Index]))
            goto Fail;
    }

    if (!InitializeSecurityDescriptor(NewDescriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(NewDescriptor, TRUE, Acl, FALSE) ||
        !SetSecurityDescriptorOwner(NewDescriptor, UserSid, FALSE) ||
        !SetSecurityDescriptorGroup(NewDescriptor, UserSid, FALSE))
    {
        goto Fail;
    }

    *Descriptor = NewDescriptor;
    return S_OK;

Fail:
    Size = GetLastError();
    HeapFree(GetProcessHeap(), 0, NewDescriptor);
    return HRESULT_FROM_WIN32(Size);
}

/* One inheritable, critical ACE that gives a container Access. Free with HeapFree. */
static
HRESULT
AcpCreatePackageAcl(
    _In_ PSID PackageSid,
    _In_ ACCESS_MASK Access,
    _Out_ PACL *Acl)
{
    DWORD Size = sizeof(ACL) + FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(PackageSid);
    PACE_HEADER Ace;
    PACL NewAcl;

    NewAcl = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!NewAcl)
        return E_OUTOFMEMORY;

    if (!InitializeAcl(NewAcl, Size, ACL_REVISION) ||
        !AddAccessAllowedAceEx(NewAcl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, Access, PackageSid) ||
        !GetAce(NewAcl, 0, (PVOID *)&Ace))
    {
        Size = GetLastError();
        HeapFree(GetProcessHeap(), 0, NewAcl);
        return HRESULT_FROM_WIN32(Size);
    }

    /* Rtl only takes inheritance flags when adding the ACE */
    Ace->AceFlags |= CRITICAL_ACE_FLAG;

    *Acl = NewAcl;
    return S_OK;
}

/* An inheritable low integrity label with no write up. Free with HeapFree. */
static
HRESULT
AcpCreateLowLabel(
    _Out_ PACL *Sacl)
{
    SID_IDENTIFIER_AUTHORITY LabelAuthority = {SECURITY_MANDATORY_LABEL_AUTHORITY};
    ULONG Buffer[(sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG)];
    PSYSTEM_MANDATORY_LABEL_ACE Ace = (PSYSTEM_MANDATORY_LABEL_ACE)Buffer;
    PSID LowSid = (PSID)&Ace->SidStart;
    DWORD AceSize, Size;
    PACL NewAcl;

    /* The ACE is built by hand, RtlAddMandatoryAce takes its arguments in another order on Windows */
    RtlInitializeSid(LowSid, &LabelAuthority, 1);
    *RtlSubAuthoritySid(LowSid, 0) = SECURITY_MANDATORY_LOW_RID;
    AceSize = FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE, SidStart) + RtlLengthSid(LowSid);
    Ace->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    Ace->Header.AceFlags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    Ace->Header.AceSize = (WORD)AceSize;
    Ace->Mask = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;

    Size = sizeof(ACL) + AceSize;
    NewAcl = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!NewAcl)
        return E_OUTOFMEMORY;

    if (!InitializeAcl(NewAcl, Size, ACL_REVISION) || !AddAce(NewAcl, ACL_REVISION, MAXDWORD, Ace, AceSize))
    {
        Size = GetLastError();
        HeapFree(GetProcessHeap(), 0, NewAcl);
        return HRESULT_FROM_WIN32(Size);
    }

    *Sacl = NewAcl;
    return S_OK;
}

static
LONG
AcpSetString(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    return RegSetValueExW(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

/* Creates a key under HKEY_USERS and replaces its security with that of a mapping key */
static
HRESULT
AcpCreateMappingKey(
    _In_ PAC_PROFILE Profile,
    _In_ PCWSTR KeyPath,
    _In_opt_ PSID ParentSid,
    _In_opt_ PSID PackageSid,
    _In_ ACCESS_MASK PackageAccess,
    _In_ BOOL OnlyIfNew,
    _Out_ PHKEY Key)
{
    PSECURITY_DESCRIPTOR Descriptor;
    DWORD Disposition;
    LONG Error;
    HRESULT hr;

    Error = RegCreateKeyExW(HKEY_USERS, KeyPath, 0, NULL, 0, KEY_ALL_ACCESS, NULL, Key, &Disposition);
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);
    if (OnlyIfNew && Disposition != REG_CREATED_NEW_KEY)
        return S_OK;

    hr = AcpCreateMappingSecurity(Profile->UserSid, ParentSid, PackageSid, PackageAccess, &Descriptor);
    if (SUCCEEDED(hr))
    {
        Error = RegSetKeySecurity(*Key,
                                  OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  Descriptor);
        hr = HRESULT_FROM_WIN32(Error);
        HeapFree(GetProcessHeap(), 0, Descriptor);
    }
    if (FAILED(hr))
    {
        RegCloseKey(*Key);
        *Key = NULL;
    }
    return hr;
}

/* Deletes a key and everything below it */
static
LONG
AcpDeleteKeyTree(
    _In_ HKEY Root,
    _In_ PCWSTR SubKey)
{
    WCHAR Name[256];
    DWORD Length;
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(Root, SubKey, 0, KEY_ENUMERATE_SUB_KEYS, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    do
    {
        Length = _countof(Name);
        Error = RegEnumKeyExW(Key, 0, Name, &Length, NULL, NULL, NULL, NULL);
        if (Error == ERROR_SUCCESS)
            Error = AcpDeleteKeyTree(Key, Name);
    } while (Error == ERROR_SUCCESS);
    RegCloseKey(Key);

    if (Error != ERROR_NO_MORE_ITEMS)
        return Error;
    return RegDeleteKeyW(Root, SubKey);
}

/**
 * @brief
 * Records the container under Mappings, which is how kernel32 finds the
 * folder of a new container process. An existing mapping only gets its
 * display name refreshed and ERROR_ALREADY_EXISTS is returned.
 */
static
HRESULT
AcpRegisterMapping(
    _In_ PAC_PROFILE Profile,
    _In_ PCWSTR DisplayName)
{
    WCHAR Path[AC_KEY_CHARS];
    HKEY Key, SubKey;
    LONG Error;
    HRESULT hr;

    Error = RegOpenKeyExW(HKEY_USERS, Profile->Mapping, 0, KEY_READ | KEY_SET_VALUE, &Key);
    if (Error == ERROR_SUCCESS)
    {
        Error = AcpSetString(Key, L"DisplayName", DisplayName);
        RegCloseKey(Key);
        return HRESULT_FROM_WIN32(Error ? Error : ERROR_ALREADY_EXISTS);
    }

    if (!Profile->ParentSid)
    {
        hr = StringCchPrintfW(Path, _countof(Path), L"%s\\%s\\Mappings", Profile->UserSidString, AcKeyRoot);
        if (SUCCEEDED(hr))
            hr = AcpCreateMappingKey(Profile, Path, NULL, NULL, 0, TRUE, &Key);
        if (FAILED(hr))
            return hr;
        RegCloseKey(Key);
    }

    hr = AcpCreateMappingKey(Profile, Profile->Mapping, Profile->ParentSid, Profile->Sid, KEY_READ, FALSE, &Key);
    if (FAILED(hr))
        return hr;

    Error = AcpSetString(Key, L"DisplayName", DisplayName);
    if (Error == ERROR_SUCCESS)
        Error = AcpSetString(Key, L"Moniker", Profile->Moniker);
    if (Error == ERROR_SUCCESS && Profile->ParentSid)
        Error = AcpSetString(Key, L"ParentMoniker", Profile->ParentMoniker);
    hr = HRESULT_FROM_WIN32(Error);
    RegCloseKey(Key);

    /* A parent can hold the mappings of its children */
    if (SUCCEEDED(hr) && !Profile->ParentSid)
    {
        hr = StringCchPrintfW(Path, _countof(Path), L"%s\\Children", Profile->Mapping);
        if (SUCCEEDED(hr))
            hr = AcpCreateMappingKey(Profile, Path, NULL, Profile->Sid, KEY_ALL_ACCESS, FALSE, &SubKey);
        if (SUCCEEDED(hr))
            RegCloseKey(SubKey);
    }

    if (FAILED(hr))
        AcpDeleteKeyTree(HKEY_USERS, Profile->Mapping);
    return hr;
}

/* Gives the user back control over a key tree a container may have locked it out of */
static
VOID
AcpReclaimKeyTree(
    _In_ HKEY Root,
    _In_ PCWSTR SubKey,
    _In_ PSECURITY_DESCRIPTOR Descriptor)
{
    WCHAR Name[256];
    DWORD Length, Index;
    HKEY Key;

    if (RegOpenKeyExW(Root, SubKey, 0, WRITE_DAC, &Key) != ERROR_SUCCESS)
        return;
    RegSetKeySecurity(Key, DACL_SECURITY_INFORMATION, Descriptor);
    RegCloseKey(Key);

    if (RegOpenKeyExW(Root, SubKey, 0, KEY_ENUMERATE_SUB_KEYS, &Key) != ERROR_SUCCESS)
        return;
    for (Index = 0;; Index++)
    {
        Length = _countof(Name);
        if (RegEnumKeyExW(Key, Index, Name, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        AcpReclaimKeyTree(Key, Name, Descriptor);
    }
    RegCloseKey(Key);
}

static
LONG
AcpRemoveKeyTree(
    _In_ PAC_PROFILE Profile,
    _In_ PCWSTR KeyPath)
{
    PSECURITY_DESCRIPTOR Descriptor;
    LONG Error;

    Error = AcpDeleteKeyTree(HKEY_USERS, KeyPath);
    if (Error != ERROR_ACCESS_DENIED)
        return Error;

    if (FAILED(AcpCreateMappingSecurity(Profile->UserSid, NULL, NULL, 0, &Descriptor)))
        return Error;
    AcpReclaimKeyTree(HKEY_USERS, KeyPath, Descriptor);
    HeapFree(GetProcessHeap(), 0, Descriptor);

    return AcpDeleteKeyTree(HKEY_USERS, KeyPath);
}

/* Deletes a folder and all it holds, returning the first error */
static
DWORD
AcpRemoveTree(
    _In_ PCWSTR Path)
{
    WIN32_FIND_DATAW Data;
    SIZE_T Count = wcslen(Path) + MAX_PATH + 2;
    DWORD Error = ERROR_SUCCESS, Result;
    PWSTR Child;
    HANDLE Find;

    Child = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(WCHAR));
    if (!Child)
        return ERROR_NOT_ENOUGH_MEMORY;

    StringCchPrintfW(Child, Count, L"%s\\*", Path);
    Find = FindFirstFileW(Child, &Data);
    if (Find == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Child);
        return Error;
    }

    do
    {
        if (!wcscmp(Data.cFileName, L".") || !wcscmp(Data.cFileName, L".."))
            continue;

        StringCchPrintfW(Child, Count, L"%s\\%s", Path, Data.cFileName);
        if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(Data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            Result = AcpRemoveTree(Child);
        }
        else
        {
            Result = ERROR_SUCCESS;
            SetFileAttributesW(Child, FILE_ATTRIBUTE_NORMAL);
            if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!RemoveDirectoryW(Child))
                    Result = GetLastError();
            }
            else if (!DeleteFileW(Child))
            {
                Result = GetLastError();
            }
        }
        if (Error == ERROR_SUCCESS)
            Error = Result;
    } while (FindNextFileW(Find, &Data));

    FindClose(Find);
    HeapFree(GetProcessHeap(), 0, Child);

    SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(Path) && Error == ERROR_SUCCESS)
        Error = GetLastError();
    return Error;
}

static
DWORD
AcpRemoveFolder(
    _In_ PCWSTR Path)
{
    WCHAR Raw[MAX_PATH + 4];

    /* Keep Win32 from trimming names such as ones with a trailing dot */
    if (Path[0] && Path[1] == L':' && SUCCEEDED(StringCchPrintfW(Raw, _countof(Raw), L"\\\\?\\%s", Path)))
        return AcpRemoveTree(Raw);
    return AcpRemoveTree(Path);
}

/**
 * @brief
 * Deletes the folders, storage key and mapping of a profile. Parts that are
 * already gone are not an error.
 */
static
HRESULT
AcpDeleteProfile(
    _In_ PAC_PROFILE Profile)
{
    HRESULT hr = S_OK;
    DWORD Error;

    Error = AcpRemoveFolder(Profile->Root);
    if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND)
        hr = HRESULT_FROM_WIN32(Error);

    Error = AcpRemoveKeyTree(Profile, Profile->Storage);
    if (SUCCEEDED(hr) && Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
        hr = HRESULT_FROM_WIN32(Error);

    Error = AcpDeleteKeyTree(HKEY_USERS, Profile->Mapping);
    if (SUCCEEDED(hr) && Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
        hr = HRESULT_FROM_WIN32(Error);

    return hr;
}

static
HRESULT
AcpEnsureFolder(
    _In_ PCWSTR Path)
{
    HRESULT hr;

    hr = AcpPathExists(Path);
    if (hr == S_FALSE && !CreateDirectoryW(Path, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    return FAILED(hr) ? hr : S_OK;
}

static
HRESULT
AcpCreateFolder(
    _In_ PCWSTR Path,
    _In_ DWORD Attributes,
    _In_opt_ PACL Dacl,
    _In_opt_ PACL Sacl)
{
    DWORD Error;

    if (!CreateDirectoryW(Path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    if (!SetFileAttributesW(Path, Attributes))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!Dacl)
        return S_OK;

    Error = SetNamedSecurityInfoW((PWSTR)Path,
                                  SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                                  NULL,
                                  NULL,
                                  Dacl,
                                  Sacl);
    return HRESULT_FROM_WIN32(Error);
}

/**
 * @brief
 * Creates the AC folder, or the child folder, with its known folders. The
 * container gets full access and the tree gets a low integrity label.
 */
static
HRESULT
AcpCreateFolders(
    _In_ PAC_PROFILE Profile)
{
    WCHAR Path[MAX_PATH];
    PACL Dacl = NULL, Sacl = NULL;
    ULONG Index;
    HRESULT hr;

    hr = AcpCreatePackageAcl(Profile->Sid, FILE_ALL_ACCESS, &Dacl);
    if (SUCCEEDED(hr))
        hr = AcpCreateLowLabel(&Sacl);
    if (SUCCEEDED(hr))
        hr = AcpCreateFolder(Profile->Folder, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, Dacl, Sacl);
    if (FAILED(hr))
        goto Quit;

    for (Index = 0; Index < _countof(AcKnownFolders) && SUCCEEDED(hr); Index++)
    {
        hr = StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Profile->Folder, AcKnownFolders[Index].Name);
        if (SUCCEEDED(hr))
            hr = AcpCreateFolder(Path, AcKnownFolders[Index].Attributes, NULL, NULL);
    }

    for (Index = 0; Profile->ParentSid && Index < _countof(AcChildFolders) && SUCCEEDED(hr); Index++)
    {
        hr = StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Profile->Folder, AcChildFolders[Index]);
        if (SUCCEEDED(hr))
            hr = AcpCreateFolder(Path, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, Dacl, Sacl);
    }

    if (FAILED(hr))
        AcpRemoveFolder(Profile->Root);

Quit:
    if (Dacl)
        HeapFree(GetProcessHeap(), 0, Dacl);
    if (Sacl)
        HeapFree(GetProcessHeap(), 0, Sacl);
    return hr;
}

/**
 * @brief
 * Creates the registry storage of a container, which only the container has
 * explicit access to. A parent also gets a Children key for its children.
 */
static
HRESULT
AcpCreateRegistryStorage(
    _In_ PAC_PROFILE Profile)
{
    SECURITY_ATTRIBUTES Attributes;
    PSECURITY_DESCRIPTOR Descriptor;
    WCHAR Text[AC_KEY_CHARS];
    HKEY Key = NULL, Children = NULL;
    PACL Dacl;
    LONG Error;
    HRESULT hr;

    if (!Profile->ParentSid)
    {
        hr = StringCchPrintfW(Text, _countof(Text), AcStorageRootSddl, Profile->UserSidString);
        if (FAILED(hr))
            return hr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Text, SDDL_REVISION_1, &Descriptor, NULL))
            return HRESULT_FROM_WIN32(GetLastError());

        Attributes.nLength = sizeof(Attributes);
        Attributes.lpSecurityDescriptor = Descriptor;
        Attributes.bInheritHandle = FALSE;
        hr = StringCchPrintfW(Text, _countof(Text), L"%s\\%s\\Storage", Profile->UserSidString, AcKeyRoot);
        if (SUCCEEDED(hr))
        {
            Error = RegCreateKeyExW(HKEY_USERS, Text, 0, NULL, 0, KEY_READ | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                                    &Attributes, &Key, NULL);
            hr = HRESULT_FROM_WIN32(Error);
            if (Error == ERROR_SUCCESS)
                RegCloseKey(Key);
            Key = NULL;
        }
        LocalFree(Descriptor);
        if (FAILED(hr))
            return hr;
    }

    hr = AcpCreatePackageAcl(Profile->Sid, KEY_ALL_ACCESS, &Dacl);
    if (FAILED(hr))
        return hr;

    Error = RegCreateKeyExW(HKEY_USERS, Profile->Storage, 0, NULL, 0, AC_STORAGE_ACCESS, NULL, &Key, NULL);
    if (Error == ERROR_SUCCESS)
        Error = SetSecurityInfo(Key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, Dacl, NULL);
    if (Error == ERROR_SUCCESS && !Profile->ParentSid)
        Error = RegCreateKeyExW(Key, L"Children", 0, NULL, 0, AC_STORAGE_ACCESS, NULL, &Children, NULL);
    if (Error == ERROR_SUCCESS && Children)
        Error = SetSecurityInfo(Children, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, Dacl, NULL);

    if (Children)
        RegCloseKey(Children);
    if (Key)
        RegCloseKey(Key);
    HeapFree(GetProcessHeap(), 0, Dacl);
    return HRESULT_FROM_WIN32(Error);
}

/* Creates the folders and the storage key of a container whose mapping exists */
static
HRESULT
AcpCreateStorage(
    _In_ PAC_PROFILE Profile)
{
    HRESULT hr;

    if (!Profile->ParentSid)
    {
        hr = AcpEnsureFolder(Profile->Packages);
        if (SUCCEEDED(hr))
            hr = AcpEnsureFolder(Profile->Root);
        if (FAILED(hr))
            return hr;
    }

    hr = AcpCreateRegistryStorage(Profile);
    if (FAILED(hr))
    {
        AcpRemoveFolder(Profile->Root);
        return hr;
    }

    hr = AcpCreateFolders(Profile);
    if (FAILED(hr))
        AcpRemoveKeyTree(Profile, Profile->Storage);
    return hr;
}

static
HRESULT
AcpCreateProfile(
    _In_ PAC_PROFILE Profile,
    _In_ PCWSTR DisplayName)
{
    ULONG Present, Missing;
    HRESULT hr;

    hr = AcpCountState(Profile, &Present, &Missing);
    if (FAILED(hr))
        return hr;

    if (!Missing)
    {
        hr = AcpRegisterMapping(Profile, DisplayName);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        {
            AcpDeleteProfile(Profile);
            return hr;
        }
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    /* Whatever an interrupted create or delete left behind is started over */
    if (Present)
        AcpDeleteProfile(Profile);

    hr = AcpRegisterMapping(Profile, DisplayName);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        return hr;

    hr = AcpCreateStorage(Profile);
    if (FAILED(hr))
    {
        AcpDeleteKeyTree(HKEY_USERS, Profile->Mapping);

        /* A failed create must not read as an existing profile */
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            hr = HRESULT_FROM_WIN32(ERROR_BAD_ENVIRONMENT);
    }
    return hr;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Creates the mapping, registry storage and folders of an AppContainer.
 * Called from inside a container it creates a child of that container.
 *
 * @return
 * S_OK and the container SID, to be released with FreeSid, or
 * HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) without a SID when the profile
 * already exists.
 */
HRESULT
WINAPI
CreateAppContainerProfile(
    _In_ PCWSTR AppContainerName,
    _In_ PCWSTR DisplayName,
    _In_ PCWSTR Description,
    _In_reads_opt_(CapabilityCount) PSID_AND_ATTRIBUTES Capabilities,
    _In_ DWORD CapabilityCount,
    _Outptr_ PSID *AppContainerSid)
{
    AC_PROFILE Profile;
    HRESULT hr;

    if (!AppContainerName || !DisplayName || !Description || !AppContainerSid)
        return E_INVALIDARG;
    if (!Capabilities != !CapabilityCount)
        return E_INVALIDARG;
    if (!AcpValidLength(AppContainerName, AC_NAME_MAX) ||
        !AcpValidLength(DisplayName, AC_DISPLAY_MAX) ||
        !AcpValidLength(Description, AC_DESCRIPTION_MAX))
    {
        return E_INVALIDARG;
    }

    /* Windows passes the description and capabilities to the firewall, ReactOS has no network isolation */

    hr = AcpOpenProfile(&Profile, AppContainerName, TRUE);
    if (SUCCEEDED(hr))
        hr = AcpCreateProfile(&Profile, DisplayName);
    if (SUCCEEDED(hr))
    {
        *AppContainerSid = Profile.Sid;
        Profile.Sid = NULL;
    }

    AcpCloseProfile(&Profile);
    return hr;
}

HRESULT
WINAPI
DeleteAppContainerProfile(
    _In_ PCWSTR AppContainerName)
{
    AC_PROFILE Profile;
    HRESULT hr;

    if (!AcpValidLength(AppContainerName, AC_NAME_MAX))
        return E_INVALIDARG;

    hr = AcpOpenProfile(&Profile, AppContainerName, FALSE);
    if (SUCCEEDED(hr))
        hr = AcpDeleteProfile(&Profile);

    AcpCloseProfile(&Profile);
    return hr;
}

/**
 * @brief
 * Derives the SID of an AppContainer from its name. Release it with FreeSid.
 */
HRESULT
WINAPI
DeriveAppContainerSidFromAppContainerName(
    _In_ PCWSTR AppContainerName,
    _Outptr_ PSID *AppContainerSid)
{
    if (!AppContainerSid || !AcpValidLength(AppContainerName, AC_NAME_MAX))
        return E_INVALIDARG;

    return AcpSidFromName(AppContainerName, AppContainerSid);
}

/**
 * @brief
 * Derives the SID of a child of an AppContainer. Release it with FreeSid.
 */
HRESULT
WINAPI
DeriveRestrictedAppContainerSidFromAppContainerSidAndRestrictedName(
    _In_ PSID AppContainerSid,
    _In_ PCWSTR RestrictedAppContainerName,
    _Outptr_ PSID *RestrictedAppContainerSid)
{
    if (!AppContainerSid || !RestrictedAppContainerName || !RestrictedAppContainerSid)
        return E_INVALIDARG;

    return AcpChildSidFromName(AppContainerSid, RestrictedAppContainerName, RestrictedAppContainerSid);
}

/**
 * @brief
 * Returns the AC folder of an AppContainer, allocated with CoTaskMemAlloc.
 */
HRESULT
WINAPI
GetAppContainerFolderPath(
    _In_ PCWSTR AppContainerSid,
    _Outptr_ PWSTR *Path)
{
    APPCONTAINER_SID_TYPE Type;
    AC_PROFILE Profile;
    PSID Sid = NULL;
    SIZE_T Size;
    NTSTATUS Status;
    HRESULT hr;

    if (!AppContainerSid || !Path)
        return E_INVALIDARG;

    if (!ConvertStringSidToSidW(AppContainerSid, &Sid))
        return HRESULT_FROM_WIN32(GetLastError());

    Status = RtlGetAppContainerSidType(Sid, &Type);
    if (!NT_SUCCESS(Status))
    {
        LocalFree(Sid);
        return AcpHrFromStatus(Status);
    }

    hr = AcpOpenCaller(&Profile, NULL);
    if (SUCCEEDED(hr))
    {
        Size = GetLengthSid(Sid);
        Profile.Sid = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (Profile.Sid)
            RtlCopySid((ULONG)Size, Profile.Sid, Sid);
        else
            hr = E_OUTOFMEMORY;
    }
    LocalFree(Sid);

    if (SUCCEEDED(hr) && Type == ChildAppContainerSidType)
    {
        Status = RtlGetAppContainerParent(Profile.Sid, &Profile.ParentSid);
        if (!NT_SUCCESS(Status))
            hr = AcpHrFromStatus(Status);
    }
    if (SUCCEEDED(hr))
        hr = AcpDescribe(&Profile);

    if (SUCCEEDED(hr))
    {
        Size = (wcslen(Profile.Folder) + 1) * sizeof(WCHAR);
        *Path = CoTaskMemAlloc(Size);
        if (*Path)
            RtlCopyMemory(*Path, Profile.Folder, Size);
        else
            hr = E_OUTOFMEMORY;
    }

    AcpCloseProfile(&Profile);
    return hr;
}

/**
 * @brief
 * Opens the registry storage of the AppContainer the caller runs in.
 */
HRESULT
WINAPI
GetAppContainerRegistryLocation(
    _In_ REGSAM DesiredAccess,
    _Outptr_ PHKEY AppContainerKey)
{
    APPCONTAINER_SID_TYPE Type;
    AC_PROFILE Profile;
    NTSTATUS Status;
    HKEY Key;
    LONG Error;
    HRESULT hr;

    if (!AppContainerKey)
        return E_INVALIDARG;

    hr = AcpOpenCaller(&Profile, &Profile.Sid);
    if (SUCCEEDED(hr) && !Profile.Sid)
        hr = E_UNEXPECTED;

    if (SUCCEEDED(hr))
    {
        Status = RtlGetAppContainerSidType(Profile.Sid, &Type);
        if (NT_SUCCESS(Status) && Type == ChildAppContainerSidType)
            Status = RtlGetAppContainerParent(Profile.Sid, &Profile.ParentSid);
        if (!NT_SUCCESS(Status))
            hr = AcpHrFromStatus(Status);
    }
    if (SUCCEEDED(hr))
        hr = AcpDescribe(&Profile);

    if (SUCCEEDED(hr))
    {
        Error = RegOpenKeyExW(HKEY_USERS, Profile.Storage, 0, DesiredAccess, &Key);
        if (Error == ERROR_SUCCESS)
            *AppContainerKey = Key;
        hr = HRESULT_FROM_WIN32(Error);
    }

    AcpCloseProfile(&Profile);
    return hr;
}
