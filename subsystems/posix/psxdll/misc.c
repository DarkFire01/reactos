/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL miscellaneous syscalls (times/sysconf/getgroups/uname)
 *              plus the client-side working-directory helpers (getcwd/chdir),
 *              which translate between POSIX and the NT device-path CWD.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// times(struct tms *buf) -- the return value is elapsed real time in CLK_TCK
// (100, i.e. 10ms) ticks. Computed CLIENT-SIDE from the NT system clock (100ns
// units), faithful to the MS psxdll -- it does NOT round-trip to the server
// (whose times handler is a zero-returning stub). A monotonically advancing
// clock is exactly what timer-driven clients (Xt/xeyes) need; going through the
// stub froze time and made them busy-loop. Masked to stay positive across a
// session; callers only take deltas.
//
long __cdecl
times(void *TmsBuffer)
{
    LARGE_INTEGER SystemTime;

    if (TmsBuffer != NULL)
        RtlZeroMemory(TmsBuffer, 4 * sizeof(ULONG));    // utime/stime/cutime/cstime

    NtQuerySystemTime(&SystemTime);
    return (long)((ULONG)(SystemTime.QuadPart / 100000) & 0x7FFFFFFF);
}

//
// time(time_t *t) -- seconds since the epoch, computed from the NT clock (no
// server round-trip). NtQuerySystemTime is 100ns since 1601; convert to the
// 1970 epoch.
//
long __cdecl
time(long *Result)
{
    LARGE_INTEGER SystemTime;
    LONGLONG Seconds;

    NtQuerySystemTime(&SystemTime);
    // 11644473600 seconds between 1601-01-01 and 1970-01-01.
    Seconds = (SystemTime.QuadPart / 10000000) - 11644473600LL;
    if (Result != NULL)
        *Result = (long)Seconds;
    return (long)Seconds;
}

//
// gettimeofday(struct timeval *tv, struct timezone *tz) -- EXTENSION export
// (ordinal 120; the real NT 4.0 psxdll stopped at 119 and never had it).
// Real microsecond resolution from the NT clock, same 1601->1970 epoch
// conversion as time(). tz is vestigial POSIX; zero it.
//
int __cdecl
gettimeofday(void *Tv, void *Tz)
{
    LARGE_INTEGER SystemTime;
    LONGLONG Usec1970;

    if (Tv != NULL)
    {
        NtQuerySystemTime(&SystemTime);
        Usec1970 = (SystemTime.QuadPart / 10) - 11644473600000000LL;
        ((long *)Tv)[0] = (long)(Usec1970 / 1000000);   // tv_sec
        ((long *)Tv)[1] = (long)(Usec1970 % 1000000);   // tv_usec
    }
    if (Tz != NULL)
    {
        ((int *)Tz)[0] = 0;     // tz_minuteswest
        ((int *)Tz)[1] = 0;     // tz_dsttime
    }
    return 0;
}

//
// settimeofday(const struct timeval *tv, const struct timezone *tz) --
// EXTENSION export (ordinal 122). NtSetSystemTime needs the caller to hold
// SeSystemtimePrivilege; without it this fails EPERM, which is exactly the
// POSIX contract for non-root settimeofday.
//
int __cdecl
settimeofday(const void *Tv, const void *Tz)
{
    LARGE_INTEGER SystemTime;
    const long *In = (const long *)Tv;
    NTSTATUS Status;

    if (In == NULL || In[1] < 0 || In[1] >= 1000000)
    {
        PsxSetErrno(22 /* EINVAL */);
        return -1;
    }

    SystemTime.QuadPart = ((LONGLONG)In[0] + 11644473600LL) * 10000000
                          + (LONGLONG)In[1] * 10;
    Status = NtSetSystemTime(&SystemTime, NULL);
    if (!NT_SUCCESS(Status))
    {
        PsxSetErrno(1 /* EPERM */);
        return -1;
    }
    return 0;
}

//
// nanosleep(const struct timespec *req, struct timespec *rem) -- EXTENSION
// export (ordinal 121). NtDelayExecution counts 100ns units natively, so this
// is the honest primitive (sleep() above is the derived one). Sub-100ns
// residue rounds up so we never return early.
//
int __cdecl
nanosleep(const void *Requested, void *Remaining)
{
    LARGE_INTEGER Interval;
    const long *Req = (const long *)Requested;

    if (Req == NULL || Req[0] < 0 || Req[1] < 0 || Req[1] >= 1000000000)
    {
        PsxSetErrno(22 /* EINVAL */);
        return -1;
    }

    Interval.QuadPart = -((LONGLONG)Req[0] * 10000000 + ((LONGLONG)Req[1] + 99) / 100);
    NtDelayExecution(FALSE, &Interval);

    if (Remaining != NULL)
    {
        ((long *)Remaining)[0] = 0;     // uninterrupted: nothing remains
        ((long *)Remaining)[1] = 0;
    }
    return 0;
}

