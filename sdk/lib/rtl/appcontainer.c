/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AppContainer (package) and capability SID helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#define NDEBUG
#include <debug.h>

/* Shared with sid.c so that RtlFreeSid releases what RtlGetAppContainerParent returns */
#define TAG_SID 'diSp'

static SID_IDENTIFIER_AUTHORITY RtlpAppPackageAuthority = SECURITY_APP_PACKAGE_AUTHORITY;

/* Access rights asked of the capability check descriptor: DELETE and one specific right */
#define RTLP_CAPABILITY_CHECK_ACCESS (DELETE | 0x1)

static GENERIC_MAPPING RtlpCheckTokenCapabilityMapping =
{
    STANDARD_RIGHTS_READ,
    STANDARD_RIGHTS_WRITE,
    STANDARD_RIGHTS_EXECUTE,
    STANDARD_RIGHTS_ALL | 0x1
};

/* PRIVATE FUNCTIONS ********************************************************/

static
BOOLEAN
RtlpIsAppPackageSid(
    _In_ PSID Sid,
    _In_ ULONG BaseRid)
{
    PISID Value = Sid;

    return Value->Revision == SID_REVISION &&
           Value->SubAuthorityCount >= 2 &&
           RtlEqualMemory(&Value->IdentifierAuthority, &RtlpAppPackageAuthority, sizeof(RtlpAppPackageAuthority)) &&
           Value->SubAuthority[0] == BaseRid;
}

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Returns TRUE for a SID in the S-1-15-2 (application package) space.
 */
BOOLEAN
NTAPI
RtlIsPackageSid(
    _In_ PSID Sid)
{
    return RtlpIsAppPackageSid(Sid, SECURITY_APP_PACKAGE_BASE_RID);
}

/**
 * @brief
 * Returns TRUE for a SID in the S-1-15-3 (capability) space.
 */
BOOLEAN
NTAPI
RtlIsCapabilitySid(
    _In_ PSID Sid)
{
    return RtlpIsAppPackageSid(Sid, SECURITY_CAPABILITY_BASE_RID);
}

/**
 * @brief
 * Classifies a package SID as a parent (8 sub-authorities) or child (12) AppContainer.
 *
 * @return
 * STATUS_SUCCESS for a parent or child AppContainer SID, STATUS_NOT_APPCONTAINER otherwise.
 */
NTSTATUS
NTAPI
RtlGetAppContainerSidType(
    _In_ PSID AppContainerSid,
    _Out_ PAPPCONTAINER_SID_TYPE AppContainerSidType)
{
    if (!RtlIsPackageSid(AppContainerSid))
    {
        *AppContainerSidType = NotAppContainerSidType;
        return STATUS_NOT_APPCONTAINER;
    }

    switch (*RtlSubAuthorityCountSid(AppContainerSid))
    {
        case SECURITY_PARENT_PACKAGE_RID_COUNT:
            *AppContainerSidType = ParentAppContainerSidType;
            return STATUS_SUCCESS;

        case SECURITY_CHILD_PACKAGE_RID_COUNT:
            *AppContainerSidType = ChildAppContainerSidType;
            return STATUS_SUCCESS;

        default:
            *AppContainerSidType = InvalidAppContainerSidType;
            return STATUS_NOT_APPCONTAINER;
    }
}

/**
 * @brief
 * Returns TRUE when the child AppContainer SID was derived from the parent one,
 * that is, when it starts with the parent's eight sub-authorities.
 */
