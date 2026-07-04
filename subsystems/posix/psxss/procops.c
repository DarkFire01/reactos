/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process-lifecycle syscalls: fork (kernel address-space clone),
 *              execve (image replacement keeping the POSIX pid), and waitpid
 *              (reap zombie children). Faithful to the NT 4.0 handlers
 *              sub_1F4ABD3 (fork), sub_1F4AE55 (execve), sub_1F4B4FA (waitpid).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/kefuncs.h>    // NtWaitForMultipleObjects
#include <ndk/psfuncs.h>    // NtQueryInformationProcess, PROCESS_BASIC_INFORMATION

#define PSX_ENOENT      2
#define PSX_ENOEXEC     8
#define PSX_EACCES      13
#define PSX_ECHILD      10
#define PSX_EAGAIN      11
#define PSX_ENOMEM      12
#define PSX_EINVAL      22
#define PSX_ENAMETOOLONG 38

// The child's fork() syscall site returns this sentinel in Eax; psxdll reads it
// as "you are the child" and re-attaches to the subsystem.
#define PSX_FORK_CHILD_MARKER   0x7777

//
// fork() -- ApiNumber 0x00. Clone the parent's address space (NtCreateProcess
// with the parent as the source), clone the parent's calling-thread context with
// Eax = the child marker, create the child thread, and build the child's process
// record inheriting the parent's POSIX state. The parent receives the child pid.
//
VOID
PsxSrvFork(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;        // body +0x30, +0x34, +0x38
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

    // Clone the parent address space (the kernel-side fork).
    PsxImpersonateClient(Process, Message);
    Status = NtCreateProcess(&ChildProcess, PROCESS_ALL_ACCESS, NULL,
                             Process->ProcessHandle, TRUE, NULL, NULL, ExceptionPort);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
        goto Fail;

    NtSetInformationProcess(ChildProcess, ProcessDefaultHardErrorMode, &Zero, sizeof(ULONG));

    // Clone the parent's calling-thread CONTEXT; the child resumes at the fork()
    // return site with Eax = the child marker.
    Status = NtOpenThread(&ParentThread, THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          &ObjectAttributes, &Message->Header.ClientId);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Context.ContextFlags = CONTEXT_FULL;
    Status = NtGetContextThread(ParentThread, &Context);
    if (!NT_SUCCESS(Status))
        goto Fail;
    Context.Eax = PSX_FORK_CHILD_MARKER;

    RtlZeroMemory(&InitialTeb, sizeof(InitialTeb));
    InitialTeb.StackBase = (PVOID)Args[0];          // +0x30
    InitialTeb.StackLimit = (PVOID)Args[1];         // +0x34
    InitialTeb.AllocatedStackBase = (PVOID)Args[2]; // +0x38

    Status = NtCreateThread(&ChildThread, THREAD_ALL_ACCESS, NULL, ChildProcess,
                            &ChildClientId, &Context, &InitialTeb, TRUE);
    if (!NT_SUCCESS(Status))
        goto Fail;

    // Build the child's process record, inheriting the parent's POSIX state.
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

    // Inherit the parent's signal dispositions + blocked mask; nothing pending.
    Child->BlockedSignals = Process->BlockedSignals;
    RtlCopyMemory(Child->SigActions, Process->SigActions, sizeof(Child->SigActions));

    // Inherit the connect startup-block strings (CWD/root). The fork child's own
    // psxdll CWD is already a live copy via the address-space clone, so mark the
    // exchange done -- but keep the strings on the record so a later execve (which
    // starts a fresh psxdll with an empty CWD) can re-arm and re-deliver them.
    RtlCopyMemory(Child->StartupCwd, Process->StartupCwd, sizeof(Child->StartupCwd));
    RtlCopyMemory(Child->StartupRoot, Process->StartupRoot, sizeof(Child->StartupRoot));
    Child->StartupCwdLen = Process->StartupCwdLen;
    Child->StartupRootLen = Process->StartupRootLen;
    Child->StartupBlockValid = Process->StartupBlockValid;
    Child->StartupBlockDone = TRUE;

    // Share each inherited descriptor.
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
    Message->ReturnValue = (LONG)Child->Pid;        // parent gets the child pid
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

//
// execve(path, argv, envp) -- ApiNumber 0x01. Replace the caller's image while
// keeping its POSIX pid/session/fds: create a new NT process from the image,
// re-key this process record to it, and terminate the old image. Successful
// exec does not return to the caller.
//   body +0x30: image NT path length (low word)
//   body +0x34: client pointer to the image NT path (UNICODE)
//   body +0x38: client pointer to the command line
//
VOID
PsxSrvExecve(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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
    ULONG Zero = 0;
    SIZE_T BytesRead = 0;
    NTSTATUS Status;

    if ((PathLength == 0) || (PathLength > 0x200))
    {
        Message->Errno = PSX_ENAMETOOLONG;
        Message->ReturnValue = -1;
        return;
    }

    // Pull the image NT path out of the caller's address space.
    Status = NtReadVirtualMemory(Process->ProcessHandle, (PVOID)Args[1],
                                 PathBuffer, PathLength, &BytesRead);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    ImagePath.Buffer = PathBuffer;
    ImagePath.Length = PathLength;
    ImagePath.MaximumLength = sizeof(PathBuffer);

    // argv + envp: the client marshals them into ONE self-relative offset-table
    // blob (the same format the child's crt0 walks) allocated in the shared LPC
    // view, and sends a single already-translated server-view pointer at body
    // +0x38 = Args[2]. It points into our mapping of the view, so we use it
    // DIRECTLY -- no NtReadVirtualMemory. Hand it verbatim to
    // RtlCreateProcessParameters as CommandLine with Environment = NULL, exactly
    // like the session-spawn path. Faithful to the real psxss execve (sub_1F4AE55):
    // CommandLine{Length=0x38A4, Buffer=blob}, Environment=0. The offset table is
    // self-terminating, so clamping Length to the mapped view is safe (and stops a
    // bogus pointer from over-reading our address space).
    if (!PsxValidateClientPointer(Process, Args[2], sizeof(ULONG)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }
    {
        ULONG_PTR Avail = Process->ViewEnd - (ULONG_PTR)Args[2];
        CommandLine.Length = (Avail < 0x38A4) ? (USHORT)Avail : (USHORT)0x38A4;
    }
    CommandLine.Buffer = (PWSTR)(ULONG_PTR)Args[2];
    CommandLine.MaximumLength = CommandLine.Length;

    Status = RtlCreateProcessParameters(&Parameters, &ImagePath,
                                        &NtCurrentPeb()->ProcessParameters->DllPath,
                                        NULL /* POSIX cwd is delivered via the connect exchange */,
                                        &CommandLine, NULL /* Environment */,
                                        NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));
    Status = RtlCreateUserProcess(&ImagePath, OBJ_CASE_INSENSITIVE, Parameters, NULL, NULL,
                                  NtCurrentProcess(), TRUE, NULL, NULL, &ProcessInfo);
    RtlDestroyProcessParameters(Parameters);
    if (!NT_SUCCESS(Status))
    {
        // The errno here is a contract with the shell: ENOEXEC is the ONLY
        // value that makes bash/sh fall back to reading the file itself and
        // running it as a script (#! parsing is client-side in the shell), so
        // every "the file exists but the loader can't map it as a PE" status
        // must map to it. Everything else keeps the old ENOENT behavior.
        switch (Status)
        {
            case STATUS_INVALID_IMAGE_NOT_MZ:       // shell scripts, text files
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

    NtSetInformationProcess(ProcessInfo.ProcessHandle, ProcessExceptionPort,
                            &ExceptionPort, sizeof(HANDLE));
    NtSetInformationProcess(ProcessInfo.ProcessHandle, ProcessDefaultHardErrorMode,
                            &Zero, sizeof(ULONG));

    // Identity transfer: the SAME POSIX pid continues with the new image. Re-key
    // this record to the new NT process and tear down the old one. The old
    // process's imminent death must NOT reap this record.
    OldProcess = Process->ProcessHandle;
    Process->ExecInProgress = TRUE;
    Process->ClientId = ProcessInfo.ClientId;
    Process->ProcessHandle = ProcessInfo.ProcessHandle;
    // fds are preserved across exec (TODO: honor FD_CLOEXEC).

    // The new image is a fresh psxdll with an empty CWD, so re-arm the connect
    // startup-block exchange: on its ApiPort connect it must receive the CWD/root
    // path-translation strings -- otherwise relative paths (".") resolve to "\"
    // in the execve'd image. The CWD must be the caller's CURRENT one, not the
    // spawn-time snapshot on this record: chdir() is client-side psxdll state
    // the server never hears about, so execve ships it at +0x3C/+0x40 (appended
    // to the argv/env blob in the shared view).
    {
        ULONG_PTR CwdPtr = (ULONG_PTR)Args[3];
        ULONG CwdLen = Args[4];

        if ((CwdLen > 0) && (CwdLen < sizeof(Process->StartupCwd)) &&
            PsxValidateClientPointer(Process, Args[3], CwdLen))
        {
            RtlCopyMemory(Process->StartupCwd, (PVOID)CwdPtr, CwdLen);
            Process->StartupCwd[CwdLen] = '\0';
            Process->StartupCwdLen = (USHORT)CwdLen;
            Process->StartupBlockValid = TRUE;
        }
    }
    Process->StartupBlockDone = FALSE;

    NtResumeThread(ProcessInfo.ThreadHandle, NULL);
    NtClose(ProcessInfo.ThreadHandle);

    if (OldProcess != NULL)
    {
        NtTerminateProcess(OldProcess, 0);
        NtClose(OldProcess);
    }

    // The caller is gone; the reply (if delivered) is harmless.
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// Does Child belong to Parent and match the waitpid pid filter?
//   pid > 0  -> that specific child;  pid == 0 -> the caller's process group;
//   pid == -1 -> any child;           pid < -1 -> process group (-pid).
//
static BOOLEAN
PsxIsWaitMatch(IN PPSX_PROCESS Child, IN PPSX_PROCESS Parent, IN LONG WantPid)
{
    if (Child->ParentPid != Parent->Pid)
        return FALSE;
    if (WantPid > 0)
        return (Child->Pid == (ULONG)WantPid);
    if (WantPid == 0)
        return (Child->ProcessGroup == Parent->ProcessGroup);
    if (WantPid == -1)
        return TRUE;
    return (Child->ProcessGroup == (ULONG)(-WantPid));
}

//
// waitpid(pid, &status, options) -- ApiNumber 0x02. Reap a matching zombie
// child (returning pid + status); ECHILD if there are none; with WNOHANG return
// 0 if none are ready; otherwise block until a matching child changes state.
//   body +0x30: pid;  body +0x34: status (reply);  body +0x38: options
//
VOID
PsxSrvWaitpid(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    LONG WantPid = (LONG)Args[0];
    ULONG Options = Args[2];

    // Only WNOHANG (1) and WUNTRACED (2) are valid.
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
            if (!PsxIsWaitMatch(Child, Process, WantPid))
                continue;

            // A child that terminated WITHOUT calling _exit() (crashed, killed by
            // an exception, or a failed image-load in ntdll) never became a zombie
            // -- and its process handle is permanently signaled, so a naive wait
            // spins forever. Detect an already-dead process here and zombify it
            // (WIFSIGNALED: killed by a signal) so waitpid() reaps it.
            if ((Child->State != PSX_STATE_ZOMBIE) && (Child->ProcessHandle != NULL))
            {
                PROCESS_BASIC_INFORMATION Pbi;
                if (NT_SUCCESS(NtQueryInformationProcess(Child->ProcessHandle,
                        ProcessBasicInformation, &Pbi, sizeof(Pbi), NULL)) &&
                    (Pbi.ExitStatus != STATUS_PENDING))
                {
                    Child->State = PSX_STATE_ZOMBIE;
                    Child->ExitStatus = PSX_SIGKILL;    // low 7 bits = signal
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

            Args[1] = (ULONG)ChildStatus;           // body +0x34: status out
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

        if ((Options & 1) != 0)                     // WNOHANG: nothing ready
        {
            Args[1] = 0;
            Message->Errno = 0;
            Message->ReturnValue = 0;
            return;
        }

        if (HandleCount == 0)
        {
            // No handle to wait on (shouldn't happen) -- avoid a busy spin.
            Message->Errno = PSX_ECHILD;
            Message->ReturnValue = -1;
            return;
        }

        // Block until one of the matching children changes state, then re-scan.
        // (Runs on one of several worker threads, so a child's _exit -- served by
        // another worker -- can still make progress.)
        NtWaitForMultipleObjects(HandleCount, WaitHandles, WaitAny, FALSE, NULL);
    }
}
