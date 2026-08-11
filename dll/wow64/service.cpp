#include "ros_wow64_private.h"

#define WINE_WOW_IMPL_CASE(name) case Num ## name: {\
    NTSTATUS WINAPI wow64_Nt ## name (UINT* pArgs); \
    status = wow64_Nt ## name ((UINT*)(PVOID)pArgs); \
    break; }

static
NTSTATUS 
Wow64WinHandler(ULONG syscallNum, 
                UINT* pArgs);

extern "C"
NTSTATUS
WINAPI
Wow64SystemServiceEx(ULONG syscallNum, 
                     UINT* pArgs)
{   
    NTSTATUS status;

    static const char* mapping[] = 
    {
#define SVC_(name, argc) ""#name ,
#include "../../../ntoskrnl/include/sysfuncs.h"   
#undef SVC_
    };

#define BUILD_WOW6432
#define SVC_(name, argc) constexpr int Num ## name = __COUNTER__;
#include "../../../ntoskrnl/include/sysfuncs.h"   
#undef SVC_
#undef BUILD_WOW6432
    
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
        WINE_WOW_IMPL_CASE(QueryKey);
        WINE_WOW_IMPL_CASE(DeleteValueKey);
        WINE_WOW_IMPL_CASE(CreateKey);
        WINE_WOW_IMPL_CASE(DeleteKey);
        WINE_WOW_IMPL_CASE(EnumerateKey);
        WINE_WOW_IMPL_CASE(EnumerateValueKey);
        WINE_WOW_IMPL_CASE(SetValueKey);

        /* security.c */
        WINE_WOW_IMPL_CASE(AccessCheck);
        WINE_WOW_IMPL_CASE(AdjustGroupsToken);
        WINE_WOW_IMPL_CASE(AdjustPrivilegesToken);
        WINE_WOW_IMPL_CASE(CreateToken);
        WINE_WOW_IMPL_CASE(FilterToken);
        WINE_WOW_IMPL_CASE(DuplicateToken);
        WINE_WOW_IMPL_CASE(CompareTokens);
        WINE_WOW_IMPL_CASE(ImpersonateAnonymousToken);
        WINE_WOW_IMPL_CASE(OpenProcessToken);
        WINE_WOW_IMPL_CASE(OpenProcessTokenEx);
        WINE_WOW_IMPL_CASE(OpenThreadToken);
        WINE_WOW_IMPL_CASE(OpenThreadTokenEx);
        WINE_WOW_IMPL_CASE(PrivilegeCheck);
        WINE_WOW_IMPL_CASE(QueryInformationToken);
        WINE_WOW_IMPL_CASE(QuerySecurityObject);
        WINE_WOW_IMPL_CASE(SetInformationToken);
        WINE_WOW_IMPL_CASE(SetSecurityObject);

        /* system.c */
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
        WINE_WOW_IMPL_CASE(Continue);
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
        
        /* wow64.c */
        WINE_WOW_IMPL_CASE(OpenProcess);
        WINE_WOW_IMPL_CASE(OpenThread);
        WINE_WOW_IMPL_CASE(CreateThread);
        WINE_WOW_IMPL_CASE(CreateProcess);
        WINE_WOW_IMPL_CASE(CreateProcessEx);
        WINE_WOW_IMPL_CASE(QueryInformationThread);
        WINE_WOW_IMPL_CASE(QueryInformationProcess);
        WINE_WOW_IMPL_CASE(SetInformationProcess);
        WINE_WOW_IMPL_CASE(ResumeThread);
        WINE_WOW_IMPL_CASE(ApphelpCacheControl);
        WINE_WOW_IMPL_CASE(SuspendThread);
        WINE_WOW_IMPL_CASE(GetContextThread);

        /* csr.c */
        WINE_WOW_IMPL_CASE(Wow64CsrAllocateCaptureBuffer);
        WINE_WOW_IMPL_CASE(Wow64CsrAllocateMessagePointer);
        WINE_WOW_IMPL_CASE(Wow64CsrCaptureMessageBuffer);
        WINE_WOW_IMPL_CASE(Wow64CsrCaptureMessageString);
        WINE_WOW_IMPL_CASE(Wow64CsrClientCallServer);
        WINE_WOW_IMPL_CASE(Wow64CsrClientConnectToServer);
        WINE_WOW_IMPL_CASE(Wow64CsrFreeCaptureBuffer);
        WINE_WOW_IMPL_CASE(Wow64CsrGetProcessId);
        WINE_WOW_IMPL_CASE(Wow64CsrNewThread);
        WINE_WOW_IMPL_CASE(Wow64CsrIdentifyAlertableThread);
        WINE_WOW_IMPL_CASE(Wow64CsrSetPriorityClass);
        WINE_WOW_IMPL_CASE(Wow64CsrProbeForWrite);
        WINE_WOW_IMPL_CASE(Wow64CsrProbeForRead);
        WINE_WOW_IMPL_CASE(Wow64CsrCaptureMessageMultiUnicodeStringsInPlace);
        WINE_WOW_IMPL_CASE(Wow64CsrCaptureTimeout);

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
                status = Wow64WinHandler(syscallNum - 0x1000, pArgs);
                Wow64FreeTempData();
                return status;
            }
            
            if (syscallNum < sizeof(mapping) / sizeof(*mapping))
            {
                DPRINT1("[Syscall %lX:%hs] ", syscallNum, mapping[syscallNum]);
            }
            else
            {
                DPRINT1("[Syscall %lX:???] ", syscallNum);
            }
            DPRINT1("WARNING: Unhandled 32-bit syscall 0x%lX(args at %p)\n", syscallNum, pArgs);
            status = STATUS_NOT_IMPLEMENTED;
        }
    }

    Wow64FreeTempData();
    return status;
}

static
NTSTATUS 
Wow64WinHandler(ULONG syscallNum, 
                UINT* pArgs)
{
    ANSI_STRING ImportStr = RTL_CONSTANT_STRING("sdwhwin32");
    static PSYSTEM_SERVICE_TABLE pServiceTable = NULL; 
    static PVOID Wow64WinDll = NULL;
    UNICODE_STRING Wow64WinDllStr = RTL_CONSTANT_STRING(L"wow64win.dll");
    NTSTATUS Status;
        
    ULONG_PTR *HandlerTable = NULL;
    NTSTATUS (*Service)(UINT* pArgs) = NULL;
    
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
        
        Status = LdrGetProcedureAddress(Wow64WinDll, 
                                        &ImportStr,
                                        0, 
                                        (PVOID*)&pServiceTable);
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

#if 0
    if (pServiceTable->ArgumentTable != NULL)
    {
        if (pServiceTable->ArgumentTable[syscallNum] != numArgs)
        {
            DPRINT1("WARNING: Wow64Win service expects a different number of "
                    "arguments than %d for %lX:%s.\n", 
                    numArgs, syscallNum, mapping[syscallNum]);
        }
    }
#endif
    
    Service = (decltype(Service))HandlerTable[syscallNum];
    return Service(pArgs);
}