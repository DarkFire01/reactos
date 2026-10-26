/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmse.c
 * PURPOSE:         Configuration Manager - Security Subsystem Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

/* FUNCTIONS *****************************************************************/

PSECURITY_DESCRIPTOR
NTAPI
CmpHiveRootSecurityDescriptor(VOID)
{
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    PACL Acl, AclCopy;
    PSID Sid[5];
    SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    SID_IDENTIFIER_AUTHORITY AppPackageAuthority = {SECURITY_APP_PACKAGE_AUTHORITY};
    ULONG AceLength, AclLength, SidLength;
    PACE_HEADER AceHeader;
    ULONG i;
    PAGED_CODE();

    /* Phase 1: Allocate SIDs */
    SidLength = RtlLengthRequiredSid(1);
    Sid[0] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    Sid[1] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    Sid[2] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    SidLength = RtlLengthRequiredSid(2);
    Sid[3] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    Sid[4] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);

    /* Make sure all SIDs were allocated */
    if (!Sid[0] || !Sid[1] || !Sid[2] || !Sid[3] || !Sid[4])
    {
        /* Bugcheck */
        KeBugCheckEx(REGISTRY_ERROR, 11, 1, 0, 0);
    }

    /* Phase 2: Initialize all SIDs */
    Status = RtlInitializeSid(Sid[0], &WorldAuthority, 1);
    Status |= RtlInitializeSid(Sid[1], &NtAuthority, 1);
    Status |= RtlInitializeSid(Sid[2], &NtAuthority, 1);
    Status |= RtlInitializeSid(Sid[3], &NtAuthority, 2);
    Status |= RtlInitializeSid(Sid[4], &AppPackageAuthority, SECURITY_BUILTIN_APP_PACKAGE_RID_COUNT);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 2, 0, 0);

    /* Phase 2: Setup SID Sub Authorities */
    *RtlSubAuthoritySid(Sid[0], 0) = SECURITY_WORLD_RID;
    *RtlSubAuthoritySid(Sid[1], 0) = SECURITY_RESTRICTED_CODE_RID;
    *RtlSubAuthoritySid(Sid[2], 0) = SECURITY_LOCAL_SYSTEM_RID;
    *RtlSubAuthoritySid(Sid[3], 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid(Sid[3], 1) = DOMAIN_ALIAS_RID_ADMINS;
    *RtlSubAuthoritySid(Sid[4], 0) = SECURITY_APP_PACKAGE_BASE_RID;
    *RtlSubAuthoritySid(Sid[4], 1) = SECURITY_BUILTIN_PACKAGE_ANY_PACKAGE;

    /* Make sure all SIDs are valid */
    ASSERT(RtlValidSid(Sid[0]));
    ASSERT(RtlValidSid(Sid[1]));
    ASSERT(RtlValidSid(Sid[2]));
    ASSERT(RtlValidSid(Sid[3]));
    ASSERT(RtlValidSid(Sid[4]));

    /* Phase 3: Calculate ACL Length */
    AclLength = sizeof(ACL);
    for (i = 0; i < 5; i++)
    {
        /* This is what MSDN says to do */
        AceLength = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart);
        AceLength += SeLengthSid(Sid[i]);
        AclLength += AceLength;
    }

    /* Phase 3: Allocate the ACL */
    Acl = ExAllocatePoolWithTag(PagedPool, AclLength, TAG_CMSD);
    if (!Acl) KeBugCheckEx(REGISTRY_ERROR, 11, 3, 0, 0);

    /* Phase 4: Create the ACL */
    Status = RtlCreateAcl(Acl, AclLength, ACL_REVISION);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 4, Status, 0);

    /* Phase 5: Build the ACL */
    Status = RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_ALL_ACCESS, Sid[2]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_ALL_ACCESS, Sid[3]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[0]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[1]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[4]);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 5, Status, 0);

    /* Phase 5: Make the ACEs inheritable */
    Status = RtlGetAce(Acl, 0, (PVOID*)&AceHeader);
    ASSERT(NT_SUCCESS(Status));
    AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;
    Status = RtlGetAce(Acl, 1, (PVOID*)&AceHeader);
    ASSERT(NT_SUCCESS(Status));
    AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;
    Status = RtlGetAce(Acl, 2, (PVOID*)&AceHeader);
    ASSERT(NT_SUCCESS(Status));
    AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;
    Status = RtlGetAce(Acl, 3, (PVOID*)&AceHeader);
    ASSERT(NT_SUCCESS(Status));
    AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;
    Status = RtlGetAce(Acl, 4, (PVOID*)&AceHeader);
    ASSERT(NT_SUCCESS(Status));
    AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;

    /* Phase 6: Allocate the security descriptor and make space for the ACL */
    SecurityDescriptor = ExAllocatePoolWithTag(PagedPool,
                                               sizeof(SECURITY_DESCRIPTOR) +
                                               AclLength,
                                               TAG_CMSD);
    if (!SecurityDescriptor) KeBugCheckEx(REGISTRY_ERROR, 11, 6, 0, 0);

    /* Phase 6: Make a copy of the ACL */
    AclCopy = (PACL)((PISECURITY_DESCRIPTOR)SecurityDescriptor + 1);
    RtlCopyMemory(AclCopy, Acl, AclLength);

    /* Phase 7: Create the security descriptor */
    Status = RtlCreateSecurityDescriptor(SecurityDescriptor,
                                         SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 7, Status, 0);

    /* Phase 8: Set the ACL as a DACL */
    Status = RtlSetDaclSecurityDescriptor(SecurityDescriptor,
                                          TRUE,
                                          AclCopy,
                                          FALSE);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 8, Status, 0);

    /* Free the SIDs and original ACL */
    for (i = 0; i < 5; i++) ExFreePoolWithTag(Sid[i], TAG_CMSD);
    ExFreePoolWithTag(Acl, TAG_CMSD);

    /* Return the security descriptor */
    return SecurityDescriptor;
}

