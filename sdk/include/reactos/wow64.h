/*
 * Wow64 helper utilities
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * FILE:            sdk/include/reactos/wow64.h
 * PROGRAMMER:      Marcin Jabłoński
 */

#pragma once

#define TEB64_TLS_OFFSET            0x1480
#define WOW64_TLS_FILESYSREDIR      8

#ifdef BUILD_WOW6432

/**********************************************************************
 * Exported functions 
 */
NTSYSAPI
NTSTATUS 
NTAPI
NtWow64ReadVirtualMemory64(HANDLE ProcessHandle,
                           UINT64 BaseAddress,
                           PVOID Buffer,
                           UINT64 BufferSize,
                           PUINT64 NumberOfBytesRead);

NTSYSAPI
NTSTATUS 
NTAPI
NtWow64WriteVirtualMemory64(HANDLE ProcessHandle,
                            UINT64 BaseAddress,
                            CONST VOID* Buffer,
                            UINT64 BufferSize,
                            PUINT64 NumberOfBytesWritten);

static 
inline 
WORD
Wow64ReadNativeWord(UINT64 Address)
{
    WORD Result;
    UINT64 BytesRead = 0;
    
    NTSTATUS Status = 
        NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                   Address,
                                   &Result,
                                   sizeof(Result),
                                   &BytesRead);

    if (!NT_SUCCESS(Status) || BytesRead != sizeof(Result))
    {
        /* TODO: Throw an access violation exception or something */
        __debugbreak();
        ASSERT(FALSE);
    }
    
    return Result;
}

static 
inline 
ULONG 
Wow64ReadNativeULong(UINT64 Address)
{
    ULONG Result;
    UINT64 BytesRead = 0;
    
    NTSTATUS Status = 
        NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                   Address,
                                   &Result,
                                   sizeof(Result),
                                   &BytesRead);

    if (!NT_SUCCESS(Status) || BytesRead != sizeof(Result))
    {
        __debugbreak();
        ASSERT(FALSE);
    }
    
    return Result;
}

static 
inline 
UINT64 
Wow64ReadNativePtr(UINT64 Address)
{
    UINT64 Result;
    UINT64 BytesRead = 0;
    
    NTSTATUS Status = 
        NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                   Address,
                                   &Result,
                                   sizeof(Result),
                                   &BytesRead);

    if (!NT_SUCCESS(Status) || BytesRead != sizeof(Result))
    {
        __debugbreak();
        ASSERT(FALSE);
    }
    
    return Result;
}

static 
inline 
void 
Wow64WriteNativeULong(UINT64 Address, ULONG Value)
{
    UINT64 BytesWritten = 0;
    
    NTSTATUS Status = 
        NtWow64WriteVirtualMemory64(NtCurrentProcess(),
                                    Address,
                                    &Value,
                                    sizeof(Value),
                                    &BytesWritten);

    if (!NT_SUCCESS(Status) || BytesWritten != sizeof(Value))
    {
        __debugbreak();
        ASSERT(FALSE);
    }
}

static 
inline 
void
Wow64WriteNativePtr(UINT64 Address, UINT64 Value)
{
    UINT64 BytesWritten = 0;
    
    NTSTATUS Status = 
        NtWow64WriteVirtualMemory64(NtCurrentProcess(),
                                    Address,
                                    &Value,
                                    sizeof(Value),
                                    &BytesWritten);

    if (!NT_SUCCESS(Status) || BytesWritten != sizeof(Value))
    {
        __debugbreak();
        ASSERT(FALSE);
    }
}

static
inline
void
Wow64WriteNative(UINT64 NativeAddress, CONST VOID* Address, SIZE_T Size)
{
    UINT64 BytesWritten = 0;
    
    NTSTATUS Status = 
        NtWow64WriteVirtualMemory64(NtCurrentProcess(),
                                    NativeAddress,
                                    Address,
                                    Size,
                                    &BytesWritten);

    if (!NT_SUCCESS(Status) || BytesWritten != Size)
    {
        __debugbreak();
        ASSERT(FALSE);
    }
}

static
inline
void
Wow64ReadNative(UINT64 NativeAddress, PVOID Address, SIZE_T Size)
{
    UINT64 BytesRead = 0;
    
    NTSTATUS Status = 
        NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                   NativeAddress,
                                   Address,
                                   Size,
                                   &BytesRead);

    if (!NT_SUCCESS(Status) || BytesRead != Size)
    {
        __debugbreak();
        ASSERT(FALSE);
    }
}

#define WOW64_READ_PTR_FIELD(Ptr, StructType, Field) \
    Wow64ReadNativePtr((UINT64)(Ptr) + (ULONG_PTR)&((StructType*)0)->Field)
