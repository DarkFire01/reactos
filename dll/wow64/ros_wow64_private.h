/*
 * WoW64 private definitions
 *
 * Copyright 2021 Alexandre Julliard
 * Copyright 2025 Marcin Jabłoński
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

/* FIXME: for now, the WOW64 directory path is hardcoded. 
   It is currently set to D: for ease of debugging 
   (for ease of swapping of 32 bit DLLs while the system is running). */
#define TMP_WOW_DIR "D:"

#define WIN32_NO_STATUS
#include <Windows.h>
#include <stdio.h>
#include <assert.h>
#include <ntndk.h>
#include "struct32.h"

#include <debug.h>

/* FIXME: See wow64.c */
// #define WOW64_TLS_CPURESERVED      1
#define WOW64_TLS_TEMPLIST         3
#define WOW64_TLS_USERCALLBACKDATA 5
#define WOW64_TLS_APCLIST          7
#define WOW64_TLS_FILESYSREDIR     8
#define WOW64_TLS_WOW64INFO        10
#define WOW64_TLS_MAX_NUMBER       19

#define FIXME(...) do { DPRINT1(__VA_ARGS__); __debugbreak(); } while(0);

static inline ULONG get_ulong( UINT **args ) { return *(*args)++; }
static inline HANDLE get_handle( UINT **args ) { return UlongToHandle( *(*args)++ ); }
static inline void *get_ptr( UINT **args ) { return ULongToPtr( *(*args)++ ); }

extern ULONG_PTR highest_user_address;
extern ULONG_PTR default_zero_bits;

extern PVOID NtDll32LdrpRoutine;
extern PVOID NtDll32KiUserExceptionDispatcher;

static ULONG_PTR args_alignment = 4;
static USHORT current_machine = IMAGE_FILE_MACHINE_I386;
static USHORT native_machine = IMAGE_FILE_MACHINE_AMD64;

#define Wow64AllocateTemp(...) _alloca(__VA_ARGS__)

#define WINE_WOW_IMPL_CASE(name) case Num ## name: \
    NTSTATUS WINAPI wow64_Nt ## name (UINT* pArgs); \
    status = wow64_Nt ## name (pArgs); \
    break;

unsigned long __readfsdword(unsigned long);

#pragma section(".text")

__declspec(allocate(".text"))
static unsigned char FarReturn64Impl[] = 
{
    0x48, /* REX.W */
    0xCB  /* retf*/
};

__declspec(allocate(".text"))
static unsigned char FarReturn32Impl[] =
{
    0xCB  /* retf */
};

__declspec(allocate(".text"))
static unsigned char Enter32Impl[] =
{
    0x48, 0x89, 0xCC, /* mov rsp, rcx */
    0x48,             /* REX.W */
    0xCB              /* retf*/
};

NTSTATUS 
WINAPI 
Wow64KiUserCallbackDispatcher(ULONG nCallback, 
                              PVOID IN pArgs, 
                              ULONG nArgLen, 
                              PVOID* OUT ppReturn, 
                              PULONG OUT pnRetLen);

/* TODO: Refactor, all of this could be done in a cleaner way */
static
ULONG_PTR
CallOrJump32(ULONG Address, ULONG nArgc, PULONG Args, BOOL bJump)
{    
#define MAX_ARGS 32
#pragma pack(push, 1)
    struct Enter32
    {
        ULONG_PTR Rip32;
        ULONG_PTR SegCs32;
        ULONG     FarReturn32;
        ULONG     Arguments[0];
    };
    
    struct Leave32
    {
        ULONG Rip64;
        ULONG SegCs64;
    };        
    
    typedef ULONG_PTR(*Enter32ImplType)(struct Enter32* enter);
    
    Enter32ImplType pfnEnter32 = (Enter32ImplType)Enter32Impl;
    struct Leave32* leave;
    struct Enter32* enter;
    
    /* Skip the return address for jumps */
    nArgc -= bJump;
    
    BYTE StackBuffer[sizeof(struct Enter32) + sizeof(struct Leave32) + MAX_ARGS * sizeof(ULONG)];
    
    leave = (struct Leave32*)(StackBuffer + sizeof(StackBuffer) - sizeof(*leave));
    enter = (struct Enter32*)(StackBuffer + sizeof(StackBuffer) - sizeof(*leave) 
                              - sizeof(*enter) - sizeof(ULONG) * nArgc);
#pragma pack(pop)
    
    ASSERT(nArgc <= MAX_ARGS);
    
    if (!bJump)
    {
        enter->FarReturn32 = (ULONG)(ULONG_PTR)FarReturn32Impl;
    }
    
    for (int i = 0; i < nArgc + bJump; i++)
    {
        (enter->Arguments - bJump)[i] = Args[i];
    }
    
    enter->Rip32 = (ULONG_PTR) Address;
    enter->SegCs32 = 0x23;
    
    leave->Rip64 = PtrToUlong(_ReturnAddress());
    leave->SegCs64 = 0x33;
    
    pfnEnter32(enter);
    
    /* We should never get here. */
    ASSERT(FALSE);
    return 0;
#undef MAX_ARGS
}

