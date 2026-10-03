/*
 * PROJECT:     ReactOS Cabinet Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The buffer compression API that lives beside the cabinet one
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A caller picks an algorithm, hands over a buffer and gets the compressed form
 * back. Only COMPRESS_ALGORITHM_NULL, which stores the data as it is, is
 * supported here. The others are refused at creation rather than answered with
 * a stream no one else can read back.
 */

#include <windef.h>
#include <winbase.h>
#include <compressapi.h>
#include <reactos/debug.h>

#define COMPRESSOR_TAG      'pmoC'
#define DECOMPRESSOR_TAG    'pmcD'

typedef struct _COMPRESS_CONTEXT
{
    ULONG Tag;
    DWORD Algorithm;
    COMPRESS_ALLOCATION_ROUTINES Routines;
    BOOL HasRoutines;
} COMPRESS_CONTEXT, *PCOMPRESS_CONTEXT;

static
PVOID
CompressAllocate(
    _In_opt_ PCOMPRESS_ALLOCATION_ROUTINES Routines,
    _In_ SIZE_T Size)
{
    if (Routines != NULL)
        return Routines->Allocate(Routines->UserContext, Size);

    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

static
VOID
CompressFree(
    _In_ PCOMPRESS_CONTEXT Context)
{
    if (Context->HasRoutines)
    {
        Context->Routines.Free(Context->Routines.UserContext, Context);
        return;
    }

    HeapFree(GetProcessHeap(), 0, Context);
}

static
PCOMPRESS_CONTEXT
CompressCreate(
    _In_ ULONG Tag,
    _In_ DWORD Algorithm,
    _In_opt_ PCOMPRESS_ALLOCATION_ROUTINES Routines)
{
    PCOMPRESS_CONTEXT Context;

    Context = CompressAllocate(Routines, sizeof(*Context));
    if (Context == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    Context->Tag = Tag;
    Context->Algorithm = Algorithm;
    Context->HasRoutines = (Routines != NULL);
    if (Routines != NULL)
        Context->Routines = *Routines;

    return Context;
}

static
PCOMPRESS_CONTEXT
CompressContext(
    _In_ COMPRESSOR_HANDLE Handle,
    _In_ ULONG Tag)
{
    PCOMPRESS_CONTEXT Context = (PCOMPRESS_CONTEXT)Handle;

    if (Context == NULL || Context->Tag != Tag)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    return Context;
}

/* The algorithm is the low bits, COMPRESS_RAW only drops the format header */
static
BOOL
CompressAlgorithmSupported(
    _In_ DWORD Algorithm)
{
    DWORD Bare = Algorithm & ~COMPRESS_RAW;

    if (Bare == COMPRESS_ALGORITHM_INVALID || Bare >= COMPRESS_ALGORITHM_MAX)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (Bare != COMPRESS_ALGORITHM_NULL)
    {
        DPRINT1("Compression algorithm %lu is not implemented\n", Bare);
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    return TRUE;
}

/* Both directions move the bytes unchanged under COMPRESS_ALGORITHM_NULL */
static
BOOL
CompressCopy(
    _In_reads_bytes_opt_(InputSize) LPCVOID Input,
    _In_ SIZE_T InputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID Output,
    _In_ SIZE_T OutputSize,
    _Out_opt_ PSIZE_T OutputUsed)
{
    if (InputSize != 0 && Input == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (OutputUsed != NULL)
        *OutputUsed = InputSize;

    /* A NULL buffer is how a caller asks for the size it needs */
    if (Output == NULL || OutputSize < InputSize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    RtlCopyMemory(Output, Input, InputSize);
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CreateCompressor(
    DWORD Algorithm,
    PCOMPRESS_ALLOCATION_ROUTINES AllocationRoutines,
    PCOMPRESSOR_HANDLE CompressorHandle)
{
    PCOMPRESS_CONTEXT Context;

    if (CompressorHandle == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!CompressAlgorithmSupported(Algorithm))
        return FALSE;

    Context = CompressCreate(COMPRESSOR_TAG, Algorithm, AllocationRoutines);
    if (Context == NULL)
        return FALSE;

    *CompressorHandle = (COMPRESSOR_HANDLE)Context;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetCompressorInformation(
    COMPRESSOR_HANDLE CompressorHandle,
    COMPRESS_INFORMATION_CLASS CompressInformationClass,
    LPCVOID CompressInformation,
    SIZE_T CompressInformationSize)
{
    if (CompressContext(CompressorHandle, COMPRESSOR_TAG) == NULL)
        return FALSE;

    if (CompressInformation == NULL ||
        CompressInformationClass == COMPRESS_INFORMATION_CLASS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Stored data has neither a block size nor a level to set */
    UNREFERENCED_PARAMETER(CompressInformationSize);
    SetLastError(ERROR_UNSUPPORTED_TYPE);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
QueryCompressorInformation(
    COMPRESSOR_HANDLE CompressorHandle,
    COMPRESS_INFORMATION_CLASS CompressInformationClass,
    PVOID CompressInformation,
    SIZE_T CompressInformationSize)
{
    if (CompressContext(CompressorHandle, COMPRESSOR_TAG) == NULL)
        return FALSE;

    if (CompressInformation == NULL ||
        CompressInformationClass == COMPRESS_INFORMATION_CLASS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UNREFERENCED_PARAMETER(CompressInformationSize);
    SetLastError(ERROR_UNSUPPORTED_TYPE);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
Compress(
    COMPRESSOR_HANDLE CompressorHandle,
    LPCVOID UncompressedData,
    SIZE_T UncompressedDataSize,
    PVOID CompressedBuffer,
    SIZE_T CompressedBufferSize,
    PSIZE_T CompressedDataSize)
{
    if (CompressContext(CompressorHandle, COMPRESSOR_TAG) == NULL)
        return FALSE;

    if (CompressedDataSize == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return CompressCopy(UncompressedData,
                        UncompressedDataSize,
                        CompressedBuffer,
                        CompressedBufferSize,
                        CompressedDataSize);
}

/*
 * @implemented
 */
BOOL
WINAPI
ResetCompressor(
    COMPRESSOR_HANDLE CompressorHandle)
{
    /* Nothing carries over between calls, so there is no state to drop */
    return CompressContext(CompressorHandle, COMPRESSOR_TAG) != NULL;
}

/*
 * @implemented
 */
BOOL
WINAPI
CloseCompressor(
    COMPRESSOR_HANDLE CompressorHandle)
{
    PCOMPRESS_CONTEXT Context = CompressContext(CompressorHandle, COMPRESSOR_TAG);

    if (Context == NULL)
        return FALSE;

    CompressFree(Context);
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CreateDecompressor(
    DWORD Algorithm,
    PCOMPRESS_ALLOCATION_ROUTINES AllocationRoutines,
    PDECOMPRESSOR_HANDLE DecompressorHandle)
{
    PCOMPRESS_CONTEXT Context;

    if (DecompressorHandle == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!CompressAlgorithmSupported(Algorithm))
        return FALSE;

    Context = CompressCreate(DECOMPRESSOR_TAG, Algorithm, AllocationRoutines);
    if (Context == NULL)
        return FALSE;

    *DecompressorHandle = (DECOMPRESSOR_HANDLE)Context;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetDecompressorInformation(
    DECOMPRESSOR_HANDLE DecompressorHandle,
    COMPRESS_INFORMATION_CLASS CompressInformationClass,
    LPCVOID CompressInformation,
    SIZE_T CompressInformationSize)
{
    if (CompressContext(DecompressorHandle, DECOMPRESSOR_TAG) == NULL)
        return FALSE;

    if (CompressInformation == NULL ||
        CompressInformationClass == COMPRESS_INFORMATION_CLASS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UNREFERENCED_PARAMETER(CompressInformationSize);
    SetLastError(ERROR_UNSUPPORTED_TYPE);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
QueryDecompressorInformation(
    DECOMPRESSOR_HANDLE DecompressorHandle,
    COMPRESS_INFORMATION_CLASS CompressInformationClass,
    PVOID CompressInformation,
    SIZE_T CompressInformationSize)
{
    if (CompressContext(DecompressorHandle, DECOMPRESSOR_TAG) == NULL)
        return FALSE;

    if (CompressInformation == NULL ||
        CompressInformationClass == COMPRESS_INFORMATION_CLASS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UNREFERENCED_PARAMETER(CompressInformationSize);
    SetLastError(ERROR_UNSUPPORTED_TYPE);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
Decompress(
    DECOMPRESSOR_HANDLE DecompressorHandle,
    LPCVOID CompressedData,
    SIZE_T CompressedDataSize,
    PVOID UncompressedBuffer,
    SIZE_T UncompressedBufferSize,
    PSIZE_T UncompressedDataSize)
{
    if (CompressContext(DecompressorHandle, DECOMPRESSOR_TAG) == NULL)
        return FALSE;

    return CompressCopy(CompressedData,
                        CompressedDataSize,
                        UncompressedBuffer,
                        UncompressedBufferSize,
                        UncompressedDataSize);
}

/*
 * @implemented
 */
BOOL
WINAPI
ResetDecompressor(
    DECOMPRESSOR_HANDLE DecompressorHandle)
{
    return CompressContext(DecompressorHandle, DECOMPRESSOR_TAG) != NULL;
}

/*
 * @implemented
 */
BOOL
WINAPI
CloseDecompressor(
    DECOMPRESSOR_HANDLE DecompressorHandle)
{
    PCOMPRESS_CONTEXT Context = CompressContext(DecompressorHandle, DECOMPRESSOR_TAG);

    if (Context == NULL)
        return FALSE;

    CompressFree(Context);
    return TRUE;
}