#define WOW64_WRITE_PTR_FIELD(Ptr, StructType, Field, Value) \
    Wow64WriteNativePtr((UINT64)(Ptr) + (ULONG_PTR)&((StructType*)0)->Field, Value)
#define WOW64_READ_ULONG_FIELD(Ptr, StructType, Field) \
    Wow64ReadNativeULong((UINT64)(Ptr) + (ULONG_PTR)&((StructType*)0)->Field)
#define WOW64_WRITE_ULONG_FIELD(Ptr, StructType, Field, Value) \
    Wow64WriteNativeULong((UINT64)(Ptr) + (ULONG_PTR)&((StructType*)0)->Field, Value)
#define WOW64_READ_WORD_FIELD(Ptr, StructType, Field) \
    Wow64ReadNativeWord((UINT64)(Ptr) + (ULONG_PTR)&((StructType*)0)->Field)
#define WOW64_READ_BYTE_FIELD(Ptr, StructType, Field) ((BYTE)(WOW64_READ_WORD_FIELD(Ptr, StructType, Field) & 0xFF))
    
#define WOW64_READ_HANDLE_FIELD(Ptr, StructType, Field) ((HANDLE)WOW64_READ_ULONG_FIELD(Ptr, StructType, Field))
#define WOW64_WRITE_HANDLE_FIELD(Ptr, StructType, Field, Value) (WOW64_WRITE_PTR_FIELD(Ptr, StructType, Field, (ULONG_PTR)Value))
    
#define WOW64_CONTAINING_RECORD(Ptr, StructType, Field) ((Ptr) - (ULONG_PTR)&((StructType*)NULL)->Field)
#define WOW64_FIELD_PTR(Ptr, Type, Field) ((Ptr) + (ULONG_PTR)&((Type*)NULL)->Field)

#define WOW64_CAST_TO_PTR(Ptr) ((PVOID)(ULONG_PTR)(Ptr))
#define WOW64_CAST_TO_HANDLE(H) ((HANDLE)(ULONG_PTR)(H))
#define WOW64_CAST_FROM_PTR(Ptr) ((UINT64)((ULONG_PTR)(Ptr)))
#define WOW64_CAST_FROM_HANDLE(H) ((UINT64)((ULONG_PTR)(H)))

#define NtCurrentPeb64() ((PPEB64)(((ULONG_PTR)NtCurrentPeb()) - ALIGN_UP_BY(sizeof(PEB64), PAGE_SIZE)))
#define NtCurrentTeb64() ((PTEB64)(((ULONG_PTR)NtCurrentTeb()) - ALIGN_UP_BY(sizeof(TEB64), PAGE_SIZE)))

typedef UINT64 ULONG_PTR_NATIVE;

#else

#define Wow64WriteNative(NativeAddress, Address, Size) memcpy(NativeAddress, Address, Size)
#define Wow64ReadNative(NativeAddress, Address, Size) memcpy(Address, NativeAddress, Size)

#define WOW64_READ_PTR_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_PTR_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
#define WOW64_READ_ULONG_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_ULONG_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
#define WOW64_READ_WORD_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_READ_BYTE_FIELD(Ptr, StructType, Field) (Ptr->Field)
    
#define WOW64_READ_HANDLE_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_HANDLE_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
    
#define WOW64_CONTAINING_RECORD(Ptr, StructType, Field) CONTAINING_RECORD(Ptr, StructType, Field)
#define WOW64_FIELD_PTR(Ptr, Type, Field) (&(Ptr)->Field)

#define WOW64_CAST_TO_PTR(Ptr) ((PVOID)(Ptr))
#define WOW64_CAST_TO_HANDLE(H) (H)
#define WOW64_CAST_FROM_PTR(Ptr) ((PVOID)(Ptr))
#define WOW64_CAST_FROM_HANDLE(H) (H)

typedef ULONG_PTR ULONG_PTR_NATIVE;

#endif

#ifdef USE_LPC6432
#define LPC_ULONG_PTR ULONGLONG
#define LPC_UNICODE_STRING UNICODE_STRING64
#define LPC_PTR(x) LPC_PVOID
#define LPC_PTRTYPE(x) LPC_PVOID
#else
#define LPC_ULONG_PTR ULONG_PTR
#define LPC_UNICODE_STRING UNICODE_STRING
#define LPC_PTR(x) x*
#define LPC_PTRTYPE(x) x
#endif

#define TO_LPC_HANDLE(h) WOW64_CAST_FROM_HANDLE(h)
#define FROM_LPC_HANDLE(h) WOW64_CAST_TO_HANDLE(h)
