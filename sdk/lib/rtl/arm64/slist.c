/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS Run-Time Library
 * PURPOSE:           Interlocked SLIST for ARM64 (matches amd64/slist.S)
 */

#include <rtl.h>
#define NDEBUG
#include <debug.h>

#ifdef _M_ARM64

/* Same constants as sdk/lib/rtl/amd64/slist.S */
#define SLIST8A_DEPTH_INC        0x0000000000000001ull
#define SLIST8A_DEPTH_MASK       0x000000000000FFFFull
#define SLIST8A_SEQUENCE_MASK    0x0000000001FF0000ull
#define SLIST8A_SEQUENCE_INC     0x0000000000010000ull
#define SLIST8A_NEXTENTRY_MASK   0xFFFFFFFFFE000000ull
#define SLIST8A_NEXTENTRY_SHIFT  21
#define SLIST8_POINTER_MASK      0x000007FFFFFFFFFFull

#define SLIST16A_DEPTH_INC       0x0000000000000001ull
#define SLIST16A_SEQUENCE_INC    0x0000000000010000ull
#define SLIST16B_HEADERTYPE_MASK 0x0000000000000001ull
#define SLIST16B_INIT_MASK       0x0000000000000002ull
#define SLIST16B_NEXTENTRY_MASK  0xFFFFFFFFFFFFFFF0ull

extern BOOLEAN RtlpUse16ByteSLists;

BOOLEAN RtlpExpectSListFault;

static FORCEINLINE PSLIST_ENTRY
RtlpDecodeSList8Next(_In_ PSLIST_HEADER Head, _In_ ULONGLONG Alignment)
{
    ULONGLONG Encoded = Alignment & SLIST8A_NEXTENTRY_MASK;
    return (PSLIST_ENTRY)((Encoded >> SLIST8A_NEXTENTRY_SHIFT) |
                          ((ULONG_PTR)Head & ~SLIST8_POINTER_MASK));
}

