/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security descriptors from SDDL strings
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Drivers describe the security of their devices with SDDL. The owner, the
 * group and the DACL are understood, with allowing and denying ACEs. A SACL,
 * conditional ACEs and object ACEs are not.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define TAG_SE_SDDL         'dSeS'
#define SEP_SDDL_MAX_ACES   32
#define SEP_SDDL_SID_SIZE   (FIELD_OFFSET(SID, SubAuthority) + SID_MAX_SUB_AUTHORITIES * sizeof(ULONG))

/* Two letter names SDDL gives well known accounts */
typedef struct _SEP_SDDL_SID_ALIAS
{
    WCHAR Alias[2];
    UCHAR Authority;
    UCHAR SubAuthorityCount;
    ULONG SubAuthority[2];
} SEP_SDDL_SID_ALIAS;

static const SEP_SDDL_SID_ALIAS SepSddlSidAliases[] =
{
    { { L'W', L'D' }, 1,  1, { 0, 0 } },      /* Everyone */
    { { L'C', L'O' }, 3,  1, { 0, 0 } },      /* Creator owner */
    { { L'C', L'G' }, 3,  1, { 1, 0 } },      /* Creator group */
    { { L'N', L'U' }, 5,  1, { 2, 0 } },      /* Network logon */
    { { L'I', L'U' }, 5,  1, { 4, 0 } },      /* Interactive logon */
    { { L'S', L'U' }, 5,  1, { 6, 0 } },      /* Service logon */
    { { L'A', L'N' }, 5,  1, { 7, 0 } },      /* Anonymous */
    { { L'A', L'U' }, 5,  1, { 11, 0 } },     /* Authenticated users */
    { { L'R', L'C' }, 5,  1, { 12, 0 } },     /* Restricted code */
    { { L'S', L'Y' }, 5,  1, { 18, 0 } },     /* Local system */
    { { L'L', L'S' }, 5,  1, { 19, 0 } },     /* Local service */
    { { L'N', L'S' }, 5,  1, { 20, 0 } },     /* Network service */
    { { L'B', L'A' }, 5,  2, { 32, 544 } },   /* Administrators */
    { { L'B', L'U' }, 5,  2, { 32, 545 } },   /* Users */
    { { L'B', L'G' }, 5,  2, { 32, 546 } },   /* Guests */
    { { L'P', L'U' }, 5,  2, { 32, 547 } },   /* Power users */
    { { L'A', L'O' }, 5,  2, { 32, 548 } },   /* Account operators */
    { { L'S', L'O' }, 5,  2, { 32, 549 } },   /* Server operators */
    { { L'P', L'O' }, 5,  2, { 32, 550 } },   /* Print operators */
    { { L'B', L'O' }, 5,  2, { 32, 551 } },   /* Backup operators */
    { { L'R', L'D' }, 5,  2, { 32, 555 } },   /* Remote desktop users */
    { { L'N', L'O' }, 5,  2, { 32, 556 } },   /* Network configuration operators */
    { { L'A', L'C' }, 15, 2, { 2, 1 } },      /* All application packages */
};

/* Two letter names SDDL gives access rights */
typedef struct _SEP_SDDL_RIGHT_ALIAS
{
    WCHAR Alias[2];
    ACCESS_MASK Mask;
} SEP_SDDL_RIGHT_ALIAS;

static const SEP_SDDL_RIGHT_ALIAS SepSddlRightAliases[] =
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
};

/* Two letter names SDDL gives ACE flags */
typedef struct _SEP_SDDL_FLAG_ALIAS
{
    WCHAR Alias[2];
    UCHAR Flag;
} SEP_SDDL_FLAG_ALIAS;

static const SEP_SDDL_FLAG_ALIAS SepSddlAceFlagAliases[] =
{
    { { L'O', L'I' }, OBJECT_INHERIT_ACE },
    { { L'C', L'I' }, CONTAINER_INHERIT_ACE },
    { { L'N', L'P' }, NO_PROPAGATE_INHERIT_ACE },
    { { L'I', L'O' }, INHERIT_ONLY_ACE },
    { { L'I', L'D' }, INHERITED_ACE },
};

