/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A hash table that grows and shrinks a bucket at a time
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Entries hang off buckets by their Linkage, keyed on a signature the caller
 * supplies, and several entries may share one signature. The table grows and
 * shrinks one bucket at a time rather than doubling: Pivot says how far through
 * the buckets the next size has reached, so only the bucket being split is ever
 * touched, and a lookup consults Pivot to know which of the two divisors its
 * signature was placed with.
 *
 * The directory is one flat array of chain heads, grown in place. That costs a
 * copy per growth and saves the two level walk every lookup would otherwise pay.
 *
 * A signature of all ones is reserved: an enumeration marks its place with an
 * entry of its own carrying that signature, so a lookup for it would find the
 * marker rather than anything a caller put in.
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

#define TAG_HASHTABLE 'BTHR'

/* Buckets a table starts with, and the fewest it keeps */
#define RTLP_HASH_MINIMUM_SHIFT 2
#define RTLP_HASH_MINIMUM_SIZE (1 << RTLP_HASH_MINIMUM_SHIFT)

/* How full the buckets get before growing, and how empty before shrinking */
#define RTLP_HASH_GROW_AT 4
#define RTLP_HASH_SHRINK_AT 2

/* What an enumeration's own entry carries, which no caller may use */
#define RTLP_HASH_ENUMERATOR_SIGNATURE ((ULONG_PTR)~((ULONG_PTR)0))

/* The chain head of one bucket */
static
PLIST_ENTRY
RtlpHashBucket(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ ULONG Index)
{
    return &((PLIST_ENTRY)HashTable->Directory)[Index];
}

/**
 * @brief
 * Works out which bucket a signature belongs in.
 *
 * @remarks
 * Below the pivot the buckets have already been split, so those signatures are
 * placed with the wider mask. Above it they are still placed with the narrower
 * one, and the pivot moving is what carries a bucket from one to the other.
 */
static
ULONG
RtlpHashBucketIndex(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ ULONG_PTR Signature)
{
    ULONG Index = (ULONG)(Signature & HashTable->DivisorMask);

    if (Index < HashTable->Pivot)
        Index = (ULONG)(Signature & ((HashTable->DivisorMask << 1) | 1));

    return Index;
}

/* The first entry at or after this link carrying the signature */
static
PLIST_ENTRY
RtlpHashFindSignature(
    _In_ PLIST_ENTRY ChainHead,
    _In_ PLIST_ENTRY From,
    _In_ ULONG_PTR Signature)
{
    PLIST_ENTRY Walk;

    for (Walk = From; Walk != ChainHead; Walk = Walk->Flink)
    {
        PRTL_DYNAMIC_HASH_TABLE_ENTRY Entry;

        Entry = CONTAINING_RECORD(Walk, RTL_DYNAMIC_HASH_TABLE_ENTRY, Linkage);

        if (Entry->Signature == Signature)
            return Walk;
    }

    return NULL;
}

/* Moves every chain of the old directory into a new one of Size buckets */
static
BOOLEAN
RtlpHashResizeDirectory(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ ULONG Size)
{
    PLIST_ENTRY Directory;
    PLIST_ENTRY Old = HashTable->Directory;
    ULONG Carry = min(Size, HashTable->TableSize);
    ULONG Index;

    Directory = RtlpAllocateMemory(Size * sizeof(LIST_ENTRY), TAG_HASHTABLE);
    if (Directory == NULL)
        return FALSE;

    for (Index = 0; Index < Size; Index++)
        InitializeListHead(&Directory[Index]);

    /*
     * The entries on a chain point back at its head, so a chain cannot simply
     * be copied: it is carried over head and all, which fixes up those links.
     */
    for (Index = 0; Index < Carry; Index++)
    {
        if (IsListEmpty(&Old[Index]))
            continue;

        Directory[Index].Flink = Old[Index].Flink;
        Directory[Index].Blink = Old[Index].Blink;
        Old[Index].Flink->Blink = &Directory[Index];
        Old[Index].Blink->Flink = &Directory[Index];
    }

    HashTable->Directory = Directory;
    RtlpFreeMemory(Old, TAG_HASHTABLE);

    return TRUE;
}

/**
 * @brief
 * Readies a hash table, allocating the table itself when asked to.
 *
 * @param[in,out] HashTable
 * Receives the table. A pointer that is already set names a table the caller
 * owns and this only fills in; one that is NULL asks for one to be made.
 *
 * @param[in] Shift
 * How many bits of a signature to start with. Less than the smallest useful
 * number asks for the smallest table.
 *
 * @param[in] Flags
 * Nothing yet, and must be zero.
 */
