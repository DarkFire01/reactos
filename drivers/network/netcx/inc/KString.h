/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Pool allocated counted string
 *
 * Layout recovered from NetAdapterCx.pdb: a UNICODE_STRING that carries its
 * own nonpaged allocation, so it can be handed around in a KPtr.
 */

#pragma once

#include <KMacros.h>
#include <knew.h>
#include <KPtr.h>

namespace Rtl
{

struct KRTL_CLASS_DPC_ALLOC KString :
    public NONPAGED_OBJECT<'KStr'>,
    public UNICODE_STRING
{
};

/* The copy and its header share one allocation, so freeing the header frees both. */
inline
KPoolPtr<UNICODE_STRING>
DuplicateUnicodeString(
    _In_ UNICODE_STRING const &Source,
    _In_ ULONG PoolTag)
{
    auto const copy = static_cast<UNICODE_STRING *>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(UNICODE_STRING) + Source.MaximumLength, PoolTag));

    if (copy == nullptr)
        return {};

    copy->Buffer = reinterpret_cast<PWCH>(copy + 1);
    copy->Length = Source.Length;
    copy->MaximumLength = Source.MaximumLength;
    RtlCopyMemory(copy->Buffer, Source.Buffer, Source.Length);

    return KPoolPtr<UNICODE_STRING>(copy);
}

}
