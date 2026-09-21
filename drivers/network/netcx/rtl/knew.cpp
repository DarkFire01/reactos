/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Global allocation operators for kernel mode C++
 *
 * There is no C++ runtime in kernel mode, so the nothrow tag and every global
 * operator the compiler or knew.h reaches for is supplied here. Tagged
 * allocations come back zeroed, the same as the pool calls around them.
 */

#include <ntddk.h>
#include <knew.h>

const std::nothrow_t std::nothrow{};

PAGED
void *
operator new(
    _In_ size_t s,
    _In_ std::nothrow_t const &,
    _In_ ULONG tag)
{
    PAGED_CODE();
    return ExAllocatePool2(POOL_FLAG_PAGED, s, tag);
}

PAGED
void *
operator new[](
    _In_ size_t s,
    _In_ std::nothrow_t const &,
    _In_ ULONG tag)
{
    PAGED_CODE();
    return ExAllocatePool2(POOL_FLAG_PAGED, s, tag);
}

PAGED
void
operator delete(
    _In_opt_ void *p,
    _In_ ULONG tag)
{
    PAGED_CODE();

    if (p != nullptr)
        ExFreePoolWithTag(p, tag);
}

PAGED
void
operator delete[](
    _In_opt_ void *p,
    _In_ ULONG tag)
{
    PAGED_CODE();

    if (p != nullptr)
        ExFreePoolWithTag(p, tag);
}

/* The untagged forms cannot know the tag, so they skip the check. */
PAGEDX
void
__cdecl
operator delete[](
    _In_opt_ void *p)
{
    if (p != nullptr)
        ExFreePoolWithTag(p, 0);
}

void
__cdecl
operator delete(
    _In_opt_ void *p)
{
    if (p != nullptr)
        ExFreePoolWithTag(p, 0);
}

void
__cdecl
operator delete(
    _In_opt_ void *p,
    _In_ size_t)
{
    if (p != nullptr)
        ExFreePoolWithTag(p, 0);
}

/* Pool blocks are never over aligned, so there is nothing extra to undo. */
void
__cdecl
operator delete(
    _In_opt_ void *p,
    _In_ size_t,
    _In_ std::align_val_t)
{
    if (p != nullptr)
        ExFreePoolWithTag(p, 0);
}
