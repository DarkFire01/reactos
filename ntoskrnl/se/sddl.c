/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Reading a security descriptor out of its written form
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * The written form of a descriptor is four optional sections, each a letter and
 * a colon followed by its body: an owner, a group, a discretionary list and a
 * system list. A list is a run of flag letters and then a bracketed run of
 * entries, and an entry is six fields separated by semicolons, of which a
 * plain allowing entry uses three.
 *
 * What comes back is one block holding the descriptor and everything it points
 * at, so the caller frees it with a single call.
 */

#define TAG_SDDL 'LDDS'

#define SDDL_REVISION_1 1

/* TYPES **********************************************************************/

typedef struct _SEP_SDDL_NAME
{
    WCHAR Name[2];
    ULONG Value;
} SEP_SDDL_NAME, *PSEP_SDDL_NAME;

/* GLOBALS ********************************************************************/

/* The rights a two letter name stands for, per the security descriptor string format */
static const SEP_SDDL_NAME SepSddlRights[] =
{
    { { L'G', L'A' }, GENERIC_ALL },
    { { L'G', L'R' }, GENERIC_READ },
    { { L'G', L'W' }, GENERIC_WRITE },
    { { L'G', L'X' }, GENERIC_EXECUTE },
    { { L'R', L'C' }, READ_CONTROL },
    { { L'S', L'D' }, DELETE },
    { { L'W', L'D' }, WRITE_DAC },
    { { L'W', L'O' }, WRITE_OWNER },
    { { L'F', L'A' }, FILE_ALL_ACCESS },
    { { L'F', L'R' }, FILE_GENERIC_READ },
    { { L'F', L'W' }, FILE_GENERIC_WRITE },
    { { L'F', L'X' }, FILE_GENERIC_EXECUTE },
    { { L'K', L'A' }, KEY_ALL_ACCESS },
    { { L'K', L'R' }, KEY_READ },
    { { L'K', L'W' }, KEY_WRITE },
    { { L'K', L'X' }, KEY_EXECUTE },
    { { L'C', L'C' }, 0x00000001 },
    { { L'D', L'C' }, 0x00000002 },
    { { L'L', L'C' }, 0x00000004 },
    { { L'S', L'W' }, 0x00000008 },
    { { L'R', L'P' }, 0x00000010 },
    { { L'W', L'P' }, 0x00000020 },
    { { L'D', L'T' }, 0x00000040 },
    { { L'L', L'O' }, 0x00000080 },
    { { L'C', L'R' }, 0x00000100 },
};

/* The flags a two letter name stands for on an entry */
static const SEP_SDDL_NAME SepSddlAceFlags[] =
{
    { { L'C', L'I' }, CONTAINER_INHERIT_ACE },
    { { L'O', L'I' }, OBJECT_INHERIT_ACE },
    { { L'N', L'P' }, NO_PROPAGATE_INHERIT_ACE },
    { { L'I', L'O' }, INHERIT_ONLY_ACE },
    { { L'I', L'D' }, INHERITED_ACE },
    { { L'S', L'A' }, SUCCESSFUL_ACCESS_ACE_FLAG },
    { { L'F', L'A' }, FAILED_ACCESS_ACE_FLAG },
};

/* The kind of entry a two letter name stands for */
static const SEP_SDDL_NAME SepSddlAceTypes[] =
{
    { { L'A', L'\0' }, ACCESS_ALLOWED_ACE_TYPE },
    { { L'D', L'\0' }, ACCESS_DENIED_ACE_TYPE },
    { { L'A', L'U' }, SYSTEM_AUDIT_ACE_TYPE },
    { { L'A', L'L' }, SYSTEM_ALARM_ACE_TYPE },
};

/*
 * The accounts a two letter name stands for. Each is the authority and the
 * subauthorities the name is defined as, not a lookup of anything.
 */
typedef struct _SEP_SDDL_SID
{
    WCHAR Name[2];
    UCHAR Authority;
    UCHAR Count;
    ULONG SubAuthority[2];
} SEP_SDDL_SID;