/* Builds a self-relative copy of the default root descriptor, for keys that carry no security cell */
static
PSECURITY_DESCRIPTOR
CmpDefaultKeySecurity(VOID)
{
    PSECURITY_DESCRIPTOR Absolute, Relative;
    ULONG Length = 0;

    Absolute = CmpHiveRootSecurityDescriptor();
    RtlAbsoluteToSelfRelativeSD(Absolute, NULL, &Length);
    Relative = ExAllocatePoolWithTag(PagedPool, Length, TAG_CMSD);
    if (Relative && !NT_SUCCESS(RtlAbsoluteToSelfRelativeSD(Absolute, Relative, &Length)))
    {
        ExFreePoolWithTag(Relative, TAG_CMSD);
        Relative = NULL;
    }

    ExFreePoolWithTag(Absolute, TAG_CMSD);
    return Relative;
}

/* The hive's security lock guards the security cell lists and their reference counts */
static
VOID
CmpLockHiveSecurity(
    _In_ PHHIVE Hive)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&((PCMHIVE)Hive)->SecurityLock);
}

static
VOID
CmpUnlockHiveSecurity(
    _In_ PHHIVE Hive)
{
    ExReleasePushLockExclusive(&((PCMHIVE)Hive)->SecurityLock);
    KeLeaveCriticalRegion();
}

/* Returns a security cell of the hive, or NULL if the index does not name one */
static
PCM_KEY_SECURITY
CmpGetSecurityCell(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX Cell)
{
    PCM_KEY_SECURITY Security;

    if (Cell == HCELL_NIL || !HvIsCellAllocated(Hive, Cell))
        return NULL;

    Security = (PCM_KEY_SECURITY)HvGetCell(Hive, Cell);
    if (!Security)
        return NULL;

    HvReleaseCell(Hive, Cell);
    return (Security->Signature == CM_KEY_SECURITY_SIGNATURE) ? Security : NULL;
}

