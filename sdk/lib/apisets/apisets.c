/*
 * PROJECT:     ReactOS apisets
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Resolving the apiset to a ReactOS system dll
 * COPYRIGHT:   Copyright 2024 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include "apisetsp.h"


const ULONGLONG API_ = (ULONGLONG)0x2D004900500041; /// L"API-"
const ULONGLONG EXT_ = (ULONGLONG)0x2D005400580045; /// L"EXT-";

WORD PrefixSize = sizeof(L"api-") - sizeof(WCHAR);
WORD ExtensionSize = sizeof(L".dll") - sizeof(WCHAR);


// The official prototype according to the Windows kit 8.1 is:
//NTSTATUS
//ApiSetResolveToHost (
//    _In_ PCAPI_SET_NAMESPACE_ARRAY Schema,
//    _In_ PCUNICODE_STRING FileNameIn,
//    _In_opt_ PCUNICODE_STRING ParentName,
//    _Out_ PBOOLEAN Resolved,
//    _Out_ PUNICODE_STRING HostBinary
//    );


/**
 * @brief Finds an apiset that differs from the wanted one only in its last version component.
 *
 * A module is built against one revision of a contract and the schema on the machine carries
 * another, so the name is keyed on everything ahead of the final version component. That is
 * what the kernel does for its own api sets, and it is why a binary built against an older
 * Windows still resolves here.
 *
 * @param Name The apiset name, without its extension.
 * @param ApisetVersion The apiset generation that is active for this process.
 * @param Output Receives the hosting module.
 *
 * @return TRUE when a sibling revision serves this contract.
 */
static
BOOLEAN
ApiSetpResolveBySibling(
    _In_ PCUNICODE_STRING Name,
    _In_ DWORD ApisetVersion,
    _Out_ PUNICODE_STRING Output)
{
    UNICODE_STRING Stem = *Name;
    LONG LBnd, UBnd, Index, First;

    /* Drop the last version component, and the hyphen ahead of it */
    while ((Stem.Length >= sizeof(WCHAR)) &&
           (Stem.Buffer[Stem.Length / sizeof(WCHAR) - 1] != L'-'))
    {
        Stem.Length -= sizeof(WCHAR);
    }

    if (Stem.Length < 2 * sizeof(WCHAR))
        return FALSE;
    Stem.Length -= sizeof(WCHAR);

    /* Find any entry carrying this stem; the table is sorted, so they sit together */
    LBnd = 0;
    UBnd = g_ApisetsCount - 1;
    First = -1;
    while (LBnd <= UBnd)
    {
        UNICODE_STRING Candidate;

        Index = (UBnd - LBnd) / 2 + LBnd;
        Candidate = g_Apisets[Index].Name;
        if (Candidate.Length > Stem.Length)
            Candidate.Length = Stem.Length;

        LONG Result = RtlCompareUnicodeString(&Stem, &Candidate, TRUE);
        if (Result == 0)
        {
            First = Index;
            break;
        }
        else if (Result < 0)
        {
            UBnd = Index - 1;
        }
        else
        {
            LBnd = Index + 1;
        }
    }

    if (First < 0)
        return FALSE;

    /* Walk back to the first of them */
    while (First > 0)
    {
        UNICODE_STRING Candidate = g_Apisets[First - 1].Name;
        if (Candidate.Length > Stem.Length)
            Candidate.Length = Stem.Length;
        if (RtlCompareUnicodeString(&Stem, &Candidate, TRUE) != 0)
            break;
        First--;
    }

    for (Index = First; Index < g_ApisetsCount; Index++)
    {
        UNICODE_STRING Full = g_Apisets[Index].Name;
        UNICODE_STRING Candidate = Full;

        if (Candidate.Length > Stem.Length)
            Candidate.Length = Stem.Length;
        if (RtlCompareUnicodeString(&Stem, &Candidate, TRUE) != 0)
            break;

        /* Only a version component may follow the stem, never a longer name */
        if (Full.Length <= Stem.Length ||
            Full.Buffer[Stem.Length / sizeof(WCHAR)] != L'-')
        {
            continue;
        }

        if ((g_Apisets[Index].dwOsVersions & ApisetVersion) &&
            (g_Apisets[Index].Target.Length != 0))
        {
            *Output = g_Apisets[Index].Target;
            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS
ApiSetResolveToHost(
    _In_ DWORD ApisetVersion,
    _In_ PCUNICODE_STRING ApiToResolve,
    _Out_ PBOOLEAN Resolved,
    _Out_ PUNICODE_STRING Output
)
{
    if (ApiToResolve->Length < PrefixSize)
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // Grab the first four chars from the string, converting the first 3 to uppercase
    PWSTR ApiSetNameBuffer = ApiToResolve->Buffer;
    ULONGLONG ApiSetNameBufferPrefix = ((ULONGLONG *)ApiSetNameBuffer)[0] & 0xFFFFFFDFFFDFFFDF;
    // Check if it matches either 'api-' or 'ext-'
    if (!(ApiSetNameBufferPrefix == API_ || ApiSetNameBufferPrefix == EXT_))
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // If there is an extension, cut it off (we store apisets without extension)
    UNICODE_STRING Tmp = *ApiToResolve;
    const WCHAR *Extension = Tmp.Buffer + (Tmp.Length - ExtensionSize) / sizeof(WCHAR);
    if (!_wcsnicmp(Extension, L".dll", ExtensionSize / sizeof(WCHAR)))
        Tmp.Length -= ExtensionSize;

    // Binary search the apisets
    // Ideally we should use bsearch here, but that drags in another dependency and we do not want that here.
    LONG UBnd = g_ApisetsCount - 1;
    LONG LBnd = 0;
    while (LBnd <= UBnd)
    {
        LONG Index = (UBnd - LBnd) / 2 + LBnd;

        LONG result = RtlCompareUnicodeString(&Tmp, &g_Apisets[Index].Name, TRUE);
        if (result == 0)
        {
            // Check if this version is included
            if (g_Apisets[Index].dwOsVersions & ApisetVersion)
            {
                // Return a static string (does not have to be freed)
                *Resolved = TRUE;
                *Output = g_Apisets[Index].Target;
                return STATUS_SUCCESS;
            }

            /* The contract is known but not for this generation, so try its other revisions */
            *Resolved = ApiSetpResolveBySibling(&Tmp, ApisetVersion, Output);
            return STATUS_SUCCESS;
        }
        else if (result < 0)
        {
            UBnd = Index - 1;
        }
        else
        {
            LBnd = Index + 1;
        }
    }
    /* No entry names this exact revision, so fall back on the ones that share its stem */
    *Resolved = ApiSetpResolveBySibling(&Tmp, ApisetVersion, Output);
    return STATUS_SUCCESS;
}