static const SEP_SDDL_SID SepSddlSids[] =
{
    { { L'W', L'D' }, 1, 1, { 0, 0 } },                              /* Everyone */
    { { L'S', L'Y' }, 5, 1, { 18, 0 } },                             /* Local system */
    { { L'L', L'S' }, 5, 1, { 19, 0 } },                             /* Local service */
    { { L'N', L'S' }, 5, 1, { 20, 0 } },                             /* Network service */
    { { L'I', L'U' }, 5, 1, { 4, 0 } },                              /* Interactive */
    { { L'N', L'U' }, 5, 1, { 2, 0 } },                              /* Network */
    { { L'A', L'N' }, 5, 1, { 7, 0 } },                              /* Anonymous */
    { { L'A', L'U' }, 5, 1, { 11, 0 } },                             /* Authenticated */
    { { L'B', L'A' }, 5, 2, { 32, 544 } },                           /* Administrators */
    { { L'B', L'U' }, 5, 2, { 32, 545 } },                           /* Users */
    { { L'B', L'G' }, 5, 2, { 32, 546 } },                           /* Guests */
    { { L'P', L'U' }, 5, 2, { 32, 547 } },                           /* Power users */
};

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
NTAPI
SepSddlMatch(
    _In_reads_(2) const WCHAR *Name,
    _In_z_ PCWSTR Text)
{
    if (Name[1] == UNICODE_NULL)
        return (Text[0] == Name[0]);

    return ((Text[0] == Name[0]) && (Text[1] == Name[1]));
}

/* Reads a number the string writes in hexadecimal or in decimal */
static
BOOLEAN
NTAPI
SepSddlReadNumber(
    _Inout_ PCWSTR *Text,
    _Out_ PULONG Value)
{
    PCWSTR At = *Text;
    ULONG Result = 0;
    ULONG Base = 10;
    BOOLEAN Any = FALSE;

    if ((At[0] == L'0') && ((At[1] == L'x') || (At[1] == L'X')))
    {
        Base = 16;
        At += 2;
    }

    for (;;)
    {
        ULONG Digit;

        if ((*At >= L'0') && (*At <= L'9'))
            Digit = *At - L'0';
        else if ((Base == 16) && (*At >= L'a') && (*At <= L'f'))
            Digit = (*At - L'a') + 10;
        else if ((Base == 16) && (*At >= L'A') && (*At <= L'F'))
            Digit = (*At - L'A') + 10;
        else
            break;

        Result = (Result * Base) + Digit;
        Any = TRUE;
        At++;
    }

    if (!Any)
        return FALSE;

    *Text = At;
    *Value = Result;
    return TRUE;
}

/*
 * Reads an account, either written out in full or named by its two letters,
 * into the buffer when there is one and measures it either way.
 */
static
BOOLEAN
NTAPI
SepSddlReadSid(
    _Inout_ PCWSTR *Text,
    _Out_writes_bytes_opt_(*Length) PSID Sid,
    _Inout_ PULONG Length)
{
    SID_IDENTIFIER_AUTHORITY Authority = { { 0, 0, 0, 0, 0, 0 } };
    ULONG SubAuthority[SID_MAX_SUB_AUTHORITIES];
    PCWSTR At = *Text;
    ULONG Count = 0;
    ULONG Revision, Value;
    ULONG Index;

    if ((At[0] == L'S') && (At[1] == L'-'))
    {
        At += 2;

        if (!SepSddlReadNumber(&At, &Revision) || (Revision != SID_REVISION))
            return FALSE;

        if (*At != L'-')
            return FALSE;
        At++;

        if (!SepSddlReadNumber(&At, &Value))
            return FALSE;

        /* The authority is six bytes, written as one number, most significant first */
        Authority.Value[5] = (UCHAR)(Value & 0xFF);
        Authority.Value[4] = (UCHAR)((Value >> 8) & 0xFF);
        Authority.Value[3] = (UCHAR)((Value >> 16) & 0xFF);
        Authority.Value[2] = (UCHAR)((Value >> 24) & 0xFF);

        while ((*At == L'-') && (Count < SID_MAX_SUB_AUTHORITIES))
        {
            At++;
            if (!SepSddlReadNumber(&At, &SubAuthority[Count]))
                return FALSE;
            Count++;
        }
    }
    else
    {
        const SEP_SDDL_SID *Known = NULL;

        for (Index = 0; Index < RTL_NUMBER_OF(SepSddlSids); Index++)
        {
            if (SepSddlMatch(SepSddlSids[Index].Name, At))
            {
                Known = &SepSddlSids[Index];
                break;
            }
        }

        if (Known == NULL)
            return FALSE;

        Authority.Value[5] = Known->Authority;
        for (Count = 0; Count < Known->Count; Count++)
            SubAuthority[Count] = Known->SubAuthority[Count];

        At += 2;
    }

    if (Count == 0)
        return FALSE;

    if (Sid != NULL)
    {
        if (*Length < RtlLengthRequiredSid(Count))
            return FALSE;

        RtlInitializeSid(Sid, &Authority, (UCHAR)Count);
        for (Index = 0; Index < Count; Index++)
            *RtlSubAuthoritySid(Sid, Index) = SubAuthority[Index];
    }

    *Length = RtlLengthRequiredSid(Count);
    *Text = At;
    return TRUE;
}