BOOLEAN
NTAPI
RtlIsParentOfChildAppContainer(
    _In_ PSID ParentAppContainerSid,
    _In_ PSID ChildAppContainerSid)
{
    APPCONTAINER_SID_TYPE Type;
    ULONG Index;

    if (!NT_SUCCESS(RtlGetAppContainerSidType(ParentAppContainerSid, &Type)) ||
        Type != ParentAppContainerSidType ||
        !NT_SUCCESS(RtlGetAppContainerSidType(ChildAppContainerSid, &Type)) ||
        Type != ChildAppContainerSidType)
    {
        return FALSE;
    }

    for (Index = 1; Index < SECURITY_PARENT_PACKAGE_RID_COUNT; Index++)
    {
        if (*RtlSubAuthoritySid(ParentAppContainerSid, Index) != *RtlSubAuthoritySid(ChildAppContainerSid, Index))
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Builds the parent AppContainer SID of a child one. Free the result with RtlFreeSid.
 */
NTSTATUS
NTAPI
RtlGetAppContainerParent(
    _In_ PSID ChildAppContainerSid,
    _Out_ PSID *ParentAppContainerSid)
{
    APPCONTAINER_SID_TYPE Type;
    PSID Parent;
    ULONG Index;

    *ParentAppContainerSid = NULL;

    if (!NT_SUCCESS(RtlGetAppContainerSidType(ChildAppContainerSid, &Type)) ||
        Type != ChildAppContainerSidType)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Parent = RtlpAllocateMemory(RtlLengthRequiredSid(SECURITY_PARENT_PACKAGE_RID_COUNT), TAG_SID);
    if (Parent == NULL)
        return STATUS_NO_MEMORY;

    RtlInitializeSid(Parent, &RtlpAppPackageAuthority, SECURITY_PARENT_PACKAGE_RID_COUNT);
    for (Index = 0; Index < SECURITY_PARENT_PACKAGE_RID_COUNT; Index++)
    {
        *RtlSubAuthoritySid(Parent, Index) = *RtlSubAuthoritySid(ChildAppContainerSid, Index);
    }

    *ParentAppContainerSid = Parent;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Checks whether a token holds a capability, with an access check against a
 * descriptor that grants the user and the capability. A token outside an
 * AppContainer passes through its user entry and always has the capability.
 *
 * @param[in] TokenHandle
 * An impersonation token with TOKEN_QUERY access, or NULL for the effective token.
 *
 * @param[in] CapabilitySidToCheck
 * A capability (S-1-15-3) SID.
 *
 * @param[out] HasCapability
 * Receives the answer.
 */
NTSTATUS
NTAPI
RtlCheckTokenCapability(
    _In_opt_ HANDLE TokenHandle,
    _In_ PSID CapabilitySidToCheck,
    _Out_ PBOOLEAN HasCapability)
{
    SECURITY_QUALITY_OF_SERVICE Qos;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_DESCRIPTOR Descriptor;
    UCHAR UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    UCHAR AclBuffer[sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE)];
    UCHAR PrivilegeBuffer[sizeof(PRIVILEGE_SET) + 4 * sizeof(LUID_AND_ATTRIBUTES)];
    PTOKEN_USER User = (PTOKEN_USER)UserBuffer;
    PACL Dacl = (PACL)AclBuffer;
    ULONG PrivilegeLength = sizeof(PrivilegeBuffer);
    ULONG HandleAttributes;
    ULONG Length;
    HANDLE Token = NULL;
    HANDLE ProcessToken;
    ACCESS_MASK Granted = 0;
    NTSTATUS AccessStatus;
    NTSTATUS Status;

    *HasCapability = FALSE;

    if (!RtlIsCapabilitySid(CapabilitySidToCheck))
        return STATUS_INVALID_PARAMETER;

    HandleAttributes = (RtlpGetMode() == KernelMode) ? OBJ_KERNEL_HANDLE : 0;

    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
    Qos.EffectiveOnly = FALSE;
    InitializeObjectAttributes(&ObjectAttributes, NULL, HandleAttributes, NULL, NULL);
    ObjectAttributes.SecurityQualityOfService = &Qos;

    if (TokenHandle == NULL)
    {
        Status = ZwOpenThreadTokenEx(NtCurrentThread(), TOKEN_QUERY, TRUE, HandleAttributes, &Token);
        if (Status == STATUS_NO_TOKEN)
        {
            Status = ZwOpenProcessTokenEx(NtCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, HandleAttributes, &ProcessToken);
            if (!NT_SUCCESS(Status))
                return Status;

            Status = ZwDuplicateToken(ProcessToken, TOKEN_QUERY, &ObjectAttributes, FALSE, TokenImpersonation, &Token);
            ZwClose(ProcessToken);
        }

        if (!NT_SUCCESS(Status))
            return Status;

        TokenHandle = Token;
    }

    Status = ZwQueryInformationToken(TokenHandle, TokenUser, User, sizeof(UserBuffer), &Length);
    if (NT_SUCCESS(Status))
    {
        RtlCreateSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&Descriptor, User->User.Sid, FALSE);
        RtlSetGroupSecurityDescriptor(&Descriptor, User->User.Sid, FALSE);
        RtlCreateAcl(Dacl, sizeof(AclBuffer), ACL_REVISION2);
        RtlAddAccessAllowedAce(Dacl, ACL_REVISION2, RTLP_CAPABILITY_CHECK_ACCESS, User->User.Sid);
        RtlAddAccessAllowedAce(Dacl, ACL_REVISION2, RTLP_CAPABILITY_CHECK_ACCESS, CapabilitySidToCheck);
        RtlSetDaclSecurityDescriptor(&Descriptor, TRUE, Dacl, FALSE);

        Status = ZwAccessCheck(&Descriptor,
                               TokenHandle,
                               RTLP_CAPABILITY_CHECK_ACCESS,
                               &RtlpCheckTokenCapabilityMapping,
                               (PPRIVILEGE_SET)PrivilegeBuffer,
                               &PrivilegeLength,
                               &Granted,
                               &AccessStatus);
        if (NT_SUCCESS(Status))
            *HasCapability = (NT_SUCCESS(AccessStatus) && Granted == RTLP_CAPABILITY_CHECK_ACCESS);
    }

    if (Token)
        ZwClose(Token);

    return Status;
}

/* EOF */
