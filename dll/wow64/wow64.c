/*
 * Wine WOW64 ReactOS port 
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64.dll
 * FILE:            dll/wow64/wow64.c
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "ros_wow64_private.h"
#include "sysfuncnum.h"
#include <ndk/rtlfuncs.h>

//#define NDEBUG
#include <debug.h>

/* FIXME: for now, the WOW64 directory path is hardcoded. 
   It is currently set to D: for ease of debugging 
   (for ease of swapping of 32 bit DLLs while the system is running). */
#define TMP_WOW_DIR "D:"

typedef MEMORY_BASIC_INFORMATION32 *PMEMORY_BASIC_INFORMATION32;

/* PEB Data */
static USHORT UnicodeCopy[2048]; /* ??? */
static UCHAR AnsiCopy[512], OemCopy[512]; /* ??? */
static ULONG_PTR FixmeProcessHeaps[100]; /* FIXME */

static UNICODE_STRING NtDll32Str = RTL_CONSTANT_STRING(L"" TMP_WOW_DIR "\\ntdll.dll");
static ANSI_STRING ImportStr = RTL_CONSTANT_STRING("LdrInitializeThunk");
static PVOID NtDll32LdrpRoutine = NULL;
static PVOID NtDll32 = NULL;

void SetupFs(ULONG_PTR segSelector);
void Enter32(PVOID where, PVOID ntdll32Base, ULONG_PTR entrypoint);

/* From wine/dlls/ntdll/unix/env.c */
static inline void dup_unicode_string( const UNICODE_STRING *src, WCHAR **dst, UNICODE_STRING32 *str )
{
    if (!src->Buffer) return;
    str->Buffer = PtrToUlong( *dst );
    str->Length = src->Length;
    str->MaximumLength = src->MaximumLength;
    memcpy( *dst, src->Buffer, src->MaximumLength );
    *dst += (src->MaximumLength + 1) / sizeof(WCHAR);
}

static void *build_wow64_parameters( const RTL_USER_PROCESS_PARAMETERS *params )
{
    RTL_USER_PROCESS_PARAMETERS32 *wow64_params = NULL;

    NTSTATUS status;
    WCHAR *dst;
    SIZE_T size = (sizeof(*wow64_params)
                   + params->CurrentDirectory.DosPath.MaximumLength
                   + params->DllPath.MaximumLength
                   + params->ImagePathName.MaximumLength
                   + params->CommandLine.MaximumLength
                   + params->WindowTitle.MaximumLength
                   + params->DesktopInfo.MaximumLength
                   + params->ShellInfo.MaximumLength
                   + ((params->RuntimeData.MaximumLength + 1) & ~1));

    status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&wow64_params, 32, &size,
                                      MEM_COMMIT, PAGE_READWRITE );
    ASSERT( !status );

    wow64_params->MaximumLength   = size;
    wow64_params->Length          = size;
    wow64_params->Flags           = params->Flags;
    wow64_params->DebugFlags      = params->DebugFlags;
    wow64_params->ConsoleHandle   = HandleToULong( params->ConsoleHandle );
    wow64_params->ConsoleFlags    = params->ConsoleFlags;
    wow64_params->StandardInput   = HandleToULong( params->StandardInput );
    wow64_params->StandardOutput  = HandleToULong( params->StandardOutput );
    wow64_params->StandardError   = HandleToULong( params->StandardError );
    wow64_params->StartingX       = params->StartingX;
    wow64_params->StartingY       = params->StartingY;
    wow64_params->CountX          = params->CountX;
    wow64_params->CountY          = params->CountY;
    wow64_params->CountCharsX     = params->CountCharsX;
    wow64_params->CountCharsY     = params->CountCharsY;
    wow64_params->FillAttribute   = params->FillAttribute;
    wow64_params->WindowFlags     = params->WindowFlags;
    wow64_params->ShowWindowFlags = params->ShowWindowFlags;

    dst = (WCHAR *)(wow64_params + 1);
    dup_unicode_string( &params->CurrentDirectory.DosPath, &dst, &wow64_params->CurrentDirectory.DosPath );
    dup_unicode_string( &params->DllPath, &dst, &wow64_params->DllPath );
    dup_unicode_string( &params->ImagePathName, &dst, &wow64_params->ImagePathName );
    dup_unicode_string( &params->CommandLine, &dst, &wow64_params->CommandLine );
    dup_unicode_string( &params->WindowTitle, &dst, &wow64_params->WindowTitle );
    dup_unicode_string( &params->DesktopInfo, &dst, &wow64_params->DesktopInfo );
    dup_unicode_string( &params->ShellInfo, &dst, &wow64_params->ShellInfo );
    dup_unicode_string( &params->RuntimeData, &dst, &wow64_params->RuntimeData );

    wow64_params->Environment = PtrToUlong( dst );
    memcpy( dst, params->Environment, wcslen(params->Environment) );
    return wow64_params;
}