/* Reads the rights, either as a number or as a run of two letter names */
static
BOOLEAN
NTAPI
SepSddlReadRights(
    _Inout_ PCWSTR *Text,
    _Out_ PACCESS_MASK Mask)
{
    PCWSTR At = *Text;
    ACCESS_MASK Result = 0;
    ULONG Value;

    if ((*At >= L'0') && (*At <= L'9'))
    {
        if (!SepSddlReadNumber(&At, &Value))
            return FALSE;

        *Mask = Value;
        *Text = At;
        return TRUE;
    }

    while (*At != L';')
    {
        ULONG Index;
        BOOLEAN Found = FALSE;

        if (*At == UNICODE_NULL)
            return FALSE;

        for (Index = 0; Index < RTL_NUMBER_OF(SepSddlRights); Index++)
        {
            if (SepSddlMatch(SepSddlRights[Index].Name, At))
            {
                Result |= SepSddlRights[Index].Value;
                At += 2;
                Found = TRUE;
                break;
            }
        }

        if (!Found)
            return FALSE;
    }

    *Mask = Result;
    *Text = At;
    return TRUE;
}

/*
 * Reads one bracketed entry, writing it where told and measuring it either way.
 * The two object fields carry nothing that a list without object entries uses,
 * so they are stepped over rather than read.
 */
static
BOOLEAN
NTAPI
SepSddlReadAce(
    _Inout_ PCWSTR *Text,
    _Out_writes_bytes_opt_(*Length) PVOID Ace,
    _Inout_ PULONG Length)
{
    PCWSTR At = *Text;
    ACCESS_MASK Mask = 0;
    UCHAR Type = ACCESS_ALLOWED_ACE_TYPE;
    UCHAR Flags = 0;
    PCWSTR SidText;
    ULONG SidLength;
    ULONG Index;
    ULONG Needed;

    if (*At != L'(')
        return FALSE;
    At++;

    /* The kind of entry */
    for (Index = 0; Index < RTL_NUMBER_OF(SepSddlAceTypes); Index++)
    {
        if (SepSddlMatch(SepSddlAceTypes[Index].Name, At))
        {
            Type = (UCHAR)SepSddlAceTypes[Index].Value;
            break;
        }
    }
    while ((*At != L';') && (*At != UNICODE_NULL))
        At++;
    if (*At != L';')
        return FALSE;
    At++;

    /* Its flags, of which there may be none */
    while (*At != L';')
    {
        BOOLEAN Found = FALSE;

        if (*At == UNICODE_NULL)
            return FALSE;

        for (Index = 0; Index < RTL_NUMBER_OF(SepSddlAceFlags); Index++)
        {
            if (SepSddlMatch(SepSddlAceFlags[Index].Name, At))
            {
                Flags |= (UCHAR)SepSddlAceFlags[Index].Value;
                At += 2;
                Found = TRUE;
                break;
            }
        }

        if (!Found)
            return FALSE;
    }
    At++;

    if (!SepSddlReadRights(&At, &Mask))
        return FALSE;
    if (*At != L';')
        return FALSE;
    At++;

    /* The two object fields, which a list of this kind leaves empty */
    for (Index = 0; Index < 2; Index++)
    {
        while ((*At != L';') && (*At != UNICODE_NULL))
            At++;
        if (*At != L';')
            return FALSE;
        At++;
    }

    /* Where the account starts, so the filling pass can read it again */
    SidText = At;

    SidLength = 0;
    if (!SepSddlReadSid(&At, NULL, &SidLength))
        return FALSE;

    if (*At != L')')
        return FALSE;
    At++;

    /* An entry is its header, its mask and the account, and is aligned */
    Needed = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + SidLength;
    Needed = ROUND_UP(Needed, sizeof(ULONG));

    if (Ace != NULL)
    {
        PACCESS_ALLOWED_ACE Target = Ace;
        PCWSTR Read = SidText;
        ULONG Room = SidLength;

        if (*Length < Needed)
            return FALSE;

        Target->Header.AceType = Type;
        Target->Header.AceFlags = Flags;
        Target->Header.AceSize = (USHORT)Needed;
        Target->Mask = Mask;

        if (!SepSddlReadSid(&Read, (PSID)&Target->SidStart, &Room))
            return FALSE;
    }

    *Length = Needed;
    *Text = At;
    return TRUE;
}

