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


/* Drops the trailing "-<revision>" so all revisions of a contract compare equal */
static
VOID
ApisetStripRevision(
    _Inout_ PUNICODE_STRING Name)
{
    USHORT Chars = Name->Length / sizeof(WCHAR);

    while (Chars > 0 && Name->Buffer[Chars - 1] != L'-')
        Chars--;

    if (Chars > 1)
        Name->Length = (Chars - 1) * sizeof(WCHAR);
}

static
BOOLEAN
ApisetSameContract(
    _In_ PCUNICODE_STRING Contract,
    _In_ LONG Index)
{
    UNICODE_STRING Entry = g_Apisets[Index].Name;

    ApisetStripRevision(&Entry);
    return RtlEqualUnicodeString(Contract, &Entry, TRUE);
}


// The official prototype according to the Windows kit 8.1 is:
//NTSTATUS
//ApiSetResolveToHost (
//    _In_ PCAPI_SET_NAMESPACE_ARRAY Schema,
//    _In_ PCUNICODE_STRING FileNameIn,
//    _In_opt_ PCUNICODE_STRING ParentName,
//    _Out_ PBOOLEAN Resolved,
//    _Out_ PUNICODE_STRING HostBinary
//    );


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

    /* Like Windows, every revision of a contract resolves to the same host */
    ApisetStripRevision(&Tmp);

    LONG UBnd = g_ApisetsCount - 1;
    LONG LBnd = 0;
    while (LBnd <= UBnd)
    {
        LONG Index = (UBnd - LBnd) / 2 + LBnd;
        UNICODE_STRING Entry = g_Apisets[Index].Name;
        LONG First, Last, Probe;

        ApisetStripRevision(&Entry);
        LONG result = RtlCompareUnicodeString(&Tmp, &Entry, TRUE);
        if (result == 0)
        {
            /* Revisions of one contract are adjacent, find the span */
            First = Index;
            while (First > 0 && ApisetSameContract(&Tmp, First - 1))
                First--;

            Last = Index;
            while (Last + 1 < g_ApisetsCount && ApisetSameContract(&Tmp, Last + 1))
                Last++;

            /* The newest revision present in the requested OS version wins */
            for (Probe = Last; Probe >= First; Probe--)
            {
                /* A set stays present in every version after the one it appeared in */
                if (g_Apisets[Probe].dwOsVersions & (ApisetVersion | (ApisetVersion - 1)))
                {
                    *Resolved = TRUE;
                    *Output = g_Apisets[Probe].Target;
                    return STATUS_SUCCESS;
                }
            }

            *Resolved = FALSE;
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
    *Resolved = FALSE;
    return STATUS_SUCCESS;
}
