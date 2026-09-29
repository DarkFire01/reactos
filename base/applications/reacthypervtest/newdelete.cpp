/*
 * PROJECT:     ReactHypervTest
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Where the few things here that are made one at a time come from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The runtime this is built against does not bring these with it, and a class
 * that is made rather than declared needs them. There is nothing clever here on
 * purpose: everything that matters for how long it lives is held by something
 * with a name, and these are only what stands behind that.
 */

#include <windows.h>
#include <stddef.h>

void *operator new(size_t Size)
{
    /* Never nothing, because a run of zero bytes still has to have an address */
    return HeapAlloc(GetProcessHeap(), 0, (Size != 0) ? Size : 1);
}

void *operator new[](size_t Size)
{
    return operator new(Size);
}

void operator delete(void *Block) noexcept
{
    if (Block != nullptr)
        HeapFree(GetProcessHeap(), 0, Block);
}

void operator delete[](void *Block) noexcept
{
    operator delete(Block);
}

void operator delete(void *Block, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);

    operator delete(Block);
}

void operator delete[](void *Block, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);

    operator delete(Block);
}
