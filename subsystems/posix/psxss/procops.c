/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process lifecycle system calls: fork, execve and waitpid
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/kefuncs.h>    // NtWaitForMultipleObjects
#include <ndk/psfuncs.h>    // NtQueryInformationProcess, PROCESS_BASIC_INFORMATION

#define PSX_ENOENT          2
#define PSX_ENOEXEC         8
#define PSX_EACCES          13
#define PSX_ECHILD          10
#define PSX_EAGAIN          11
#define PSX_ENOMEM          12
#define PSX_EINVAL          22
#define PSX_ENAMETOOLONG    38

/* Value placed in the child's Eax so psxdll knows fork() returned in the child */
#define PSX_FORK_CHILD_MARKER   0x7777

/* Largest argv/envp blob handed to the new image as its command line */
#define PSX_EXEC_BLOB_MAX       0x38A4

/**
 * @brief fork() (ApiNumber 0x00). Clones the caller's address space and calling
 * thread, then builds a child record that inherits the caller's POSIX state.
 */
VOID
PsxSrvFork(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    HANDLE ChildProcess = NULL;
    HANDLE ChildThread = NULL;
    HANDLE ParentThread = NULL;
    CONTEXT Context;
    INITIAL_TEB InitialTeb;
    CLIENT_ID ChildClientId;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PPSX_PROCESS Child;
    HANDLE ExceptionPort = g_ApiPort;
    ULONG Zero = 0;
    ULONG Index;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);

    PsxImpersonateClient(Process, Message);
    Status = NtCreateProcess(&ChildProcess,
                             PROCESS_ALL_ACCESS,
                             NULL,
                             Process->ProcessHandle,
                             TRUE,
                             NULL,
                             NULL,
                             ExceptionPort);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
        goto Fail;

    NtSetInformationProcess(ChildProcess, ProcessDefaultHardErrorMode, &Zero, sizeof(Zero));

    /* The child resumes at the fork() return site with Eax set to the child marker */
    Status = NtOpenThread(&ParentThread,
                          THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          &ObjectAttributes,
                          &Message->Header.ClientId);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Context.ContextFlags = CONTEXT_FULL;
    Status = NtGetContextThread(ParentThread, &Context);
    if (!NT_SUCCESS(Status))
        goto Fail;
    Context.Eax = PSX_FORK_CHILD_MARKER;

    /* Body +0x30, +0x34 and +0x38 carry the caller's stack bounds */
    RtlZeroMemory(&InitialTeb, sizeof(InitialTeb));
    InitialTeb.StackBase = (PVOID)Args[0];
    InitialTeb.StackLimit = (PVOID)Args[1];
    InitialTeb.AllocatedStackBase = (PVOID)Args[2];

    Status = NtCreateThread(&ChildThread,
                            THREAD_ALL_ACCESS,
                            NULL,
                            ChildProcess,
                            &ChildClientId,
                            &Context,
                            &InitialTeb,
                            TRUE);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Child = PsxAllocateProcess();
    if (Child == NULL)
        goto Fail;

    Child->ClientId = ChildClientId;
    Child->Pid = (ULONG)(ULONG_PTR)ChildClientId.UniqueProcess;
    Child->ParentPid = Process->Pid;
    Child->ProcessGroup = Process->ProcessGroup;
    Child->SessionId = Process->SessionId;
    Child->Uid = Process->Uid;
    Child->EffectiveUid = Process->EffectiveUid;
    Child->Gid = Process->Gid;
    Child->EffectiveGid = Process->EffectiveGid;
    Child->Umask = Process->Umask;
    Child->ProcessHandle = ChildProcess;
    Child->State = PSX_STATE_RUNNING;

    /* Dispositions and the blocked mask are inherited; nothing is pending */
    Child->BlockedSignals = Process->BlockedSignals;
    RtlCopyMemory(Child->SigActions, Process->SigActions, sizeof(Child->SigActions));

    /*
     * The cloned psxdll already has a live CWD, so the startup exchange is marked
     * done. The strings stay on the record so a later execve can deliver them again.
     */
    RtlCopyMemory(Child->StartupCwd, Process->StartupCwd, sizeof(Child->StartupCwd));
    RtlCopyMemory(Child->StartupRoot, Process->StartupRoot, sizeof(Child->StartupRoot));
    Child->StartupCwdLen = Process->StartupCwdLen;
    Child->StartupRootLen = Process->StartupRootLen;
    Child->StartupBlockValid = Process->StartupBlockValid;
    Child->StartupBlockDone = TRUE;

    for (Index = 0; Index < PSX_OPEN_MAX; Index++)
    {
        if (Process->FdTable[Index] != NULL)
        {
            InterlockedIncrement(&Process->FdTable[Index]->RefCount);
            Child->FdTable[Index] = Process->FdTable[Index];
        }
    }

    PsxInsertProcess(Child);
    NtClose(ParentThread);
    NtResumeThread(ChildThread, NULL);
    NtClose(ChildThread);

    Message->Errno = 0;
    Message->ReturnValue = (LONG)Child->Pid;
    return;

