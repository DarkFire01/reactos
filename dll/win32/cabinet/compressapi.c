/*
 * PROJECT:     ReactOS Cabinet
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Compression API with MSZIP, XPRESS and XPRESS Huffman codecs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <compressapi.h>
#include <zlib.h>

/*
 * Without COMPRESS_RAW the output is a buffer: a 24 byte header followed by
 * blocks, each a 32-bit compressed size and the block data. A block whose
 * compressed size equals its uncompressed size is stored as is.
 */
#define CMP_BUFFER_SIGNATURE    0xC0E5510A
#define CMP_BUFFER_HEADER_SIZE  24
#define CMP_BUFFER_SIZED_BLOCKS 0x0001
#define CMP_BUFFER_MAX_BLOCK    0x4000000
#define CMP_MSZIP_MAX_BLOCK     0x8000

#define CMP_COMPRESSOR_SIGNATURE   0x504D4F43
#define CMP_DECOMPRESSOR_SIGNATURE 0x504D4344

#define LZ_MIN_MATCH 3
#define LZ_HASH_BITS 15
#define LZ_HASH_SIZE (1 << LZ_HASH_BITS)

#define XPRESS_MAX_OFFSET 8192
#define XPRESS_WINDOW     8192

#define HUFF_CHUNK_SIZE     65536
#define HUFF_SYMBOLS        512
#define HUFF_TABLE_BYTES    (HUFF_SYMBOLS / 2)
#define HUFF_MAX_CODE_BITS  15
#define HUFF_EOF_SYMBOL     256
#define HUFF_MAX_OFFSET     65535
#define HUFF_WINDOW         65536

#define MSZIP_SIGNATURE_SIZE 2
#define MSZIP_WORST_EXTRA    12

typedef struct _CMP_HANDLE
{
    ULONG Signature;
    ULONG Algorithm;
    BOOL Raw;
    ULONG Level;
    COMPRESS_ALLOCATION_ROUTINES Routines;
} CMP_HANDLE, *PCMP_HANDLE;

typedef struct _CMP_BUFFER_HEADER
{
    ULONG Signature;
    USHORT HeaderSize;
    UCHAR Checksum;
    UCHAR Algorithm;
    ULONGLONG UncompressedSize;
    ULONG BlockSize;
    ULONG Flags;
} CMP_BUFFER_HEADER, *PCMP_BUFFER_HEADER;

C_ASSERT(sizeof(CMP_BUFFER_HEADER) == CMP_BUFFER_HEADER_SIZE);

typedef struct _LZ_STATE
{
    const BYTE *Data;
    ULONG Size;
    ULONG WindowMask;
    ULONG MaxOffset;
    ULONG ChainDepth;
    LONG *Head;
    LONG *Prev;
} LZ_STATE, *PLZ_STATE;

typedef struct _HUFF_TOKEN
{
    ULONG Length;
    ULONG Value;
} HUFF_TOKEN, *PHUFF_TOKEN;

typedef struct _HUFF_WRITER
{
    PBYTE Out;
    SIZE_T OutSize;
    SIZE_T Seq1;
    SIZE_T Seq2;
    SIZE_T Next;
    ULONG Buffer;
    ULONG Count;
    BOOL Overflow;
} HUFF_WRITER, *PHUFF_WRITER;

typedef struct _HUFF_NODE
{
    ULONG Weight;
    USHORT Symbol;
    USHORT Parent;
} HUFF_NODE, *PHUFF_NODE;

/* Per call working memory of the LZ based encoders */
typedef struct _LZ_WORKSPACE
{
    LZ_STATE Lz;
    PHUFF_TOKEN Tokens;
} LZ_WORKSPACE, *PLZ_WORKSPACE;

