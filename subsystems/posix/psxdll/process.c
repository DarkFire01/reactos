/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process system calls (fork, exec family, wait, sessions)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/* Keeps the on-stack argv under a page so no stack probe helper is needed */
#define PSX_ARG_MAX 256

/* Status a fork child sees returned from the fork request */
#define PSX_FORK_CHILD_MARKER   0x7777

#define PSX_ENOENT      2
#define PSX_EIO         5
#define PSX_ENOMEM      12

/**
 * @brief Packs argv and envp into one offset table in the shared section.
 *
 * Layout: argv offsets, 0, envp offsets, 0, then the strings. Offsets are byte
 * offsets from the table start. The caller's current directory follows the
 * strings so the server can hand it to the new image.
 *
 * @return The shared section block, or NULL on allocation failure.
 */
static
PVOID
PsxBuildExecBlock(
    _In_opt_ char *const Argv[],
    _In_opt_ char *const Envp[],
    _Out_ PULONG CwdOffset,
    _Out_ PULONG CwdLength)
{
    ULONG ArgCount = 0;
    ULONG EnvCount = 0;
    ULONG TableIndex = 0;
    ULONG StringBytes = 0;
    ULONG TableBytes;
    ULONG TotalBytes;
    ULONG CurrentOffset;
    ULONG CwdLen;
    ULONG Length;
    ULONG i;
    PLONG Table;
    PCHAR Strings;
    PVOID Block;

    if (Argv)
    {
        while (Argv[ArgCount])
            ArgCount++;
    }
    if (Envp)
    {
        while (Envp[EnvCount])
            EnvCount++;
    }

    CwdLen = PsxStartupCwdLen ? PsxStartupCwdLen : PsxStringLengthA(PsxStartupCwd);

    TableBytes = ((ArgCount + 1) + (EnvCount + 1)) * sizeof(LONG);
    for (i = 0; i < ArgCount; i++)
        StringBytes += PsxStringLengthA(Argv[i]) + 1;
    for (i = 0; i < EnvCount; i++)
        StringBytes += PsxStringLengthA(Envp[i]) + 1;
    TotalBytes = TableBytes + StringBytes + CwdLen + 1;

    Block = PsxAllocShared(TotalBytes);
    if (Block == NULL)
        return NULL;

    Table = (PLONG)Block;
    Strings = (PCHAR)Block + TableBytes;
    CurrentOffset = TableBytes;

    for (i = 0; i < ArgCount; i++)
    {
        Length = PsxStringLengthA(Argv[i]) + 1;
        Table[TableIndex++] = (LONG)CurrentOffset;
        RtlCopyMemory(Strings, Argv[i], Length);
        Strings += Length;
        CurrentOffset += Length;
    }
    Table[TableIndex++] = 0;

    for (i = 0; i < EnvCount; i++)
    {
        Length = PsxStringLengthA(Envp[i]) + 1;
        Table[TableIndex++] = (LONG)CurrentOffset;
        RtlCopyMemory(Strings, Envp[i], Length);
        Strings += Length;
        CurrentOffset += Length;
    }
    Table[TableIndex++] = 0;

    RtlCopyMemory(Strings, PsxStartupCwd, CwdLen);
    Strings[CwdLen] = '\0';
    *CwdOffset = CurrentOffset;
    *CwdLength = CwdLen;
    return Block;
}

/**
 * @brief Replaces the process image. Returns only on failure.
 */
