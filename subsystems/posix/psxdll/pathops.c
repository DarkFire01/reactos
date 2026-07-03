/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL path-based syscalls (stat/access/link/unlink/rename/...).
 *              Paths and the struct stat buffer live in the shared section and
 *              travel as server-relative pointers.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// POSIX struct stat is 40 bytes (0x28); the server fills a copy in the shared
// section which we copy back verbatim (layout is identical to <sys/stat.h>).
//
#define PSX_STAT_SIZE   40

//
// Place a marshalled path UNICODE_STRING at a Raw byte offset, translating the
// POSIX path client-side. Returns FALSE (ENOENT) on translation failure.
//
static BOOLEAN
PsxPutPath(PPSX_API_MESSAGE Message, ULONG RawOffset, const char *Path)
{
    UNICODE_STRING Nt;
    if (!PsxMarshalPath(Path, &Nt))
        return FALSE;
    *(PUNICODE_STRING)(Message->Data.Raw + RawOffset) = Nt;
    return TRUE;
}

static VOID
PsxDropPath(PPSX_API_MESSAGE Message, ULONG RawOffset)
{
    PsxFreeMarshalledPath((PUNICODE_STRING)(Message->Data.Raw + RawOffset));
}

//
// stat(path, buf) -- ApiNumber 0x1F.
//
int __cdecl
stat(const char *Path, void *StatBuffer)
{
    PSX_API_MESSAGE Message;
    PVOID Shared;
    LONG Result;

    Shared = PsxAllocShared(PSX_STAT_SIZE);
    if (Shared == NULL) { PsxSetErrno(12 /* ENOMEM */); return -1; }

    PsxInitMessage(&Message, PsxApiStat, PSX_BODY_DATALEN(sizeof(PSX_STAT_REQUEST)));
    if (!PsxMarshalPath(Path, &Message.Data.Stat.Path))
    {
        PsxFreeShared(Shared);
        PsxSetErrno(2 /* ENOENT */);
        return -1;
    }
    Message.Data.Stat.StatBuffer = PsxServerPtr(Shared);

    Result = PsxCallServer(&Message);
    if (Result >= 0)
        RtlCopyMemory(StatBuffer, Shared, PSX_STAT_SIZE);

    PsxFreeMarshalledPath(&Message.Data.Stat.Path);
    PsxFreeShared(Shared);
    return (int)Result;
}

//
// fstat(fd, buf) -- ApiNumber 0x20.
//
int __cdecl
fstat(int FileDescriptor, void *StatBuffer)
{
    PSX_API_MESSAGE Message;
    PVOID Shared;
    LONG Result;

    Shared = PsxAllocShared(PSX_STAT_SIZE);
    if (Shared == NULL) { PsxSetErrno(12 /* ENOMEM */); return -1; }

    PsxInitMessage(&Message, PsxApiFstat, PSX_BODY_DATALEN(sizeof(PSX_FSTAT_REQUEST)));
    Message.Data.Fstat.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.Fstat.StatBuffer = PsxServerPtr(Shared);

    Result = PsxCallServer(&Message);
    if (Result >= 0)
        RtlCopyMemory(StatBuffer, Shared, PSX_STAT_SIZE);

    PsxFreeShared(Shared);
    return (int)Result;
}

//
// access(path, mode) -- ApiNumber 0x21 (mode mask R_OK/W_OK/X_OK/F_OK).
//
int __cdecl
access(const char *Path, int ModeMask)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiAccess, PSX_BODY_DATALEN(sizeof(PSX_OPEN_REQUEST)));
    if (!PsxMarshalPath(Path, &Message.Data.Open.Path))
    {
        PsxSetErrno(2 /* ENOENT */);
        return -1;
    }
    Message.Data.Open.OpenFlag = (ULONG)ModeMask;
    Result = PsxCallServer(&Message);
    PsxFreeMarshalledPath(&Message.Data.Open.Path);
    return (int)Result;
}