static inline Call32(ULONG Addr, ULONG nArgc, PULONG Args)
{
    return CallOrJump32(Addr, nArgc, Args, FALSE);
}

static inline Jump32(ULONG Addr, ULONG nArgc, PULONG Args)
{
    return CallOrJump32(Addr, nArgc, Args, TRUE);
}

static inline PTEB32 NtCurrentTeb32()
{
    return (PTEB32)(ULONG_PTR)__readfsdword(0x18);
}

static inline PPEB32 NtCurrentPeb32()
{
    return (PPEB32)UlongToPtr(NtCurrentTeb32()->ProcessEnvironmentBlock);
}

struct object_attr64
{
    OBJECT_ATTRIBUTES   attr;
    UNICODE_STRING      str;
    SECURITY_DESCRIPTOR sd;
};

struct user_callback_frame
{
    struct user_callback_frame *prev_frame;
    struct mem_header          *temp_list;
    void                      **ret_ptr;
    ULONG                      *ret_len;
    NTSTATUS                    status;
    jmp_buf                     jmpbuf;
};

typedef struct user_callback_frame USER_CALLBACK_FRAME, *PUSER_CALLBACK_FRAME;
typedef OBJECT_ATTRIBUTES32 *POBJECT_ATTRIBUTES32;

typedef struct tagSYSTEM_SERVICE_TABLE
{
    ULONG_PTR *ServiceTable;
    ULONG_PTR *CounterTable;
    ULONG_PTR ServiceLimit;
    BYTE *ArgumentTable;
} SYSTEM_SERVICE_TABLE, *PSYSTEM_SERVICE_TABLE;

typedef struct _I386_FLOATING_SAVE_AREA
{
    DWORD ControlWord;
    DWORD StatusWord;
    DWORD TagWord;
    DWORD ErrorOffset;
    DWORD ErrorSelector;
    DWORD DataOffset;
    DWORD DataSelector;
    BYTE  RegisterArea[80];
    DWORD Cr0NpxState;
} I386_FLOATING_SAVE_AREA, *PI386_FLOATING_SAVE_AREA;

#include "pshpack4.h"
typedef struct _I386_CONTEXT 
{
    ULONG ContextFlags;
    ULONG Dr0;
    ULONG Dr1;
    ULONG Dr2;
    ULONG Dr3;
    ULONG Dr6;
    ULONG Dr7;
    I386_FLOATING_SAVE_AREA FloatSave;
    ULONG SegGs;
    ULONG SegFs;
    ULONG SegEs;
    ULONG SegDs;
    ULONG Edi;
    ULONG Esi;
    ULONG Ebx;
    ULONG Edx;
    ULONG Ecx;
    ULONG Eax;
    ULONG Ebp;
    ULONG Eip;
    ULONG SegCs;
    ULONG EFlags;
    ULONG Esp;
    ULONG SegSs;
    UCHAR ExtendedRegisters[512];
} I386_CONTEXT, *PI386_CONTEXT;
#include "poppack.h"

typedef struct _WOW64_PATH_REDIRECTION
{
    UNICODE_STRING From;
    UNICODE_STRING To;
} WOW64_PATH_REDIRECTION, *PWOW64_PATH_REDIRECTION;

