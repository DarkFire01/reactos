/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS System Libraries
 * FILE:            dll/ntdll/wow64/ntdll32.h
 * PURPOSE:         WOW64 NTDLL Functions Declarations
 * PROGRAMMER:      Marcin Jabłoński
 */

#pragma once

#if defined(_WOW64) && defined(_M_IX86)

/* Exported functions */
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
                            PVOID Buffer,
                            UINT64 BufferSize,
                            PUINT64 NumberOfBytesWritten);

/* Helper inline functions */

static 
__inline 
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
        __debugbreak();
        ASSERT(FALSE);
    }
    
    return Result;
}

static 
__inline 
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
__inline 
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
__inline 
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
__inline 
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

#define WOW64_CAST_TO_PTR(Ptr) ((PVOID)(ULONG_PTR)(Ptr))
#define WOW64_CAST_TO_HANDLE(H) ((HANDLE)(ULONG_PTR)(H))

#else

#define WOW64_READ_PTR_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_PTR_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
#define WOW64_READ_ULONG_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_ULONG_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
#define WOW64_READ_WORD_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_READ_BYTE_FIELD(Ptr, StructType, Field) (Ptr->Field)
    
#define WOW64_READ_HANDLE_FIELD(Ptr, StructType, Field) (Ptr->Field)
#define WOW64_WRITE_HANDLE_FIELD(Ptr, StructType, Field, Value) (Ptr->Field = (Value))
    
#define WOW64_CONTAINING_RECORD(Ptr, StructType, Field) CONTAINING_RECORD(Ptr, StructType, Field)

#define WOW64_CAST_TO_PTR(Ptr) (Ptr)
#define WOW64_CAST_TO_HANDLE(H) (H)

#endif