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
 * The 64 bit indexed bitmap only exists on 64 bit kernels. On x86 a machine
 * word is 32 bits, which the plain bitmap already indexes.
 */
#ifndef _WIN64
typedef RTL_BITMAP RTL_BITMAP_EX, *PRTL_BITMAP_EX;
#define RtlInitializeBitMapEx       RtlInitializeBitMap
#define RtlClearAllBitsEx           RtlClearAllBits
#define RtlCheckBitEx               RtlCheckBit
#define RtlSetBitEx                 RtlSetBit
#define RtlClearBitEx               RtlClearBit
#define RtlFindSetBitsEx            RtlFindSetBits
#define RtlFindSetBitsAndClearEx    RtlFindSetBitsAndClear
#endif

/* The device reset interface is gated past this build's NTDDI floor in wdm.h. */
#include <devicereset.h>

/* Live kernel dumps for device reset diagnostics. */
#include <ndk/dbgkfuncs.h>

/*
 * wdm.h makes this vanish on x86 and amd64, where caches are DMA coherent. The
 * drop names a local only in the flush, so the arguments are kept referenced.
 */
#if defined(_M_IX86) || defined(_M_AMD64)
#undef KeFlushIoBuffers
#define KeFlushIoBuffers(Mdl, ReadOperation, DmaOperation) \
    ((void)(Mdl), (void)(ReadOperation), (void)(DmaOperation))
#endif