static 
BOOLEAN 
RedirectPath(const WOW64_PATH_REDIRECTION* Redirection, 
             POBJECT_ATTRIBUTES ObjectAttributes,
             PUNICODE_STRING Buffer)
{
    PUNICODE_STRING ObjectName = ObjectAttributes->ObjectName;
    const UNICODE_STRING* From = &Redirection->From;
    const UNICODE_STRING* To = &Redirection->To;
    
    USHORT NewLength = ObjectName->Length - From->Length + To->Length;
    
    /* FIXME */
    if (Buffer->MaximumLength < NewLength)
    {
        return FALSE;
    }
    
    if (_wcsnicmp(ObjectName->Buffer, From->Buffer, From->Length / sizeof(WCHAR)) == 0)
    {
        Buffer->Length = NewLength;
        
        RtlCopyMemory(Buffer->Buffer, To->Buffer, To->Length);
        
        RtlCopyMemory(Buffer->Buffer + To->Length / sizeof(WCHAR), 
                      ObjectName->Buffer + From->Length / sizeof(WCHAR),
                      ObjectName->Length - From->Length);

        ObjectAttributes->ObjectName = Buffer;  
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN get_file_redirect(OBJECT_ATTRIBUTES* attr, UNICODE_STRING* buffer)
{
    static const WOW64_PATH_REDIRECTION Redirections[] = 
    {
#define REDIRECTION(From, To) { RTL_CONSTANT_STRING(From), RTL_CONSTANT_STRING(To) }
        REDIRECTION(L"\\??\\X:\\reactos\\system32", L"\\??\\" TMP_WOW_DIR),
        REDIRECTION(L"\\??\\X:\\reactos\\winsxs", L"\\??\\" TMP_WOW_DIR "\\winsxs"),
        REDIRECTION(L"\\KnownDlls", L"\\KnownDlls32")
#undef  REDIRECTION
    };
    
    size_t i;
    PUNICODE_STRING ObjectName = attr->ObjectName;
    
    if (!attr || !ObjectName || !ObjectName->Buffer)
    {
        return FALSE;
    }
    
    for (i = 0; i < sizeof(Redirections) / sizeof(*Redirections); i++)
    {
        if (RedirectPath(&Redirections[i], attr, buffer))
        {
            return TRUE;
        }
    }
    
    return FALSE;
}

static inline void *apc_32to64( ULONG func )
{
    /* UNIMPLEMENTED */
    return NULL;
}

static BOOL is_process_wow64( HANDLE handle )
{
    ULONG_PTR info;

    if (handle == NtCurrentProcess()) return TRUE;
    if (NtQueryInformationProcess( handle, ProcessWow64Information, &info, sizeof(info), NULL ))
        return FALSE;
    return !!info;
}

static inline ULONG_PTR get_zero_bits( ULONG_PTR zero_bits )
{
    return zero_bits ? zero_bits : default_zero_bits;
}

static inline ULONG64 get_ulong64( UINT **args )
{
    ULONG64 ret;

    *args = (UINT *)(((ULONG_PTR)*args + args_alignment - 1) & ~(args_alignment - 1));
    ret = *(ULONG64 *)*args;
    *args += 2;
    return ret;
}

static inline void **addr_32to64( void **addr, ULONG *addr32 )
{
    if (!addr32) return NULL;
    *addr = ULongToPtr( *addr32 );
    return addr;
}

static inline SIZE_T *size_32to64( SIZE_T *size, ULONG *size32 )
{
    if (!size32) return NULL;
    *size = *size32;
    return size;
}

static inline void *apc_param_32to64( ULONG func, ULONG context )
{
    if (!func) return ULongToPtr( context );
    return (void *)(ULONG_PTR)(((ULONG64)func << 32) | context);
}

static inline IO_STATUS_BLOCK *iosb_32to64( IO_STATUS_BLOCK *io, IO_STATUS_BLOCK32 *io32 )
{
    if (!io32) return NULL;
    io->Pointer = io32;
    return io;
}

static inline UNICODE_STRING *unicode_str_32to64( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    if (!str32) return NULL;
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->Buffer = ULongToPtr( str32->Buffer );
    return str;
}

static inline CLIENT_ID *client_id_32to64( CLIENT_ID *id, const CLIENT_ID32 *id32 )
{
    if (!id32) return NULL;
    id->UniqueProcess = LongToHandle( id32->UniqueProcess );
    id->UniqueThread = LongToHandle( id32->UniqueThread );
    return id;
}

static inline SECURITY_DESCRIPTOR *secdesc_32to64( SECURITY_DESCRIPTOR *out, const SECURITY_DESCRIPTOR *in )
{
    /* relative descr has the same layout for 32 and 64 */
    const SECURITY_DESCRIPTOR_RELATIVE *sd = (const SECURITY_DESCRIPTOR_RELATIVE *)in;

    if (!in) return NULL;
    out->Revision = sd->Revision;
    out->Sbz1     = sd->Sbz1;
    out->Control  = sd->Control & ~SE_SELF_RELATIVE;
    if (sd->Control & SE_SELF_RELATIVE)
    {
        out->Owner = sd->Owner ? (PSID)((BYTE *)sd + sd->Owner) : NULL;
        out->Group = sd->Group ? (PSID)((BYTE *)sd + sd->Group) : NULL;
        out->Sacl = ((sd->Control & SE_SACL_PRESENT) && sd->Sacl) ? (PSID)((BYTE *)sd + sd->Sacl) : NULL;
        out->Dacl = ((sd->Control & SE_DACL_PRESENT) && sd->Dacl) ? (PSID)((BYTE *)sd + sd->Dacl) : NULL;
    }
    else
    {
        out->Owner = ULongToPtr( sd->Owner );
        out->Group = ULongToPtr( sd->Group );
        out->Sacl = (sd->Control & SE_SACL_PRESENT) ? ULongToPtr( sd->Sacl ) : NULL;
        out->Dacl = (sd->Control & SE_DACL_PRESENT) ? ULongToPtr( sd->Dacl ) : NULL;
    }
    return out;
}

static inline OBJECT_ATTRIBUTES *objattr_32to64( struct object_attr64 *out, const OBJECT_ATTRIBUTES32 *in )
{
    memset( out, 0, sizeof(*out) );
    if (!in) return NULL;
    if (in->Length != sizeof(*in)) return &out->attr;

    out->attr.Length = sizeof(out->attr);
    out->attr.RootDirectory = LongToHandle( in->RootDirectory );
    out->attr.Attributes = in->Attributes;
    out->attr.ObjectName = unicode_str_32to64( &out->str, ULongToPtr( in->ObjectName ));
    out->attr.SecurityQualityOfService = ULongToPtr( in->SecurityQualityOfService );
    out->attr.SecurityDescriptor = secdesc_32to64( &out->sd, ULongToPtr( in->SecurityDescriptor ));
    return &out->attr;
}

static inline OBJECT_ATTRIBUTES *RosWow64RedirObjAttributes(struct object_attr64 *out,
                                                            const OBJECT_ATTRIBUTES32 *in,
                                                            UNICODE_STRING *buffer)
{
    OBJECT_ATTRIBUTES *attr = objattr_32to64( out, in );

    if (attr) get_file_redirect( attr, buffer );
    return attr;
}
 
#define objattr_32to64_redirect(out, in) RosWow64RedirObjAttributes(out, in, &tmpStr)

#define FIXME_DECLARE_TMP_BUF \
    WCHAR tmpBuf[MAX_PATH]; \
    UNICODE_STRING tmpStr;\
    tmpStr.Buffer = tmpBuf;\
    tmpStr.Length = 0;\
    tmpStr.MaximumLength = sizeof(tmpBuf)\

static inline TOKEN_USER *token_user_32to64( TOKEN_USER *out, const TOKEN_USER32 *in )
{
    out->User.Sid = ULongToPtr( in->User.Sid );
    out->User.Attributes = in->User.Attributes;
    return out;
}

static inline TOKEN_OWNER *token_owner_32to64( TOKEN_OWNER *out, const TOKEN_OWNER32 *in )
{
    out->Owner = ULongToPtr( in->Owner );
    return out;
}

static inline TOKEN_PRIMARY_GROUP *token_primary_group_32to64( TOKEN_PRIMARY_GROUP *out, const TOKEN_PRIMARY_GROUP32 *in )
{
    out->PrimaryGroup = ULongToPtr( in->PrimaryGroup );
    return out;
}

static inline TOKEN_DEFAULT_DACL *token_default_dacl_32to64( TOKEN_DEFAULT_DACL *out, const TOKEN_DEFAULT_DACL32 *in )
{
    out->DefaultDacl = ULongToPtr( in->DefaultDacl );
    return out;
}

static inline void put_handle( ULONG *handle32, HANDLE handle )
{
    *handle32 = HandleToULong( handle );
}

static inline void put_addr( ULONG *addr32, void *addr )
{
    if (addr32) *addr32 = PtrToUlong( addr );
}

static inline void put_size( ULONG *size32, SIZE_T size )
{
    if (size32) *size32 = min( size, MAXDWORD );
}

static inline void put_client_id( CLIENT_ID32 *id32, const CLIENT_ID *id )
{
    if (!id32) return;
    id32->UniqueProcess = HandleToLong( id->UniqueProcess );
    id32->UniqueThread = HandleToLong( id->UniqueThread );
}

static inline void put_iosb( IO_STATUS_BLOCK32 *io32, const IO_STATUS_BLOCK *io )
{
    /* sync I/O modifies the 64-bit iosb right away, so in that case we update the 32-bit one */
    /* async I/O leaves the 64-bit one untouched and updates the 32-bit one directly later on */
    if (io32 && io->Pointer != io32)
    {
        io32->Status = io->Status;
        io32->Information = io->Information;
    }
}

extern void put_section_image_info( SECTION_IMAGE_INFORMATION32 *info32,
                                    const SECTION_IMAGE_INFORMATION *info );
                                    
static void put_vm_counters( VM_COUNTERS_EX32 *info32, const VM_COUNTERS_EX *info,
                             ULONG size )
{
    DPRINT("UNIMPLEMENTED");
    __debugbreak();
}

static 
PPORT_VIEW 
PortView32To64(PPORT_VIEW portView64, 
               PPORT_VIEW32 portView32)
{
    portView64->Length = sizeof(*portView64);
    portView64->SectionHandle = UlongToHandle(portView32->SectionHandle);
    portView64->SectionOffset = portView32->SectionOffset;
    portView64->ViewSize = portView32->ViewSize;
    portView64->ViewBase = UlongToPtr(portView32->ViewBase);
    portView64->ViewRemoteBase = UlongToPtr(portView32->ViewRemoteBase);
    
    return portView64;
}

static 
PREMOTE_PORT_VIEW 
RemotePortView32To64(PREMOTE_PORT_VIEW portView64,
                     PREMOTE_PORT_VIEW32 portView32)
{
    portView64->Length = sizeof(*portView64);
    portView64->ViewSize = portView32->ViewSize;
    portView64->ViewBase = UlongToPtr(portView32->ViewBase);
    
    return portView64;
}

static 
PPORT_VIEW32 
PortView64To32(PPORT_VIEW32 portView32, 
               PPORT_VIEW portView64)
{
    portView32->Length = sizeof(*portView32);
    portView32->SectionHandle = HandleToULong(portView64->SectionHandle);
    portView32->SectionOffset = portView64->SectionOffset;
    portView32->ViewSize = portView64->ViewSize;
    portView32->ViewBase = PtrToUlong(portView64->ViewBase);
    portView32->ViewRemoteBase = PtrToUlong(portView64->ViewRemoteBase);
    
    return portView32;
}

static 
PREMOTE_PORT_VIEW32 
RemotePortView64To32(PREMOTE_PORT_VIEW32 portView32, 
                     PREMOTE_PORT_VIEW portView64)
{
    portView32->Length = sizeof(*portView32);
    portView32->ViewSize = portView64->ViewSize;
    portView32->ViewBase = PtrToUlong(portView64->ViewBase);
    
    return portView32;
}

/* FIXME: this is an incomplete implementation */
static
VOID
CopyContext32To64(PCONTEXT pContext,
                  PI386_CONTEXT pContext32)
{
    pContext->ContextFlags = pContext32->ContextFlags | CONTEXT_AMD64;
    pContext->Rip = pContext32->Eip;
    pContext->Rax = pContext32->Eax;
    pContext->Rbx = pContext32->Ebx;
    pContext->Rcx = pContext32->Ecx;
    pContext->Rdx = pContext32->Edx;
    pContext->Rsp = pContext32->Esp;
    pContext->Rbp = pContext32->Ebp;
    pContext->Rsi = pContext32->Esi;
    pContext->Rdi = pContext32->Edi;
    pContext->EFlags = pContext32->EFlags;
    pContext->SegCs = pContext32->SegCs;
    pContext->SegDs = pContext32->SegDs;
    pContext->SegEs = pContext32->SegEs;
    pContext->SegFs = pContext32->SegFs;
    pContext->SegGs = pContext32->SegGs;
    pContext->SegSs = pContext32->SegSs;
}

static
VOID
CopyContext64To32(PI386_CONTEXT pContext32,
                  PCONTEXT pContext)
{
    pContext32->Eip = pContext->Rip;
    pContext32->Eax = pContext->Rax;
    pContext32->Ebx = pContext->Rbx;
    pContext32->Ecx = pContext->Rcx;
    pContext32->Edx = pContext->Rdx;
    pContext32->Esi = pContext->Rdi;
    pContext32->Edi = pContext->Rsi;
    pContext32->Ebp = pContext->Rbp;
    pContext32->Esp = pContext->Rsp;
    pContext32->SegCs = pContext->SegCs;
    pContext32->SegDs = pContext->SegDs;
    pContext32->SegEs = pContext->SegEs;
    pContext32->SegFs = pContext->SegFs;
    pContext32->SegGs = pContext->SegGs;
    pContext32->SegSs = pContext->SegSs;
    pContext32->EFlags = pContext->EFlags;
    pContext32->ContextFlags = pContext->ContextFlags;
}