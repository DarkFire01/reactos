/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stub functions for ksecdd
 * COPYRIGHT:   Copyright 2014 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"

#define NDEBUG
#include <debug.h>

typedef PVOID PSECURITY_PACKAGE_OPTIONS, PSecurityUserData;

/* FUNCTIONS ******************************************************************/

SECURITY_STATUS
SEC_ENTRY
AcceptSecurityContext(
    _In_opt_ PCredHandle phCredential,
    _In_opt_ PCtxtHandle phContext,
    _In_opt_ PSecBufferDesc pInput,
    _In_ ULONG fContextReq,
    _In_ ULONG TargetDataRep,
    _In_opt_ PCtxtHandle phNewContext,
    _In_opt_ PSecBufferDesc pOutput,
    _Out_ PULONG pfContextAttr,
    _Out_opt_ PTimeStamp ptsExpiry)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
AcquireCredentialsHandleW(
    _In_opt_ PSSPI_SEC_STRING pPrincipal,
    _In_ PSSPI_SEC_STRING pPackage,
    _In_ ULONG fCredentialUse,
    _In_opt_ PVOID pvLogonId,
    _In_opt_ PVOID pAuthData,
    _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
    _In_opt_ PVOID pvGetKeyArgument,
    _Out_ PCredHandle phCredential,
    _Out_opt_ PTimeStamp ptsExpiry)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
AddCredentialsW(
    _In_ PCredHandle hCredentials,
    _In_opt_ PSSPI_SEC_STRING pPrincipal,
    _In_ PSSPI_SEC_STRING pPackage,
    _In_ ULONG fCredentialUse,
    _In_opt_ PVOID pAuthData,
    _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
    _In_opt_ PVOID pvGetKeyArgument,
    _Out_opt_ PTimeStamp ptsExpiry)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
ApplyControlToken(
    _In_ PCtxtHandle phContext,
    _In_ PSecBufferDesc pInput)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