BOOLEAN
NTAPI
RtlCreateHashTable(
    _Inout_ PRTL_DYNAMIC_HASH_TABLE *HashTable,
    _In_ ULONG Shift,
    _In_ ULONG Flags)
{
    PRTL_DYNAMIC_HASH_TABLE Table;
    BOOLEAN Allocated = FALSE;
    ULONG Index;

    if ((HashTable == NULL) || (Flags != 0))
        return FALSE;

    if (Shift < RTLP_HASH_MINIMUM_SHIFT)
        Shift = RTLP_HASH_MINIMUM_SHIFT;

    /* A shift wide enough to overflow the divisor describes no table */
    if (Shift >= ((sizeof(ULONG) * 8) - 1))
        return FALSE;

    Table = *HashTable;
    if (Table == NULL)
    {
        Table = RtlpAllocateMemory(sizeof(*Table), TAG_HASHTABLE);
        if (Table == NULL)
            return FALSE;

        Allocated = TRUE;
    }

    RtlZeroMemory(Table, sizeof(*Table));
    Table->Shift = Shift;
    Table->TableSize = 1 << Shift;
    Table->DivisorMask = Table->TableSize - 1;
    Table->Flags = Allocated ? RTL_HASH_ALLOCATED_HEADER : 0;

    Table->Directory = RtlpAllocateMemory(Table->TableSize * sizeof(LIST_ENTRY),
                                          TAG_HASHTABLE);
    if (Table->Directory == NULL)
    {
        if (Allocated)
            RtlpFreeMemory(Table, TAG_HASHTABLE);

        return FALSE;
    }

    for (Index = 0; Index < Table->TableSize; Index++)
        InitializeListHead(RtlpHashBucket(Table, Index));

    *HashTable = Table;

    return TRUE;
}

/**
 * @brief
 * Gives up a hash table's own memory. The entries in it were never ours.
 */
VOID
NTAPI
RtlDeleteHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable)
{
    ASSERT(HashTable->NumEntries == 0);
    ASSERT(HashTable->NumEnumerators == 0);

    RtlpFreeMemory(HashTable->Directory, TAG_HASHTABLE);
    HashTable->Directory = NULL;

    if ((HashTable->Flags & RTL_HASH_ALLOCATED_HEADER) != 0)
        RtlpFreeMemory(HashTable, TAG_HASHTABLE);
}

/**
 * @brief
 * Puts an entry in the table under a signature.
 *
 * @param[in,out] Context
 * Filled in with where the entry went, so a caller holding one can carry on
 * from there without looking the signature up again.
 */
BOOLEAN
NTAPI
RtlInsertEntryHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ PRTL_DYNAMIC_HASH_TABLE_ENTRY Entry,
    _In_ ULONG_PTR Signature,
    _Inout_opt_ PRTL_DYNAMIC_HASH_TABLE_CONTEXT Context)
{
    PLIST_ENTRY ChainHead;

    ASSERT(Signature != RTLP_HASH_ENUMERATOR_SIGNATURE);

    ChainHead = RtlpHashBucket(HashTable,
                               RtlpHashBucketIndex(HashTable, Signature));

    if (IsListEmpty(ChainHead))
        HashTable->NonEmptyBuckets++;

    Entry->Signature = Signature;
    InsertHeadList(ChainHead, &Entry->Linkage);
    HashTable->NumEntries++;

    if (Context != NULL)
    {
        Context->ChainHead = ChainHead;
        Context->PrevLinkage = Entry->Linkage.Blink;
        Context->Signature = Signature;
    }

    return TRUE;
}

/**
 * @brief
 * Takes an entry back out of the table.
 */
BOOLEAN
NTAPI
RtlRemoveEntryHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ PRTL_DYNAMIC_HASH_TABLE_ENTRY Entry,
    _Inout_opt_ PRTL_DYNAMIC_HASH_TABLE_CONTEXT Context)
{
    PLIST_ENTRY ChainHead;

    ChainHead = RtlpHashBucket(HashTable,
                               RtlpHashBucketIndex(HashTable, Entry->Signature));

    if (Context != NULL)
    {
        Context->ChainHead = ChainHead;
        Context->PrevLinkage = Entry->Linkage.Blink;
        Context->Signature = Entry->Signature;
    }

    RemoveEntryList(&Entry->Linkage);
    HashTable->NumEntries--;

    if (IsListEmpty(ChainHead))
        HashTable->NonEmptyBuckets--;

    return TRUE;
}