Fail:
    if (ParentThread != NULL)
        NtClose(ParentThread);
    if (ChildThread != NULL)
        NtClose(ChildThread);
    if (ChildProcess != NULL)
    {
        NtTerminateProcess(ChildProcess, 0);
        NtClose(ChildProcess);
    }
    Message->Errno = PSX_EAGAIN;
    Message->ReturnValue = -1;
}

/**
 * @brief execve() (ApiNumber 0x01). Starts a new image under the caller's POSIX
 * pid, session and descriptors, then terminates the old image.
 *
 * Body layout: +0x30 path length, +0x34 client path pointer, +0x38 server view
 * pointer to the argv/envp blob, +0x3C/+0x40 current CWD pointer and length.
 */
VOID
PsxSrvExecve(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    USHORT PathLength = (USHORT)Args[0];
    WCHAR PathBuffer[260];
    UNICODE_STRING ImagePath;
    UNICODE_STRING CommandLine;
    PRTL_USER_PROCESS_PARAMETERS Parameters = NULL;
    RTL_USER_PROCESS_INFORMATION ProcessInfo;
    HANDLE ExceptionPort = g_ApiPort;
    HANDLE OldProcess;
    ULONG_PTR Available;
    ULONG_PTR CwdPointer;
    ULONG CwdLength;
    ULONG Zero = 0;
    SIZE_T BytesRead = 0;
    NTSTATUS Status;

    if ((PathLength == 0) || (PathLength > 0x200))
    {
        Message->Errno = PSX_ENAMETOOLONG;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtReadVirtualMemory(Process->ProcessHandle,
                                 (PVOID)Args[1],
                                 PathBuffer,
                                 PathLength,
                                 &BytesRead);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    ImagePath.Buffer = PathBuffer;
    ImagePath.Length = PathLength;
    ImagePath.MaximumLength = sizeof(PathBuffer);

    /*
     * The argv/envp blob already lives in our mapping of the shared view and is
     * passed as the command line unchanged. The table is self-terminating, so the
     * length is clamped to the end of the view.
     */
    if (!PsxValidateClientPointer(Process, Args[2], sizeof(ULONG)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    Available = Process->ViewEnd - (ULONG_PTR)Args[2];
    if (Available < PSX_EXEC_BLOB_MAX)
        CommandLine.Length = (USHORT)Available;
    else
        CommandLine.Length = (USHORT)PSX_EXEC_BLOB_MAX;
    CommandLine.Buffer = (PWSTR)(ULONG_PTR)Args[2];
    CommandLine.MaximumLength = CommandLine.Length;

    /* The POSIX CWD is delivered through the connect exchange, not here */
    Status = RtlCreateProcessParameters(&Parameters,
                                        &ImagePath,
                                        &NtCurrentPeb()->ProcessParameters->DllPath,
                                        NULL,
                                        &CommandLine,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));
    Status = RtlCreateUserProcess(&ImagePath,
                                  OBJ_CASE_INSENSITIVE,
                                  Parameters,
                                  NULL,
                                  NULL,
                                  NtCurrentProcess(),
                                  TRUE,
                                  NULL,
                                  NULL,
                                  &ProcessInfo);
    RtlDestroyProcessParameters(Parameters);
    if (!NT_SUCCESS(Status))
    {
        /* Shells only fall back to running a file as a script on ENOEXEC */
        switch (Status)
        {
            case STATUS_INVALID_IMAGE_NOT_MZ:
            case STATUS_INVALID_IMAGE_FORMAT:
            case STATUS_INVALID_IMAGE_NE_FORMAT:
            case STATUS_INVALID_IMAGE_LE_FORMAT:
            case STATUS_INVALID_IMAGE_PROTECT:
            case STATUS_INVALID_IMAGE_WIN_16:
                Message->Errno = PSX_ENOEXEC;
                break;

            case STATUS_ACCESS_DENIED:
                Message->Errno = PSX_EACCES;
                break;

            default:
                Message->Errno = PSX_ENOENT;
                break;
        }
        Message->ReturnValue = -1;
        return;
    }

    if (ProcessInfo.ImageInformation.SubSystemType != IMAGE_SUBSYSTEM_POSIX_CUI)
    {
        NtTerminateProcess(ProcessInfo.ProcessHandle, STATUS_INVALID_IMAGE_FORMAT);
        NtClose(ProcessInfo.ProcessHandle);
        NtClose(ProcessInfo.ThreadHandle);
        Message->Errno = PSX_ENOEXEC;
        Message->ReturnValue = -1;
        return;
    }

    NtSetInformationProcess(ProcessInfo.ProcessHandle,
                            ProcessExceptionPort,
                            &ExceptionPort,
                            sizeof(ExceptionPort));
    NtSetInformationProcess(ProcessInfo.ProcessHandle,
                            ProcessDefaultHardErrorMode,
                            &Zero,
                            sizeof(Zero));

    /* Re-key the record to the new image; the old image's exit must not reap it */
    OldProcess = Process->ProcessHandle;
    Process->ExecInProgress = TRUE;
    Process->ClientId = ProcessInfo.ClientId;
    Process->ProcessHandle = ProcessInfo.ProcessHandle;

    /* TODO: Honor FD_CLOEXEC; descriptors are currently all preserved */

    /*
     * The new psxdll starts with an empty CWD, so re-arm the startup exchange.
     * chdir() is client-side state, so the caller's current CWD comes with the request.
     */
    CwdPointer = (ULONG_PTR)Args[3];
    CwdLength = Args[4];
    if ((CwdLength > 0) &&
        (CwdLength < sizeof(Process->StartupCwd)) &&
        PsxValidateClientPointer(Process, Args[3], CwdLength))
    {
        RtlCopyMemory(Process->StartupCwd, (PVOID)CwdPointer, CwdLength);
        Process->StartupCwd[CwdLength] = '\0';
        Process->StartupCwdLen = (USHORT)CwdLength;
        Process->StartupBlockValid = TRUE;
    }
    Process->StartupBlockDone = FALSE;

    NtResumeThread(ProcessInfo.ThreadHandle, NULL);
    NtClose(ProcessInfo.ThreadHandle);

    if (OldProcess != NULL)
    {
        NtTerminateProcess(OldProcess, 0);
        NtClose(OldProcess);
    }

    /* The caller is gone, so this reply is normally never seen */
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief Checks whether Child belongs to Parent and matches the waitpid() pid filter.
 */
static
BOOLEAN
PsxIsWaitMatch(
    _In_ PPSX_PROCESS Child,
    _In_ PPSX_PROCESS Parent,
    _In_ LONG WantPid)
{
    if (Child->ParentPid != Parent->Pid)
        return FALSE;

    /* A specific child */
    if (WantPid > 0)
        return (Child->Pid == (ULONG)WantPid);

    /* The caller's process group */
    if (WantPid == 0)
        return (Child->ProcessGroup == Parent->ProcessGroup);

    /* Any child */
    if (WantPid == -1)
        return TRUE;

    /* Process group -WantPid */
    return (Child->ProcessGroup == (ULONG)(-WantPid));
}

/**
 * @brief waitpid() (ApiNumber 0x02). Reaps a matching zombie child, or blocks until
 * one changes state unless WNOHANG is set.
 *
 * Body layout: +0x30 pid, +0x34 status (reply), +0x38 options.
 */
VOID
PsxSrvWaitpid(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    LONG WantPid = (LONG)Args[0];
    ULONG Options = Args[2];

    /* Only WNOHANG (1) and WUNTRACED (2) are valid */
    if ((Options & ~3u) != 0)
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    for (;;)
    {
        HANDLE WaitHandles[MAXIMUM_WAIT_OBJECTS];
        ULONG HandleCount = 0;
        ULONG LiveChildren = 0;
        PPSX_PROCESS Zombie = NULL;
        PLIST_ENTRY Entry;

        RtlEnterCriticalSection(&g_PsxProcessLock);
        for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
        {
            PPSX_PROCESS Child = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
            PROCESS_BASIC_INFORMATION BasicInfo;

            if (!PsxIsWaitMatch(Child, Process, WantPid))
                continue;

            /*
             * A child that died without calling _exit() never became a zombie and its
             * handle stays signaled. Treat it as killed by SIGKILL so it can be reaped.
             */
            if ((Child->State != PSX_STATE_ZOMBIE) && (Child->ProcessHandle != NULL))
            {
                if (NT_SUCCESS(NtQueryInformationProcess(Child->ProcessHandle,
                                                         ProcessBasicInformation,
                                                         &BasicInfo,
                                                         sizeof(BasicInfo),
                                                         NULL)) &&
                    (BasicInfo.ExitStatus != STATUS_PENDING))
                {
                    Child->State = PSX_STATE_ZOMBIE;
                    Child->ExitStatus = PSX_SIGKILL;
                }
            }

            if (Child->State == PSX_STATE_ZOMBIE)
            {
                Zombie = Child;
                break;
            }

            LiveChildren++;
            if ((HandleCount < MAXIMUM_WAIT_OBJECTS) && (Child->ProcessHandle != NULL))
                WaitHandles[HandleCount++] = Child->ProcessHandle;
        }

        if (Zombie != NULL)
        {
            ULONG ChildPid = Zombie->Pid;
            LONG ChildStatus = Zombie->ExitStatus;
            HANDLE ChildHandle = Zombie->ProcessHandle;

            RemoveEntryList(&Zombie->Entry);
            RtlLeaveCriticalSection(&g_PsxProcessLock);

            if (ChildHandle != NULL)
                NtClose(ChildHandle);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Zombie);

            Args[1] = (ULONG)ChildStatus;
            Message->Errno = 0;
            Message->ReturnValue = (LONG)ChildPid;
            return;
        }
        RtlLeaveCriticalSection(&g_PsxProcessLock);

        if (LiveChildren == 0)
        {
            Message->Errno = PSX_ECHILD;
            Message->ReturnValue = -1;
            return;
        }

        /* WNOHANG with nothing ready */
        if ((Options & 1) != 0)
        {
            Args[1] = 0;
            Message->Errno = 0;
            Message->ReturnValue = 0;
            return;
        }

        /* No handle to wait on; fail instead of spinning */
        if (HandleCount == 0)
        {
            Message->Errno = PSX_ECHILD;
            Message->ReturnValue = -1;
            return;
        }

        /* Other workers keep serving _exit() while this one waits */
        NtWaitForMultipleObjects(HandleCount, WaitHandles, WaitAny, FALSE, NULL);
    }
}