CredMarshalTargetInfo(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
DeleteSecurityContext(
    _In_ PCtxtHandle phContext)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
EfsDecryptFek(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

VOID
SEC_ENTRY
EfsGenerateKey(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
EnumerateSecurityPackagesW(
    _Out_ PULONG pcPackages,
    _Deref_out_ PSecPkgInfoW* ppPackageInfo)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
ExportSecurityContext(
    _In_ PCtxtHandle phContext,
    _In_ ULONG fFlags,
    _Out_ PSecBuffer pPackedContext,
    _Out_ PVOID* pToken)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
FreeContextBuffer(
    _Inout_ PVOID pvContextBuffer)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
FreeCredentialsHandle(
    _In_ PCredHandle phCredential)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
GenerateDirEfs(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

VOID
SEC_ENTRY
GenerateSessionKey(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
GetSecurityUserInfo(
    _In_opt_    PLUID LogonId,
    _In_        ULONG Flags,
    _Outptr_    PSecurityUserData *UserInformation)
{
    UNIMPLEMENTED;
    *UserInformation = NULL;
    return STATUS_UNSUCCESSFUL;
}

SECURITY_STATUS
SEC_ENTRY
ImpersonateSecurityContext(
    _In_ PCtxtHandle phContext)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
ImportSecurityContextW(
    _In_ PSSPI_SEC_STRING pszPackage,
    _In_ PSecBuffer pPackedContext,
    _In_ PVOID Token,
    _Out_ PCtxtHandle phContext)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
InitializeSecurityContextW(
    _In_opt_ PCredHandle phCredential,
    _In_opt_ PCtxtHandle phContext,
    _In_opt_ PSSPI_SEC_STRING pTargetName,
    _In_ ULONG fContextReq,
    _In_ ULONG Reserved1,
    _In_ ULONG TargetDataRep,
    _In_opt_ PSecBufferDesc pInput,
    _In_ ULONG Reserved2,
    _Inout_opt_ PCtxtHandle phNewContext,
    _Inout_opt_ PSecBufferDesc pOutput,
    _Out_ PULONG pfContextAttr,
    _Out_opt_ PTimeStamp ptsExpiry)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

PSecurityFunctionTableW
SEC_ENTRY
InitSecurityInterfaceW(void)
{

    UNIMPLEMENTED_DBGBREAK();
    return NULL;
}

VOID
SEC_ENTRY
KSecRegisterSecurityProvider(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

VOID
SEC_ENTRY
KSecValidateBuffer(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

VOID
SEC_ENTRY
LsaEnumerateLogonSessions(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

VOID
SEC_ENTRY
LsaGetLogonSessionData(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
MakeSignature(
    _In_ PCtxtHandle phContext,
    _In_ ULONG fQOP,
    _In_ PSecBufferDesc pMessage,
    _In_ ULONG MessageSeqNo)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
MapSecurityError(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
QueryContextAttributesW(
    _In_ PCtxtHandle phContext,
    _In_ ULONG ulAttribute,
    _Out_ PVOID pBuffer)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
QueryCredentialsAttributesW(
    _In_ PCredHandle phCredential,
    _In_ ULONG ulAttribute,
    _Inout_ PVOID pBuffer)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
QuerySecurityContextToken(
    _In_ PCtxtHandle phContext,
    _Out_ PVOID* Token)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
QuerySecurityPackageInfoW(
    _In_ PSSPI_SEC_STRING pPackageName,
    _Deref_out_ PSecPkgInfoW *ppPackageInfo)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

SECURITY_STATUS
SEC_ENTRY
RevertSecurityContext(
    _In_ PCtxtHandle phContext)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
SealMessage(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

NTSTATUS
SEC_ENTRY
SecLookupAccountName(
    _In_ PUNICODE_STRING Name,
    _Inout_ PULONG SidSize,
    _Out_ PSID Sid,
    _Out_ PSID_NAME_USE NameUse,
    _Out_opt_ PULONG DomainSize,
    _Inout_opt_ PUNICODE_STRING ReferencedDomain)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

NTSTATUS
SEC_ENTRY
SecLookupAccountSid(
    _In_ PSID Sid,
    _Out_ PULONG NameSize,
    _Inout_ PUNICODE_STRING NameBuffer,
    _Out_ PULONG DomainSize OPTIONAL,
    _Out_opt_ PUNICODE_STRING DomainBuffer,
    _Out_ PSID_NAME_USE NameUse)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

typedef struct _KSEC_WELL_KNOWN_SID
{
    UCHAR Type;
    UCHAR Authority;
    UCHAR SubAuthorityCount;
    ULONG SubAuthority[6];
} KSEC_WELL_KNOWN_SID, *PKSEC_WELL_KNOWN_SID;

/* Every type Windows can build without a domain SID; the rest need one the kernel does not have */
static const KSEC_WELL_KNOWN_SID KsecWellKnownSids[] =
{
    { 0, 0, 1, { 0 } },
    { 1, 1, 1, { 0 } },
    { 2, 2, 1, { 0 } },
    { 3, 3, 1, { 0 } },
    { 4, 3, 1, { 1 } },
    { 5, 3, 1, { 2 } },
    { 6, 3, 1, { 3 } },
    { 7, 5, 0, { 0 } },
    { 8, 5, 1, { 1 } },
    { 9, 5, 1, { 2 } },
    { 10, 5, 1, { 3 } },
    { 11, 5, 1, { 4 } },
    { 12, 5, 1, { 6 } },
    { 13, 5, 1, { 7 } },
    { 14, 5, 1, { 8 } },
    { 15, 5, 1, { 9 } },
    { 16, 5, 1, { 10 } },
    { 17, 5, 1, { 11 } },
    { 18, 5, 1, { 12 } },
    { 19, 5, 1, { 13 } },
    { 20, 5, 1, { 14 } },
    { 22, 5, 1, { 18 } },
    { 23, 5, 1, { 19 } },
    { 24, 5, 1, { 20 } },
    { 25, 5, 1, { 32 } },
    { 26, 5, 2, { 32, 544 } },
    { 27, 5, 2, { 32, 545 } },
    { 28, 5, 2, { 32, 546 } },
    { 29, 5, 2, { 32, 547 } },
    { 30, 5, 2, { 32, 548 } },
    { 31, 5, 2, { 32, 549 } },
    { 32, 5, 2, { 32, 550 } },
    { 33, 5, 2, { 32, 551 } },
    { 34, 5, 2, { 32, 552 } },
    { 35, 5, 2, { 32, 554 } },
    { 36, 5, 2, { 32, 555 } },
    { 37, 5, 2, { 32, 556 } },
    { 51, 5, 2, { 64, 10 } },
    { 52, 5, 2, { 64, 21 } },
    { 53, 5, 2, { 64, 14 } },
    { 54, 5, 1, { 15 } },
    { 55, 5, 1, { 1000 } },
    { 56, 5, 2, { 32, 557 } },
    { 57, 5, 2, { 32, 558 } },
    { 58, 5, 2, { 32, 559 } },
    { 59, 5, 2, { 32, 560 } },
    { 60, 5, 2, { 32, 561 } },
    { 61, 5, 2, { 32, 562 } },
    { 62, 5, 2, { 32, 568 } },
    { 63, 5, 1, { 17 } },
    { 64, 5, 2, { 32, 569 } },
    { 65, 16, 1, { 0 } },
    { 66, 16, 1, { 4096 } },
    { 67, 16, 1, { 8192 } },
    { 68, 16, 1, { 12288 } },
    { 69, 16, 1, { 16384 } },
    { 70, 5, 1, { 33 } },
    { 71, 3, 1, { 4 } },
    { 74, 5, 1, { 22 } },
    { 76, 5, 2, { 32, 573 } },
    { 78, 5, 2, { 32, 574 } },
    { 79, 16, 1, { 8448 } },
    { 81, 2, 1, { 1 } },
    { 82, 5, 2, { 65, 1 } },
    { 84, 15, 2, { 2, 1 } },
    { 85, 15, 2, { 3, 1 } },
    { 86, 15, 2, { 3, 2 } },
    { 87, 15, 2, { 3, 3 } },
    { 88, 15, 2, { 3, 4 } },
    { 89, 15, 2, { 3, 5 } },
    { 90, 15, 2, { 3, 6 } },
    { 91, 15, 2, { 3, 7 } },
    { 92, 15, 2, { 3, 9 } },
    { 93, 15, 2, { 3, 8 } },
    { 94, 15, 2, { 3, 10 } },
    { 95, 5, 2, { 32, 575 } },
    { 96, 5, 2, { 32, 576 } },
    { 97, 5, 2, { 32, 577 } },
    { 98, 5, 6, { 84, 0, 0, 0, 0, 0 } },
    { 99, 5, 2, { 32, 578 } },
    { 101, 5, 2, { 32, 579 } },
    { 102, 5, 2, { 32, 580 } },
    { 103, 18, 1, { 1 } },
    { 104, 18, 1, { 2 } },
    { 105, 5, 1, { 113 } },
    { 106, 5, 1, { 114 } },
    { 108, 15, 2, { 3, 11 } },
    { 109, 15, 2, { 3, 12 } },
    { 111, 5, 2, { 32, 581 } },
    { 112, 5, 2, { 32, 582 } },
    { 115, 18, 1, { 4 } },
    { 116, 18, 1, { 5 } },
    { 117, 18, 1, { 6 } },
    { 118, 18, 1, { 3 } },
    { 119, 5, 2, { 32, 583 } },
    { 120, 5, 2, { 32, 584 } },
    { 121, 5, 2, { 32, 585 } },
};

NTSTATUS
SEC_ENTRY
SecLookupWellKnownSid(
    _In_ WELL_KNOWN_SID_TYPE SidType,
    _Out_ PSID Sid,
    _In_ ULONG SidBufferSize,
    _Inout_opt_ PULONG SidSize)
{
    SID_IDENTIFIER_AUTHORITY Authority = { { 0, 0, 0, 0, 0, 0 } };
    const KSEC_WELL_KNOWN_SID *Entry = NULL;
    ULONG Index, Needed;

    for (Index = 0; Index < ARRAYSIZE(KsecWellKnownSids); Index++)
    {
        if (KsecWellKnownSids[Index].Type == (ULONG)SidType)
        {
            Entry = &KsecWellKnownSids[Index];
            break;
        }
    }

    if (Entry == NULL)
        return STATUS_INVALID_PARAMETER;

    Needed = RtlLengthRequiredSid(Entry->SubAuthorityCount);
    if (SidSize != NULL)
        *SidSize = Needed;
    if (Sid == NULL || SidBufferSize < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    Authority.Value[5] = Entry->Authority;
    RtlInitializeSid(Sid, &Authority, Entry->SubAuthorityCount);
    for (Index = 0; Index < Entry->SubAuthorityCount; Index++)
        *RtlSubAuthoritySid(Sid, Index) = Entry->SubAuthority[Index];

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SecMakeSPN(
    _In_ PUNICODE_STRING ServiceClass,
    _In_ PUNICODE_STRING ServiceName,
    _In_opt_ PUNICODE_STRING InstanceName,
    _In_opt_ USHORT InstancePort,
    _In_opt_ PUNICODE_STRING Referrer,
    _Inout_ PUNICODE_STRING Spn,
    _Out_opt_ PULONG Length,
    _In_ BOOLEAN Allocate)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

NTSTATUS
NTAPI
SecMakeSPNEx(
    _In_ PUNICODE_STRING ServiceClass,
    _In_ PUNICODE_STRING ServiceName,
    _In_opt_ PUNICODE_STRING InstanceName,
    _In_opt_ USHORT InstancePort,
    _In_opt_ PUNICODE_STRING Referrer,
    _In_opt_ PUNICODE_STRING TargetInfo,
    _Inout_ PUNICODE_STRING Spn,
    _Out_opt_ PULONG Length,
    _In_ BOOLEAN Allocate)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
SEC_ENTRY
SecSetPagingMode(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}
VOID
SEC_ENTRY
UnsealMessage(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
}

SECURITY_STATUS
SEC_ENTRY
VerifySignature(
    _In_ PCtxtHandle phContext,
    _In_ PSecBufferDesc pMessage,
    _In_ ULONG MessageSeqNo,
    _Out_ PULONG pfQOP)
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}
