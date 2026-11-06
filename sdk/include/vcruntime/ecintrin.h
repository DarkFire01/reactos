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

/* The x64 string instructions and 64 bit shift helpers have no ARM64EC intrinsic */
#define __EC_STOS(Name, Type)                                                       \
    static __inline void Name(Type *Destination, Type Data, unsigned __int64 Count) \
    {                                                                               \
        while (Count--)                                                             \
            *Destination++ = Data;                                                  \
    }
#define __EC_MOVS(Name, Type)                                                                   \
    static __inline void Name(Type *Destination, const Type *Source, unsigned __int64 Count)    \
    {                                                                                           \
        while (Count--)                                                                         \
            *Destination++ = *Source++;                                                         \
    }

__EC_STOS(__ec_stosb, unsigned char)
__EC_STOS(__ec_stosw, unsigned short)
__EC_STOS(__ec_stosd, unsigned long)
__EC_STOS(__ec_stosq, unsigned __int64)
__EC_MOVS(__ec_movsb, unsigned char)
__EC_MOVS(__ec_movsw, unsigned short)
__EC_MOVS(__ec_movsd, unsigned long)
__EC_MOVS(__ec_movsq, unsigned __int64)

#undef __EC_STOS
#undef __EC_MOVS

#define __stosb(Destination, Data, Count)   __ec_stosb(Destination, Data, Count)
#define __stosw(Destination, Data, Count)   __ec_stosw(Destination, Data, Count)
#define __stosd(Destination, Data, Count)   __ec_stosd(Destination, Data, Count)
#define __stosq(Destination, Data, Count)   __ec_stosq(Destination, Data, Count)
#define __movsb(Destination, Source, Count) __ec_movsb(Destination, Source, Count)
#define __movsw(Destination, Source, Count) __ec_movsw(Destination, Source, Count)
#define __movsd(Destination, Source, Count) __ec_movsd(Destination, Source, Count)
#define __movsq(Destination, Source, Count) __ec_movsq(Destination, Source, Count)

/* MXCSR from FPCR and FPSR: rounding and flush to zero from FPCR, sticky flags from FPSR, exceptions masked */
static __inline unsigned int __ec_mm_getcsr(void)
{
    unsigned __int64 Fpcr = _ReadStatusReg(ARM64_FPCR);
    unsigned __int64 Fpsr = _ReadStatusReg(ARM64_FPSR);
    unsigned int RMode = (unsigned int)(Fpcr >> 22) & 3;
    unsigned int Csr = 0x1F80;

    /* FPCR rounds up with 1 and down with 2, MXCSR the other way around */
    Csr |= (((RMode & 1) << 1) | (RMode >> 1)) << 13;
    Csr |= (unsigned int)((Fpcr >> 24) & 1) << 15;
    Csr |= (unsigned int)(Fpsr & 1);
    Csr |= (unsigned int)((Fpsr >> 1) & 0xF) << 2;
    Csr |= (unsigned int)((Fpsr >> 7) & 1) << 1;
    return Csr;
}

static __inline void __ec_mm_setcsr(unsigned int Csr)
{
    unsigned __int64 Fpcr = _ReadStatusReg(ARM64_FPCR) & ~((3ULL << 22) | (1ULL << 24));
    unsigned __int64 Fpsr = _ReadStatusReg(ARM64_FPSR) & ~0x9FULL;
    unsigned int Rc = (Csr >> 13) & 3;

    Fpcr |= (unsigned __int64)(((Rc & 1) << 1) | (Rc >> 1)) << 22;
    Fpcr |= (unsigned __int64)((Csr >> 15) & 1) << 24;
    Fpsr |= Csr & 1;
    Fpsr |= ((Csr >> 2) & 0xF) << 1;
    Fpsr |= ((Csr >> 1) & 1) << 7;
    _WriteStatusReg(ARM64_FPCR, (__int64)Fpcr);
    _WriteStatusReg(ARM64_FPSR, (__int64)Fpsr);
}

#define _mm_getcsr                      __ec_mm_getcsr
#define _mm_setcsr                      __ec_mm_setcsr

/* The same plain x64 processor the x64 emulator reports to x64 code */
static __inline void __ec_cpuidex(int CpuInfo[4], int Leaf, int SubLeaf)
{
    (void)SubLeaf;
    CpuInfo[0] = (Leaf == 0) ? 1 : 0;
    CpuInfo[1] = (Leaf == 0) ? 0x756E6547 : 0;
    CpuInfo[2] = (Leaf == 0) ? 0x6C65746E : ((Leaf == 1) ? 0x00000001 : 0);
    CpuInfo[3] = (Leaf == 0) ? 0x49656E69 : ((Leaf == 1) ? 0x06808001 : 0);
}

#define __cpuid(CpuInfo, Leaf)              __ec_cpuidex(CpuInfo, Leaf, 0)
#define __cpuidex(CpuInfo, Leaf, SubLeaf)   __ec_cpuidex(CpuInfo, Leaf, SubLeaf)

/* int 2Ch raises an assertion failure, the ARM64 break code for it is 0xF001 */
#define __int2c()                       __break(0xF001)

#define __ll_lshift(Mask, Bit)         ((unsigned __int64)(Mask) << ((Bit) & 63))
#define __ll_rshift(Mask, Bit)          ((__int64)(Mask) >> ((Bit) & 63))
#define __ull_rshift(Mask, Bit)         ((unsigned __int64)(Mask) >> ((Bit) & 63))

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