int
__cdecl
execve(
    _In_z_ const char *Path,
    _In_opt_ char *const Argv[],
    _In_opt_ char *const Envp[])
{
    PSX_API_MESSAGE Message;
    CHAR NtAnsi[PSX_PATH_MAX * 2];
    /* Static so it stays valid while the server reads it */
    static WCHAR NtWide[PSX_PATH_MAX * 2];
    ULONG AnsiLength;
    ULONG CwdOffset = 0;
    ULONG CwdLength = 0;
    ULONG i;
    PULONG Args;
    PVOID Block;
    LONG Result;

    AnsiLength = PsxBuildNtPath(Path, NtAnsi, sizeof(NtAnsi));
    if (AnsiLength == 0)
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }

    for (i = 0; i < AnsiLength; i++)
        NtWide[i] = (WCHAR)(UCHAR)NtAnsi[i];

    Block = PsxBuildExecBlock(Argv, Envp, &CwdOffset, &CwdLength);
    if (Block == NULL)
    {
        PsxSetErrno(PSX_ENOMEM);
        return -1;
    }

    /* The image path is read from the caller address space, the rest from the shared section */
    PsxInitMessage(&Message, PsxApiExecve, PSX_BODY_DATALEN(5 * sizeof(ULONG)));
    Args = (PULONG)Message.Data.Raw;
    Args[0] = AnsiLength * sizeof(WCHAR);
    Args[1] = (ULONG)(ULONG_PTR)NtWide;
    Args[2] = PsxServerPtr(Block);
    Args[3] = PsxServerPtr((PCHAR)Block + CwdOffset);
    Args[4] = CwdLength;

    Result = PsxCallServer(&Message);
    PsxFreeShared(Block);
    return (int)Result;
}

int
__cdecl
execv(
    _In_z_ const char *Path,
    _In_opt_ char *const Argv[])
{
    char **Env = (PsxEnvironLocation != NULL) ? *PsxEnvironLocation : NULL;

    return execve(Path, Argv, (char *const *)Env);
}

/**
 * @brief Like execv, but searches PATH when File has no slash.
 */
int
__cdecl
execvp(
    _In_z_ const char *File,
    _In_opt_ char *const Argv[])
{
    const char *SearchPath;
    const char *Char;
    char Candidate[PSX_PATH_MAX];
    ULONG FileIndex;
    ULONG Length;

    for (Char = File; *Char != '\0'; Char++)
    {
        if (*Char == '/')
            return execv(File, Argv);
    }

    SearchPath = getenv("PATH");
    if (SearchPath == NULL)
        SearchPath = "/bin:/usr/bin";

    while (*SearchPath != '\0')
    {
        Length = 0;
        while (*SearchPath != '\0' && *SearchPath != ':' && Length < sizeof(Candidate) - 2)
            Candidate[Length++] = *SearchPath++;

        if (Length > 0 && Candidate[Length - 1] != '/')
            Candidate[Length++] = '/';

        for (FileIndex = 0; File[FileIndex] != '\0' && Length < sizeof(Candidate) - 1; FileIndex++)
            Candidate[Length++] = File[FileIndex];
        Candidate[Length] = '\0';

        /* Returns only on failure, then try the next directory */
        execv(Candidate, Argv);
        if (*SearchPath == ':')
            SearchPath++;
    }

    PsxSetErrno(PSX_ENOENT);
    return -1;
}

/**
 * @brief Acknowledges a completed fork so the server can finish it.
 */
static
VOID
PsxPostOpSync(VOID)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiClearExecFlag, PSX_BODY_DATALEN(0));
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);
}

/**
 * @brief Creates a child process.
 *
 * The child resumes from the port request with PSX_FORK_CHILD_MARKER as its
 * status, so the request is sent directly instead of through PsxCallServer.
 */
int
__cdecl
fork(void)
{
    PSX_API_MESSAGE Message;
    PTEB Teb = NtCurrentTeb();
    NTSTATUS Status;

    PsxInitMessage(&Message, PsxApiFork, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)(ULONG_PTR)Teb->NtTib.StackBase;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)Teb->NtTib.StackLimit;
    ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Teb->DeallocationStack;

    Status = NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);

    if (Status == (NTSTATUS)PSX_FORK_CHILD_MARKER)
    {
        /* The inherited connection belongs to the parent, so open a new one */
        PsxApiPort = NULL;
        PsxSharedHeap = NULL;
        PsxTtyForkReset();
        if (!NT_SUCCESS(PsxInitialize()))
            NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)1);

        PsxPostOpSync();
        return 0;
    }

    if (!NT_SUCCESS(Status))
    {
        PsxSetErrno(PSX_EIO);
        return -1;
    }
    if (Message.Errno != 0)
    {
        PsxSetErrno(Message.Errno);
        return -1;
    }

    /* Parent: the child pid */
    return (int)Message.ReturnValue;
}

