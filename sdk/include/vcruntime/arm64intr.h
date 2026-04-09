/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 intriniscs
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum _tag_ARM64INTR_BARRIER_TYPE
{
    _ARM64_BARRIER_SY     = 0xF,
    _ARM64_BARRIER_ST     = 0xE,
    _ARM64_BARRIER_LD     = 0xD,
    _ARM64_BARRIER_ISH    = 0xB,
    _ARM64_BARRIER_ISHST  = 0xA,
    _ARM64_BARRIER_ISHLD  = 0x9,
    _ARM64_BARRIER_NSH    = 0x7,
    _ARM64_BARRIER_NSHST  = 0x6,
    _ARM64_BARRIER_NSHLD  = 0x5,
    _ARM64_BARRIER_OSH    = 0x3,
    _ARM64_BARRIER_OSHST  = 0x2,
    _ARM64_BARRIER_OSHLD  = 0x1
} _ARM64INTR_BARRIER_TYPE;

void __dmb(unsigned int _Type);
void __dsb(unsigned int _Type);
void __isb(unsigned int _Type);

#pragma intrinsic(__dmb)
#pragma intrinsic(__dsb)
#pragma intrinsic(__isb)

// Define __break as a macro to avoid compiler intrinsic conflicts
// Maps to __debugbreak() which triggers debugging
#define __break(__imm) __debugbreak()

// Yield processor execution hint
__forceinline
void YieldProcessor(void)
{
    __isb(_ARM64_BARRIER_SY);
}

// Read timestamp counter
unsigned __int64 __rdtsc(void);

// Interlock bit test operations - ARM64 MSVC library should provide these
unsigned char _interlockedbittestandset64(__int64 volatile *_Base, long _Bit);
unsigned char _interlockedbittestandreset64(__int64 volatile *_Base, long _Bit);
unsigned char _InterlockedBitTestAndSet64(__int64 volatile *_Base, long _Bit);
unsigned char _InterlockedBitTestAndReset64(__int64 volatile *_Base, long _Bit);

#if defined(__cplusplus)
} // extern "C"
#endif
