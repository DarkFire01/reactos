/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL process syscalls. The variadic exec wrappers build an
 *              argv/envp vector and forward to execv/execvp/execve (the vector
 *              forms are the ones that carry the server marshalling). fork and
 *              waitpid remain server-side TODO.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

// Keep the on-stack argv vector under a 4 KiB page so MSVC emits no __chkstk
// probe (psxdll links only ntdll -- no CRT stack helpers). 256 args is ample
// for the exec list forms.
#define PSX_ARG_MAX 256

// The child thread the server clones resumes from the fork LPC call with Eax set
// to this marker (its apparent return value); the parent's call returns normally.
#define PSX_FORK_CHILD_MARKER  0x7777

static ULONG PsxStrLen(PCSTR s) { PCSTR p = s; while (*p) p++; return (ULONG)(p - s); }

//
// Marshal argv[] + envp[] into ONE self-relative offset table in the shared
// section (the format crt0 unpacks: argv int32 offsets, NUL, env int32 offsets,
// NUL, then the packed NUL-terminated strings; each offset is byte-relative to
// the table start). The server passes it verbatim to the new image's command
// line. Returns a shared-section block (server-relative address via PsxServerPtr).
//
static PVOID
PsxBuildExecBlock(char *const Argv[], char *const Envp[])
{
    ULONG nargv = 0, nenv = 0, i, t = 0, off, strBytes = 0, tableBytes, total;
    PLONG Table;
    PCHAR Strings;
    PVOID Blob;

    if (Argv) while (Argv[nargv]) nargv++;
    if (Envp) while (Envp[nenv]) nenv++;

    tableBytes = ((nargv + 1) + (nenv + 1)) * sizeof(LONG);
    for (i = 0; i < nargv; i++) strBytes += PsxStrLen(Argv[i]) + 1;
    for (i = 0; i < nenv; i++)  strBytes += PsxStrLen(Envp[i]) + 1;
    total = tableBytes + strBytes;

    Blob = PsxAllocShared(total);
    if (Blob == NULL)
        return NULL;
    Table = (PLONG)Blob;
    Strings = (PCHAR)Blob + tableBytes;
    off = tableBytes;

    for (i = 0; i < nargv; i++)
    {
        ULONG len = PsxStrLen(Argv[i]) + 1;
        Table[t++] = (LONG)off;                 // byte offset to the string
        RtlCopyMemory(Strings, Argv[i], len);
        Strings += len; off += len;
    }
    Table[t++] = 0;                             // argv terminator
    for (i = 0; i < nenv; i++)
    {
        ULONG len = PsxStrLen(Envp[i]) + 1;
        Table[t++] = (LONG)off;
        RtlCopyMemory(Strings, Envp[i], len);
        Strings += len; off += len;
    }
    Table[t++] = 0;                             // env terminator
    return Blob;
}

//
// execve(path, argv, envp) -- ApiNumber 0x01. Sends the wide NT image path (raw
// pointer, server reads via NtReadVirtualMemory) plus the argv/env offset table.
// On success the image is replaced and this never returns.
//
int __cdecl
execve(const char *Path, char *const Argv[], char *const Envp[])
{
    PSX_API_MESSAGE Message;
    CHAR NtAnsi[PSX_PATH_MAX * 2];
    static WCHAR NtWide[PSX_PATH_MAX * 2];      // static: outlives the LPC read
    ULONG AnsiLen, i;
    PVOID Blob;
    LONG Result;

    AnsiLen = PsxBuildNtPath(Path, NtAnsi, sizeof(NtAnsi));
    if (AnsiLen == 0) { PsxSetErrno(2 /* ENOENT */); return -1; }
    for (i = 0; i < AnsiLen; i++)
        NtWide[i] = (WCHAR)(UCHAR)NtAnsi[i];

    Blob = PsxBuildExecBlock(Argv, Envp);
    if (Blob == NULL) { PsxSetErrno(12 /* ENOMEM */); return -1; }

    PsxInitMessage(&Message, PsxApiExecve, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = AnsiLen * sizeof(WCHAR);            // +0x30 path bytes
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)NtWide;           // +0x34 raw wide-path ptr
    ((PULONG)Message.Data.Raw)[2] = PsxServerPtr(Blob);                 // +0x38 offset table

    Result = PsxCallServer(&Message);           // returns only on failure
    PsxFreeShared(Blob);
    return (int)Result;
}