/* Tells whether a cell sits in a list whose neighbors point back at it */
static
BOOLEAN
CmpIsLinkedSecurityCell(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX Cell)
{
    PCM_KEY_SECURITY Security, Flink, Blink;

    Security = CmpGetSecurityCell(Hive, Cell);
    if (!Security)
        return FALSE;

    Flink = CmpGetSecurityCell(Hive, Security->Flink);
    Blink = CmpGetSecurityCell(Hive, Security->Blink);
    return Flink && Blink && Flink->Blink == Cell && Blink->Flink == Cell;
}

/**
 * @brief
 * Returns the security descriptor stored for a key node.
 *
 * @param[in] Hive
 * The hive holding the key.
 *
 * @param[in] Cell
 * The key node cell.
 *
 * @return
 * A pointer into the key's security cell, or NULL if the key has none.
 */
PSECURITY_DESCRIPTOR
NTAPI
CmpGetKeySecurityDescriptor(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX Cell)
{
    PCM_KEY_NODE Node;
    PCM_KEY_SECURITY Security;

    Node = (PCM_KEY_NODE)HvGetCell(Hive, Cell);
    if (!Node)
        return NULL;

    Security = CmpGetSecurityCell(Hive, Node->Security);
    HvReleaseCell(Hive, Cell);

    /* A cell without a usable descriptor counts as none */
    if (!Security ||
        !RtlValidRelativeSecurityDescriptor(&Security->Descriptor, Security->DescriptorLength, 0))
    {
        return NULL;
    }

    return &Security->Descriptor;
}

/* Looks for a cell holding the same descriptor among the ones linked to Start */
static
HCELL_INDEX
CmpFindSecurityCell(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX Start,
    _In_ PSECURITY_DESCRIPTOR Descriptor,
    _In_ ULONG Length)
{
    PCM_KEY_SECURITY Security;
    HCELL_INDEX Cell = Start;
    ULONG Steps;

    for (Steps = 0; Steps < 0x10000; Steps++)
    {
        Security = CmpGetSecurityCell(Hive, Cell);
        if (!Security)
            break;

        if (Security->DescriptorLength == Length &&
            RtlEqualMemory(&Security->Descriptor, Descriptor, Length))
        {
            return Cell;
        }

        Cell = Security->Flink;
        if (Cell == Start)
            break;
    }

    return HCELL_NIL;
}

/*
 * Makes a key node use a security cell holding the descriptor. A cell already
 * in the hive is shared when one matches, otherwise a new cell is linked next
 * to the one the key or its parent used. The reference to the old cell is dropped.
 */