/*
 * Reads a list: the letters that describe the list itself, then every entry in
 * it. Called once to measure and once to fill.
 */
static
BOOLEAN
NTAPI
SepSddlReadAcl(
    _In_z_ PCWSTR Text,
    _Out_writes_bytes_opt_(*Length) PACL Acl,
    _Inout_ PULONG Length,
    _Out_ PUSHORT Control)
{
    PCWSTR At = Text;
    ULONG Used = sizeof(ACL);
    ULONG Count = 0;

    *Control = 0;

    /* What is said about the list before its entries begin */
    while ((*At != L'(') && (*At != UNICODE_NULL))
    {
        if ((At[0] == L'P') && (At[1] != L'A'))
        {
            *Control |= SE_DACL_PROTECTED;
            At++;
        }
        else if ((At[0] == L'A') && (At[1] == L'R'))
        {
            *Control |= SE_DACL_AUTO_INHERIT_REQ;
            At += 2;
        }
        else if ((At[0] == L'A') && (At[1] == L'I'))
        {
            *Control |= SE_DACL_AUTO_INHERITED;
            At += 2;
        }
        else
        {
            return FALSE;
        }
    }

    while (*At == L'(')
    {
        ULONG Needed = (Acl != NULL) ? (*Length - Used) : 0;
        PVOID Where = NULL;

        if (Acl != NULL)
            Where = (PUCHAR)Acl + Used;

        if (!SepSddlReadAce(&At, Where, &Needed))
            return FALSE;

        Used += Needed;
        Count++;
    }

    if (Acl != NULL)
    {
        if (*Length < Used)
            return FALSE;

        Acl->AclRevision = ACL_REVISION;
        Acl->Sbz1 = 0;
        Acl->AclSize = (USHORT)Used;
        Acl->AceCount = (USHORT)Count;
        Acl->Sbz2 = 0;
    }

    *Length = Used;
    return TRUE;
}

