/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The parts of <new.h> a kernel mode C++ driver may use
 *
 * The CRT's new.h drags in a user mode runtime that has no business in a
 * driver. rtl/inc/knew.h only needs the nothrow tag to overload on and
 * placement new, both of which are declarations rather than code.
 */

#pragma once

#include <ntddk.h>

namespace std
{
    struct nothrow_t { };
    extern const nothrow_t nothrow;
}

/*
 * Placement new and its matching delete. The delete is never called directly;
 * it exists so the compiler has something to unwind to if a constructor
 * throws, which in a driver it does not.
 */
#ifndef __PLACEMENT_NEW_INLINE
#define __PLACEMENT_NEW_INLINE
inline void * __cdecl operator new(size_t, void * Pointer) throw()
{
    return Pointer;
}

inline void __cdecl operator delete(void *, void *) throw()
{
}
#endif