//
// execv(path, argv) -- execve with the inherited environment.
//
int __cdecl
execv(const char *Path, char *const Argv[])
{
    char **Env = (PsxEnvironLocation != NULL) ? *PsxEnvironLocation : NULL;
    return execve(Path, Argv, (char *const *)Env);
}

//
// execvp(file, argv) -- like execv but searches $PATH when file has no '/'.
//
int __cdecl
execvp(const char *File, char *const Argv[])
{
    const char *Path;
    char Candidate[PSX_PATH_MAX];
    ULONG fi;
    const char *p;

    // A path with a slash is used verbatim.
    for (p = File; *p != '\0'; p++)
        if (*p == '/')
            return execv(File, Argv);

    Path = getenv("PATH");
    if (Path == NULL)
        Path = "/bin:/usr/bin";

    while (*Path != '\0')
    {
        ULONG di = 0;
        while (*Path != '\0' && *Path != ':' && di < sizeof(Candidate) - 2)
            Candidate[di++] = *Path++;
        if (di > 0 && Candidate[di - 1] != '/')
            Candidate[di++] = '/';
        for (fi = 0; File[fi] != '\0' && di < sizeof(Candidate) - 1; fi++)
            Candidate[di++] = File[fi];
        Candidate[di] = '\0';

        execv(Candidate, Argv);         // returns only on failure; try next dir
        if (*Path == ':')
            Path++;
    }
    PsxSetErrno(2 /* ENOENT */);
    return -1;
}

//
// PsxPostOpSync() -- ApiNumber 0x3E. A bare ack the real psxdll fires after a
// data-channel transfer / fork-child re-attach so the server finalizes (it also
// clears the exec-in-progress flag; a no-op for a fresh fork child).
//
static VOID
PsxPostOpSync(VOID)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiClearExecFlag, PSX_BODY_DATALEN(0));
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);
}

//
// fork() -- ApiNumber 0x00. The server (NtCreateProcess + NtCreateThread with a
// cloned CONTEXT) makes the child resume from the port call below with Eax ==
// PSX_FORK_CHILD_MARKER. We therefore call NtRequestWaitReplyPort DIRECTLY and
// read that return status, rather than going through PsxCallServer (which reads
// the reply's ReturnValue -- meaningless in the freshly-cloned child). The
// child thread's INITIAL_TEB stack bounds come from our TEB.
//
int __cdecl
fork(void)
{
    PSX_API_MESSAGE Message;
    PTEB Teb = NtCurrentTeb();
    NTSTATUS Status;

    PsxInitMessage(&Message, PsxApiFork, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)(ULONG_PTR)Teb->NtTib.StackBase;    // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)Teb->NtTib.StackLimit;   // +0x34
    ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Teb->DeallocationStack;  // +0x38

    Status = NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);

    if (Status == (NTSTATUS)PSX_FORK_CHILD_MARKER)
    {
        // We are the child. The inherited ApiPort connection is the PARENT's --
        // its shared section and pointer delta are wrong for us -- so re-attach
        // with our OWN connection (psxss maps our fresh section + hands back the
        // correct ViewRemoteBase). Faithful to the real psxdll fork child
        // (PsxConnectApiPort again, then PsxPostOpSync = ApiNumber 0x3E). The
        // controlling-tty session view is likewise dropped; it reconnects on the
        // child's first tty op.
        PsxApiPort = NULL;
        PsxSharedHeap = NULL;
        PsxTtyForkReset();
        if (!NT_SUCCESS(PsxInitialize()))
            NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)1);
        PsxPostOpSync();
        return 0;
    }
    if (!NT_SUCCESS(Status)) { PsxSetErrno(5 /* EIO */); return -1; }
    if (Message.Errno != 0)  { PsxSetErrno(Message.Errno); return -1; }
    return (int)Message.ReturnValue;            // parent: child pid
}