static
NTSTATUS
CmpSetKeyNodeSecurity(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX NodeCell,
    _In_ PCM_KEY_NODE Node,
    _In_ PSECURITY_DESCRIPTOR Descriptor)
{
    HSTORAGE_TYPE Type = HvGetCellType(NodeCell);
    PCM_KEY_SECURITY Security, Near, Next;
    PCM_KEY_NODE Parent;
    HCELL_INDEX Start = HCELL_NIL, Cell, NextCell;
    ULONG Length;

    ASSERT(((PISECURITY_DESCRIPTOR)Descriptor)->Control & SE_SELF_RELATIVE);
    Length = RtlLengthSecurityDescriptor(Descriptor);

    /* Begin the search where a match is most likely */
    if (CmpGetSecurityCell(Hive, Node->Security))
        Start = Node->Security;

    if (Start == HCELL_NIL && !(Node->Flags & KEY_HIVE_ENTRY) && Node->Parent != HCELL_NIL)
    {
        Parent = (PCM_KEY_NODE)HvGetCell(Hive, Node->Parent);
        if (Parent)
        {
            if (CmpGetSecurityCell(Hive, Parent->Security))
                Start = Parent->Security;
            HvReleaseCell(Hive, Node->Parent);
        }
    }

    if (Start != HCELL_NIL && HvGetCellType(Start) != Type)
        Start = HCELL_NIL;

    Cell = (Start != HCELL_NIL) ? CmpFindSecurityCell(Hive, Start, Descriptor, Length) : HCELL_NIL;
    if (Cell != HCELL_NIL)
    {
        if (!HvMarkCellDirty(Hive, Cell, FALSE))
            return STATUS_NO_LOG_SPACE;

        Security = CmpGetSecurityCell(Hive, Cell);
        Security->ReferenceCount++;
    }
    else
    {
        Cell = HvAllocateCell(Hive, FIELD_OFFSET(CM_KEY_SECURITY, Descriptor) + Length, Type, HCELL_NIL);
        if (Cell == HCELL_NIL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Security = (PCM_KEY_SECURITY)HvGetCell(Hive, Cell);
        HvReleaseCell(Hive, Cell);
        Security->Signature = CM_KEY_SECURITY_SIGNATURE;
        Security->Reserved = 0;
        Security->ReferenceCount = 1;
        Security->DescriptorLength = Length;
        RtlCopyMemory(&Security->Descriptor, Descriptor, Length);
        Security->Flink = Security->Blink = Cell;

        /* Stable cells form one list per hive, volatile ones stand alone */
        Near = (Type == Stable && Start != HCELL_NIL && CmpIsLinkedSecurityCell(Hive, Start)) ?
               CmpGetSecurityCell(Hive, Start) : NULL;
        Next = Near ? CmpGetSecurityCell(Hive, Near->Flink) : NULL;
        if (Next && HvMarkCellDirty(Hive, Start, FALSE) && HvMarkCellDirty(Hive, Near->Flink, FALSE))
        {
            NextCell = Near->Flink;
            Security->Flink = NextCell;
            Security->Blink = Start;
            Near->Flink = Cell;
            Next->Blink = Cell;
        }
    }

    /* Let go of the old cell, which frees it if nobody else uses it */
    if (Node->Security != HCELL_NIL)
    {
        Near = CmpGetSecurityCell(Hive, Node->Security);
        if (Near && CmpIsLinkedSecurityCell(Hive, Node->Security))
        {
            HvMarkCellDirty(Hive, Node->Security, FALSE);
            HvMarkCellDirty(Hive, Near->Flink, FALSE);
            HvMarkCellDirty(Hive, Near->Blink, FALSE);
            CmpFreeSecurityDescriptor(Hive, NodeCell);
        }
        else if (Near && Near->ReferenceCount > 1 && HvMarkCellDirty(Hive, Node->Security, FALSE))
        {
            /* A cell outside a sound list cannot be unlinked, so the last reference keeps it */
            Near->ReferenceCount--;
        }
    }

    Node->Security = Cell;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives a key node a security cell holding the descriptor, sharing one already
 * in the hive when it matches.
 *
 * @param[in] Hive
 * The hive holding the key.
 *
 * @param[in] NodeCell
 * The key node cell.
 *
 * @param[in] Node
 * The key node, marked dirty by the caller.
 *
 * @param[in] Descriptor
 * A self-relative security descriptor.
 *
 * @return
 * STATUS_SUCCESS, or a failure code.
 */
NTSTATUS
NTAPI
CmpAssignKeyNodeSecurity(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX NodeCell,
    _In_ PCM_KEY_NODE Node,
    _In_ PSECURITY_DESCRIPTOR Descriptor)
{
    NTSTATUS Status;

    CmpLockHiveSecurity(Hive);
    Status = CmpSetKeyNodeSecurity(Hive, NodeCell, Node, Descriptor);
    CmpUnlockHiveSecurity(Hive);
    return Status;
}

NTSTATUS
CmpQuerySecurityDescriptor(
    _In_ PCM_KEY_CONTROL_BLOCK Kcb,
    _In_ SECURITY_INFORMATION SecurityInformation,
    _Out_writes_bytes_(*BufferLength) PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PULONG BufferLength)
{
    PSECURITY_DESCRIPTOR Descriptor, Default = NULL;
    NTSTATUS Status;

    DPRINT("CmpQuerySecurityDescriptor()\n");

    if (SecurityInformation == 0)
    {
        return STATUS_ACCESS_DENIED;
    }

    Descriptor = CmpGetKeySecurityDescriptor(Kcb->KeyHive, Kcb->KeyCell);
    if (!Descriptor)
    {
        /* Keys made by older builds have no security cell */
        Default = CmpDefaultKeySecurity();
        if (!Default)
            return STATUS_INSUFFICIENT_RESOURCES;
        Descriptor = Default;
    }

    Status = SeQuerySecurityDescriptorInfo(&SecurityInformation,
                                           SecurityDescriptor,
                                           BufferLength,
                                           &Descriptor);

    if (Default)
        ExFreePoolWithTag(Default, TAG_CMSD);

    return Status;
}

NTSTATUS
CmpSetSecurityDescriptor(
    _In_ PCM_KEY_CONTROL_BLOCK Kcb,
    _In_ PSECURITY_INFORMATION SecurityInformation,
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _In_ POOL_TYPE PoolType,
    _In_ PGENERIC_MAPPING GenericMapping)
{
    PHHIVE Hive = Kcb->KeyHive;
    PSECURITY_DESCRIPTOR Current, Default = NULL;
    PCM_KEY_NODE Node;
    LARGE_INTEGER TimeStamp;
    NTSTATUS Status;

    DPRINT("CmpSetSecurityDescriptor()\n");

    Current = CmpGetKeySecurityDescriptor(Hive, Kcb->KeyCell);
    if (!Current)
    {
        Default = CmpDefaultKeySecurity();
        if (!Default)
            return STATUS_INSUFFICIENT_RESOURCES;
        Current = Default;
    }

    /* Merge the parts the caller changes into what the key has now */
    Status = SeSetSecurityDescriptorInfo(NULL,
                                         SecurityInformation,
                                         SecurityDescriptor,
                                         &Current,
                                         PoolType,
                                         GenericMapping);
    if (Default)
        ExFreePoolWithTag(Default, TAG_CMSD);
    if (!NT_SUCCESS(Status))
        return Status;

    Node = (PCM_KEY_NODE)HvGetCell(Hive, Kcb->KeyCell);
    if (!Node)
    {
        ExFreePoolWithTag(Current, TAG_SD);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    CmpLockHiveFlusherShared((PCMHIVE)Hive);
    if (HvMarkCellDirty(Hive, Kcb->KeyCell, FALSE))
    {
        CmpLockHiveSecurity(Hive);
        Status = CmpSetKeyNodeSecurity(Hive, Kcb->KeyCell, Node, Current);
        CmpUnlockHiveSecurity(Hive);
    }
    else
    {
        Status = STATUS_NO_LOG_SPACE;
    }
    CmpUnlockHiveFlusher((PCMHIVE)Hive);

    if (NT_SUCCESS(Status))
    {
        KeQuerySystemTime(&TimeStamp);
        Node->LastWriteTime = TimeStamp;
        Kcb->KcbLastWriteTime = TimeStamp;
        CmpReportNotify(Kcb, Hive, Kcb->KeyCell, REG_NOTIFY_CHANGE_SECURITY);
    }

    HvReleaseCell(Hive, Kcb->KeyCell);
    ExFreePoolWithTag(Current, TAG_SD);
    return Status;
}

NTSTATUS
CmpAssignSecurityDescriptor(
    _In_ PCM_KEY_CONTROL_BLOCK Kcb,
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    PHHIVE Hive = Kcb->KeyHive;
    PSECURITY_DESCRIPTOR Relative = NULL;
    PCM_KEY_NODE Node;
    ULONG Length = 0;
    NTSTATUS Status;

    DPRINT("CmpAssignSecurityDescriptor(%p %p)\n",
           Kcb, SecurityDescriptor);

    /* Hive cells hold self-relative descriptors */
    if (!(((PISECURITY_DESCRIPTOR)SecurityDescriptor)->Control & SE_SELF_RELATIVE))
    {
        RtlAbsoluteToSelfRelativeSD(SecurityDescriptor, NULL, &Length);
        Relative = ExAllocatePoolWithTag(PagedPool, Length, TAG_CMSD);
        if (!Relative)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = RtlAbsoluteToSelfRelativeSD(SecurityDescriptor, Relative, &Length);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Relative, TAG_CMSD);
            return Status;
        }
        SecurityDescriptor = Relative;
    }

    Node = (PCM_KEY_NODE)HvGetCell(Hive, Kcb->KeyCell);
    if (!Node)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        if (HvMarkCellDirty(Hive, Kcb->KeyCell, FALSE))
        {
            CmpLockHiveSecurity(Hive);
            Status = CmpSetKeyNodeSecurity(Hive, Kcb->KeyCell, Node, SecurityDescriptor);
            CmpUnlockHiveSecurity(Hive);
        }
        else
        {
            Status = STATUS_NO_LOG_SPACE;
        }
        HvReleaseCell(Hive, Kcb->KeyCell);
    }

    if (Relative)
        ExFreePoolWithTag(Relative, TAG_CMSD);

    return Status;
}

