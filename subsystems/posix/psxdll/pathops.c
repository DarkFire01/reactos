/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Path based system calls (stat, access, link, unlink, rename...)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/* Size of struct stat, copied back verbatim from the shared section */
#define PSX_STAT_SIZE   40

#define PSX_EPERM       1
#define PSX_ENOENT      2
#define PSX_ENOMEM      12
#define PSX_EISDIR      21

/**
 * @brief Marshals a POSIX path into the request at the given Raw byte offset.
 */
static
BOOLEAN
PsxPutPath(
    _Inout_ PPSX_API_MESSAGE Message,
    _In_ ULONG RawOffset,
    _In_opt_z_ const char *Path)
{
    UNICODE_STRING NtPath;

    if (!PsxMarshalPath(Path, &NtPath))
        return FALSE;

    *(PUNICODE_STRING)(Message->Data.Raw + RawOffset) = NtPath;
    return TRUE;
}

static
VOID
PsxDropPath(
    _Inout_ PPSX_API_MESSAGE Message,
    _In_ ULONG RawOffset)
{
    PsxFreeMarshalledPath((PUNICODE_STRING)(Message->Data.Raw + RawOffset));
}

int
__cdecl
stat(
    _In_z_ const char *Path,
    _Out_writes_bytes_(PSX_STAT_SIZE) void *StatBuffer)
{
    PSX_API_MESSAGE Message;
    PVOID Shared;
    LONG Result;

    Shared = PsxAllocShared(PSX_STAT_SIZE);
    if (Shared == NULL)
    {
        PsxSetErrno(PSX_ENOMEM);
        return -1;
    }

    PsxInitMessage(&Message, PsxApiStat, PSX_BODY_DATALEN(sizeof(PSX_STAT_REQUEST)));
    if (!PsxMarshalPath(Path, &Message.Data.Stat.Path))
    {
        PsxFreeShared(Shared);
        PsxSetErrno(PSX_ENOENT);
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

int
__cdecl
fstat(
    _In_ int FileDescriptor,
    _Out_writes_bytes_(PSX_STAT_SIZE) void *StatBuffer)
{
    PSX_API_MESSAGE Message;
    PVOID Shared;
    LONG Result;

    Shared = PsxAllocShared(PSX_STAT_SIZE);
    if (Shared == NULL)
    {
        PsxSetErrno(PSX_ENOMEM);
        return -1;
    }

    PsxInitMessage(&Message, PsxApiFstat, PSX_BODY_DATALEN(sizeof(PSX_FSTAT_REQUEST)));
    Message.Data.Fstat.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.Fstat.StatBuffer = PsxServerPtr(Shared);

    Result = PsxCallServer(&Message);
    if (Result >= 0)
        RtlCopyMemory(StatBuffer, Shared, PSX_STAT_SIZE);

    PsxFreeShared(Shared);
    return (int)Result;
}

int
__cdecl
access(
    _In_z_ const char *Path,
    _In_ int ModeMask)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiAccess, PSX_BODY_DATALEN(sizeof(PSX_OPEN_REQUEST)));
    if (!PsxMarshalPath(Path, &Message.Data.Open.Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    Message.Data.Open.OpenFlag = (ULONG)ModeMask;

    Result = PsxCallServer(&Message);
    PsxFreeMarshalledPath(&Message.Data.Open.Path);
    return (int)Result;
}

/**
 * @brief Sends a request carrying two paths (link, rename).
 */
static
int
PsxTwoPathOp(
    _In_ ULONG Api,
    _In_z_ const char *OldPath,
    _In_z_ const char *NewPath)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(2 * sizeof(UNICODE_STRING)));
    if (!PsxPutPath(&Message, 0, OldPath))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    if (!PsxPutPath(&Message, sizeof(UNICODE_STRING), NewPath))
    {
        PsxDropPath(&Message, 0);
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    PsxDropPath(&Message, sizeof(UNICODE_STRING));
    return (int)Result;
}

int
__cdecl
link(
    _In_z_ const char *OldPath,
    _In_z_ const char *NewPath)
{
    return PsxTwoPathOp(PsxApiLink, OldPath, NewPath);
}

int
__cdecl
rename(
    _In_z_ const char *OldPath,
    _In_z_ const char *NewPath)
{
    return PsxTwoPathOp(PsxApiRename, OldPath, NewPath);
}

/**
 * @brief Sends a request carrying one path and no other arguments (unlink, rmdir).
 */
static
int
PsxOnePathOp(
    _In_ ULONG Api,
    _In_z_ const char *Path)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(UNICODE_STRING)));
    if (!PsxPutPath(&Message, 0, Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

int
__cdecl
unlink(
    _In_z_ const char *Path)
{
    return PsxOnePathOp(PsxApiUnlink, Path);
}

int
__cdecl
rmdir(
    _In_z_ const char *Path)
{
    return PsxOnePathOp(PsxApiRmdir, Path);
}

/**
 * @brief Removes a file, falling back to rmdir when unlink reports a directory.
 */
int
__cdecl
remove(
    _In_z_ const char *Path)
{
    int Result = PsxOnePathOp(PsxApiUnlink, Path);

    if (Result < 0 && PsxErrnoLocation != NULL &&
        (*PsxErrnoLocation == PSX_EISDIR || *PsxErrnoLocation == PSX_EPERM))
    {
        Result = PsxOnePathOp(PsxApiRmdir, Path);
    }

    return Result;
}

/**
 * @brief Sends a request carrying one path followed by a mode (mkdir, mkfifo, chmod).
 */
static
int
PsxPathModeOp(
    _In_ ULONG Api,
    _In_z_ const char *Path,
    _In_ int Mode)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Mode;

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

int
__cdecl
mkdir(
    _In_z_ const char *Path,
    _In_ int Mode)
{
    return PsxPathModeOp(PsxApiMkdir, Path, Mode);
}

int
__cdecl
mkfifo(
    _In_z_ const char *Path,
    _In_ int Mode)
{
    return PsxPathModeOp(PsxApiMkfifo, Path, Mode);
}

int
__cdecl
chmod(
    _In_z_ const char *Path,
    _In_ int Mode)
{
    return PsxPathModeOp(PsxApiChmod, Path, Mode);
}

int
__cdecl
chown(
    _In_z_ const char *Path,
    _In_ int Uid,
    _In_ int Gid)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiChown, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + 2 * sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Uid;
    ((PULONG)Message.Data.Raw)[3] = (ULONG)Gid;

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

/**
 * @brief Sets file times. A NULL Times means "now", otherwise actime and
 * modtime travel inline after a nonzero flag.
 */
int
__cdecl
utime(
    _In_z_ const char *Path,
    _In_reads_opt_(2) const long *Times)
{
    PSX_API_MESSAGE Message;
    PULONG Args;
    LONG Result;

    PsxInitMessage(&Message, PsxApiUtime, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + 3 * sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }

    Args = (PULONG)Message.Data.Raw;
    Args[2] = (Times != NULL) ? 1 : 0;
    Args[3] = (Times != NULL) ? (ULONG)Times[0] : 0;
    Args[4] = (Times != NULL) ? (ULONG)Times[1] : 0;

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (int)Result;
}

long
__cdecl
pathconf(
    _In_z_ const char *Path,
    _In_ int Name)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiPathconf, PSX_BODY_DATALEN(sizeof(UNICODE_STRING) + sizeof(ULONG)));
    if (!PsxPutPath(&Message, 0, Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Name;

    Result = PsxCallServer(&Message);
    PsxDropPath(&Message, 0);
    return (long)Result;
}

long
__cdecl
fpathconf(
    _In_ int FileDescriptor,
    _In_ int Name)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiFpathconf, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Name;
    return (long)PsxCallServer(&Message);
}