/**
 * @brief
 * Finds the first entry under a signature.
 *
 * @param[out] Context
 * Where the search reached, which RtlGetNextEntryHashTable carries on from.
 *
 * @return
 * The entry, or NULL when the table holds none under that signature.
 */
PRTL_DYNAMIC_HASH_TABLE_ENTRY
NTAPI
RtlLookupEntryHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ ULONG_PTR Signature,
    _Out_opt_ PRTL_DYNAMIC_HASH_TABLE_CONTEXT Context)
{
    PLIST_ENTRY ChainHead;
    PLIST_ENTRY Found;

    ChainHead = RtlpHashBucket(HashTable,
                               RtlpHashBucketIndex(HashTable, Signature));
    Found = RtlpHashFindSignature(ChainHead, ChainHead->Flink, Signature);

    if (Context != NULL)
    {
        Context->ChainHead = ChainHead;
        Context->PrevLinkage = (Found != NULL) ? Found->Blink : ChainHead->Blink;
        Context->Signature = Signature;
    }

    if (Found == NULL)
        return NULL;

    return CONTAINING_RECORD(Found, RTL_DYNAMIC_HASH_TABLE_ENTRY, Linkage);
}

/**
 * @brief
 * Finds the next entry under the signature a context was left on.
 *
 * @return
 * The entry, or NULL once the signature has no more.
 */
PRTL_DYNAMIC_HASH_TABLE_ENTRY
NTAPI
RtlGetNextEntryHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _In_ PRTL_DYNAMIC_HASH_TABLE_CONTEXT Context)
{
    PLIST_ENTRY Found;

    UNREFERENCED_PARAMETER(HashTable);

    /* PrevLinkage is the link ahead of the entry the context was left on */
    Found = RtlpHashFindSignature(Context->ChainHead,
                                  Context->PrevLinkage->Flink->Flink,
                                  Context->Signature);
    if (Found == NULL)
        return NULL;

    Context->PrevLinkage = Found->Blink;

    return CONTAINING_RECORD(Found, RTL_DYNAMIC_HASH_TABLE_ENTRY, Linkage);
}

/**
 * @brief
 * Readies an enumeration, and holds the table at its present size for it.
 *
 * @remarks
 * The enumerator carries an entry of its own, which is linked into the chain it
 * has reached. Entries put in or taken out while it runs therefore cannot lose
 * its place, because the place is a real link rather than an index.
 */
BOOLEAN
NTAPI
RtlInitEnumerationHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _Out_ PRTL_DYNAMIC_HASH_TABLE_ENUMERATOR Enumerator)
{
    PLIST_ENTRY ChainHead = RtlpHashBucket(HashTable, 0);

    RtlZeroMemory(Enumerator, sizeof(*Enumerator));
    Enumerator->ChainHead = ChainHead;
    Enumerator->BucketIndex = 0;
    Enumerator->HashEntry.Signature = RTLP_HASH_ENUMERATOR_SIGNATURE;

    InsertHeadList(ChainHead, &Enumerator->HashEntry.Linkage);
    HashTable->NumEnumerators++;

    return TRUE;
}

/**
 * @brief
 * Hands back the next entry of an enumeration.
 *
 * @return
 * The entry, or NULL once every entry has been handed back once.
 */
PRTL_DYNAMIC_HASH_TABLE_ENTRY
NTAPI
RtlEnumerateEntryHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _Inout_ PRTL_DYNAMIC_HASH_TABLE_ENUMERATOR Enumerator)
{
    for (;;)
    {
        PLIST_ENTRY Next = Enumerator->HashEntry.Linkage.Flink;

        /* Step the marker over whatever comes next in this chain */
        if (Next != Enumerator->ChainHead)
        {
            RemoveEntryList(&Enumerator->HashEntry.Linkage);
            InsertHeadList(Next, &Enumerator->HashEntry.Linkage);

            return CONTAINING_RECORD(Next,
                                     RTL_DYNAMIC_HASH_TABLE_ENTRY,
                                     Linkage);
        }

        /* This chain is spent, so carry the marker to the next bucket */
        Enumerator->BucketIndex++;
        if (Enumerator->BucketIndex >= HashTable->TableSize)
            return NULL;

        RemoveEntryList(&Enumerator->HashEntry.Linkage);
        Enumerator->ChainHead = RtlpHashBucket(HashTable,
                                               Enumerator->BucketIndex);
        InsertHeadList(Enumerator->ChainHead, &Enumerator->HashEntry.Linkage);
    }
}