/**********************************************************************
 *           wow64_NtQueryInformationProcess
 */
NTSTATUS WINAPI wow64_NtQueryInformationProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );
    PROCESSINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case ProcessBasicInformation:  /* PROCESS_BASIC_INFORMATION */
        if (len == sizeof(PROCESS_BASIC_INFORMATION32))
        {
            PROCESS_BASIC_INFORMATION info;
            PROCESS_BASIC_INFORMATION32 *info32 = ptr;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                if (is_process_wow64( handle ))
                    info32->PebBaseAddress = PtrToUlong( info.PebBaseAddress ) + 0x1000;
                else
                    info32->PebBaseAddress = 0;
                info32->ExitStatus = info.ExitStatus;
                info32->AffinityMask = info.AffinityMask;
                info32->BasePriority = info.BasePriority;
                info32->UniqueProcessId = info.UniqueProcessId;
                info32->InheritedFromUniqueProcessId = info.InheritedFromUniqueProcessId;
                if (retlen) *retlen = sizeof(*info32);
            }
            return status;
        }
        if (retlen) *retlen = sizeof(PROCESS_BASIC_INFORMATION32);
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessIoCounters:  /* IO_COUNTERS */
    case ProcessTimes:  /* KERNEL_USER_TIMES */
    case ProcessDefaultHardErrorMode:  /* ULONG */
    case ProcessPriorityClass:  /* PROCESS_PRIORITY_CLASS */
    case ProcessHandleCount:  /* ULONG */
    case ProcessSessionInformation:  /* ULONG */
    case ProcessDebugFlags:  /* ULONG */
    case ProcessExecuteFlags:  /* ULONG */
    case ProcessCookie:  /* ULONG */
    case ProcessCycleTime:  /* PROCESS_CYCLE_TIME_INFORMATION */
        /* FIXME: check buffer alignment */
        return NtQueryInformationProcess( handle, class, ptr, len, retlen );

    case ProcessQuotaLimits:  /* QUOTA_LIMITS */
        if (len == sizeof(QUOTA_LIMITS32))
        {
            QUOTA_LIMITS info;
            QUOTA_LIMITS32 *info32 = ptr;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                info32->PagedPoolLimit        = info.PagedPoolLimit;
                info32->NonPagedPoolLimit     = info.NonPagedPoolLimit;
                info32->MinimumWorkingSetSize = info.MinimumWorkingSetSize;
                info32->MaximumWorkingSetSize = info.MaximumWorkingSetSize;
                info32->PagefileLimit         = info.PagefileLimit;
                info32->TimeLimit             = info.TimeLimit;
                if (retlen) *retlen = len;
            }
            return status;
        }
        if (retlen) *retlen = sizeof(QUOTA_LIMITS32);
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessVmCounters:  /* VM_COUNTERS_EX */
        if (len == sizeof(VM_COUNTERS32) || len == sizeof(VM_COUNTERS_EX32))
        {
            VM_COUNTERS_EX info;
            VM_COUNTERS_EX32 *info32 = ptr;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                put_vm_counters( info32, &info, len );
                if (retlen) *retlen = len;
            }
            return status;
        }
        if (retlen) *retlen = sizeof(VM_COUNTERS_EX32);
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessDebugPort:  /* ULONG_PTR */
    case ProcessAffinityMask:  /* ULONG_PTR */
    case ProcessWow64Information:  /* ULONG_PTR */
    case ProcessDebugObjectHandle:  /* HANDLE */
        if (len == sizeof(ULONG))
        {
            ULONG_PTR data;

            if (!(status = NtQueryInformationProcess( handle, class, &data, sizeof(data), NULL )))
            {
                *(ULONG *)ptr = data;
                if (retlen) *retlen = sizeof(ULONG);
            }
            else if (status == STATUS_PORT_NOT_SET)
            {
                *(ULONG *)ptr = 0;
                if (retlen) *retlen = sizeof(ULONG);
            }
            return status;
        }
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessImageFileName:
    case ProcessImageFileNameWin32:  /* UNICODE_STRING + string */
        {
            ULONG retsize, size = len + sizeof(UNICODE_STRING) - sizeof(UNICODE_STRING32);
            UNICODE_STRING *str = Wow64AllocateTemp( size );
            UNICODE_STRING32 *str32 = ptr;

            if (!(status = NtQueryInformationProcess( handle, class, str, size, &retsize )))
            {
                str32->Length = str->Length;
                str32->MaximumLength = str->MaximumLength;
                str32->Buffer = PtrToUlong( str32 + 1 );
                memcpy( str32 + 1, str->Buffer, str->MaximumLength );
            }
            if (retlen) *retlen = retsize + sizeof(UNICODE_STRING32) - sizeof(UNICODE_STRING);
            return status;
        }

    case ProcessImageInformation:  /* SECTION_IMAGE_INFORMATION */
        if (len == sizeof(SECTION_IMAGE_INFORMATION32))
        {
            SECTION_IMAGE_INFORMATION info;
            SECTION_IMAGE_INFORMATION32 *info32 = ptr;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                put_section_image_info( info32, &info );
                if (retlen) *retlen = sizeof(*info32);
            }
            return status;
        }
        if (retlen) *retlen = sizeof(SECTION_IMAGE_INFORMATION32);
        return STATUS_INFO_LENGTH_MISMATCH;