NTSTATUS
NTAPI
CmpSecurityMethod(IN PVOID ObjectBody,
                  IN SECURITY_OPERATION_CODE OperationCode,
                  IN PSECURITY_INFORMATION SecurityInformation,
                  IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
                  IN OUT PULONG BufferLength,
                  IN OUT PSECURITY_DESCRIPTOR *OldSecurityDescriptor,
                  IN POOL_TYPE PoolType,
                  IN PGENERIC_MAPPING GenericMapping,
                  IN KPROCESSOR_MODE AccessMode)
{
    PCM_KEY_CONTROL_BLOCK Kcb;
    NTSTATUS Status = STATUS_SUCCESS;

    DBG_UNREFERENCED_PARAMETER(OldSecurityDescriptor);
    DBG_UNREFERENCED_PARAMETER(GenericMapping);

    Kcb = ((PCM_KEY_BODY)ObjectBody)->KeyControlBlock;

    /* Acquire the hive lock */
    CmpLockRegistry();

    /* Acquire the KCB lock */
    if (OperationCode == QuerySecurityDescriptor)
    {
        /* Avoid recursive locking if somebody already holds it */
        if (!((PCM_KEY_BODY)ObjectBody)->KcbLocked)
        {
            CmpAcquireKcbLockShared(Kcb);
        }
    }
    else
    {
        ASSERT(!((PCM_KEY_BODY)ObjectBody)->KcbLocked);
        CmpAcquireKcbLockExclusive(Kcb);
    }

    /* Don't touch deleted keys */
    if (Kcb->Delete)
    {
        /* Release the KCB lock */
        CmpReleaseKcbLock(Kcb);

        /* Release the hive lock */
        CmpUnlockRegistry();
        return STATUS_KEY_DELETED;
    }

    switch (OperationCode)
    {
        case SetSecurityDescriptor:
            DPRINT("Set security descriptor\n");
            ASSERT((PoolType == PagedPool) || (PoolType == NonPagedPool));
            Status = CmpSetSecurityDescriptor(Kcb,
                                              SecurityInformation,
                                              SecurityDescriptor,
                                              PoolType,
                                              GenericMapping);
            break;

        case QuerySecurityDescriptor:
            DPRINT("Query security descriptor\n");
            Status = CmpQuerySecurityDescriptor(Kcb,
                                                *SecurityInformation,
                                                SecurityDescriptor,
                                                BufferLength);
            break;

        case DeleteSecurityDescriptor:
            DPRINT("Delete security descriptor\n");
            /* HACK */
            break;

        case AssignSecurityDescriptor:
            DPRINT("Assign security descriptor\n");
            Status = CmpAssignSecurityDescriptor(Kcb,
                                                 SecurityDescriptor);
            break;

        default:
            KeBugCheckEx(SECURITY_SYSTEM, 0, STATUS_INVALID_PARAMETER, 0, 0);
    }

    /*
     * Release the KCB lock, but only if we locked it ourselves and
     * nobody else was locking it by themselves.
     */
    if (!((PCM_KEY_BODY)ObjectBody)->KcbLocked)
    {
        CmpReleaseKcbLock(Kcb);
    }

    /* Release the hive lock */
    CmpUnlockRegistry();

    return Status;
}