//
// sysconf(name) -- ApiNumber 0x17.
//
long __cdecl
sysconf(int Name)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSysconf, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Name;
    return (long)PsxCallServer(&Message);
}

//
// getgroups(size, list) -- ApiNumber 0x0D. list is a raw client pointer the
// server fills by NtWriteVirtualMemory.
//
int __cdecl
getgroups(int Size, int *List)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiGetGroups, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Size;                    // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)List;         // +0x34 raw ptr
    return (int)PsxCallServer(&Message);
}

//
// uname(struct utsname *) -- five 14-byte fields. No server opcode; report the
// subsystem's fixed identity.
//
int __cdecl
uname(void *Utsname)
{
    static const char *Fields[5] =
        { "POSIX", "reactos", "4.0", "1", "x86" };
    PCHAR Out = (PCHAR)Utsname;
    int i, j;

    if (Utsname == NULL) { PsxSetErrno(22 /* EINVAL */); return -1; }
    for (i = 0; i < 5; i++)
    {
        const char *s = Fields[i];
        for (j = 0; j < 13 && s[j] != '\0'; j++)
            Out[i * 14 + j] = s[j];
        Out[i * 14 + j] = '\0';
    }
    return 0;
}

//
// getcwd(buf, size) -- translate the NT device CWD back to a POSIX path by
// stripping the root device prefix and turning '\' into '/'. Purely local.
//
char * __cdecl
getcwd(char *Buffer, unsigned int Size)
{
    CHAR  Tmp[PSX_PATH_MAX];
    ULONG RootLen = PsxStartupRootLen;
    PCSTR Nt = PsxStartupCwd;
    ULONG i, out = 0, Needed;

    if (Buffer != NULL && Size == 0) { PsxSetErrno(22 /* EINVAL */); return NULL; }

    // Strip the root device prefix ("\DosDevices\X:") if the CWD sits under it.
    if (RootLen != 0)
    {
        ULONG k;
        BOOLEAN Match = TRUE;
        for (k = 0; k < RootLen; k++)
            if (Nt[k] != PsxStartupRoot[k]) { Match = FALSE; break; }
        if (Match)
            Nt += RootLen;
    }

    // Translate into Tmp first so the glibc allocation extension below knows
    // the exact length up front.
    for (i = 0; Nt[i] != '\0'; i++)
    {
        if (out + 1 >= sizeof(Tmp)) { PsxSetErrno(34 /* ERANGE */); return NULL; }
        Tmp[out++] = (Nt[i] == '\\') ? '/' : Nt[i];
    }
    // Drop a trailing slash (unless the whole path is "/").
    if (out > 1 && Tmp[out - 1] == '/')
        out--;
    if (out == 0)
        Tmp[out++] = '/';
    Tmp[out] = '\0';
    Needed = out + 1;

    if (Buffer == NULL)
    {
        // The glibc extension modern code leans on (bash shell-init calls
        // getcwd(0, 0)): allocate the result. Size == 0 means exact fit; the
        // caller frees it with free() -- the CRT heap and ours are both the
        // NT process heap, so ownership transfers cleanly.
        ULONG Alloc = (Size != 0) ? Size : Needed;
        if (Alloc < Needed) { PsxSetErrno(34 /* ERANGE */); return NULL; }
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Alloc);
        if (Buffer == NULL) { PsxSetErrno(12 /* ENOMEM */); return NULL; }
    }
    else if (Size < Needed)
    {
        PsxSetErrno(34 /* ERANGE */);
        return NULL;
    }

    RtlMoveMemory(Buffer, Tmp, Needed);
    return Buffer;
}

//
// chdir(path) -- resolve to an NT device path and adopt it as the CWD (with a
// trailing separator, as the server's startup CWD carries). Verified with an
// access() existence check so a bad path reports ENOENT.
//
int __cdecl
chdir(const char *Path)
{
    CHAR Nt[PSX_PATH_MAX];
    ULONG Len;

    if (access(Path, 0 /* F_OK */) != 0)
        return -1;

    Len = PsxBuildNtPath(Path, Nt, sizeof(Nt));
    if (Len == 0 || Len + 2 >= sizeof(PsxStartupCwd))
    { PsxSetErrno(2 /* ENOENT */); return -1; }

    RtlCopyMemory(PsxStartupCwd, Nt, Len);
    if (PsxStartupCwd[Len - 1] != '\\')
        PsxStartupCwd[Len++] = '\\';
    PsxStartupCwd[Len] = '\0';
    PsxStartupCwdLen = Len;
    return 0;
}

