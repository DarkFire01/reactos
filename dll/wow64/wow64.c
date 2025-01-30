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

//#define NDEBUG
#include <debug.h>

typedef MEMORY_BASIC_INFORMATION32 *PMEMORY_BASIC_INFORMATION32;

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

    status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&wow64_params, 31, &size,
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

/* WINE FUNCS */

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

/* END OF WINEFUNCS */

NTSTATUS handler(ULONG syscallNum, ULONG numArgs, ULONG* pArgs)
{   
    NTSTATUS status;

    static const char* mapping[] = 
    {
#define SVC_(name, argc) ""#name ,
#include "../../../ntoskrnl/include/sysfuncs.h"   
#undef SVC_
    };
    
    status = STATUS_NOT_IMPLEMENTED;

    DPRINT1("[Syscall %lX:%hs]\n", syscallNum, mapping[syscallNum]);

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
            DPRINT1("WARNING: Unhandled 32-bit syscall 0x%lX(%ld args at %p)\n", syscallNum, numArgs, pArgs);
            status = STATUS_NOT_IMPLEMENTED;
        }
    }
    
    return status;
}

#define TMP_WOW_DIR "D:"

BOOL
WINAPI
DllMain(HANDLE hDll,
        DWORD dwReason,
        LPVOID lpReserved)
{
    return TRUE;
}