/* One ACE as it was read, before the DACL is built */
typedef struct _SEP_SDDL_ACE
{
    UCHAR Type;
    UCHAR Flags;
    ACCESS_MASK Mask;
    ULONG_PTR Sid[SEP_SDDL_SID_SIZE / sizeof(ULONG_PTR) + 1];
} SEP_SDDL_ACE;

/* What the string describes */
typedef struct _SEP_SDDL_PARSE
{
    PCWCH Next;
    PCWCH End;
    BOOLEAN HasOwner;
    BOOLEAN HasGroup;
    BOOLEAN HasDacl;
    BOOLEAN DaclProtected;
    ULONG_PTR Owner[SEP_SDDL_SID_SIZE / sizeof(ULONG_PTR) + 1];
    ULONG_PTR Group[SEP_SDDL_SID_SIZE / sizeof(ULONG_PTR) + 1];
    ULONG AceCount;
    SEP_SDDL_ACE Aces[SEP_SDDL_MAX_ACES];
} SEP_SDDL_PARSE, *PSEP_SDDL_PARSE;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Tells whether the two characters at the cursor are a given alias.
 */
static
BOOLEAN
SepSddlIsAlias(
    _In_ PSEP_SDDL_PARSE Parse,
    _In_reads_(2) PCWCH Alias)
{
    return (BOOLEAN)(((Parse->End - Parse->Next) >= 2) &&
                     (RtlUpcaseUnicodeChar(Parse->Next[0]) == Alias[0]) &&
                     (RtlUpcaseUnicodeChar(Parse->Next[1]) == Alias[1]));
}

/**
 * @brief Reads an unsigned number, decimal or with a 0x prefix in hex.
 */
static
BOOLEAN
SepSddlReadNumber(
    _Inout_ PSEP_SDDL_PARSE Parse,
    _Out_ PULONG64 Value)
{
    ULONG Base = 10, Digit;
    BOOLEAN Any = FALSE;
    WCHAR Char;

    *Value = 0;

    if (((Parse->End - Parse->Next) >= 2) && (Parse->Next[0] == L'0') &&
        ((Parse->Next[1] == L'x') || (Parse->Next[1] == L'X')))
    {
        Base = 16;
        Parse->Next += 2;
    }

    while (Parse->Next < Parse->End)
    {
        Char = Parse->Next[0];
        if ((Char >= L'0') && (Char <= L'9'))
            Digit = Char - L'0';
        else if ((Base == 16) && (Char >= L'a') && (Char <= L'f'))
            Digit = Char - L'a' + 10;
        else if ((Base == 16) && (Char >= L'A') && (Char <= L'F'))
            Digit = Char - L'A' + 10;
        else
            break;

        *Value = *Value * Base + Digit;
        Any = TRUE;
        Parse->Next++;
    }

    return Any;
}

/**
 * @brief Reads a SID, either an alias or written out as S-1-...
 *
 * @param[in,out] Parse
 * Parse state, the cursor moves past the SID.
 *
 * @param[out] Sid
 * Receives the SID, room for SEP_SDDL_SID_SIZE bytes.
 */