/**
 * @brief Collects variadic exec arguments up to and including the terminating NULL.
 */
static
VOID
PsxCollectArgs(
    _Out_writes_(PSX_ARG_MAX) const char *Argv[],
    _In_opt_z_ const char *Arg0,
    _Inout_ va_list *Args)
{
    const char *Arg;
    int Count = 0;

    Argv[Count++] = Arg0;
    while (Count < PSX_ARG_MAX - 1)
    {
        Arg = va_arg(*Args, const char *);
        Argv[Count++] = Arg;
        if (Arg == NULL)
            break;
    }

    Argv[PSX_ARG_MAX - 1] = NULL;
}

int
__cdecl
execl(
    _In_z_ const char *Path,
    _In_opt_z_ const char *Arg0,
    ...)
{
    const char *Argv[PSX_ARG_MAX];
    va_list Args;

    va_start(Args, Arg0);
    PsxCollectArgs(Argv, Arg0, &Args);
    va_end(Args);

    return execv(Path, (char *const *)Argv);
}

int
__cdecl
execlp(
    _In_z_ const char *File,
    _In_opt_z_ const char *Arg0,
    ...)
{
    const char *Argv[PSX_ARG_MAX];
    va_list Args;

    va_start(Args, Arg0);
    PsxCollectArgs(Argv, Arg0, &Args);
    va_end(Args);

    return execvp(File, (char *const *)Argv);
}

/**
 * @brief Like execl, but the environment pointer follows the terminating NULL.
 */
int
__cdecl
execle(
    _In_z_ const char *Path,
    _In_opt_z_ const char *Arg0,
    ...)
{
    const char *Argv[PSX_ARG_MAX];
    char *const *Envp;
    va_list Args;

    va_start(Args, Arg0);
    PsxCollectArgs(Argv, Arg0, &Args);
    Envp = va_arg(Args, char *const *);
    va_end(Args);

    return execve(Path, (char *const *)Argv, Envp);
}

int
__cdecl
setsid(void)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSetsid, PSX_BODY_DATALEN(0));
    return (int)PsxCallServer(&Message);
}

int
__cdecl
setpgid(
    _In_ int Pid,
    _In_ int Pgid)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSetpgid, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Pgid;
    return (int)PsxCallServer(&Message);
}

/**
 * @brief Waits for a child. Pid goes in slot 0 and Options in slot 2; the
 * child's status comes back in slot 1.
 */
int
__cdecl
waitpid(
    _In_ int Pid,
    _Out_opt_ int *Status,
    _In_ int Options)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiWaitpid, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Options;

    Result = PsxCallServer(&Message);
    if (Result > 0 && Status != NULL)
        *Status = (int)((PULONG)Message.Data.Raw)[1];

    return (int)Result;
}

int
__cdecl
wait(
    _Out_opt_ int *Status)
{
    return waitpid(-1, Status, 0);
}

/**
 * @brief Runs Command with "/bin/sh -c" and waits for it. A NULL Command
 * reports that a shell is available.
 */
int
__cdecl
system(
    _In_opt_z_ const char *Command)
{
    int Pid;
    int Status = 0;

    if (Command == NULL)
        return 1;

    Pid = fork();
    if (Pid < 0)
        return -1;

    if (Pid == 0)
    {
        execl("/bin/sh", "sh", "-c", Command, (char *)0);
        _exit(127);
    }

    if (waitpid(Pid, &Status, 0) < 0)
        return -1;

    return Status;
}