/**
 * @brief
 * Ends an enumeration and lets the table change size again.
 */
VOID
NTAPI
RtlEndEnumerationHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable,
    _Inout_ PRTL_DYNAMIC_HASH_TABLE_ENUMERATOR Enumerator)
{
    RemoveEntryList(&Enumerator->HashEntry.Linkage);
    Enumerator->ChainHead = NULL;
    HashTable->NumEnumerators--;
}

/**
 * @brief
 * Adds one bucket, if the table is full enough to want one.
 *
 * @return
 * Whether a bucket was added.
 */
BOOLEAN
NTAPI
RtlExpandHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable)
{
    PLIST_ENTRY Donor;
    PLIST_ENTRY Grown;
    PLIST_ENTRY Walk;
    ULONG Wider = (HashTable->DivisorMask << 1) | 1;
    ULONG Pivot;

    if (HashTable->NumEnumerators != 0)
        return FALSE;

    if (HashTable->NumEntries < (HashTable->TableSize * RTLP_HASH_GROW_AT))
        return FALSE;

    if (!RtlpHashResizeDirectory(HashTable, HashTable->TableSize + 1))
        return FALSE;

    Grown = RtlpHashBucket(HashTable, HashTable->TableSize);
    HashTable->TableSize++;

    /*
     * Only the bucket at the pivot is split, and it gives up exactly those of
     * its entries that the wider mask sends to the new bucket.
     */
    Pivot = HashTable->Pivot;
    Donor = RtlpHashBucket(HashTable, Pivot);

    Walk = Donor->Flink;
    while (Walk != Donor)
    {
        PRTL_DYNAMIC_HASH_TABLE_ENTRY Entry;
        PLIST_ENTRY Next = Walk->Flink;

        Entry = CONTAINING_RECORD(Walk, RTL_DYNAMIC_HASH_TABLE_ENTRY, Linkage);

        if ((ULONG)(Entry->Signature & Wider) != Pivot)
        {
            if (IsListEmpty(Grown))
                HashTable->NonEmptyBuckets++;

            RemoveEntryList(Walk);
            InsertHeadList(Grown, Walk);
        }

        Walk = Next;
    }

    if (IsListEmpty(Donor))
        HashTable->NonEmptyBuckets--;

    /* Once every bucket of the old size is split, that size is the new one */
    HashTable->Pivot++;
    if (HashTable->Pivot > HashTable->DivisorMask)
    {
        HashTable->Pivot = 0;
        HashTable->DivisorMask = Wider;
        HashTable->Shift++;
    }

    return TRUE;
}

/**
 * @brief
 * Gives one bucket back, if the table is empty enough to spare it.
 *
 * @return
 * Whether a bucket was given back.
 */
BOOLEAN
NTAPI
RtlContractHashTable(
    _In_ PRTL_DYNAMIC_HASH_TABLE HashTable)
{
    PLIST_ENTRY Doomed;
    PLIST_ENTRY Keeper;

    if (HashTable->NumEnumerators != 0)
        return FALSE;

    if (HashTable->TableSize <= RTLP_HASH_MINIMUM_SIZE)
        return FALSE;

    if (HashTable->NumEntries > (HashTable->TableSize * RTLP_HASH_SHRINK_AT))
        return FALSE;

    /* Undo the last split, which is the one the pivot sits just past */
    if (HashTable->Pivot == 0)
    {
        HashTable->DivisorMask = HashTable->DivisorMask >> 1;
        HashTable->Shift--;
        HashTable->Pivot = HashTable->DivisorMask;
    }
    else
    {
        HashTable->Pivot--;
    }

    Doomed = RtlpHashBucket(HashTable, HashTable->TableSize - 1);
    Keeper = RtlpHashBucket(HashTable, HashTable->Pivot);

    if (!IsListEmpty(Doomed))
    {
        if (IsListEmpty(Keeper))
            HashTable->NonEmptyBuckets++;

        /* The whole chain goes back to the bucket it was split out of */
        Keeper->Blink->Flink = Doomed->Flink;
        Doomed->Flink->Blink = Keeper->Blink;
        Doomed->Blink->Flink = Keeper;
        Keeper->Blink = Doomed->Blink;

        InitializeListHead(Doomed);
        HashTable->NonEmptyBuckets--;
    }

    HashTable->TableSize--;

    /* Losing the last bucket cannot fail, so a refused move leaves it be */
    RtlpHashResizeDirectory(HashTable, HashTable->TableSize);

    return TRUE;
}

/* EOF */