static
BOOLEAN
SepSddlReadSid(
    _Inout_ PSEP_SDDL_PARSE Parse,
    _Out_writes_bytes_(SEP_SDDL_SID_SIZE) PSID Sid)
{
    SID_IDENTIFIER_AUTHORITY Authority = { { 0 } };
    ULONG SubAuthorities[SID_MAX_SUB_AUTHORITIES];
    UCHAR Count = 0, i;
    ULONG64 Value;

    for (i = 0; i < RTL_NUMBER_OF(SepSddlSidAliases); i++)
    {
        if (!SepSddlIsAlias(Parse, SepSddlSidAliases[i].Alias))
            continue;

        Authority.Value[5] = SepSddlSidAliases[i].Authority;
        RtlInitializeSid(Sid, &Authority, SepSddlSidAliases[i].SubAuthorityCount);
        RtlCopyMemory(RtlSubAuthoritySid(Sid, 0),
                      SepSddlSidAliases[i].SubAuthority,
                      SepSddlSidAliases[i].SubAuthorityCount * sizeof(ULONG));
        Parse->Next += 2;
        return TRUE;
    }

    /* S-1-<authority>-<sub authority>... */
    if (((Parse->End - Parse->Next) < 4) ||
        (RtlUpcaseUnicodeChar(Parse->Next[0]) != L'S') ||
        (Parse->Next[1] != L'-') || (Parse->Next[2] != L'1') || (Parse->Next[3] != L'-'))
    {
        return FALSE;
    }

    Parse->Next += 4;
    if (!SepSddlReadNumber(Parse, &Value) || (Value >> 48))
        return FALSE;

    for (i = 0; i < 6; i++)
        Authority.Value[5 - i] = (UCHAR)(Value >> (8 * i));

    while ((Parse->Next < Parse->End) && (Parse->Next[0] == L'-'))
    {
        Parse->Next++;
        if ((Count == SID_MAX_SUB_AUTHORITIES) || !SepSddlReadNumber(Parse, &Value) || (Value > MAXULONG))
            return FALSE;

        SubAuthorities[Count++] = (ULONG)Value;
    }

    if (Count == 0)
        return FALSE;

    RtlInitializeSid(Sid, &Authority, Count);
    RtlCopyMemory(RtlSubAuthoritySid(Sid, 0), SubAuthorities, Count * sizeof(ULONG));
    return TRUE;
}

/**
 * @brief Reads the access mask of an ACE, aliases or one number.
 */
static
BOOLEAN
SepSddlReadRights(
    _Inout_ PSEP_SDDL_PARSE Parse,
    _Out_ PACCESS_MASK Mask)
{
    ULONG64 Value;
    ULONG i;

    *Mask = 0;

    if ((Parse->Next < Parse->End) && (Parse->Next[0] >= L'0') && (Parse->Next[0] <= L'9'))
    {
        if (!SepSddlReadNumber(Parse, &Value) || (Value > MAXULONG))
            return FALSE;

        *Mask = (ACCESS_MASK)Value;
        return TRUE;
    }

    while ((Parse->Next < Parse->End) && (Parse->Next[0] != L';'))
    {
        for (i = 0; i < RTL_NUMBER_OF(SepSddlRightAliases); i++)
        {
            if (SepSddlIsAlias(Parse, SepSddlRightAliases[i].Alias))
                break;
        }

        if (i == RTL_NUMBER_OF(SepSddlRightAliases))
            return FALSE;

        *Mask |= SepSddlRightAliases[i].Mask;
        Parse->Next += 2;
    }

    return TRUE;
}

/**
 * @brief Skips the character the grammar expects next.
 */
static
BOOLEAN
SepSddlExpect(
    _Inout_ PSEP_SDDL_PARSE Parse,
    _In_ WCHAR Char)
{
    if ((Parse->Next >= Parse->End) || (Parse->Next[0] != Char))
        return FALSE;

    Parse->Next++;
    return TRUE;
}

/**
 * @brief Reads one ACE in parentheses: type;flags;rights;object;inherited object;sid.
 */