//
// execl(path, arg0, arg1, ..., (char *)0)
//
int __cdecl
execl(const char *Path, const char *Arg0, ...)
{
    const char *Argv[PSX_ARG_MAX];
    int n = 0;
    va_list Args;

    Argv[n++] = Arg0;
    va_start(Args, Arg0);
    while (n < PSX_ARG_MAX - 1)
    {
        const char *a = va_arg(Args, const char *);
        Argv[n++] = a;
        if (a == NULL)
            break;
    }
    va_end(Args);
    Argv[PSX_ARG_MAX - 1] = NULL;
    return execv(Path, (char *const *)Argv);
}

//
// execlp(file, arg0, arg1, ..., (char *)0) -- PATH search performed by execvp.
//
int __cdecl
execlp(const char *File, const char *Arg0, ...)
{
    const char *Argv[PSX_ARG_MAX];
    int n = 0;
    va_list Args;

    Argv[n++] = Arg0;
    va_start(Args, Arg0);
    while (n < PSX_ARG_MAX - 1)
    {
        const char *a = va_arg(Args, const char *);
        Argv[n++] = a;
        if (a == NULL)
            break;
    }
    va_end(Args);
    Argv[PSX_ARG_MAX - 1] = NULL;
    return execvp(File, (char *const *)Argv);
}

//
// execle(path, arg0, arg1, ..., (char *)0, envp) -- envp follows the NULL.
//
int __cdecl
execle(const char *Path, const char *Arg0, ...)
{
    const char *Argv[PSX_ARG_MAX];
    char *const *Envp = NULL;
    int n = 0;
    va_list Args;

    Argv[n++] = Arg0;
    va_start(Args, Arg0);
    while (n < PSX_ARG_MAX - 1)
    {
        const char *a = va_arg(Args, const char *);
        Argv[n++] = a;
        if (a == NULL)
            break;
    }
    Envp = va_arg(Args, char *const *);     // environment after the arg NULL
    va_end(Args);
    Argv[PSX_ARG_MAX - 1] = NULL;
    return execve(Path, (char *const *)Argv, Envp);
}

//
// setsid() -- ApiNumber 0x10. Returns the new session/process-group id.
//
int __cdecl
setsid(void)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSetsid, PSX_BODY_DATALEN(0));
    return (int)PsxCallServer(&Message);
}

//
// setpgid(pid, pgid) -- ApiNumber 0x11.
//
int __cdecl
setpgid(int Pid, int Pgid)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSetpgid, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;     // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Pgid;    // +0x34
    return (int)PsxCallServer(&Message);
}

//
// waitpid(pid, status, options) -- ApiNumber 0x02. Filter pid @0x30, options
// @0x38; the reaped child's pid is the return value, its status returns at
// Args[1] (+0x34).
//
int __cdecl
waitpid(int Pid, int *Status, int Options)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiWaitpid, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Pid;         // +0x30
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Options;     // +0x38
    Result = PsxCallServer(&Message);
    if (Result > 0 && Status != NULL)
        *Status = (int)((PULONG)Message.Data.Raw)[1];   // +0x34
    return (int)Result;
}

int __cdecl
wait(int *Status)
{
    return waitpid(-1, Status, 0);
}

//
// system(command) -- fork a "/bin/sh -c <command>" and wait for it. A NULL
// command asks whether a shell exists (we always have one).
//
int __cdecl
system(const char *Command)
{
    int Pid, Status = 0;

    if (Command == NULL)
        return 1;

    Pid = fork();
    if (Pid < 0)
        return -1;
    if (Pid == 0)
    {
        execl("/bin/sh", "sh", "-c", Command, (char *)0);
        _exit(127);         // exec failed
    }
    if (waitpid(Pid, &Status, 0) < 0)
        return -1;
    return Status;
}
