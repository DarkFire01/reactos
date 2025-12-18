/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS System Libraries
 * FILE:            dll/ntdll/wow64/ntdll32.h
 * PURPOSE:         WOW64 NTDLL Functions Declarations
 * PROGRAMMER:      Marcin Jabłoński
 */

/* TODO: move to some sensible place - somewhere like /sdk/include/reactos/... */

#pragma once

#if defined(BUILD_WOW6432)

#define TEB64_TLS_OFFSET            0x1480

#define WOW64_TLS_FILESYSREDIR      8

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
                            PVOID Buffer,
                            UINT64 BufferSize,
                            PUINT64 NumberOfBytesWritten);

/**********************************************************************
 * Helper functions 
 */
void __writegsdword(ULONG offset, ULONG value);
ULONG __readgsdword(ULONG offset);

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

typedef struct DECLSPEC_ALIGN(16) _M128A64
{
  ULONGLONG Low;
  LONGLONG High;
} M128A64, *PM128A64;

typedef struct DECLSPEC_ALIGN(16) _XSAVE_FORMAT64 
{
  USHORT ControlWord;
  USHORT StatusWord;
  UCHAR TagWord;
  UCHAR Reserved1;
  USHORT ErrorOpcode;
  ULONG ErrorOffset;
  USHORT ErrorSelector;
  USHORT Reserved2;
  ULONG DataOffset;
  USHORT DataSelector;
  USHORT Reserved3;
  ULONG MxCsr;
  ULONG MxCsr_Mask;
  M128A64 FloatRegisters[8];
  M128A64 XmmRegisters[16];
  UCHAR Reserved4[96];
} XSAVE_FORMAT64, *PXSAVE_FORMAT64;

typedef struct DECLSPEC_ALIGN(16) _CONTEXT64 
{
    ULONG64 P1Home;
    ULONG64 P2Home;
    ULONG64 P3Home;
    ULONG64 P4Home;
    ULONG64 P5Home;
    ULONG64 P6Home;
    ULONG ContextFlags;
    ULONG MxCsr;
    USHORT SegCs;
    USHORT SegDs;
    USHORT SegEs;
    USHORT SegFs;
    USHORT SegGs;
    USHORT SegSs;
    ULONG EFlags;
    ULONG64 Dr0;
    ULONG64 Dr1;
    ULONG64 Dr2;
    ULONG64 Dr3;
    ULONG64 Dr6;
    ULONG64 Dr7;
    ULONG64 Rax;
    ULONG64 Rcx;
    ULONG64 Rdx;
    ULONG64 Rbx;
    ULONG64 Rsp;
    ULONG64 Rbp;
    ULONG64 Rsi;
    ULONG64 Rdi;
    ULONG64 R8;
    ULONG64 R9;
    ULONG64 R10;
    ULONG64 R11;
    ULONG64 R12;
    ULONG64 R13;
    ULONG64 R14;
    ULONG64 R15;
    ULONG64 Rip;
    union 
    {
        XSAVE_FORMAT64 FltSave;
        struct 
        {
            M128A64 Header[2];
            M128A64 Legacy[8];
            M128A64 Xmm0;
            M128A64 Xmm1;
            M128A64 Xmm2;
            M128A64 Xmm3;
            M128A64 Xmm4;
            M128A64 Xmm5;
            M128A64 Xmm6;
            M128A64 Xmm7;
            M128A64 Xmm8;
            M128A64 Xmm9;
            M128A64 Xmm10;
            M128A64 Xmm11;
            M128A64 Xmm12;
            M128A64 Xmm13;
            M128A64 Xmm14;
            M128A64 Xmm15;
        } DUMMYSTRUCTNAME DECLSPEC_ALIGN(16);
    } DUMMYUNIONNAME DECLSPEC_ALIGN(16);
    M128A64 VectorRegister[26];
    ULONG64 VectorControl;
    ULONG64 DebugControl;
    ULONG64 LastBranchToRip;
    ULONG64 LastBranchFromRip;
    ULONG64 LastExceptionToRip;
    ULONG64 LastExceptionFromRip;
} CONTEXT64, *PCONTEXT64;  

#ifdef _M_AMD64
C_ASSERT(sizeof(CONTEXT64) == sizeof(CONTEXT));
#endif  

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
#define WOW64_CAST_FROM_PTR(Ptr) ((UINT64)((ULONG_PTR)(Ptr)))
#define WOW64_CAST_FROM_HANDLE(H) ((UINT64)((ULONG_PTR)(H)))

#define NtCurrentPeb64() ((PPEB64)(((ULONG_PTR)NtCurrentPeb()) - ALIGN_UP_BY(sizeof(PEB64), PAGE_SIZE)))
#define NtCurrentTeb64() ((PTEB64)(((ULONG_PTR)NtCurrentTeb()) - ALIGN_UP_BY(sizeof(TEB64), PAGE_SIZE)))

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
#define WOW64_CAST_FROM_PTR(Ptr) (Ptr)
#define WOW64_CAST_FROM_HANDLE(H) (H)

#endif