static
BOOLEAN
SepSddlReadAce(
    _Inout_ PSEP_SDDL_PARSE Parse,
    _Out_ SEP_SDDL_ACE *Ace)
{
    ULONG i;

    if (!SepSddlExpect(Parse, L'('))
        return FALSE;

    /* Only access allowed and access denied ACEs */
    if ((Parse->Next < Parse->End) && (RtlUpcaseUnicodeChar(Parse->Next[0]) == L'A'))
        Ace->Type = ACCESS_ALLOWED_ACE_TYPE;
    else if ((Parse->Next < Parse->End) && (RtlUpcaseUnicodeChar(Parse->Next[0]) == L'D'))
        Ace->Type = ACCESS_DENIED_ACE_TYPE;
    else
        return FALSE;

    Parse->Next++;
    if (!SepSddlExpect(Parse, L';'))
        return FALSE;

    Ace->Flags = 0;
    while ((Parse->Next < Parse->End) && (Parse->Next[0] != L';'))
    {
        for (i = 0; i < RTL_NUMBER_OF(SepSddlAceFlagAliases); i++)
        {
            if (SepSddlIsAlias(Parse, SepSddlAceFlagAliases[i].Alias))
                break;
        }

        if (i == RTL_NUMBER_OF(SepSddlAceFlagAliases))
            return FALSE;

        Ace->Flags |= SepSddlAceFlagAliases[i].Flag;
        Parse->Next += 2;
    }

    if (!SepSddlExpect(Parse, L';') || !SepSddlReadRights(Parse, &Ace->Mask))
        return FALSE;

    /* Object ACEs are not supported, both GUIDs must be empty */
    if (!SepSddlExpect(Parse, L';') || !SepSddlExpect(Parse, L';') || !SepSddlExpect(Parse, L';'))
        return FALSE;

    if (!SepSddlReadSid(Parse, (PSID)Ace->Sid))
        return FALSE;

    return SepSddlExpect(Parse, L')');
}

/**
 * @brief Reads the whole string into the parse state.
 */
static
BOOLEAN
SepSddlReadString(
    _Inout_ PSEP_SDDL_PARSE Parse)
{
    WCHAR Component;

    while (Parse->Next < Parse->End)
    {
        if (((Parse->End - Parse->Next) < 2) || (Parse->Next[1] != L':'))
            return FALSE;

        Component = RtlUpcaseUnicodeChar(Parse->Next[0]);
        Parse->Next += 2;

        switch (Component)
        {
            case L'O':
                if (Parse->HasOwner || !SepSddlReadSid(Parse, (PSID)Parse->Owner))
                    return FALSE;
                Parse->HasOwner = TRUE;
                break;

            case L'G':
                if (Parse->HasGroup || !SepSddlReadSid(Parse, (PSID)Parse->Group))
                    return FALSE;
                Parse->HasGroup = TRUE;
                break;

            case L'D':
                if (Parse->HasDacl)
                    return FALSE;
                Parse->HasDacl = TRUE;

                /* Flags come before the ACEs */
                while ((Parse->Next < Parse->End) && (Parse->Next[0] != L'('))
                {
                    if (RtlUpcaseUnicodeChar(Parse->Next[0]) == L'P')
                    {
                        Parse->DaclProtected = TRUE;
                        Parse->Next++;
                    }
                    else if ((Parse->End - Parse->Next >= 2) &&
                             (RtlUpcaseUnicodeChar(Parse->Next[0]) == L'A') &&
                             ((RtlUpcaseUnicodeChar(Parse->Next[1]) == L'I') ||
                              (RtlUpcaseUnicodeChar(Parse->Next[1]) == L'R')))
                    {
                        /* Inheritance is computed by the object manager */
                        Parse->Next += 2;
                    }
                    else
                    {
                        return FALSE;
                    }
                }

                while ((Parse->Next < Parse->End) && (Parse->Next[0] == L'('))
                {
                    if ((Parse->AceCount == SEP_SDDL_MAX_ACES) ||
                        !SepSddlReadAce(Parse, &Parse->Aces[Parse->AceCount]))
                    {
                        return FALSE;
                    }

                    Parse->AceCount++;
                }
                break;

            default:
                return FALSE;
        }
    }

    return TRUE;
}

/* FUNCTIONS ******************************************************************/

/**
 * @brief Builds a self relative security descriptor from an SDDL string.
 *
 * @param[in] Sddl
 * The string.
 *
 * @param[out] SecurityDescriptor
 * Receives the descriptor, from paged pool, freed with SepFreeSddlSecurityDescriptor.
 *
 * @return STATUS_INVALID_PARAMETER if the string is not understood.
 */