#ifndef __REACTOS__
    case ProcessWineLdtCopy:
        return STATUS_NOT_IMPLEMENTED;
#endif
    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}

static
NTSTATUS 
Wow64WinHandler(ULONG syscallNum, 
                ULONG numArgs, 
                ULONG* pArgs)
{
    ANSI_STRING ImportStr = RTL_CONSTANT_STRING("sdwhwin32");
    static PSYSTEM_SERVICE_TABLE pServiceTable = NULL; 
    static PVOID Wow64WinDll = NULL;
    UNICODE_STRING Wow64WinDllStr = RTL_CONSTANT_STRING(L"wow64win.dll");
    NTSTATUS Status;
        
    ULONG_PTR *HandlerTable = NULL;
    NTSTATUS (*Service)(ULONG* pArgs) = NULL;
    
    static const char* mapping[] = 
    {
#define SVC_(name, argc) ""#name ,
#include "../../../win32ss/w32ksvc32.h"   
#undef SVC_
    };
    
    /* Make sure wow64win.dll is loaded. */
    if (pServiceTable == NULL)
    {
        Status = LdrLoadDll(NULL, 0, &Wow64WinDllStr, &Wow64WinDll);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Couldn't load wow64win.dll.\n");
            return Status;
        }
        
        Status = LdrGetProcedureAddress(Wow64WinDll, &ImportStr, 0, (PVOID*)&pServiceTable);
        if (!NT_SUCCESS(Status)) 
        {
            DPRINT1("Couldn't find %hs in wow64win.dll.\n", ImportStr.Buffer);
            ASSERT(FALSE);
        }
        
        DPRINT1("Loaded service table %p from %p.\n", pServiceTable, Wow64WinDll);
    }
    
    HandlerTable = pServiceTable->ServiceTable;
    if (HandlerTable == NULL)
    {
        DPRINT1("Wow64Win service table is empty.\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    
    if (HandlerTable[syscallNum] == 0)
    {
        DPRINT1("Wow64Win service table doesn't define service %lX:%s.\n", 
                syscallNum, mapping[syscallNum]);
        return STATUS_NOT_IMPLEMENTED;
    }
    
    if (pServiceTable->ArgumentTable != NULL)
    {
        if (pServiceTable->ArgumentTable[syscallNum] != numArgs)
        {
            DPRINT1("WARNING: Wow64Win service expects a different number of "
                    "arguments than %d for %lX:%s.\n", 
                    numArgs, syscallNum, mapping[syscallNum]);
        }
    }
    
    Service = (PVOID)HandlerTable[syscallNum];
    return Service(pArgs);
}

static
NTSTATUS 
Wow64Handler(ULONG syscallNum, 
             ULONG numArgs, 
             ULONG* pArgs)
{   
    NTSTATUS status;

    static const char* mapping[] = 
    {
#define SVC_(name, argc) ""#name ,
#include "../../../ntoskrnl/include/sysfuncs.h"   
#undef SVC_
    };
    
    status = STATUS_NOT_IMPLEMENTED;

#if 0
    if (syscallNum < sizeof(mapping) / sizeof(*mapping))
    {
        DPRINT1("[Syscall %lX:%hs]\n", syscallNum, mapping[syscallNum]);
    }
#endif
    
    switch (syscallNum)
    {
        /* file.c */
        WINE_WOW_IMPL_CASE(CancelIoFile);
        WINE_WOW_IMPL_CASE(CreateFile);
        WINE_WOW_IMPL_CASE(CreateMailslotFile);
        WINE_WOW_IMPL_CASE(CreateNamedPipeFile);
        WINE_WOW_IMPL_CASE(CreatePagingFile);
        WINE_WOW_IMPL_CASE(DeleteFile);
        WINE_WOW_IMPL_CASE(DeviceIoControlFile);
        WINE_WOW_IMPL_CASE(FlushBuffersFile);
        WINE_WOW_IMPL_CASE(FsControlFile);
        WINE_WOW_IMPL_CASE(LockFile);
        WINE_WOW_IMPL_CASE(NotifyChangeDirectoryFile);
        WINE_WOW_IMPL_CASE(OpenFile);
        WINE_WOW_IMPL_CASE(QueryAttributesFile);
        WINE_WOW_IMPL_CASE(QueryDirectoryFile);
        WINE_WOW_IMPL_CASE(QueryEaFile);
        WINE_WOW_IMPL_CASE(QueryFullAttributesFile);
        WINE_WOW_IMPL_CASE(QueryInformationFile);
        WINE_WOW_IMPL_CASE(QueryVolumeInformationFile);
        WINE_WOW_IMPL_CASE(ReadFile);
        WINE_WOW_IMPL_CASE(ReadFileScatter);
        WINE_WOW_IMPL_CASE(RemoveIoCompletion);
        WINE_WOW_IMPL_CASE(SetEaFile);
        WINE_WOW_IMPL_CASE(SetInformationFile);
        WINE_WOW_IMPL_CASE(SetVolumeInformationFile);
        WINE_WOW_IMPL_CASE(UnlockFile);
        WINE_WOW_IMPL_CASE(WriteFile);
        WINE_WOW_IMPL_CASE(WriteFileGather);
        
        /* registry.c */
        WINE_WOW_IMPL_CASE(QuerySystemInformation);
        WINE_WOW_IMPL_CASE(OpenKey);
        WINE_WOW_IMPL_CASE(QueryValueKey);
        WINE_WOW_IMPL_CASE(DeleteValueKey);
        WINE_WOW_IMPL_CASE(CreateKey);
        WINE_WOW_IMPL_CASE(DeleteKey);
        WINE_WOW_IMPL_CASE(EnumerateKey);
        WINE_WOW_IMPL_CASE(EnumerateValueKey);
        
        /* system.c */
        WINE_WOW_IMPL_CASE(QueryInformationProcess);
        WINE_WOW_IMPL_CASE(PowerInformation);
        WINE_WOW_IMPL_CASE(QuerySystemEnvironmentValue);
        WINE_WOW_IMPL_CASE(QuerySystemEnvironmentValueEx);
        WINE_WOW_IMPL_CASE(LoadDriver);
        WINE_WOW_IMPL_CASE(DisplayString);
        WINE_WOW_IMPL_CASE(InitiatePowerAction);
        WINE_WOW_IMPL_CASE(QuerySystemTime);
        WINE_WOW_IMPL_CASE(RaiseHardError);
        WINE_WOW_IMPL_CASE(SetIntervalProfile);
        WINE_WOW_IMPL_CASE(ShutdownSystem);
        WINE_WOW_IMPL_CASE(SetSystemInformation);
        WINE_WOW_IMPL_CASE(SetSystemTime);
        WINE_WOW_IMPL_CASE(SystemDebugControl);
        WINE_WOW_IMPL_CASE(UnloadDriver);
        
        /* sync.c */
        WINE_WOW_IMPL_CASE(CancelTimer);
        WINE_WOW_IMPL_CASE(ClearEvent);
        WINE_WOW_IMPL_CASE(CompleteConnectPort);
        WINE_WOW_IMPL_CASE(CreateDebugObject);
        WINE_WOW_IMPL_CASE(CreateDirectoryObject);
        WINE_WOW_IMPL_CASE(CreateEvent);
        WINE_WOW_IMPL_CASE(CreateIoCompletion);
        WINE_WOW_IMPL_CASE(CreateJobObject);
        WINE_WOW_IMPL_CASE(CreateKeyedEvent);
        WINE_WOW_IMPL_CASE(CreateMutant);
        WINE_WOW_IMPL_CASE(CreatePort);
        WINE_WOW_IMPL_CASE(CreateSection);
        WINE_WOW_IMPL_CASE(CreateSemaphore);
        WINE_WOW_IMPL_CASE(CreateSymbolicLinkObject);
        WINE_WOW_IMPL_CASE(CreateTimer);
        WINE_WOW_IMPL_CASE(DebugContinue);
        WINE_WOW_IMPL_CASE(DelayExecution);
        WINE_WOW_IMPL_CASE(DuplicateObject);
        WINE_WOW_IMPL_CASE(MakePermanentObject);
        WINE_WOW_IMPL_CASE(MakeTemporaryObject);
        WINE_WOW_IMPL_CASE(OpenDirectoryObject);
        WINE_WOW_IMPL_CASE(OpenEvent);
        WINE_WOW_IMPL_CASE(OpenIoCompletion);
        WINE_WOW_IMPL_CASE(OpenJobObject);
        WINE_WOW_IMPL_CASE(OpenKeyedEvent);
        WINE_WOW_IMPL_CASE(OpenMutant);
        WINE_WOW_IMPL_CASE(OpenSection);
        WINE_WOW_IMPL_CASE(OpenSemaphore);
        WINE_WOW_IMPL_CASE(OpenSymbolicLinkObject);
        WINE_WOW_IMPL_CASE(OpenTimer);
        WINE_WOW_IMPL_CASE(PulseEvent);
        WINE_WOW_IMPL_CASE(QueryEvent);
        WINE_WOW_IMPL_CASE(QueryInformationJobObject);
        WINE_WOW_IMPL_CASE(QueryIoCompletion);
        WINE_WOW_IMPL_CASE(QueryMutant);
        WINE_WOW_IMPL_CASE(QueryObject);
        WINE_WOW_IMPL_CASE(QueryPerformanceCounter);
        WINE_WOW_IMPL_CASE(QuerySection);
        WINE_WOW_IMPL_CASE(QuerySemaphore);
        WINE_WOW_IMPL_CASE(QuerySymbolicLinkObject);
        WINE_WOW_IMPL_CASE(QueryTimer);
        WINE_WOW_IMPL_CASE(QueryTimerResolution);
        WINE_WOW_IMPL_CASE(RegisterThreadTerminatePort);
        WINE_WOW_IMPL_CASE(ReleaseKeyedEvent);
        WINE_WOW_IMPL_CASE(ReleaseMutant);
        WINE_WOW_IMPL_CASE(ReleaseSemaphore);
        WINE_WOW_IMPL_CASE(ReplyWaitReceivePort);
        WINE_WOW_IMPL_CASE(RequestWaitReplyPort);
        WINE_WOW_IMPL_CASE(ResetEvent);
        WINE_WOW_IMPL_CASE(SetEvent);
        WINE_WOW_IMPL_CASE(SetInformationDebugObject);
        WINE_WOW_IMPL_CASE(SetInformationJobObject);
        WINE_WOW_IMPL_CASE(SetInformationObject);
        WINE_WOW_IMPL_CASE(SetIoCompletion);
        WINE_WOW_IMPL_CASE(SecureConnectPort);
        WINE_WOW_IMPL_CASE(SetTimer);
        WINE_WOW_IMPL_CASE(SetTimerResolution);
        WINE_WOW_IMPL_CASE(SignalAndWaitForSingleObject);
        WINE_WOW_IMPL_CASE(TerminateJobObject);
        WINE_WOW_IMPL_CASE(TestAlert);
        WINE_WOW_IMPL_CASE(WaitForDebugEvent);
        WINE_WOW_IMPL_CASE(WaitForKeyedEvent);
        WINE_WOW_IMPL_CASE(WaitForMultipleObjects);
        WINE_WOW_IMPL_CASE(WaitForSingleObject);
        WINE_WOW_IMPL_CASE(YieldExecution);
        
        /* syscall.c */
        WINE_WOW_IMPL_CASE(AddAtom);
        WINE_WOW_IMPL_CASE(AllocateLocallyUniqueId);
        WINE_WOW_IMPL_CASE(AllocateUuids);
        WINE_WOW_IMPL_CASE(CallbackReturn);
        WINE_WOW_IMPL_CASE(Close);
        WINE_WOW_IMPL_CASE(DeleteAtom);
        WINE_WOW_IMPL_CASE(FindAtom);
        WINE_WOW_IMPL_CASE(GetCurrentProcessorNumber);
        WINE_WOW_IMPL_CASE(QueryDefaultLocale);
        WINE_WOW_IMPL_CASE(QueryDefaultUILanguage);
        WINE_WOW_IMPL_CASE(QueryInformationAtom);
        WINE_WOW_IMPL_CASE(QueryInstallUILanguage);
        WINE_WOW_IMPL_CASE(RaiseException);
        WINE_WOW_IMPL_CASE(SetDebugFilterState);
        WINE_WOW_IMPL_CASE(SetDefaultLocale);
        WINE_WOW_IMPL_CASE(SetDefaultUILanguage);
        
        /* virtual.c */
        WINE_WOW_IMPL_CASE(AllocateVirtualMemory);
        WINE_WOW_IMPL_CASE(AreMappedFilesTheSame);
        WINE_WOW_IMPL_CASE(FlushInstructionCache);
        WINE_WOW_IMPL_CASE(FlushVirtualMemory);
        WINE_WOW_IMPL_CASE(FreeVirtualMemory);
        WINE_WOW_IMPL_CASE(GetWriteWatch);
        WINE_WOW_IMPL_CASE(LockVirtualMemory);
        WINE_WOW_IMPL_CASE(MapViewOfSection);
        WINE_WOW_IMPL_CASE(ProtectVirtualMemory);
        WINE_WOW_IMPL_CASE(QueryVirtualMemory);
        WINE_WOW_IMPL_CASE(Wow64AllocateVirtualMemory64);
        WINE_WOW_IMPL_CASE(Wow64ReadVirtualMemory64);
        WINE_WOW_IMPL_CASE(Wow64WriteVirtualMemory64);
        WINE_WOW_IMPL_CASE(ReadVirtualMemory);
        WINE_WOW_IMPL_CASE(ResetWriteWatch);
        WINE_WOW_IMPL_CASE(UnlockVirtualMemory);
        WINE_WOW_IMPL_CASE(UnmapViewOfSection);
        WINE_WOW_IMPL_CASE(WriteVirtualMemory);
        
        case NumTerminateThread:
        {
            HANDLE hThread = get_handle(&pArgs);
            ULONG uCode = get_ulong(&pArgs);
            
            DPRINT1("Terminating thread %p with a WOW64 call from %p\n", hThread, NtCurrentProcess());
            
            status = NtTerminateThread(hThread, uCode);
            break;
        }
        case NumTerminateProcess:
        {
            HANDLE hProcess = get_handle(&pArgs);
            ULONG uCode = get_ulong(&pArgs);
            
            DPRINT1("Terminating process %p with a WOW64 call from %p\n", hProcess, NtCurrentProcess());
            
            status = NtTerminateProcess(hProcess, uCode);
            break;
        }
        case NumQueryDebugFilterState:
        {
            ULONG u1 = get_ulong(&pArgs);
            ULONG u2 = get_ulong(&pArgs);
            status = NtQueryDebugFilterState(u1, u2);
            break;
        }
        
        default:
        {
            if (syscallNum >= 0x1000)
            {
                return Wow64WinHandler(syscallNum - 0x1000, numArgs, pArgs);
            }
            
            DPRINT1("WARNING: Unhandled 32-bit syscall 0x%lX(%ld args at %p)\n", syscallNum, numArgs, pArgs);
            status = STATUS_NOT_IMPLEMENTED;
        }
    }
    
    return status;
}

BOOL
WINAPI
DllMain(HANDLE hDll,
        DWORD dwReason,
        LPVOID lpReserved)
{
    return TRUE;
}

static
VOID 
Wow64InitProcess(VOID)
{
    NTSTATUS Status;
    PPEB32 WowPeb = NULL;
    PRTL_USER_PROCESS_PARAMETERS32 ProcParams32 = NULL;
    PPEB Peb = NtCurrentPeb();
    SIZE_T Size;
    
    Status = LdrLoadDll(L"" TMP_WOW_DIR "\\ntdll.dll", 0, &NtDll32Str, &NtDll32);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("32 bit NTDLL.DLL could not be loaded.\n");
        ASSERT(FALSE);
    }
    
    Size = sizeof(PEB32);
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), 
                                     &WowPeb, 
                                     32, 
                                     &Size, 
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PEB32 Allocation failed: %lx\n", Status);
        ASSERT(FALSE);
    }
    
    ProcParams32 = build_wow64_parameters(Peb->ProcessParameters);
    WowPeb->ProcessParameters = PtrToUlong(ProcParams32);
    
    /* FIXME: hack for process heaps */
    WowPeb->MaximumNumberOfHeaps = sizeof(FixmeProcessHeaps) / sizeof(*FixmeProcessHeaps);
    WowPeb->ProcessHeaps = PtrToUlong(FixmeProcessHeaps);
    
    WowPeb->ImageBaseAddress = PtrToUlong(Peb->ImageBaseAddress);
    
    /* CHECKME */
    WowPeb->OemCodePageData = PtrToUlong(&OemCopy);
    WowPeb->AnsiCodePageData = PtrToUlong(&AnsiCopy);
    WowPeb->UnicodeCaseTableData = PtrToUlong(&UnicodeCopy);
    RtlCopyMemory(&OemCopy, Peb->OemCodePageData, sizeof(OemCopy));
    RtlCopyMemory(&AnsiCopy, Peb->AnsiCodePageData, sizeof(AnsiCopy));
    RtlCopyMemory(&UnicodeCopy, Peb->UnicodeCaseTableData, sizeof(UnicodeCopy));
    
    /* TODO: Check types - _WOW64_PROCESS has only one field, is this supposed to be the PEB?
       According to https://stackoverflow.com/a/69171561 - yes, it is. This is, however, quite a hacky way 
       to set it. */
    Status = NtSetInformationProcess(NtCurrentProcess(), ProcessWow64Information, &WowPeb, sizeof(WowPeb));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Setting info failed: %lx\n", Status);
        ASSERT(FALSE);
    }
    
    /* Change image path name */
    ProcParams32->ImagePathName.Buffer = PtrToUlong(Peb->ProcessParameters->ImagePathName.Buffer);
    ProcParams32->ImagePathName.Length = Peb->ProcessParameters->ImagePathName.Length;
    ProcParams32->ImagePathName.MaximumLength = Peb->ProcessParameters->ImagePathName.MaximumLength;
    ProcParams32->Flags |=  RTL_USER_PROCESS_PARAMETERS_NORMALIZED;
    
    Status = LdrGetProcedureAddress(NtDll32, &ImportStr, 0, &NtDll32LdrpRoutine);
    if (!NT_SUCCESS(Status)) 
    {
        DPRINT1("Couldn't find LdrInitializeThunk in 32-bit ntdll.dll.\n");
        ASSERT(FALSE);
    }
    DPRINT("Getting init function ptr %p\n", NtDll32LdrpRoutine);
}