__declspec(dllexport)
void WINAPI Wow64LdrpInitialize(CONTEXT *context)
{
    /* FIXME: This is process initialization, this should only done once, not for every thread. */
    NTSTATUS status;
    PRTL_USER_PROCESS_PARAMETERS32 procParams;
    PPEB32 wowPeb;
    PTEB32 wowTeb;
    SIZE_T size;
    NLS_FILE_HEADER ansiCopy, oemCopy;
    
    /* FIXME: stack allocated variables for PEB/TEB contents */
    ULONG_PTR fixmeProcessHeaps[100];
    WCHAR fixmeStaticUnicodeString[256];
    
    PVOID proc;
    
    UNICODE_STRING ntdll32Str = RTL_CONSTANT_STRING(L"" TMP_WOW_DIR "\\ntdll.dll");
    
    ANSI_STRING importStr = RTL_CONSTANT_STRING("LdrInitializeThunk");
    
    HMODULE ntdll32;
    PVOID clientProgram = NtCurrentPeb()->ImageBaseAddress;
    
    status = LdrLoadDll(L"" TMP_WOW_DIR "\\ntdll.dll", 0, &ntdll32Str, &ntdll32);
    ASSERT(NT_SUCCESS(status));
    
    __debugbreak();
    
    if ((ULONG_PTR)clientProgram != 0x400000)
    {
        DPRINT("Warning: the default test program for run32on64 has base address 0x400000, "
               "ReactOS ntdll!LdrpInitializeProcess is always called with Context != NULL, "
               "which means relocation is impossible? Are you loading a different image?\n"
               "(loaded image base 0x%p)\n", clientProgram);
    }
    
    PTEB currentTeb = NtCurrentTeb();
    DPRINT("Current TEB %p\n", currentTeb);
    
    PPEB currentPeb = currentTeb->ProcessEnvironmentBlock;
    
    wowTeb = NULL;
    wowPeb = NULL;
    procParams = NULL;
    
    size = sizeof(TEB32);
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &wowTeb, 32, &size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        DPRINT("TEB32 Allocation failed: %lx\n", status);
        ASSERT(FALSE);
    }
    
    size = sizeof(PEB32);
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &wowPeb, 32, &size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("PEB32 Allocation failed: %lx\n", status);
        ASSERT(FALSE);
    }
    
    procParams = build_wow64_parameters(currentTeb->ProcessEnvironmentBlock->ProcessParameters);
    wowPeb->ProcessParameters = PtrToUlong(procParams);
    
    /* FIXME: hack for process heaps */
    wowPeb->MaximumNumberOfHeaps = sizeof(fixmeProcessHeaps) / sizeof(*fixmeProcessHeaps);
    wowPeb->ProcessHeaps = PtrToUlong(fixmeProcessHeaps);
    
    wowPeb->ImageBaseAddress = PtrToUlong(clientProgram);
    
    /* Set the 64 bit Teb's TlsSlots[1] to the TEB32 before setting process 
       information. */
    /* FIXME: This is NOT Wine compatible! Wine uses Peb->WowTebOffset which is 
       only present for NTDDI_VERSION >= NTDDI_WIN10, and uses this TLS entry
       for its own structures. Since we do not use them, this entry should be
       free for now. */
    NtCurrentTeb()->TlsSlots[1] = wowTeb;
    
    DPRINT("Initializing PEB32 and TEB32\n");
    wowTeb->NtTib.Self = (ULONG)(ULONG_PTR)wowTeb;
    
    wowTeb->StaticUnicodeString.Length = 0;
    wowTeb->StaticUnicodeString.MaximumLength = sizeof(fixmeStaticUnicodeString);
    wowTeb->StaticUnicodeString.Buffer = PtrToUlong(fixmeStaticUnicodeString);
    
    /* CHECKME */
    wowPeb->OemCodePageData = PtrToUlong(&oemCopy);
    wowPeb->AnsiCodePageData = PtrToUlong(&ansiCopy);
    RtlCopyMemory(&oemCopy, currentPeb->OemCodePageData, sizeof(oemCopy));
    RtlCopyMemory(&ansiCopy, currentPeb->AnsiCodePageData, sizeof(ansiCopy));
    
    /* TODO: Check types - _WOW64_PROCESS has only one field, is this supposed to be the PEB?
       According to https://stackoverflow.com/a/69171561 - yes, it is. This is, however, quite a hacky way 
       to set it. */
    status = NtSetInformationProcess(NtCurrentProcess(), ProcessWow64Information, &wowPeb, sizeof(wowPeb));
    if (!NT_SUCCESS(status))
    {
        DPRINT("Setting info failed: %lx\n", status);
        ASSERT(FALSE);
    }
    
    status = NtQueryInformationProcess(NtCurrentProcess(), ProcessWow64Information, &wowPeb, sizeof(wowPeb), NULL);
    if (!NT_SUCCESS(status))
    {
        DPRINT("Getting info failed: %lx\n", status);
        ASSERT(FALSE);
    }
    
    DPRINT("Got PEB32 address: %p, TEB32 %p\n", wowPeb, NtCurrentTeb()->TlsSlots[1]);
    
    void SetupFs(ULONG_PTR segSelector);
    SetupFs(0x0053);
    
    /* Change image path name */
    procParams->ImagePathName.Buffer = PtrToUlong(currentTeb->ProcessEnvironmentBlock->ProcessParameters->ImagePathName.Buffer);
    procParams->ImagePathName.Length = currentTeb->ProcessEnvironmentBlock->ProcessParameters->ImagePathName.Length;
    procParams->ImagePathName.MaximumLength = currentTeb->ProcessEnvironmentBlock->ProcessParameters->ImagePathName.MaximumLength;
    procParams->Flags |=  RTL_USER_PROCESS_PARAMETERS_NORMALIZED;
    
    wowTeb->ProcessEnvironmentBlock = PtrToUlong(wowPeb);
    
    status = LdrGetProcedureAddress(ntdll32, &importStr, 0, &proc);
    if (!NT_SUCCESS(status)) 
    {
        DPRINT1("Couldn't find LdrInitializeThunk in 32-bit ntdll.dll.\n");
        ASSERT(FALSE);
    }
    DPRINT("Getting init function ptr %p\n", proc);
    
    void Enter32(PVOID where, ULONG_PTR);
    
    DPRINT("Setting WOW32Reserved to local handler %p\n", handler);
    ASSERT((((ULONG_PTR)handler) & ~0xFFFFFFFF) == 0);
    wowTeb->WOW32Reserved = (ULONG)(ULONG_PTR)handler;
    
    //__debugbreak();
    
    /* TODO: this should be in a thread */
    DPRINT("Entering\n");
    Enter32(proc, (ULONG_PTR)ntdll32);
}