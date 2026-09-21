/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shim for the WDK <umwdm.h>
 *
 * umwdm.h is the user mode half of the WDM surface, so most of the drop only
 * reaches for it in the #else of an #ifdef _KERNEL_MODE. This build defines
 * _KERNEL_MODE, which makes those dead, but bm/bmprecomp.hpp includes it
 * unconditionally. Give it the kernel surface it actually wants there rather
 * than patching the imported source.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#error "umwdm.h has no user mode implementation here"
#endif
