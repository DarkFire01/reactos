/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guarded memory access helpers
 */

#pragma once

#include <ntddk.h>

namespace mem
{

/* An untorn read that orders nothing, of a value another thread may be changing. */
template <typename T>
inline
T
ReadNoFence(
    _In_ T const volatile *Address)
{
    static_assert(sizeof(T) == sizeof(LONG) || sizeof(T) == sizeof(LONG64),
                  "Only 32 and 64 bit values can be read without tearing");

    if constexpr (sizeof(T) == sizeof(LONG))
    {
        LONG const value = ::ReadNoFence(reinterpret_cast<LONG const volatile *>(Address));
        return *reinterpret_cast<T const *>(&value);
    }
    else
    {
        LONG64 const value = ::ReadNoFence64(reinterpret_cast<LONG64 const volatile *>(Address));
        return *reinterpret_cast<T const *>(&value);
    }
}

}