static FORCEINLINE ULONGLONG
RtlpEncodeSList8Next(_In_ PSLIST_HEADER Head, _In_ PSLIST_ENTRY Entry)
{
    return (((ULONGLONG)(ULONG_PTR)Entry << SLIST8A_NEXTENTRY_SHIFT) &
            SLIST8A_NEXTENTRY_MASK);
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPushEntrySList(
    _Inout_ PSLIST_HEADER SListHead,
    _Inout_ __drv_aliasesMem PSLIST_ENTRY SListEntry)
{
    ULONGLONG OldAlign, NewAlign, Compare, SavedNext;
    PSLIST_ENTRY OldFirst;

    if (*(volatile CHAR *)&RtlpUse16ByteSLists)
    {
        SLIST_HEADER OldHead, NewHead;
        BOOLEAN Exchanged;

        do
        {
            OldHead = *SListHead;
            OldFirst = (PSLIST_ENTRY)(OldHead.Region & SLIST16B_NEXTENTRY_MASK);
            SListEntry->Next = OldFirst;

            NewHead.Alignment =
                OldHead.Alignment + SLIST16A_DEPTH_INC + SLIST16A_SEQUENCE_INC;
            NewHead.Region =
                ((ULONGLONG)(ULONG_PTR)SListEntry) |
                (SLIST16B_HEADERTYPE_MASK | SLIST16B_INIT_MASK);

            Exchanged = _InterlockedCompareExchange128((PLONG64)SListHead,
                                                       NewHead.Region,
                                                       NewHead.Alignment,
                                                       (PLONG64)&OldHead);
        } while (!Exchanged);

        return (PSLIST_ENTRY)(OldHead.Region & SLIST16B_NEXTENTRY_MASK);
    }

    OldAlign = SListHead->Alignment;
    for (;;)
    {
        ULONGLONG Masked = OldAlign & SLIST8A_NEXTENTRY_MASK;
        if (Masked != 0)
        {
            OldFirst = RtlpDecodeSList8Next(SListHead, OldAlign);
        }
        else
        {
            OldFirst = NULL;
        }

        SListEntry->Next = OldFirst;

        NewAlign = RtlpEncodeSList8Next(SListHead, SListEntry);
        NewAlign |= (OldAlign + SLIST8A_DEPTH_INC + SLIST8A_SEQUENCE_INC) &
                    (SLIST8A_SEQUENCE_MASK | SLIST8A_DEPTH_MASK);

        SavedNext = *(volatile ULONGLONG *)&SListEntry->Next;

        Compare = OldAlign;
        OldAlign = (ULONGLONG)InterlockedCompareExchange64(
            (LONG64 volatile *)&SListHead->Alignment,
            (LONG64)NewAlign,
            (LONG64)Compare);

        if (OldAlign == Compare)
            return (PSLIST_ENTRY)SavedNext;
    }
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPopEntrySList(
    _Inout_ PSLIST_HEADER SListHead)
{
    if (*(volatile CHAR *)&RtlpUse16ByteSLists)
    {
        SLIST_HEADER OldHead, NewHead;
        BOOLEAN Exchanged;
        PSLIST_ENTRY First;
        ULONGLONG NextQword;

        restart16:
        OldHead = *SListHead;
        for (;;)
        {
            First = (PSLIST_ENTRY)(OldHead.Region & SLIST16B_NEXTENTRY_MASK);
            if (!First)
                return NULL;

            RtlpExpectSListFault = TRUE;
            _SEH2_TRY
            {
                NextQword = *(volatile ULONGLONG *)&First->Next;
            }
            _SEH2_EXCEPT(((SListHead->Region & SLIST16B_NEXTENTRY_MASK) !=
                          (OldHead.Region & SLIST16B_NEXTENTRY_MASK)) ?
                         EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                RtlpExpectSListFault = FALSE;
                goto restart16;
            }
            _SEH2_END;
            RtlpExpectSListFault = FALSE;

            NewHead.Region =
                (NextQword | (SLIST16B_HEADERTYPE_MASK | SLIST16B_INIT_MASK));
            NewHead.Alignment = OldHead.Alignment - SLIST16A_DEPTH_INC;

            Exchanged = _InterlockedCompareExchange128((PLONG64)SListHead,
                                                       NewHead.Region,
                                                       NewHead.Alignment,
                                                       (PLONG64)&OldHead);
            if (Exchanged)
                return (PSLIST_ENTRY)(OldHead.Region & SLIST16B_NEXTENTRY_MASK);

            OldHead = *SListHead;
        }
    }

    {
        ULONGLONG OldAlign, NewAlign, Compare;
        PSLIST_ENTRY First;
        ULONGLONG NextShifted;

        restart8:
        OldAlign = SListHead->Alignment;
        for (;;)
        {
            if (!(OldAlign & SLIST8A_NEXTENTRY_MASK))
                return NULL;

            First = RtlpDecodeSList8Next(SListHead, OldAlign);

            NewAlign = (OldAlign - SLIST8A_DEPTH_INC) &
                       (SLIST8A_SEQUENCE_MASK | SLIST8A_DEPTH_MASK);

            RtlpExpectSListFault = TRUE;
            _SEH2_TRY
            {
                NextShifted = (ULONGLONG)(ULONG_PTR)First->Next
                              << SLIST8A_NEXTENTRY_SHIFT;
            }
            _SEH2_EXCEPT(
                (RtlpDecodeSList8Next(SListHead, SListHead->Alignment) != First) ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                RtlpExpectSListFault = FALSE;
                goto restart8;
            }
            _SEH2_END;
            RtlpExpectSListFault = FALSE;

            NewAlign |= NextShifted;

            Compare = OldAlign;
            OldAlign = (ULONGLONG)InterlockedCompareExchange64(
                (LONG64 volatile *)&SListHead->Alignment,
                (LONG64)NewAlign,
                (LONG64)Compare);

            if (OldAlign == Compare)
                return RtlpDecodeSList8Next(SListHead, Compare);
        }
    }
}

PSLIST_ENTRY
NTAPI
RtlInterlockedFlushSList(
    _Inout_ PSLIST_HEADER SListHead)
{
    if (*(volatile CHAR *)&RtlpUse16ByteSLists)
    {
        SLIST_HEADER OldHead, NewHead;
        BOOLEAN Exchanged;

        NewHead.Alignment = 0;
        NewHead.Region = (SLIST16B_HEADERTYPE_MASK | SLIST16B_INIT_MASK);

        do
        {
            OldHead = *SListHead;
            Exchanged = _InterlockedCompareExchange128((PLONG64)SListHead,
                                                       NewHead.Region,
                                                       NewHead.Alignment,
                                                       (PLONG64)&OldHead);
        } while (!Exchanged);

        return (PSLIST_ENTRY)(OldHead.Region & SLIST16B_NEXTENTRY_MASK);
    }

    {
        ULONGLONG OldAlign, Compare;

        OldAlign = SListHead->Alignment;
        for (;;)
        {
            Compare = OldAlign;
            OldAlign = (ULONGLONG)InterlockedCompareExchange64(
                (LONG64 volatile *)&SListHead->Alignment,
                0,
                (LONG64)Compare);

            if (OldAlign == Compare)
            {
                ULONGLONG Encoded = Compare & SLIST8A_NEXTENTRY_MASK;
                return (PSLIST_ENTRY)((Encoded >> SLIST8A_NEXTENTRY_SHIFT) |
                                      ((ULONG_PTR)SListHead & ~SLIST8_POINTER_MASK));
            }
        }
    }
}

#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:ExpInterlockedPopEntrySList=RtlInterlockedPopEntrySList")
#pragma comment(linker, "/alternatename:ExpInterlockedPushEntrySList=RtlInterlockedPushEntrySList")
#pragma comment(linker, "/alternatename:ExpInterlockedFlushSList=RtlInterlockedFlushSList")
#else
#pragma redefine_extname RtlInterlockedPopEntrySList ExpInterlockedPopEntrySList
#pragma redefine_extname RtlInterlockedPushEntrySList ExpInterlockedPushEntrySList
#pragma redefine_extname RtlInterlockedFlushSList ExpInterlockedFlushSList
#endif

#endif /* _M_ARM64 */