static
PVOID
__cdecl
CmpDefaultAllocate(
    _In_ PVOID UserContext,
    _In_ SIZE_T Size)
{
    UNREFERENCED_PARAMETER(UserContext);
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

static
VOID
__cdecl
CmpDefaultFree(
    _In_ PVOID UserContext,
    _In_ PVOID Memory)
{
    UNREFERENCED_PARAMETER(UserContext);
    HeapFree(GetProcessHeap(), 0, Memory);
}

static
PVOID
CmpAllocate(
    _In_ PCMP_HANDLE Handle,
    _In_ SIZE_T Size)
{
    return Handle->Routines.Allocate(Handle->Routines.UserContext, Size);
}

static
VOID
CmpFree(
    _In_ PCMP_HANDLE Handle,
    _In_opt_ PVOID Memory)
{
    if (Memory)
        Handle->Routines.Free(Handle->Routines.UserContext, Memory);
}

static
voidpf
CmpZlibAllocate(
    _In_ voidpf Opaque,
    _In_ uInt Items,
    _In_ uInt Size)
{
    SIZE_T Bytes = (SIZE_T)Items * Size;

    if (Size && Bytes / Size != Items)
        return Z_NULL;

    return CmpAllocate(Opaque, Bytes);
}

static
VOID
CmpZlibFree(
    _In_ voidpf Opaque,
    _In_ voidpf Address)
{
    CmpFree(Opaque, Address);
}

static
VOID
CmpPutUlong(
    _Out_writes_bytes_(4) PBYTE Out,
    _In_ ULONG Value)
{
    Out[0] = (BYTE)Value;
    Out[1] = (BYTE)(Value >> 8);
    Out[2] = (BYTE)(Value >> 16);
    Out[3] = (BYTE)(Value >> 24);
}

static
ULONG
CmpGetUlong(
    _In_reads_bytes_(4) const BYTE *In)
{
    return In[0] | (In[1] << 8) | (In[2] << 16) | ((ULONG)In[3] << 24);
}

static
ULONG
CmpGetUshort(
    _In_reads_bytes_(2) const BYTE *In)
{
    return In[0] | (In[1] << 8);
}

/*
 * LZ77 match finder shared by both XPRESS encoders, hash chains over three
 * byte prefixes with a ring of previous positions as large as the window.
 */

static
ULONG
LzHash(
    _In_reads_bytes_(3) const BYTE *Data)
{
    ULONG Value = ((ULONG)Data[0] << 16) | ((ULONG)Data[1] << 8) | Data[2];

    return (Value * 0x9E3779B1) >> (32 - LZ_HASH_BITS);
}

static
VOID
LzReset(
    _Inout_ PLZ_STATE Lz,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size)
{
    Lz->Data = Data;
    Lz->Size = Size;
    memset(Lz->Head, 0xFF, LZ_HASH_SIZE * sizeof(*Lz->Head));
}

static
VOID
LzInsert(
    _Inout_ PLZ_STATE Lz,
    _In_ ULONG Position)
{
    ULONG Hash;

    if (Lz->Size - Position < LZ_MIN_MATCH)
        return;

    Hash = LzHash(&Lz->Data[Position]);
    Lz->Prev[Position & Lz->WindowMask] = Lz->Head[Hash];
    Lz->Head[Hash] = (LONG)Position;
}

/**
 * @brief Finds the longest earlier match for the data at Position.
 * Must run before Position itself is inserted.
 *
 * @return The match length, or 0 when no match of LZ_MIN_MATCH bytes exists.
 */
static
ULONG
LzFindMatch(
    _In_ PLZ_STATE Lz,
    _In_ ULONG Position,
    _Out_ PULONG Offset)
{
    const BYTE *Current = &Lz->Data[Position];
    ULONG Available = Lz->Size - Position;
    ULONG Depth = Lz->ChainDepth;
    ULONG Best = 0;
    ULONG Length;
    LONG Candidate;
    LONG Previous;

    *Offset = 0;
    if (Available < LZ_MIN_MATCH)
        return 0;

    Candidate = Lz->Head[LzHash(Current)];
    while (Candidate >= 0 && Depth--)
    {
        const BYTE *Match = &Lz->Data[Candidate];

        if (Position - (ULONG)Candidate > Lz->MaxOffset)
            break;

        if (Match[Best] == Current[Best] && Match[0] == Current[0] &&
            Match[1] == Current[1] && Match[2] == Current[2])
        {
            Length = LZ_MIN_MATCH;
            while (Length < Available && Match[Length] == Current[Length])
                Length++;

            if (Length > Best)
            {
                Best = Length;
                *Offset = Position - (ULONG)Candidate;
                if (Length == Available)
                    break;
            }
        }

        Previous = Lz->Prev[Candidate & Lz->WindowMask];
        if (Previous >= Candidate)
            break;

        Candidate = Previous;
    }

    return (Best >= LZ_MIN_MATCH) ? Best : 0;
}

static
VOID
LzInsertRange(
    _Inout_ PLZ_STATE Lz,
    _In_ ULONG Position,
    _In_ ULONG Length)
{
    while (Length--)
        LzInsert(Lz, Position++);
}

/*
 * Plain XPRESS (LZ77): 32-bit flag words, one bit per token from the top,
 * literals as bytes and matches as 16-bit offset and length pairs. Long
 * lengths continue in nibbles shared by two matches, then bytes.
 */

/**
 * @brief Encodes Lz->Data, failing when the output does not fit in OutSize.
 */
static
BOOL
XpressEncode(
    _Inout_ PLZ_STATE Lz,
    _Out_writes_bytes_to_(OutSize, *Written) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Written)
{
    SIZE_T OutPos;
    SIZE_T FlagPos = 0;
    SIZE_T NibblePos = (SIZE_T)-1;
    ULONG Flags = 0;
    ULONG FlagCount = 0;
    ULONG Position = 0;
    ULONG Length;
    ULONG Offset;
    ULONG Extra;

    *Written = 0;
    if (OutSize < sizeof(ULONG))
        return FALSE;

    OutPos = sizeof(ULONG);
    while (Position < Lz->Size)
    {
        Length = LzFindMatch(Lz, Position, &Offset);
        if (Length)
        {
            /* Room for the longest form: pair, nibble, byte and two lengths */
            if (OutSize - OutPos < 2 + 1 + 1 + 2 + 4)
                return FALSE;

            Extra = Length - LZ_MIN_MATCH;
            Out[OutPos++] = (BYTE)(((Offset - 1) << 3) | min(Extra, 7));
            Out[OutPos++] = (BYTE)((Offset - 1) >> 5);
            if (Extra >= 7)
            {
                ULONG Nibble = min(Extra - 7, 15);

                if (NibblePos == (SIZE_T)-1)
                {
                    NibblePos = OutPos;
                    Out[OutPos++] = (BYTE)Nibble;
                }
                else
                {
                    Out[NibblePos] |= (BYTE)(Nibble << 4);
                    NibblePos = (SIZE_T)-1;
                }

                if (Nibble == 15)
                {
                    if (Extra - 22 < 255)
                    {
                        Out[OutPos++] = (BYTE)(Extra - 22);
                    }
                    else
                    {
                        Out[OutPos++] = 255;
                        if (Extra <= 0xFFFF)
                        {
                            Out[OutPos++] = (BYTE)Extra;
                            Out[OutPos++] = (BYTE)(Extra >> 8);
                        }
                        else
                        {
                            Out[OutPos++] = 0;
                            Out[OutPos++] = 0;
                            CmpPutUlong(&Out[OutPos], Extra);
                            OutPos += sizeof(ULONG);
                        }
                    }
                }
            }

            Flags = (Flags << 1) | 1;
            LzInsertRange(Lz, Position, Length);
            Position += Length;
        }
        else
        {
            if (OutPos >= OutSize)
                return FALSE;

            Out[OutPos++] = Lz->Data[Position];
            Flags <<= 1;
            LzInsert(Lz, Position);
            Position++;
        }

        if (++FlagCount == 32)
        {
            CmpPutUlong(&Out[FlagPos], Flags);
            if (OutSize - OutPos < sizeof(ULONG))
                return FALSE;

            FlagPos = OutPos;
            OutPos += sizeof(ULONG);
            Flags = 0;
            FlagCount = 0;
        }
    }

    /* The unused flag bits read as matches with no input left, which ends the stream */
    if (FlagCount)
        Flags = (Flags << (32 - FlagCount)) | ((1UL << (32 - FlagCount)) - 1);
    else
        Flags = 0xFFFFFFFF;

    CmpPutUlong(&Out[FlagPos], Flags);
    *Written = OutPos;
    return TRUE;
}

static
ULONG
XpressDecode(
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_to_(OutSize, *Produced) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Produced)
{
    SIZE_T InPos = 0;
    SIZE_T OutPos = 0;
    SIZE_T NibblePos = (SIZE_T)-1;
    ULONG Flags = 0;
    ULONG FlagCount = 0;
    SIZE_T Length;
    SIZE_T Offset;
    ULONG MatchBytes;

    *Produced = 0;
    for (;;)
    {
        if (!FlagCount)
        {
            if (InSize - InPos < sizeof(ULONG))
                break;

            Flags = CmpGetUlong(&In[InPos]);
            InPos += sizeof(ULONG);
            FlagCount = 32;
        }

        FlagCount--;
        if (!(Flags & (1UL << FlagCount)))
        {
            if (InPos >= InSize)
                break;

            if (OutPos >= OutSize)
                return ERROR_BAD_COMPRESSION_BUFFER;

            Out[OutPos++] = In[InPos++];
            continue;
        }

        /* A match flag with nothing left to read is the end marker */
        if (InPos == InSize)
            break;

        if (InSize - InPos < 2)
            return ERROR_BAD_COMPRESSION_BUFFER;

        MatchBytes = CmpGetUshort(&In[InPos]);
        InPos += 2;
        Length = MatchBytes & 7;
        Offset = (MatchBytes >> 3) + 1;
        if (Length == 7)
        {
            if (NibblePos == (SIZE_T)-1)
            {
                if (InPos >= InSize)
                    return ERROR_BAD_COMPRESSION_BUFFER;

                NibblePos = InPos;
                Length = In[InPos++] & 15;
            }
            else
            {
                Length = In[NibblePos] >> 4;
                NibblePos = (SIZE_T)-1;
            }

            if (Length == 15)
            {
                if (InPos >= InSize)
                    return ERROR_BAD_COMPRESSION_BUFFER;

                Length = In[InPos++];
                if (Length == 255)
                {
                    if (InSize - InPos < 2)
                        return ERROR_BAD_COMPRESSION_BUFFER;

                    Length = CmpGetUshort(&In[InPos]);
                    InPos += 2;
                    if (!Length)
                    {
                        if (InSize - InPos < sizeof(ULONG))
                            return ERROR_BAD_COMPRESSION_BUFFER;

                        Length = CmpGetUlong(&In[InPos]);
                        InPos += sizeof(ULONG);
                    }

                    if (Length < 22)
                        return ERROR_BAD_COMPRESSION_BUFFER;

                    Length -= 22;
                }

                Length += 15;
            }

            Length += 7;
        }

        Length += LZ_MIN_MATCH;
        if (Offset > OutPos || Length > OutSize - OutPos)
            return ERROR_BAD_COMPRESSION_BUFFER;

        while (Length--)
        {
            Out[OutPos] = Out[OutPos - Offset];
            OutPos++;
        }
    }

    /* The stream ended, the output must be exactly full */
    *Produced = OutPos;
    return (OutPos == OutSize) ? ERROR_SUCCESS : ERROR_BAD_COMPRESSION_BUFFER;
}

/*
 * XPRESS Huffman: chunks covering 64K of output, each starting with 512
 * four-bit code lengths. Symbols 0-255 are literals, 256-511 matches with
 * the length in the low nibble and the offset bit count in the high one.
 */

static
ULONG
HuffHighBit(
    _In_ ULONG Value)
{
    ULONG Bit = 0;

    while (Value >>= 1)
        Bit++;

    return Bit;
}

static
int
__cdecl
HuffCompareNodes(
    _In_ const void *First,
    _In_ const void *Second)
{
    const HUFF_NODE *A = First;
    const HUFF_NODE *B = Second;

    if (A->Weight != B->Weight)
        return (A->Weight < B->Weight) ? -1 : 1;

    return (int)A->Symbol - (int)B->Symbol;
}

/**
 * @brief Builds code lengths of at most HUFF_MAX_CODE_BITS from symbol
 * frequencies. Frequencies are flattened until the limit is met.
 */
static
VOID
HuffBuildLengths(
    _Inout_updates_(HUFF_SYMBOLS) PULONG Frequency,
    _Out_writes_(HUFF_SYMBOLS) PBYTE Lengths)
{
    HUFF_NODE Nodes[2 * HUFF_SYMBOLS];
    USHORT Depth[2 * HUFF_SYMBOLS];
    ULONG Leaves;
    ULONG Next;
    ULONG LeafPos;
    ULONG NodePos;
    ULONG Pick[2];
    ULONG Index;
    ULONG Longest;

    for (;;)
    {
        Leaves = 0;
        for (Index = 0; Index < HUFF_SYMBOLS; Index++)
        {
            Lengths[Index] = 0;
            if (Frequency[Index])
            {
                Nodes[Leaves].Weight = Frequency[Index];
                Nodes[Leaves].Symbol = (USHORT)Index;
                Leaves++;
            }
        }

        qsort(Nodes, Leaves, sizeof(Nodes[0]), HuffCompareNodes);

        /* Two queues: sorted leaves and internal nodes made in weight order */
        LeafPos = 0;
        NodePos = Leaves;
        for (Next = Leaves; Next < 2 * Leaves - 1; Next++)
        {
            for (Index = 0; Index < 2; Index++)
            {
                if (LeafPos < Leaves &&
                    (NodePos >= Next || Nodes[LeafPos].Weight <= Nodes[NodePos].Weight))
                {
                    Pick[Index] = LeafPos++;
                }
                else
                {
                    Pick[Index] = NodePos++;
                }
            }

            Nodes[Next].Weight = Nodes[Pick[0]].Weight + Nodes[Pick[1]].Weight;
            Nodes[Pick[0]].Parent = (USHORT)Next;
            Nodes[Pick[1]].Parent = (USHORT)Next;
        }

        Depth[2 * Leaves - 2] = 0;
        Longest = 0;
        for (Index = 2 * Leaves - 2; Index-- > 0;)
        {
            Depth[Index] = Depth[Nodes[Index].Parent] + 1;
            if (Index < Leaves)
            {
                Lengths[Nodes[Index].Symbol] = (BYTE)min(Depth[Index], 0xFF);
                Longest = max(Longest, Depth[Index]);
            }
        }

        if (Longest <= HUFF_MAX_CODE_BITS)
            return;

        for (Index = 0; Index < HUFF_SYMBOLS; Index++)
        {
            if (Frequency[Index])
                Frequency[Index] = (Frequency[Index] >> 1) | 1;
        }
    }
}

/**
 * @brief Assigns canonical codes: shorter codes first, equal lengths by symbol.
 */
static
VOID
HuffAssignCodes(
    _In_reads_(HUFF_SYMBOLS) const BYTE *Lengths,
    _Out_writes_(HUFF_SYMBOLS) PUSHORT Codes)
{
    ULONG Count[HUFF_MAX_CODE_BITS + 1] = { 0 };
    ULONG NextCode[HUFF_MAX_CODE_BITS + 1];
    ULONG Code = 0;
    ULONG Bits;
    ULONG Index;

    for (Index = 0; Index < HUFF_SYMBOLS; Index++)
        Count[Lengths[Index]]++;

    Count[0] = 0;
    for (Bits = 1; Bits <= HUFF_MAX_CODE_BITS; Bits++)
    {
        Code = (Code + Count[Bits - 1]) << 1;
        NextCode[Bits] = Code;
    }

    for (Index = 0; Index < HUFF_SYMBOLS; Index++)
    {
        if (Lengths[Index])
            Codes[Index] = (USHORT)NextCode[Lengths[Index]]++;
    }
}

static
VOID
HuffPutWord(
    _Inout_ PHUFF_WRITER Writer,
    _In_ SIZE_T Position,
    _In_ ULONG Word)
{
    Writer->Out[Position] = (BYTE)Word;
    Writer->Out[Position + 1] = (BYTE)(Word >> 8);
}

/*
 * The decoder keeps two words of look-ahead, so a finished word goes to the
 * oldest reserved slot and loose bytes land after the reserved slots.
 */
static
VOID
HuffPutBits(
    _Inout_ PHUFF_WRITER Writer,
    _In_ ULONG Bits,
    _In_ ULONG Count)
{
    Writer->Buffer = (Writer->Buffer << Count) | Bits;
    Writer->Count += Count;
    if (Writer->Count <= 16)
        return;

    Writer->Count -= 16;
    if (Writer->OutSize - Writer->Next < 2)
    {
        Writer->Overflow = TRUE;
        return;
    }

    HuffPutWord(Writer, Writer->Seq1, (Writer->Buffer >> Writer->Count) & 0xFFFF);
    Writer->Seq1 = Writer->Seq2;
    Writer->Seq2 = Writer->Next;
    Writer->Next += 2;
}

static
VOID
HuffPutByte(
    _Inout_ PHUFF_WRITER Writer,
    _In_ ULONG Value)
{
    if (Writer->Next >= Writer->OutSize)
    {
        Writer->Overflow = TRUE;
        return;
    }

    Writer->Out[Writer->Next++] = (BYTE)Value;
}

/**
 * @brief Encodes one chunk worth of tokens behind its code length table.
 */
static
BOOL
HuffEncodeChunk(
    _In_reads_(TokenCount) const HUFF_TOKEN *Tokens,
    _In_ ULONG TokenCount,
    _In_ BOOL EndMarker,
    _Out_writes_bytes_(OutSize) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Inout_ PSIZE_T OutPos)
{
    ULONG Frequency[HUFF_SYMBOLS] = { 0 };
    BYTE Lengths[HUFF_SYMBOLS];
    USHORT Codes[HUFF_SYMBOLS];
    HUFF_WRITER Writer;
    ULONG Symbol;
    ULONG Extra;
    ULONG OffsetBits;
    ULONG Used = 0;
    ULONG Index;

    for (Index = 0; Index < TokenCount; Index++)
    {
        if (Tokens[Index].Length)
        {
            Extra = Tokens[Index].Length - LZ_MIN_MATCH;
            Symbol = 256 + min(Extra, 15) + (HuffHighBit(Tokens[Index].Value) << 4);
        }
        else
        {
            Symbol = Tokens[Index].Value;
        }
        Frequency[Symbol]++;
    }

    if (EndMarker)
        Frequency[HUFF_EOF_SYMBOL]++;

    /* A prefix code needs two symbols, add an unused one if needed */
    for (Index = 0; Index < HUFF_SYMBOLS; Index++)
        Used += !!Frequency[Index];

    for (Index = 0; Used < 2; Index++)
    {
        if (!Frequency[Index])
        {
            Frequency[Index] = 1;
            Used++;
        }
    }

    HuffBuildLengths(Frequency, Lengths);
    HuffAssignCodes(Lengths, Codes);

    if (OutSize - *OutPos < HUFF_TABLE_BYTES + 4)
        return FALSE;

    for (Index = 0; Index < HUFF_TABLE_BYTES; Index++)
        Out[*OutPos + Index] = Lengths[2 * Index] | (Lengths[2 * Index + 1] << 4);

    Writer.Out = Out;
    Writer.OutSize = OutSize;
    Writer.Seq1 = *OutPos + HUFF_TABLE_BYTES;
    Writer.Seq2 = Writer.Seq1 + 2;
    Writer.Next = Writer.Seq2 + 2;
    Writer.Buffer = 0;
    Writer.Count = 0;
    Writer.Overflow = FALSE;

    for (Index = 0; Index < TokenCount && !Writer.Overflow; Index++)
    {
        if (!Tokens[Index].Length)
        {
            Symbol = Tokens[Index].Value;
            HuffPutBits(&Writer, Codes[Symbol], Lengths[Symbol]);
            continue;
        }

        Extra = Tokens[Index].Length - LZ_MIN_MATCH;
        OffsetBits = HuffHighBit(Tokens[Index].Value);
        Symbol = 256 + min(Extra, 15) + (OffsetBits << 4);
        HuffPutBits(&Writer, Codes[Symbol], Lengths[Symbol]);

        if (Extra >= 15)
        {
            if (Extra - 15 < 255)
            {
                HuffPutByte(&Writer, Extra - 15);
            }
            else
            {
                HuffPutByte(&Writer, 255);
                if (Extra <= 0xFFFF)
                {
                    HuffPutByte(&Writer, Extra);
                    HuffPutByte(&Writer, Extra >> 8);
                }
                else
                {
                    HuffPutByte(&Writer, 0);
                    HuffPutByte(&Writer, 0);
                    HuffPutByte(&Writer, Extra);
                    HuffPutByte(&Writer, Extra >> 8);
                    HuffPutByte(&Writer, Extra >> 16);
                    HuffPutByte(&Writer, Extra >> 24);
                }
            }
        }

        if (OffsetBits)
            HuffPutBits(&Writer, Tokens[Index].Value - (1UL << OffsetBits), OffsetBits);
    }

    if (EndMarker)
        HuffPutBits(&Writer, Codes[HUFF_EOF_SYMBOL], Lengths[HUFF_EOF_SYMBOL]);

    if (Writer.Overflow || Writer.Next > OutSize)
        return FALSE;

    /* Pending bits fill the oldest slot, the other one is padding */
    HuffPutWord(&Writer, Writer.Seq1, (Writer.Buffer << (16 - Writer.Count)) & 0xFFFF);
    HuffPutWord(&Writer, Writer.Seq2, 0);
    *OutPos = Writer.Next;
    return TRUE;
}

static
BOOL
HuffEncode(
    _Inout_ PLZ_WORKSPACE Work,
    _Out_writes_bytes_to_(OutSize, *Written) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Written)
{
    PLZ_STATE Lz = &Work->Lz;
    ULONG Position = 0;
    ULONG ChunkStart;
    ULONG TokenCount;
    ULONG Length;
    ULONG Offset;
    SIZE_T OutPos = 0;

    *Written = 0;
    while (Position < Lz->Size)
    {
        /* A chunk ends after the token that reaches 64K of output */
        ChunkStart = Position;
        TokenCount = 0;
        while (Position < Lz->Size && Position - ChunkStart < HUFF_CHUNK_SIZE)
        {
            Length = LzFindMatch(Lz, Position, &Offset);
            if (Length)
            {
                Work->Tokens[TokenCount].Length = Length;
                Work->Tokens[TokenCount].Value = Offset;
                LzInsertRange(Lz, Position, Length);
                Position += Length;
            }
            else
            {
                Work->Tokens[TokenCount].Length = 0;
                Work->Tokens[TokenCount].Value = Lz->Data[Position];
                LzInsert(Lz, Position);
                Position++;
            }
            TokenCount++;
        }

        /* The decoder only needs an end marker when the last chunk is short */
        if (!HuffEncodeChunk(Work->Tokens,
                             TokenCount,
                             Position >= Lz->Size && Position - ChunkStart < HUFF_CHUNK_SIZE,
                             Out,
                             OutSize,
                             &OutPos))
        {
            return FALSE;
        }
    }

    *Written = OutPos;
    return TRUE;
}

static
ULONG
HuffBuildDecodeTable(
    _In_reads_bytes_(HUFF_TABLE_BYTES) const BYTE *Table,
    _Out_writes_(1 << HUFF_MAX_CODE_BITS) PUSHORT Decode)
{
    BYTE Lengths[HUFF_SYMBOLS];
    USHORT Codes[HUFF_SYMBOLS];
    ULONG Space = 0;
    ULONG Index;
    ULONG First;
    ULONG Span;

    for (Index = 0; Index < HUFF_TABLE_BYTES; Index++)
    {
        Lengths[2 * Index] = Table[Index] & 15;
        Lengths[2 * Index + 1] = Table[Index] >> 4;
    }

    for (Index = 0; Index < HUFF_SYMBOLS; Index++)
    {
        if (Lengths[Index])
            Space += 1UL << (HUFF_MAX_CODE_BITS - Lengths[Index]);
    }

    if (!Space || Space > (1UL << HUFF_MAX_CODE_BITS))
        return ERROR_BAD_COMPRESSION_BUFFER;

    memset(Decode, 0, sizeof(USHORT) << HUFF_MAX_CODE_BITS);
    HuffAssignCodes(Lengths, Codes);
    for (Index = 0; Index < HUFF_SYMBOLS; Index++)
    {
        if (!Lengths[Index])
            continue;

        First = (ULONG)Codes[Index] << (HUFF_MAX_CODE_BITS - Lengths[Index]);
        Span = 1UL << (HUFF_MAX_CODE_BITS - Lengths[Index]);
        while (Span--)
            Decode[First + Span] = (USHORT)((Index << 4) | Lengths[Index]);
    }

    return ERROR_SUCCESS;
}

/**
 * @brief Decodes into Out until it is full. A stream that ends early fails.
 */
static
ULONG
HuffDecode(
    _In_ PCMP_HANDLE Handle,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_to_(OutSize, *Produced) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Produced)
{
    PUSHORT Decode;
    SIZE_T InPos = 0;
    SIZE_T OutPos = 0;
    SIZE_T BlockEnd;
    SIZE_T Length;
    SIZE_T Offset;
    ULONG NextBits;
    LONG ExtraBits;
    ULONG Entry;
    ULONG Symbol;
    ULONG OffsetBits;
    ULONG Status = ERROR_SUCCESS;

/* Reads past the end yield zeros, at most the two look-ahead words */
#define HUFF_READ16() \
    ((InPos += 2) <= InSize ? CmpGetUshort(&In[InPos - 2]) : \
     (InPos - InSize > 4 ? (Status = ERROR_BAD_COMPRESSION_BUFFER, 0) : 0))

#define HUFF_CONSUME(Bits) \
    do { \
        NextBits <<= (Bits); \
        ExtraBits -= (LONG)(Bits); \
        if (ExtraBits < 0) \
        { \
            NextBits |= HUFF_READ16() << (-ExtraBits); \
            ExtraBits += 16; \
        } \
    } while (0)

    *Produced = 0;
    Decode = CmpAllocate(Handle, sizeof(USHORT) << HUFF_MAX_CODE_BITS);
    if (!Decode)
        return ERROR_OUTOFMEMORY;

    while (OutPos < OutSize && Status == ERROR_SUCCESS)
    {
        if (InSize - InPos < HUFF_TABLE_BYTES || InPos > InSize)
        {
            Status = ERROR_BAD_COMPRESSION_BUFFER;
            break;
        }

        Status = HuffBuildDecodeTable(&In[InPos], Decode);
        if (Status != ERROR_SUCCESS)
            break;

        InPos += HUFF_TABLE_BYTES;
        NextBits = HUFF_READ16() << 16;
        NextBits |= HUFF_READ16();
        ExtraBits = 16;
        BlockEnd = OutPos + HUFF_CHUNK_SIZE;

        while (OutPos < BlockEnd && OutPos < OutSize && Status == ERROR_SUCCESS)
        {
            Entry = Decode[NextBits >> (32 - HUFF_MAX_CODE_BITS)];
            if (!(Entry & 15))
            {
                Status = ERROR_BAD_COMPRESSION_BUFFER;
                break;
            }

            Symbol = Entry >> 4;
            HUFF_CONSUME(Entry & 15);
            if (Symbol < 256)
            {
                Out[OutPos++] = (BYTE)Symbol;
                continue;
            }

            /* The end marker only counts once all input has been read */
            if (Symbol == HUFF_EOF_SYMBOL && InPos >= InSize)
            {
                Status = ERROR_BAD_COMPRESSION_BUFFER;
                break;
            }

            Symbol -= 256;
            Length = Symbol & 15;
            OffsetBits = Symbol >> 4;
            if (Length == 15)
            {
                if (InPos >= InSize)
                {
                    Status = ERROR_BAD_COMPRESSION_BUFFER;
                    break;
                }

                Length = In[InPos++];
                if (Length == 255)
                {
                    if (InSize - InPos < 2 || InPos > InSize)
                    {
                        Status = ERROR_BAD_COMPRESSION_BUFFER;
                        break;
                    }

                    Length = CmpGetUshort(&In[InPos]);
                    InPos += 2;
                    if (!Length)
                    {
                        if (InSize - InPos < sizeof(ULONG) || InPos > InSize)
                        {
                            Status = ERROR_BAD_COMPRESSION_BUFFER;
                            break;
                        }

                        Length = CmpGetUlong(&In[InPos]);
                        InPos += sizeof(ULONG);
                    }

                    if (Length < 15)
                    {
                        Status = ERROR_BAD_COMPRESSION_BUFFER;
                        break;
                    }

                    Length -= 15;
                }

                Length += 15;
            }

            Length += LZ_MIN_MATCH;
            Offset = OffsetBits ? (NextBits >> (32 - OffsetBits)) : 0;
            Offset += (SIZE_T)1 << OffsetBits;
            HUFF_CONSUME(OffsetBits);

            if (Offset > OutPos)
            {
                Status = ERROR_BAD_COMPRESSION_BUFFER;
                break;
            }

            /* A match cannot run past the output buffer */
            if (Length > OutSize - OutPos)
            {
                Status = ERROR_BAD_COMPRESSION_BUFFER;
                break;
            }

            while (Length--)
            {
                Out[OutPos] = Out[OutPos - Offset];
                OutPos++;
            }
        }
    }

#undef HUFF_CONSUME
#undef HUFF_READ16

    CmpFree(Handle, Decode);

    /* Output that fills up with more than the end marker word unread was too small */
    if (Status == ERROR_SUCCESS && InPos < InSize && InSize - InPos > 2)
        Status = ERROR_BAD_COMPRESSION_BUFFER;

    if (Status == ERROR_SUCCESS)
        *Produced = OutPos;

    return Status;
}

/*
 * MSZIP: "CK" and a deflate stream per block of at most 32K. Blocks of one
 * buffer use the previous block as their preset dictionary.
 */

static
ULONG
MszipEncodeBlock(
    _Inout_ z_stream *Stream,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _In_reads_bytes_opt_(DictionarySize) const BYTE *Dictionary,
    _In_ SIZE_T DictionarySize,
    _Out_writes_bytes_to_(OutSize, *Written) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Written)
{
    *Written = 0;
    if (OutSize < MSZIP_SIGNATURE_SIZE)
        return ERROR_INSUFFICIENT_BUFFER;

    if (deflateReset(Stream) != Z_OK)
        return ERROR_INTERNAL_ERROR;

    if (Dictionary && deflateSetDictionary(Stream, Dictionary, (uInt)DictionarySize) != Z_OK)
        return ERROR_INTERNAL_ERROR;

    Out[0] = 'C';
    Out[1] = 'K';
    Stream->next_in = (Bytef *)In;
    Stream->avail_in = (uInt)InSize;
    Stream->next_out = Out + MSZIP_SIGNATURE_SIZE;
    Stream->avail_out = (uInt)min(OutSize - MSZIP_SIGNATURE_SIZE, MAXDWORD);
    if (deflate(Stream, Z_FINISH) == Z_STREAM_END)
    {
        *Written = MSZIP_SIGNATURE_SIZE + Stream->total_out;
        return ERROR_SUCCESS;
    }

    /* Data that does not shrink goes out as one stored deflate block */
    if (InSize > 0xFFFF || OutSize - MSZIP_SIGNATURE_SIZE < InSize + 5)
        return ERROR_INSUFFICIENT_BUFFER;

    Out[2] = 0x01;
    Out[3] = (BYTE)InSize;
    Out[4] = (BYTE)(InSize >> 8);
    Out[5] = (BYTE)~InSize;
    Out[6] = (BYTE)(~InSize >> 8);
    memcpy(Out + 7, In, InSize);
    *Written = InSize + 7;
    return ERROR_SUCCESS;
}

/**
 * @brief Inflates one block into Out until the stream ends or Out is full.
 */
static
ULONG
MszipDecodeBlock(
    _Inout_ z_stream *Stream,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _In_reads_bytes_opt_(DictionarySize) const BYTE *Dictionary,
    _In_ SIZE_T DictionarySize,
    _Out_writes_bytes_to_(OutSize, *Produced) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Produced)
{
    int Result;

    *Produced = 0;
    if (InSize < MSZIP_SIGNATURE_SIZE || In[0] != 'C' || In[1] != 'K')
        return ERROR_BAD_COMPRESSION_BUFFER;

    if (inflateReset(Stream) != Z_OK)
        return ERROR_INTERNAL_ERROR;

    if (Dictionary && inflateSetDictionary(Stream, Dictionary, (uInt)DictionarySize) != Z_OK)
        return ERROR_INTERNAL_ERROR;

    Stream->next_in = (Bytef *)In + MSZIP_SIGNATURE_SIZE;
    Stream->avail_in = (uInt)(InSize - MSZIP_SIGNATURE_SIZE);
    Stream->next_out = Out;
    Stream->avail_out = (uInt)OutSize;
    Result = inflate(Stream, Z_FINISH);

    /* Output filled up before the stream ended: keep what fit */
    if (!Stream->avail_out && (Result == Z_STREAM_END || Result == Z_BUF_ERROR || Result == Z_OK))
    {
        *Produced = OutSize;
        return ERROR_SUCCESS;
    }

    /* Windows tells a stream one byte short apart from a shorter one */
    if (Result == Z_STREAM_END && Stream->avail_out > 1)
        return ERROR_ERRORS_ENCOUNTERED;

    return ERROR_BAD_COMPRESSION_BUFFER;
}

/*
 * Algorithm dispatch shared by raw calls and the blocks of a buffer.
 */

typedef struct _CMP_CODEC
{
    PCMP_HANDLE Handle;
    z_stream Stream;
    BOOL StreamReady;
    PLZ_WORKSPACE Work;
} CMP_CODEC, *PCMP_CODEC;

static
SIZE_T
CmpRawWorstCase(
    _In_ ULONG Algorithm,
    _In_ SIZE_T Size)
{
    /* The XPRESS formats fall back to storing what does not shrink */
    if (Algorithm == COMPRESS_ALGORITHM_MSZIP)
        return Size + MSZIP_WORST_EXTRA;

    return Size;
}

static
SIZE_T
CmpBufferMaxBlock(
    _In_ ULONG Algorithm)
{
    return (Algorithm == COMPRESS_ALGORITHM_MSZIP) ? CMP_MSZIP_MAX_BLOCK : CMP_BUFFER_MAX_BLOCK;
}

static
SIZE_T
CmpBufferWorstCase(
    _In_ ULONG Algorithm,
    _In_ SIZE_T Size)
{
    SIZE_T BlockSize = min(Size, CmpBufferMaxBlock(Algorithm));
    SIZE_T FullBlocks = BlockSize ? Size / BlockSize : 0;
    SIZE_T Remainder = BlockSize ? Size % BlockSize : 0;
    SIZE_T Total;

    Total = 32 + FullBlocks * (CmpRawWorstCase(Algorithm, BlockSize) + 8);
    if (Remainder)
        Total += CmpRawWorstCase(Algorithm, Remainder) + 8;

    return Total;
}

static
ULONG
CmpCodecInit(
    _Out_ PCMP_CODEC Codec,
    _In_ PCMP_HANDLE Handle,
    _In_ BOOL Compressing)
{
    PLZ_WORKSPACE Work;
    ULONG Window;
    int Result;

    memset(Codec, 0, sizeof(*Codec));
    Codec->Handle = Handle;

    if (Handle->Algorithm == COMPRESS_ALGORITHM_MSZIP)
    {
        Codec->Stream.zalloc = CmpZlibAllocate;
        Codec->Stream.zfree = CmpZlibFree;
        Codec->Stream.opaque = Handle;
        if (Compressing)
            Result = deflateInit2(&Codec->Stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 9, Z_DEFAULT_STRATEGY);
        else
            Result = inflateInit2(&Codec->Stream, -MAX_WBITS);

        if (Result != Z_OK)
            return ERROR_OUTOFMEMORY;

        Codec->StreamReady = TRUE;
        return ERROR_SUCCESS;
    }

    if (!Compressing ||
        (Handle->Algorithm != COMPRESS_ALGORITHM_XPRESS &&
         Handle->Algorithm != COMPRESS_ALGORITHM_XPRESS_HUFF))
    {
        return ERROR_SUCCESS;
    }

    Work = CmpAllocate(Handle, sizeof(*Work));
    if (!Work)
        return ERROR_OUTOFMEMORY;

    memset(Work, 0, sizeof(*Work));
    Codec->Work = Work;

    Window = (Handle->Algorithm == COMPRESS_ALGORITHM_XPRESS) ? XPRESS_WINDOW : HUFF_WINDOW;
    Work->Lz.WindowMask = Window - 1;
    Work->Lz.MaxOffset = (Handle->Algorithm == COMPRESS_ALGORITHM_XPRESS) ? XPRESS_MAX_OFFSET : HUFF_MAX_OFFSET;
    Work->Lz.ChainDepth = Handle->Level ? 256 : 32;
    Work->Lz.Head = CmpAllocate(Handle, LZ_HASH_SIZE * sizeof(LONG));
    Work->Lz.Prev = CmpAllocate(Handle, Window * sizeof(LONG));
    if (!Work->Lz.Head || !Work->Lz.Prev)
        return ERROR_OUTOFMEMORY;

    if (Handle->Algorithm == COMPRESS_ALGORITHM_XPRESS_HUFF)
    {
        Work->Tokens = CmpAllocate(Handle, HUFF_CHUNK_SIZE * sizeof(HUFF_TOKEN));
        if (!Work->Tokens)
            return ERROR_OUTOFMEMORY;
    }

    return ERROR_SUCCESS;
}

static
VOID
CmpCodecCleanup(
    _Inout_ PCMP_CODEC Codec,
    _In_ BOOL Compressing)
{
    PCMP_HANDLE Handle = Codec->Handle;

    if (Codec->StreamReady)
    {
        if (Compressing)
            deflateEnd(&Codec->Stream);
        else
            inflateEnd(&Codec->Stream);
    }

    if (Codec->Work)
    {
        CmpFree(Handle, Codec->Work->Lz.Head);
        CmpFree(Handle, Codec->Work->Lz.Prev);
        CmpFree(Handle, Codec->Work->Tokens);
        CmpFree(Handle, Codec->Work);
    }
}

/**
 * @brief Compresses one block. Fails with ERROR_INSUFFICIENT_BUFFER when
 * the result does not fit.
 */
static
ULONG
CmpEncodeBlock(
    _Inout_ PCMP_CODEC Codec,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _In_reads_bytes_opt_(DictionarySize) const BYTE *Dictionary,
    _In_ SIZE_T DictionarySize,
    _Out_writes_bytes_to_(OutSize, *Written) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Written)
{
    ULONG Algorithm = Codec->Handle->Algorithm;
    BOOL Packed = FALSE;

    *Written = 0;
    switch (Algorithm)
    {
        case COMPRESS_ALGORITHM_MSZIP:
            return MszipEncodeBlock(&Codec->Stream, In, InSize, Dictionary, DictionarySize, Out, OutSize, Written);

        case COMPRESS_ALGORITHM_XPRESS:
        case COMPRESS_ALGORITHM_XPRESS_HUFF:
            /* Only output smaller than the input is worth keeping */
            if (InSize > 1 && InSize <= MAXLONG)
            {
                LzReset(&Codec->Work->Lz, In, (ULONG)InSize);
                if (Algorithm == COMPRESS_ALGORITHM_XPRESS)
                    Packed = XpressEncode(&Codec->Work->Lz, Out, min(OutSize, InSize - 1), Written);
                else
                    Packed = HuffEncode(Codec->Work, Out, min(OutSize, InSize - 1), Written);
            }

            if (Packed)
                return ERROR_SUCCESS;

            break;
    }

    if (OutSize < InSize)
        return ERROR_INSUFFICIENT_BUFFER;

    memcpy(Out, In, InSize);
    *Written = InSize;
    return ERROR_SUCCESS;
}

/**
 * @brief Decompresses one block into Out.
 */
static
ULONG
CmpDecodeBlock(
    _Inout_ PCMP_CODEC Codec,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _In_reads_bytes_opt_(DictionarySize) const BYTE *Dictionary,
    _In_ SIZE_T DictionarySize,
    _Out_writes_bytes_to_(OutSize, *Produced) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Produced)
{
    ULONG Algorithm = Codec->Handle->Algorithm;

    *Produced = 0;
    if (Algorithm == COMPRESS_ALGORITHM_MSZIP)
        return MszipDecodeBlock(&Codec->Stream, In, InSize, Dictionary, DictionarySize, Out, OutSize, Produced);

    /* Equal sizes mean the block was stored, packed data is always smaller */
    if (InSize == OutSize)
    {
        memcpy(Out, In, InSize);
        *Produced = OutSize;
        return ERROR_SUCCESS;
    }

    if (InSize > OutSize)
        return ERROR_BAD_COMPRESSION_BUFFER;

    if (Algorithm == COMPRESS_ALGORITHM_XPRESS)
        return XpressDecode(In, InSize, Out, OutSize, Produced);

    if (Algorithm == COMPRESS_ALGORITHM_XPRESS_HUFF)
        return HuffDecode(Codec->Handle, In, InSize, Out, OutSize, Produced);

    return ERROR_BAD_COMPRESSION_BUFFER;
}

static
ULONG
CmpCompressRaw(
    _In_ PCMP_HANDLE Handle,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_opt_(OutSize) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Size)
{
    CMP_CODEC Codec;
    ULONG Status;

    *Size = CmpRawWorstCase(Handle->Algorithm, InSize);
    if (Handle->Algorithm == COMPRESS_ALGORITHM_MSZIP)
    {
        /* A raw MSZIP stream is a single block and needs worst case room */
        if (InSize > CMP_MSZIP_MAX_BLOCK)
        {
            *Size = CMP_MSZIP_MAX_BLOCK;
            return ERROR_BUFFER_OVERFLOW;
        }

        if (OutSize < *Size)
            return ERROR_INSUFFICIENT_BUFFER;
    }

    if (!Out || !OutSize)
        return ERROR_INSUFFICIENT_BUFFER;

    Status = CmpCodecInit(&Codec, Handle, TRUE);
    if (Status == ERROR_SUCCESS)
        Status = CmpEncodeBlock(&Codec, In, InSize, NULL, 0, Out, OutSize, Size);

    CmpCodecCleanup(&Codec, TRUE);
    if (Status == ERROR_INSUFFICIENT_BUFFER)
        *Size = CmpRawWorstCase(Handle->Algorithm, InSize);

    return Status;
}

static
UCHAR
CmpHeaderChecksum(
    _In_ const CMP_BUFFER_HEADER *Header,
    _In_ ULONG HeaderSize)
{
    const BYTE *Bytes = (const BYTE *)Header;
    uLong Crc;

    /* CRC-32 over the header without the checksum byte itself */
    Crc = crc32(0, Bytes, FIELD_OFFSET(CMP_BUFFER_HEADER, Checksum));
    Crc = crc32(Crc, Bytes + FIELD_OFFSET(CMP_BUFFER_HEADER, Algorithm),
                HeaderSize - FIELD_OFFSET(CMP_BUFFER_HEADER, Algorithm));
    return (UCHAR)Crc;
}

static
ULONG
CmpCompressBuffer(
    _In_ PCMP_HANDLE Handle,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_opt_(OutSize) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Size)
{
    CMP_BUFFER_HEADER Header;
    CMP_CODEC Codec;
    SIZE_T BlockSize = min(InSize, CmpBufferMaxBlock(Handle->Algorithm));
    SIZE_T Done = 0;
    SIZE_T OutPos = CMP_BUFFER_HEADER_SIZE;
    SIZE_T Length;
    SIZE_T Written;
    const BYTE *Previous = NULL;
    SIZE_T PreviousSize = 0;
    ULONG Status;

    *Size = CmpBufferWorstCase(Handle->Algorithm, InSize);
    if (!Out || OutSize < CMP_BUFFER_HEADER_SIZE)
        return ERROR_INSUFFICIENT_BUFFER;

    Status = CmpCodecInit(&Codec, Handle, TRUE);
    while (Status == ERROR_SUCCESS && Done < InSize)
    {
        Length = min(BlockSize, InSize - Done);
        if (OutSize - OutPos < sizeof(ULONG))
        {
            Status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }

        Status = CmpEncodeBlock(&Codec,
                                In + Done,
                                Length,
                                Previous,
                                PreviousSize,
                                Out + OutPos + sizeof(ULONG),
                                OutSize - OutPos - sizeof(ULONG),
                                &Written);
        if (Status != ERROR_SUCCESS)
            break;

        CmpPutUlong(Out + OutPos, (ULONG)Written);
        OutPos += sizeof(ULONG) + Written;
        Previous = In + Done;
        PreviousSize = Length;
        Done += Length;
    }

    CmpCodecCleanup(&Codec, TRUE);
    if (Status != ERROR_SUCCESS)
        return Status;

    Header.Signature = CMP_BUFFER_SIGNATURE;
    Header.HeaderSize = CMP_BUFFER_HEADER_SIZE;
    Header.Algorithm = (UCHAR)Handle->Algorithm;
    Header.UncompressedSize = InSize;
    Header.BlockSize = (ULONG)BlockSize;
    Header.Flags = 0;
    Header.Checksum = CmpHeaderChecksum(&Header, CMP_BUFFER_HEADER_SIZE);
    memcpy(Out, &Header, CMP_BUFFER_HEADER_SIZE);

    *Size = OutPos;
    return ERROR_SUCCESS;
}

/**
 * @brief Walks the blocks of a buffer, decoding them when Out is given.
 */
static
ULONG
CmpWalkBuffer(
    _Inout_ PCMP_CODEC Codec,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _In_ SIZE_T HeaderSize,
    _In_ BOOL SizedBlocks,
    _In_ SIZE_T BlockSize,
    _In_ SIZE_T Total,
    _Out_writes_bytes_opt_(Total) PBYTE Out,
    _Out_ PSIZE_T Counted)
{
    SIZE_T InPos = HeaderSize;
    SIZE_T Done = 0;
    SIZE_T Entry = SizedBlocks ? 2 * sizeof(ULONG) : sizeof(ULONG);
    SIZE_T Packed;
    SIZE_T Length;
    SIZE_T Produced;
    ULONG Status;

    *Counted = 0;
    for (;;)
    {
        if (!SizedBlocks && Done >= Total)
            break;

        if (InSize - InPos < Entry)
            return ERROR_BAD_COMPRESSION_BUFFER;

        Packed = CmpGetUlong(&In[InPos]);
        if (SizedBlocks)
        {
            Length = CmpGetUlong(&In[InPos + sizeof(ULONG)]);

            /* A zero entry ends a list of sized blocks */
            if (!Packed)
            {
                if (Length)
                    return ERROR_BAD_COMPRESSION_BUFFER;
                break;
            }

            if (!Length || Length > CMP_BUFFER_MAX_BLOCK)
                return ERROR_BAD_COMPRESSION_BUFFER;
        }
        else
        {
            if (!Packed)
                return ERROR_BAD_COMPRESSION_BUFFER;

            Length = min(BlockSize, Total - Done);
        }

        InPos += Entry;
        if (InSize - InPos < Packed)
            return ERROR_BAD_COMPRESSION_BUFFER;

        if (Out)
        {
            if (Total - Done < Length)
                return ERROR_BAD_COMPRESSION_BUFFER;

            Status = CmpDecodeBlock(Codec,
                                    In + InPos,
                                    Packed,
                                    Done ? Out + Done - min(Done, CMP_MSZIP_MAX_BLOCK) : NULL,
                                    min(Done, CMP_MSZIP_MAX_BLOCK),
                                    Out + Done,
                                    Length,
                                    &Produced);
            if (Status != ERROR_SUCCESS)
                return (Status == ERROR_OUTOFMEMORY) ? Status : ERROR_BAD_COMPRESSION_BUFFER;

            if (Produced != Length)
                return ERROR_BAD_COMPRESSION_BUFFER;
        }

        if (Done + Length < Done)
            return ERROR_BAD_COMPRESSION_BUFFER;

        Done += Length;
        InPos += Packed;
    }

    *Counted = Done;
    return ERROR_SUCCESS;
}

static
ULONG
CmpDecompressBuffer(
    _In_ PCMP_HANDLE Handle,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_opt_(OutSize) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Size)
{
    CMP_BUFFER_HEADER Header;
    CMP_CODEC Codec;
    BOOL SizedBlocks;
    SIZE_T Total;
    SIZE_T Counted;
    ULONG Status;

    *Size = 0;
    if (!InSize)
    {
        *Size = CmpBufferWorstCase(Handle->Algorithm, OutSize);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    if (InSize < CMP_BUFFER_HEADER_SIZE + sizeof(ULONG))
        return ERROR_BAD_COMPRESSION_BUFFER;

    memcpy(&Header, In, CMP_BUFFER_HEADER_SIZE);
    if (Header.Signature != CMP_BUFFER_SIGNATURE ||
        Header.HeaderSize < CMP_BUFFER_HEADER_SIZE ||
        InSize < (SIZE_T)Header.HeaderSize + sizeof(ULONG) ||
        (Header.Flags & 0xFFFF & ~CMP_BUFFER_SIZED_BLOCKS) ||
        Header.Algorithm != Handle->Algorithm ||
        Header.BlockSize > CMP_BUFFER_MAX_BLOCK ||
        Header.Checksum != CmpHeaderChecksum((const CMP_BUFFER_HEADER *)In, Header.HeaderSize))
    {
        return ERROR_BAD_COMPRESSION_BUFFER;
    }

    SizedBlocks = !!(Header.Flags & CMP_BUFFER_SIZED_BLOCKS);
    Status = CmpCodecInit(&Codec, Handle, FALSE);
    if (Status != ERROR_SUCCESS)
        goto Exit;

    if (SizedBlocks)
    {
        /* The total comes from the block list rather than the header */
        Status = CmpWalkBuffer(&Codec, In, InSize, Header.HeaderSize, TRUE, 0, (SIZE_T)-1, NULL, &Total);
        if (Status != ERROR_SUCCESS)
            goto Exit;
    }
    else
    {
        if (Header.UncompressedSize > (SIZE_T)-1)
        {
            Status = ERROR_BAD_COMPRESSION_BUFFER;
            goto Exit;
        }

        Total = (SIZE_T)Header.UncompressedSize;

        /* A block size under 32K only describes a single block buffer */
        if (Header.BlockSize < CMP_MSZIP_MAX_BLOCK && Total != Header.BlockSize)
        {
            Status = ERROR_BAD_COMPRESSION_BUFFER;
            goto Exit;
        }
    }

    if (!Total || (!SizedBlocks && !Header.BlockSize))
    {
        Status = ERROR_BAD_COMPRESSION_BUFFER;
        goto Exit;
    }

    if (!Out || OutSize < Total)
    {
        *Size = Total;
        Status = ERROR_INSUFFICIENT_BUFFER;
        goto Exit;
    }

    Status = CmpWalkBuffer(&Codec, In, InSize, Header.HeaderSize, SizedBlocks, Header.BlockSize, Total, Out, &Counted);
    if (Status == ERROR_SUCCESS)
        *Size = Counted;

Exit:
    CmpCodecCleanup(&Codec, FALSE);
    return Status;
}

static
ULONG
CmpDecompressRaw(
    _In_ PCMP_HANDLE Handle,
    _In_reads_bytes_(InSize) const BYTE *In,
    _In_ SIZE_T InSize,
    _Out_writes_bytes_(OutSize) PBYTE Out,
    _In_ SIZE_T OutSize,
    _Out_ PSIZE_T Size)
{
    CMP_CODEC Codec;
    ULONG Status;

    *Size = 0;
    if (!InSize)
    {
        *Size = CmpRawWorstCase(Handle->Algorithm, OutSize);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    Status = CmpCodecInit(&Codec, Handle, FALSE);
    if (Status == ERROR_SUCCESS)
        Status = CmpDecodeBlock(&Codec, In, InSize, NULL, 0, Out, OutSize, Size);

    CmpCodecCleanup(&Codec, FALSE);
    return Status;
}

static
BOOL
CmpCreate(
    _In_ DWORD Algorithm,
    _In_opt_ PCOMPRESS_ALLOCATION_ROUTINES AllocationRoutines,
    _Out_ PVOID *Handle,
    _In_ ULONG Signature)
{
    COMPRESS_ALLOCATION_ROUTINES Routines;
    PCMP_HANDLE Context;
    DWORD Kind = Algorithm & ~COMPRESS_RAW;

    if (!Handle || Kind < COMPRESS_ALGORITHM_NULL || Kind >= COMPRESS_ALGORITHM_MAX)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* There is no LZMS codec */
    if (Kind == COMPRESS_ALGORITHM_LZMS)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    if (AllocationRoutines)
    {
        if (!AllocationRoutines->Allocate || !AllocationRoutines->Free)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        Routines = *AllocationRoutines;
    }
    else
    {
        Routines.Allocate = CmpDefaultAllocate;
        Routines.Free = CmpDefaultFree;
        Routines.UserContext = NULL;
    }

    Context = Routines.Allocate(Routines.UserContext, sizeof(*Context));
    if (!Context)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    Context->Signature = Signature;
    Context->Algorithm = Kind;
    Context->Raw = !!(Algorithm & COMPRESS_RAW);
    Context->Level = 0;
    Context->Routines = Routines;
    *Handle = Context;
    return TRUE;
}

static
PCMP_HANDLE
CmpValidate(
    _In_opt_ PVOID Handle,
    _In_ ULONG Signature)
{
    PCMP_HANDLE Context = Handle;

    if (!Context || Context->Signature != Signature)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    return Context;
}

static
BOOL
CmpClose(
    _In_opt_ PVOID Handle,
    _In_ ULONG Signature)
{
    PCMP_HANDLE Context = CmpValidate(Handle, Signature);
    COMPRESS_ALLOCATION_ROUTINES Routines;

    if (!Context)
        return FALSE;

    Routines = Context->Routines;
    Context->Signature = 0;
    Routines.Free(Routines.UserContext, Context);
    return TRUE;
}

static
BOOL
CmpInformation(
    _In_opt_ PVOID Handle,
    _In_ ULONG Signature,
    _In_ COMPRESS_INFORMATION_CLASS InformationClass,
    _Inout_updates_bytes_(InformationSize) PVOID Information,
    _In_ SIZE_T InformationSize,
    _In_ BOOL Set)
{
    PCMP_HANDLE Context = CmpValidate(Handle, Signature);
    DWORD Level;

    if (!Context)
        return FALSE;

    /* Only the XPRESS encoders have a setting, their level */
    if (Signature != CMP_COMPRESSOR_SIGNATURE ||
        InformationClass != COMPRESS_INFORMATION_CLASS_LEVEL ||
        (Context->Algorithm != COMPRESS_ALGORITHM_XPRESS &&
         Context->Algorithm != COMPRESS_ALGORITHM_XPRESS_HUFF))
    {
        SetLastError(ERROR_UNSUPPORTED_TYPE);
        return FALSE;
    }

    if (InformationSize != sizeof(DWORD))
    {
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }

    if (!Set)
    {
        *(PDWORD)Information = Context->Level;
        return TRUE;
    }

    Level = *(const DWORD *)Information;
    if (Level > 1)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    Context->Level = Level;
    return TRUE;
}

/**
 * @brief Creates a compressor for one of the COMPRESS_ALGORITHM values,
 * optionally ORed with COMPRESS_RAW.
 */
BOOL
WINAPI
CreateCompressor(
    _In_ DWORD Algorithm,
    _In_opt_ PCOMPRESS_ALLOCATION_ROUTINES AllocationRoutines,
    _Out_ PCOMPRESSOR_HANDLE CompressorHandle)
{
    return CmpCreate(Algorithm, AllocationRoutines, CompressorHandle, CMP_COMPRESSOR_SIGNATURE);
}

BOOL
WINAPI
SetCompressorInformation(
    _In_ COMPRESSOR_HANDLE CompressorHandle,
    _In_ COMPRESS_INFORMATION_CLASS CompressInformationClass,
    _In_reads_bytes_(CompressInformationSize) LPCVOID CompressInformation,
    _In_ SIZE_T CompressInformationSize)
{
    return CmpInformation(CompressorHandle,
                          CMP_COMPRESSOR_SIGNATURE,
                          CompressInformationClass,
                          (PVOID)CompressInformation,
                          CompressInformationSize,
                          TRUE);
}

BOOL
WINAPI
QueryCompressorInformation(
    _In_ COMPRESSOR_HANDLE CompressorHandle,
    _In_ COMPRESS_INFORMATION_CLASS CompressInformationClass,
    _Out_writes_bytes_(CompressInformationSize) PVOID CompressInformation,
    _In_ SIZE_T CompressInformationSize)
{
    return CmpInformation(CompressorHandle,
                          CMP_COMPRESSOR_SIGNATURE,
                          CompressInformationClass,
                          CompressInformation,
                          CompressInformationSize,
                          FALSE);
}

/**
 * @brief Compresses a buffer in one call. With no output buffer, or one
 * that is too small, fails with ERROR_INSUFFICIENT_BUFFER and reports the
 * size that is always enough.
 */
BOOL
WINAPI
Compress(
    _In_ COMPRESSOR_HANDLE CompressorHandle,
    _In_reads_bytes_opt_(UncompressedDataSize) LPCVOID UncompressedData,
    _In_ SIZE_T UncompressedDataSize,
    _Out_writes_bytes_opt_(CompressedBufferSize) PVOID CompressedBuffer,
    _In_ SIZE_T CompressedBufferSize,
    _Out_ PSIZE_T CompressedDataSize)
{
    PCMP_HANDLE Context = CmpValidate(CompressorHandle, CMP_COMPRESSOR_SIGNATURE);
    SIZE_T Size = 0;
    ULONG Status;

    if (!Context)
        return FALSE;

    if (!UncompressedDataSize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (Context->Raw)
        Status = CmpCompressRaw(Context, UncompressedData, UncompressedDataSize, CompressedBuffer, CompressedBufferSize, &Size);
    else
        Status = CmpCompressBuffer(Context, UncompressedData, UncompressedDataSize, CompressedBuffer, CompressedBufferSize, &Size);

    if (CompressedDataSize &&
        (Status == ERROR_SUCCESS || Status == ERROR_INSUFFICIENT_BUFFER || Status == ERROR_BUFFER_OVERFLOW))
    {
        *CompressedDataSize = Size;
    }

    if (Status != ERROR_SUCCESS)
    {
        SetLastError(Status);
        return FALSE;
    }

    return TRUE;
}

BOOL
WINAPI
ResetCompressor(
    _In_ COMPRESSOR_HANDLE CompressorHandle)
{
    /* Every call starts from a clean state, there is nothing to drop */
    return CmpValidate(CompressorHandle, CMP_COMPRESSOR_SIGNATURE) != NULL;
}

BOOL
WINAPI
CloseCompressor(
    _In_ COMPRESSOR_HANDLE CompressorHandle)
{
    return CmpClose(CompressorHandle, CMP_COMPRESSOR_SIGNATURE);
}

/**
 * @brief Creates a decompressor for one of the COMPRESS_ALGORITHM values,
 * optionally ORed with COMPRESS_RAW.
 */
BOOL
WINAPI
CreateDecompressor(
    _In_ DWORD Algorithm,
    _In_opt_ PCOMPRESS_ALLOCATION_ROUTINES AllocationRoutines,
    _Out_ PDECOMPRESSOR_HANDLE DecompressorHandle)
{
    return CmpCreate(Algorithm, AllocationRoutines, DecompressorHandle, CMP_DECOMPRESSOR_SIGNATURE);
}

BOOL
WINAPI
SetDecompressorInformation(
    _In_ DECOMPRESSOR_HANDLE DecompressorHandle,
    _In_ COMPRESS_INFORMATION_CLASS CompressInformationClass,
    _In_reads_bytes_(CompressInformationSize) LPCVOID CompressInformation,
    _In_ SIZE_T CompressInformationSize)
{
    return CmpInformation(DecompressorHandle,
                          CMP_DECOMPRESSOR_SIGNATURE,
                          CompressInformationClass,
                          (PVOID)CompressInformation,
                          CompressInformationSize,
                          TRUE);
}

BOOL
WINAPI
QueryDecompressorInformation(
    _In_ DECOMPRESSOR_HANDLE DecompressorHandle,
    _In_ COMPRESS_INFORMATION_CLASS CompressInformationClass,
    _Out_writes_bytes_(CompressInformationSize) PVOID CompressInformation,
    _In_ SIZE_T CompressInformationSize)
{
    return CmpInformation(DecompressorHandle,
                          CMP_DECOMPRESSOR_SIGNATURE,
                          CompressInformationClass,
                          CompressInformation,
                          CompressInformationSize,
                          FALSE);
}

/**
 * @brief Decompresses a buffer in one call. Buffers report their size when
 * the output is missing or short; raw data needs the exact output size.
 */
BOOL
WINAPI
Decompress(
    _In_ DECOMPRESSOR_HANDLE DecompressorHandle,
    _In_reads_bytes_opt_(CompressedDataSize) LPCVOID CompressedData,
    _In_ SIZE_T CompressedDataSize,
    _Out_writes_bytes_opt_(UncompressedBufferSize) PVOID UncompressedBuffer,
    _In_ SIZE_T UncompressedBufferSize,
    _Out_opt_ PSIZE_T UncompressedDataSize)
{
    PCMP_HANDLE Context = CmpValidate(DecompressorHandle, CMP_DECOMPRESSOR_SIGNATURE);
    SIZE_T Size = 0;
    ULONG Status;

    if (!Context)
        return FALSE;

    if (Context->Raw)
    {
        if (!UncompressedBuffer || !UncompressedBufferSize)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        Status = CmpDecompressRaw(Context, CompressedData, CompressedDataSize, UncompressedBuffer, UncompressedBufferSize, &Size);
    }
    else
    {
        Status = CmpDecompressBuffer(Context, CompressedData, CompressedDataSize, UncompressedBuffer, UncompressedBufferSize, &Size);
    }

    if (UncompressedDataSize && (Status == ERROR_SUCCESS || Status == ERROR_INSUFFICIENT_BUFFER))
        *UncompressedDataSize = Size;

    if (Status != ERROR_SUCCESS)
    {
        SetLastError(Status);
        return FALSE;
    }

    return TRUE;
}

BOOL
WINAPI
ResetDecompressor(
    _In_ DECOMPRESSOR_HANDLE DecompressorHandle)
{
    return CmpValidate(DecompressorHandle, CMP_DECOMPRESSOR_SIGNATURE) != NULL;
}

BOOL
WINAPI
CloseDecompressor(
    _In_ DECOMPRESSOR_HANDLE DecompressorHandle)
{
    return CmpClose(DecompressorHandle, CMP_DECOMPRESSOR_SIGNATURE);
}
