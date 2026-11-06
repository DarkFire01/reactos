/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     x64 intrinsics expressed with ARM64 ones for ARM64EC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef _M_ARM64EC

/* The TEB that gs points to on x64 lives in x18 */
#define __readgsbyte(Offset)            __readx18byte(Offset)
#define __readgsword(Offset)            __readx18word(Offset)
#define __readgsdword(Offset)           __readx18dword(Offset)
#define __readgsqword(Offset)           __readx18qword(Offset)
#define __writegsbyte(Offset, Data)     __writex18byte(Offset, Data)
#define __writegsword(Offset, Data)     __writex18word(Offset, Data)
#define __writegsdword(Offset, Data)    __writex18dword(Offset, Data)
#define __writegsqword(Offset, Data)    __writex18qword(Offset, Data)
#define __addgsbyte(Offset, Data)       __writex18byte(Offset, (unsigned char)(__readx18byte(Offset) + (Data)))
#define __addgsword(Offset, Data)       __writex18word(Offset, (unsigned short)(__readx18word(Offset) + (Data)))
#define __addgsdword(Offset, Data)      __writex18dword(Offset, __readx18dword(Offset) + (Data))
#define __addgsqword(Offset, Data)      __writex18qword(Offset, __readx18qword(Offset) + (Data))
#define __incgsbyte(Offset)             __addgsbyte(Offset, 1)
#define __incgsword(Offset)             __addgsword(Offset, 1)
#define __incgsdword(Offset)            __addgsdword(Offset, 1)
#define __incgsqword(Offset)            __addgsqword(Offset, 1)

/* x64 store ordering is at least what a full inner shareable barrier gives */
#define __faststorefence()              __dmb(_ARM64_BARRIER_ISH)
#define _mm_mfence()                    __dmb(_ARM64_BARRIER_ISH)
#define _mm_lfence()                    __dmb(_ARM64_BARRIER_ISHLD)
#define _mm_sfence()                    __dmb(_ARM64_BARRIER_ISHST)
#define _mm_pause()                     __yield()

#define __emul(a, b)                    ((__int64)(int)(a) * (__int64)(int)(b))
#define __emulu(a, b)                   ((unsigned __int64)(unsigned int)(a) * (unsigned __int64)(unsigned int)(b))

/* The non prefetching forms only differ in a cache hint */
#define _InterlockedAnd_np              _InterlockedAnd
#define _InterlockedAnd8_np             _InterlockedAnd8
#define _InterlockedAnd16_np            _InterlockedAnd16
#define _InterlockedAnd64_np            _InterlockedAnd64
#define _InterlockedOr_np               _InterlockedOr
#define _InterlockedOr8_np              _InterlockedOr8
#define _InterlockedOr16_np             _InterlockedOr16
#define _InterlockedOr64_np             _InterlockedOr64
#define _InterlockedXor_np              _InterlockedXor
#define _InterlockedXor8_np             _InterlockedXor8
#define _InterlockedXor16_np            _InterlockedXor16
#define _InterlockedXor64_np            _InterlockedXor64
#define _InterlockedCompareExchange_np         _InterlockedCompareExchange
#define _InterlockedCompareExchange16_np       _InterlockedCompareExchange16
#define _InterlockedCompareExchange64_np       _InterlockedCompareExchange64
#define _InterlockedCompareExchangePointer_np  _InterlockedCompareExchangePointer
#define _InterlockedCompareExchange128_np      _InterlockedCompareExchange128

#endif /* _M_ARM64EC */
