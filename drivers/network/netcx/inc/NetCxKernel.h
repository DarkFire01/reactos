/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel surface the class extension is built against
 *
 * The drop expects an internal header that covers everything ntifs.h does. The
 * stub it finds under that name only covers ntddk.h, so this one is included
 * ahead of every source by the CMakeLists to supply push locks, the 64 bit
 * indexed bitmap and the internal macros listed below.
 */

#pragma once

#include <ntifs.h>

/* Internal assertion names the drop uses in place of the NT_ ones. */
#define WIN_ASSERT(Expression)  NT_ASSERT(Expression)
#define WIN_VERIFY(Expression)  NT_VERIFY(Expression)

/* Internal RTL helpers. Alignment is always a power of two. */
#define RTL_IS_POWER_OF_TWO(Value) \
    (((Value) != 0) && (((Value) & ((Value) - 1)) == 0))

#define RTL_NUM_ALIGN_DOWN(Number, Alignment) \
    ((Number) - ((Number) & ((Alignment) - 1)))

#define RTL_NUM_ALIGN_UP(Number, Alignment) \
    RTL_NUM_ALIGN_DOWN((Number) + (Alignment) - 1, (Alignment))

/*
 * KMDF 1.25 routine. The KMDF in this tree is 1.17 and has no persistent state
 * key, so it is reported missing and callers fall back to the Parameters key.
 */
#define WdfDriverOpenPersistentStateRegistryKey(Driver, DesiredAccess, KeyAttributes, Key) \
    ((void)(Driver), (void)(DesiredAccess), (void)(KeyAttributes), (void)(Key), STATUS_NOT_SUPPORTED)

/* The device reset interface is gated past this build's NTDDI floor in wdm.h. */
#include <devicereset.h>

/* Live kernel dumps for device reset diagnostics. */
#include <ndk/dbgkfuncs.h>
