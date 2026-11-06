/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 interlocked S-List routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ksarm64.h>

//
// The 16 byte header holds the depth in the low 16 bits of its first
// quadword and the first entry in the second one. Exclusive pair loads and
// stores update both halves at once, so no sequence number is needed to
// catch a list that changed in between.
//

    IMPORT A64NAME(RtlRaiseStatus)
    TEXTAREA

//
// VOID
// RtlInitializeSListHead(
//     _Out_ PSLIST_HEADER SListHead);
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlInitializeSListHead),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlInitializeSListHead)
    tst     x0, #15
    b.ne    RtlpSListHeadMisaligned
    stp     xzr, xzr, [x0]
    ret
LABEL(RtlpSListHeadMisaligned)
    ldr     w0, =STATUS_DATATYPE_MISALIGNMENT
    b       A64NAME(RtlRaiseStatus)
    LEAF_END A64NAME(RtlInitializeSListHead)

//
// PSLIST_ENTRY
// RtlFirstEntrySList(
//     _In_ const SLIST_HEADER *SListHead);
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlFirstEntrySList),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlFirstEntrySList)
    ldr     x0, [x0, #8]
    ret
    LEAF_END A64NAME(RtlFirstEntrySList)

//
// USHORT
// RtlQueryDepthSList(
//     _In_ PSLIST_HEADER SListHead);
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlQueryDepthSList),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlQueryDepthSList)
    ldrh    w0, [x0]
    ret
    LEAF_END A64NAME(RtlQueryDepthSList)

//
// PSLIST_ENTRY
// RtlInterlockedPopEntrySList(
//     _Inout_ PSLIST_HEADER SListHead);
//
// The entry can be freed by another thread before its link is read. The
// kernel recognizes a fault on ExpInterlockedPopEntrySListFault and restarts
// at ExpInterlockedPopEntrySListResume.
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlInterlockedPopEntrySList),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlInterlockedPopEntrySList)
    ALTERNATE_ENTRY A64NAME(ExpInterlockedPopEntrySList)
    ALTERNATE_ENTRY A64NAME(ExpInterlockedPopEntrySListResume)
    ldxp    x16, x17, [x0]
    cbz     x17, RtlpPopEntrySListEmpty
    ALTERNATE_ENTRY A64NAME(ExpInterlockedPopEntrySListFault)
    ldr     x1, [x17]
    sub     x16, x16, #1
    ALTERNATE_ENTRY A64NAME(ExpInterlockedPopEntrySListEnd)
    stxp    w2, x16, x1, [x0]
    cbnz    w2, A64NAME(ExpInterlockedPopEntrySListResume)
    mov     x0, x17
    ret
LABEL(RtlpPopEntrySListEmpty)
    clrex
    mov     x0, xzr
    ret
    LEAF_END A64NAME(RtlInterlockedPopEntrySList)

//
// PSLIST_ENTRY
// RtlInterlockedPushEntrySList(
//     _Inout_ PSLIST_HEADER SListHead,
//     _Inout_ PSLIST_ENTRY ListEntry);
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlInterlockedPushEntrySList),2,0
    LEAF_ENTRY_COMDAT A64NAME(RtlInterlockedPushEntrySList)
    ALTERNATE_ENTRY A64NAME(ExpInterlockedPushEntrySList)
    mov     x2, x1
    mov     w3, #1
    b       RtlpPushSList
    LEAF_END A64NAME(RtlInterlockedPushEntrySList)

//
// PSLIST_ENTRY
// RtlInterlockedPushListSList(
//     _Inout_ PSLIST_HEADER SListHead,
//     _Inout_ PSLIST_ENTRY List,
//     _Inout_ PSLIST_ENTRY ListEnd,
//     _In_ ULONG Count);
//
// The new entries get linked in front of the current first entry before
// they are published, a header that moved meanwhile means linking again.
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlInterlockedPushListSList),4,0
    LEAF_ENTRY_COMDAT A64NAME(RtlInterlockedPushListSList)
    ALTERNATE_ENTRY A64NAME(RtlInterlockedPushListSListEx)
LABEL(RtlpPushSList)
    ldr     x17, [x0, #8]
    str     x17, [x2]
    dmb     ish
    ldxp    x16, x4, [x0]
    cmp     x4, x17
    b.ne    RtlpPushSList
    add     x16, x16, w3, uxtw
    stxp    w5, x16, x1, [x0]
    cbnz    w5, RtlpPushSList
    mov     x0, x17
    ret
    LEAF_END A64NAME(RtlInterlockedPushListSList)

//
// PSLIST_ENTRY
// RtlInterlockedFlushSList(
//     _Inout_ PSLIST_HEADER SListHead);
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlInterlockedFlushSList),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlInterlockedFlushSList)
    ALTERNATE_ENTRY A64NAME(ExpInterlockedFlushSList)
    ldxp    x16, x17, [x0]
    stxp    w2, xzr, xzr, [x0]
    cbnz    w2, A64NAME(RtlInterlockedFlushSList)
    mov     x0, x17
    ret
    LEAF_END A64NAME(RtlInterlockedFlushSList)

    END