/* Finds where a section's body ends, which is where the next one begins */
static
PCWSTR
NTAPI
SepSddlEndOfSection(
    _In_z_ PCWSTR Text)
{
    PCWSTR At = Text;
    ULONG Depth = 0;

    while (*At != UNICODE_NULL)
    {
        if (*At == L'(')
            Depth++;
        else if (*At == L')')
            Depth--;
        else if ((Depth == 0) && (At[1] == L':') &&
                 ((At[0] == L'O') || (At[0] == L'G') ||
                  (At[0] == L'D') || (At[0] == L'S')))
        {
            break;
        }

        At++;
    }

    return At;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
SeConvertStringSecurityDescriptorToSecurityDescriptor(
    _In_ PCWSTR StringSecurityDescriptor,
    _In_ ULONG StringSDRevision,
    _Outptr_ PSECURITY_DESCRIPTOR *SecurityDescriptor,
    _Out_opt_ PULONG SecurityDescriptorSize)
{
    PCWSTR Section[4] = { NULL, NULL, NULL, NULL };
    ULONG Size[4] = { 0, 0, 0, 0 };
    PISECURITY_DESCRIPTOR_RELATIVE Relative;
    PCWSTR At;
    ULONG Total, Used;
    USHORT Control = SE_SELF_RELATIVE;
    USHORT AclControl;
    ULONG Index;

    PAGED_CODE();

    if ((StringSecurityDescriptor == NULL) || (SecurityDescriptor == NULL))
        return STATUS_INVALID_PARAMETER;

    if (StringSDRevision != SDDL_REVISION_1)
        return STATUS_UNKNOWN_REVISION;

    /* Where each section starts, in the order they are written down here */
    At = StringSecurityDescriptor;
    while (*At != UNICODE_NULL)
    {
        ULONG Which;

        if (At[1] != L':')
            return STATUS_INVALID_PARAMETER;

        switch (At[0])
        {
            case L'O': Which = 0; break;
            case L'G': Which = 1; break;
            case L'D': Which = 2; break;
            case L'S': Which = 3; break;
            default: return STATUS_INVALID_PARAMETER;
        }

        if (Section[Which] != NULL)
            return STATUS_INVALID_PARAMETER;

        Section[Which] = At + 2;
        At = SepSddlEndOfSection(At + 2);
    }

    /* What each of them needs */
    for (Index = 0; Index < 2; Index++)
    {
        if (Section[Index] != NULL)
        {
            PCWSTR Read = Section[Index];

            Size[Index] = 0;
            if (!SepSddlReadSid(&Read, NULL, &Size[Index]))
                return STATUS_INVALID_PARAMETER;
        }
    }

    for (Index = 2; Index < 4; Index++)
    {
        if (Section[Index] != NULL)
        {
            Size[Index] = 0;
            if (!SepSddlReadAcl(Section[Index], NULL, &Size[Index], &AclControl))
                return STATUS_INVALID_PARAMETER;

            /* The same two bits describe either list, so they are moved across */
            if (Index == 3)
                AclControl = (USHORT)(AclControl << 1);

            Control |= AclControl;
        }
    }

    Total = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    for (Index = 0; Index < 4; Index++)
        Total += ROUND_UP(Size[Index], sizeof(ULONG));

    Relative = ExAllocatePoolZero(PagedPool, Total, TAG_SDDL);
    if (Relative == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Relative->Revision = SECURITY_DESCRIPTOR_REVISION;
    Relative->Sbz1 = 0;

    Used = sizeof(SECURITY_DESCRIPTOR_RELATIVE);

    for (Index = 0; Index < 4; Index++)
    {
        ULONG Room = Size[Index];
        PVOID Where;

        if (Section[Index] == NULL)
            continue;

        Where = (PUCHAR)Relative + Used;

        if (Index < 2)
        {
            PCWSTR Read = Section[Index];

            if (!SepSddlReadSid(&Read, (PSID)Where, &Room))
            {
                ExFreePoolWithTag(Relative, TAG_SDDL);
                return STATUS_INVALID_PARAMETER;
            }

            if (Index == 0)
                Relative->Owner = Used;
            else
                Relative->Group = Used;
        }
        else
        {
            if (!SepSddlReadAcl(Section[Index], (PACL)Where, &Room, &AclControl))
            {
                ExFreePoolWithTag(Relative, TAG_SDDL);
                return STATUS_INVALID_PARAMETER;
            }

            if (Index == 2)
            {
                Relative->Dacl = Used;
                Control |= SE_DACL_PRESENT;
            }
            else
            {
                Relative->Sacl = Used;
                Control |= SE_SACL_PRESENT;
            }
        }

        Used += ROUND_UP(Room, sizeof(ULONG));
    }

    Relative->Control = Control;

    *SecurityDescriptor = Relative;
    if (SecurityDescriptorSize != NULL)
        *SecurityDescriptorSize = Total;

    return STATUS_SUCCESS;
}

/* EOF */