//
// isatty2(fd) -- ApiNumber 0x16 with the session-confirm flag set. The plain
// isatty() lives in fileio.c; isatty2 asks posix.exe to confirm the tty.
//
int __cdecl
isatty2(int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiIsatty, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = 1;      // confirm with the session leader
    if (PsxCallServer(&Message) < 0)
        return 0;
    return (int)((PULONG)Message.Data.Raw)[1];
}

//
// getenv(name) -- search the environ vector the crt0 published (via
// __PdxInitializeData). Returns a pointer into the "NAME=value" string.
//
char * __cdecl
getenv(const char *Name)
{
    char **Env;
    ULONG NameLen, i;

    if (Name == NULL || PsxEnvironLocation == NULL || *PsxEnvironLocation == NULL)
        return NULL;

    NameLen = 0;
    while (Name[NameLen] != '\0')
        NameLen++;

    for (Env = *PsxEnvironLocation; *Env != NULL; Env++)
    {
        char *Entry = *Env;
        for (i = 0; i < NameLen; i++)
            if (Entry[i] != Name[i])
                break;
        if (i == NameLen && Entry[NameLen] == '=')
            return Entry + NameLen + 1;
    }
    return NULL;
}

//
// __PdxGetCmdLine (ordinal 16) -- return the process command line, which the
// spawner (psxss) packs into PEB->ProcessParameters->CommandLine.Buffer as the
// base-relative argv/env offset table the crt0 unpacks in place.
//
char * __cdecl
__PdxGetCmdLine(void)
{
    PPEB Peb = NtCurrentPeb();
    if (Peb != NULL && Peb->ProcessParameters != NULL)
        return (char *)Peb->ProcessParameters->CommandLine.Buffer;
    return NULL;
}

//
// Controlling-terminal + login helpers. The subsystem has a single controlling
// terminal ("/dev/tty") and, until the SAM-backed getpwuid lands, a fixed login
// name. ctermid/cuserid copy into the caller's buffer when given one.
//
static char *
PsxCopyOrStatic(char *Dst, const char *Src)
{
    static char Static[16];
    char *Out = (Dst != NULL) ? Dst : Static;
    int i = 0;
    while (Src[i] != '\0' && i < 15) { Out[i] = Src[i]; i++; }
    Out[i] = '\0';
    return Out;
}

char * __cdecl ctermid(char *Buffer) { return PsxCopyOrStatic(Buffer, "/dev/tty"); }
char * __cdecl cuserid(char *Buffer) { return PsxCopyOrStatic(Buffer, "root"); }

char * __cdecl
getlogin(void)
{
    static char Name[] = "root";
    return Name;
}

char * __cdecl
ttyname(int FileDescriptor)
{
    static char Name[] = "/dev/tty";
    return isatty(FileDescriptor) ? Name : NULL;
}

//
// setuid/setgid -- the subsystem identity is fixed by the caller's NT token
// (there is no server opcode to change it). Succeed only for the current id;
// anything else is EPERM, matching a POSIX process without appropriate privilege.
//
int __cdecl setuid(unsigned int Uid)
{
    if (Uid == (unsigned int)getuid()) return 0;
    PsxSetErrno(1 /* EPERM */); return -1;
}

int __cdecl setgid(unsigned int Gid)
{
    if (Gid == (unsigned int)getgid()) return 0;
    PsxSetErrno(1 /* EPERM */); return -1;
}

//
// fileno(FILE *) -- the fd is the `_file` char in the reskit struct _iobuf, at
// byte offset 18 (int _cnt; uchar *_ptr,*_base; int _bufsiz; short _flag; char
// _file). Programs normally use the header macro; this is the callable export.
//
int __cdecl fileno(void *Stream)
{
    if (Stream == NULL) { PsxSetErrno(9 /* EBADF */); return -1; }
    return (int)*((signed char *)Stream + 18);
}

//
// getreg -- no POSIX machine-register model exists on the subsystem.
//
int __cdecl getreg(int Which)
{
    UNREFERENCED_PARAMETER(Which);
    PsxSetErrno(40 /* ENOSYS */);
    return -1;
}