NTSTATUS
NTAPI
SepConvertSddlToSecurityDescriptor(
    _In_ PCUNICODE_STRING Sddl,
    _Outptr_ PSECURITY_DESCRIPTOR *SecurityDescriptor)
{
    SECURITY_DESCRIPTOR Absolute;
    PSEP_SDDL_PARSE Parse;
    PSECURITY_DESCRIPTOR Relative = NULL;
    PACL Dacl = NULL;
    ULONG DaclSize, RelativeSize = 0, i;
    NTSTATUS Status;

    PAGED_CODE();

    *SecurityDescriptor = NULL;

    Parse = ExAllocatePoolZero(PagedPool, sizeof(*Parse), TAG_SE_SDDL);
    if (!Parse)
        return STATUS_INSUFFICIENT_RESOURCES;

    Parse->Next = Sddl->Buffer;
    Parse->End = Sddl->Buffer + Sddl->Length / sizeof(WCHAR);

    if (!SepSddlReadString(Parse))
    {
        DPRINT1("SDDL string not understood: %wZ\n", Sddl);
        Status = STATUS_INVALID_PARAMETER;
        goto Quit;
    }

    RtlCreateSecurityDescriptor(&Absolute, SECURITY_DESCRIPTOR_REVISION);

    if (Parse->HasOwner)
        RtlSetOwnerSecurityDescriptor(&Absolute, (PSID)Parse->Owner, FALSE);

    if (Parse->HasGroup)
        RtlSetGroupSecurityDescriptor(&Absolute, (PSID)Parse->Group, FALSE);

    if (Parse->HasDacl)
    {
        DaclSize = sizeof(ACL);
        for (i = 0; i < Parse->AceCount; i++)
            DaclSize += sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG) + RtlLengthSid((PSID)Parse->Aces[i].Sid);

        Dacl = ExAllocatePoolWithTag(PagedPool, DaclSize, TAG_SE_SDDL);
        if (!Dacl)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Quit;
        }

        Status = RtlCreateAcl(Dacl, DaclSize, ACL_REVISION);
        if (!NT_SUCCESS(Status))
            goto Quit;

        for (i = 0; i < Parse->AceCount; i++)
        {
            if (Parse->Aces[i].Type == ACCESS_ALLOWED_ACE_TYPE)
            {
                Status = RtlAddAccessAllowedAceEx(Dacl, ACL_REVISION, Parse->Aces[i].Flags,
                                                  Parse->Aces[i].Mask, (PSID)Parse->Aces[i].Sid);
            }
            else
            {
                Status = RtlAddAccessDeniedAceEx(Dacl, ACL_REVISION, Parse->Aces[i].Flags,
                                                 Parse->Aces[i].Mask, (PSID)Parse->Aces[i].Sid);
            }

            if (!NT_SUCCESS(Status))
                goto Quit;
        }

        RtlSetDaclSecurityDescriptor(&Absolute, TRUE, Dacl, FALSE);
        if (Parse->DaclProtected)
            Absolute.Control |= SE_DACL_PROTECTED;
    }

    /* Measure first, then copy into a block of the exact size */
    Status = RtlAbsoluteToSelfRelativeSD(&Absolute, NULL, &RelativeSize);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        goto Quit;

    Relative = ExAllocatePoolWithTag(PagedPool, RelativeSize, TAG_SE_SDDL);
    if (!Relative)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    Status = RtlAbsoluteToSelfRelativeSD(&Absolute, Relative, &RelativeSize);
    if (NT_SUCCESS(Status))
    {
        *SecurityDescriptor = Relative;
        Relative = NULL;
    }

Quit:
    if (Relative)
        ExFreePoolWithTag(Relative, TAG_SE_SDDL);
    if (Dacl)
        ExFreePoolWithTag(Dacl, TAG_SE_SDDL);
    ExFreePoolWithTag(Parse, TAG_SE_SDDL);
    return Status;
}

/**
 * @brief Frees a descriptor SepConvertSddlToSecurityDescriptor built.
 */
VOID
NTAPI
SepFreeSddlSecurityDescriptor(
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    ExFreePoolWithTag(SecurityDescriptor, TAG_SE_SDDL);
}