//
// Two-path ops: link(0x1A) / rename(0x1E). Src UNICODE_STRING @0x30, dst @0x38.
//
static int
PsxTwoPathOp(ULONG Api, const char *OldPath, const char *NewPath)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(2 * sizeof(UNICODE_STRING)));
    if (!PsxPutPath(&Message, 0, OldPath))
    { PsxSetErrno(2); return -1; }
    if (!PsxPutPath(&Message, sizeof(UNICODE_STRING), NewPath))
    { PsxDropPath(&Message, 0); PsxSetErrno(2); return -1; }

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    PsxDropPath(&Message, sizeof(UNICODE_STRING));
    return (int)Result;
}

int __cdecl link(const char *OldPath, const char *NewPath)
{ return PsxTwoPathOp(PsxApiLink, OldPath, NewPath); }

int __cdecl rename(const char *OldPath, const char *NewPath)
{ return PsxTwoPathOp(PsxApiRename, OldPath, NewPath); }

//
// One-path ops with no extra args: unlink(0x1D) / rmdir(0x3B). remove() picks
// unlink (files); directories fall through to rmdir on EISDIR/EPERM.
//
static int
PsxOnePathOp(ULONG Api, const char *Path)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(UNICODE_STRING)));
    if (!PsxPutPath(&Message, 0, Path))
    { PsxSetErrno(2); return -1; }
    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

int __cdecl unlink(const char *Path) { return PsxOnePathOp(PsxApiUnlink, Path); }
int __cdecl rmdir(const char *Path)  { return PsxOnePathOp(PsxApiRmdir, Path); }

int __cdecl
remove(const char *Path)
{
    int Result = PsxOnePathOp(PsxApiUnlink, Path);
    if (Result < 0 && PsxErrnoLocation != NULL &&
        (*PsxErrnoLocation == 21 /* EISDIR */ || *PsxErrnoLocation == 1 /* EPERM */))
        Result = PsxOnePathOp(PsxApiRmdir, Path);
    return Result;
}

//
// One-path ops with a mode at +0x38: mkdir(0x1B) / mkfifo(0x1C).
//
static int
PsxPathModeOp(ULONG Api, const char *Path, int Mode)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    { PsxSetErrno(2); return -1; }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Mode;    // +0x38
    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

int __cdecl mkdir(const char *Path, int Mode)  { return PsxPathModeOp(PsxApiMkdir, Path, Mode); }
int __cdecl mkfifo(const char *Path, int Mode) { return PsxPathModeOp(PsxApiMkfifo, Path, Mode); }
int __cdecl chmod(const char *Path, int Mode)  { return PsxPathModeOp(PsxApiChmod, Path, Mode); }

//
// chown(path, uid, gid) -- ApiNumber 0x23. uid @0x38, gid @0x3C.
//
int __cdecl
chown(const char *Path, int Uid, int Gid)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiChown, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + 2 * sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    { PsxSetErrno(2); return -1; }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Uid;     // +0x38
    ((PULONG)Message.Data.Raw)[3] = (ULONG)Gid;     // +0x3C
    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

//
// utime(path, times[2]) -- ApiNumber 0x24. times==NULL means "now"; otherwise
// the two time_t (actime, modtime) travel inline.
//
int __cdecl
utime(const char *Path, const long *Times)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiUtime, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + 3 * sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    { PsxSetErrno(2); return -1; }
    ((PULONG)Message.Data.Raw)[2] = (Times != NULL) ? 1 : 0;             // +0x38 (0 => now)
    ((PULONG)Message.Data.Raw)[3] = (Times != NULL) ? (ULONG)Times[0] : 0; // +0x3C actime
    ((PULONG)Message.Data.Raw)[4] = (Times != NULL) ? (ULONG)Times[1] : 0; // +0x40 modtime
    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

//
// pathconf(path, name) -- 0x25 / fpathconf(fd, name) -- 0x26.
//
long __cdecl
pathconf(const char *Path, int Name)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiPathconf, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    { PsxSetErrno(2); return -1; }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Name;    // +0x38
    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (long)Result;
}

long __cdecl
fpathconf(int FileDescriptor, int Name)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiFpathconf, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;  // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Name;            // +0x34
    return (long)PsxCallServer(&Message);
}
