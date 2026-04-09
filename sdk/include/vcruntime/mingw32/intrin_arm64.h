/*
	ARM64 Intrinsic Functions for GCC compatibility
	Based on intrin_arm.h pattern
*/

#ifndef KJK_INTRIN_ARM64_H_
#define KJK_INTRIN_ARM64_H_

#ifndef __GNUC__
#error Unsupported compiler
#endif

#define _ReturnAddress() (__builtin_return_address(0))
#define _ReadWriteBarrier() __sync_synchronize()

__INTRIN_INLINE void YieldProcessor(void) { __asm__ __volatile__("isb" ::: "memory"); }

__INTRIN_INLINE void __break(unsigned int value) { __asm__ __volatile__("brk %0" : : "I" (value)); }

__INTRIN_INLINE unsigned short _byteswap_ushort(unsigned short value)
{
	return (value >> 8) | (value << 8);
}

__INTRIN_INLINE unsigned _CountLeadingZeros(long Mask)
{
    return Mask ? __builtin_clzll(Mask) : 64;
}

__INTRIN_INLINE unsigned _CountTrailingZeros(long Mask)
{
    return Mask ? __builtin_ctzll(Mask) : 64;
}

__INTRIN_INLINE unsigned char _BitScanForward(unsigned long * const Index, const unsigned long Mask)
{
	*Index = __builtin_ctzll(Mask);
	return Mask ? 1 : 0;
}

__INTRIN_INLINE char _interlockedbittestandset64(volatile __int64 *p, long b)
{
	return __sync_fetch_and_or(p, 1LL << b) ? 1 : 0;
}

__INTRIN_INLINE char _interlockedbittestandreset64(volatile __int64 *p, long b)
{
	return (__sync_fetch_and_and(p, ~(1LL << b)) >> b) & 1;
}

#endif // KJK_INTRIN_ARM64_H_
