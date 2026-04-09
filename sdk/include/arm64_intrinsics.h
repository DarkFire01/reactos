// ARM64 Compiler Intrinsics
// Direct definitions to support __break and related functions

#pragma once

#ifdef _ARM64_

//
// Define MSVC ARM64 intrinsics that may not be available in all toolchains
//

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef __break
#define __break(n) __debugbreak()
#endif

#ifndef YieldProcessor  
__forceinline void YieldProcessor(void) {
    __isb(_ARM64_BARRIER_SY);
}
#endif

//
// Atomic bit operations for ARM64
//
#ifndef _interlockedbittestandset64
__forceinline char _interlockedbittestandset64(__int64 volatile *p, long b) {
    return _InterlockedBitTestAndSet64(p, b);
}
#endif

#ifndef _interlockedbittestandreset64
__forceinline char _interlockedbittestandreset64(__int64 volatile *p, long b) {
    return _InterlockedBitTestAndReset64(p, b);
}
#endif

#if defined(__cplusplus)
}
#endif

#endif // _ARM64_