static
LONG
Wow64UnhandledExceptionHandler(IN PEXCEPTION_POINTERS ExceptionInfo)
{
    __debugbreak();
    return EXCEPTION_EXECUTE_HANDLER;
}

static
void
Wow64Trampoline(VOID)
{
    IMAGE_NT_HEADERS32* NtHeaders = NULL;
    PPEB Peb;
    
    Peb = NtCurrentPeb();
    NtHeaders = (IMAGE_NT_HEADERS32*)RtlImageNtHeader(Peb->ImageBaseAddress);

    _SEH2_TRY
    {
        Enter32(NtDll32LdrpRoutine,
                NtDll32,
                (ULONG_PTR)Peb->ImageBaseAddress
                 + (ULONG_PTR)NtHeaders->OptionalHeader.AddressOfEntryPoint);
    }
    _SEH2_EXCEPT(Wow64UnhandledExceptionHandler(_SEH2_GetExceptionInformation()))
    {
        DPRINT1("Terminating WOW64 thread due to unhandled exception.");
        NtTerminateProcess(NtCurrentProcess(), _SEH2_GetExceptionCode());
    }
}

static
VOID 
Wow64InitThread(PCONTEXT pContext)
{
    NTSTATUS Status;
    SIZE_T Size;
    PTEB32 WowTeb = NULL;
    PPEB32 WowPeb = NULL;
    PTEB Teb = NtCurrentTeb();
    WowTeb = NULL;

    /* Allocate memory for the 32 bit TEB. 
       TODO: This should be done in kernel mode probably */
    Size = sizeof(TEB32);
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), 
                                     &WowTeb, 
                                     32, 
                                     &Size, 
                                     MEM_COMMIT, 
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("TEB32 Allocation failed: %lx\n", Status);
        ASSERT(FALSE);
    }

    /* Set the 64 bit Teb's TlsSlots[1] to the TEB32 before setting process 
       information. */
    /* FIXME: This is NOT Wine compatible! Wine uses Peb->WowTebOffset which is 
       only present for NTDDI_VERSION >= NTDDI_WIN10, and uses this TLS entry
       for its own structures. Since we do not use them (yet), this entry should 
       be free for now. */
    Teb->TlsSlots[1] = WowTeb;

    WowTeb->NtTib.Self = PtrToUlong(WowTeb);

    WowTeb->StaticUnicodeString.Length = 0;
    WowTeb->StaticUnicodeString.MaximumLength = sizeof(WowTeb->StaticUnicodeBuffer);
    WowTeb->StaticUnicodeString.Buffer = PtrToUlong(WowTeb->StaticUnicodeBuffer);

    Status = NtQueryInformationProcess(NtCurrentProcess(), 
                                       ProcessWow64Information, 
                                       &WowPeb, 
                                       sizeof(WowPeb), 
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Getting Wow64 info failed: %lx\n", Status);
        ASSERT(FALSE);
    }

    WowTeb->ProcessEnvironmentBlock = PtrToUlong(WowPeb);

    /* Point the FS segment register to the CMTEB entry in the GDT */
    SetupFs(0x0053);

    /* CMTEB GDT entry's fields are set on thread context switches, make sure 
       correct values are loaded before executing. */
    while(NtYieldExecution() == STATUS_NO_YIELD_PERFORMED);

    /* Make sure the handler routine pointer fits into the 32-bit TEB */
    ASSERT((((ULONG_PTR)Wow64Handler) & ~0xFFFFFFFF) == 0);
    WowTeb->WOW32Reserved = PtrToUlong(Wow64Handler);

    WowTeb->ClientId.UniqueProcess = HandleToULong(Teb->ClientId.UniqueProcess);
    WowTeb->ClientId.UniqueThread = HandleToULong(Teb->ClientId.UniqueThread);

    pContext->Rip = (ULONG_PTR)Wow64Trampoline;
}

__declspec(dllexport)
void 
WINAPI 
Wow64LdrpInitialize(PCONTEXT pContext)
{
    static LONG ProcessInitialized = 0;
    
    if (_InterlockedCompareExchange(&ProcessInitialized,
                                    1,
                                    0) == 0)
    {
        Wow64InitProcess();
    }
    
    /* TODO: Parse Context to (somehow) get the start address for the new thread.
       This is somewhat problematic, because the 64-bit side can give us 64-bit 
       pointer to functions in 64-bit DLLs. */
    Wow64InitThread(pContext);
}