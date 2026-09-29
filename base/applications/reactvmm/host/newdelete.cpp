/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Where new and delete come from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The runtime this links against has no C++ allocation in it, so a module that
 * uses the language has to say where its own comes from. Every one in the tree
 * does this for itself; this is that, and nothing more.
 *
 * The sized form matters: from C++14 the compiler emits a call to it rather
 * than the plain one wherever it knows the size, so a module that defines only
 * the plain one still fails to link.
 */

#include <windows.h>

#include <stdlib.h>

void *operator new(size_t Size)
{
    /* Never zero, so that two allocations cannot come back the same pointer */
    return malloc((Size != 0) ? Size : 1);
}

void *operator new[](size_t Size)
{
    return operator new(Size);
}

void operator delete(void *Block) noexcept
{
    free(Block);
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
