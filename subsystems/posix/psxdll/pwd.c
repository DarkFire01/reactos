/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     User and group database lookups
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The server packs each record into a shared section buffer with the string
 * fields holding byte offsets from the start of the record. The record is
 * copied to static storage and the offsets are turned into pointers; each call
 * overwrites the previous result.
 */

#include "psxdllp.h"

#define PSX_RECORD_SIZE 256
#define PSX_ENOMEM      12

typedef struct _PSX_PASSWD
{
    char *pw_name;
    ULONG pw_uid;
    ULONG pw_gid;
    char *pw_dir;
    char *pw_shell;
} PSX_PASSWD;

typedef struct _PSX_GROUP
{
    char *gr_name;
    ULONG gr_gid;
    char **gr_mem;
} PSX_GROUP;

static PSX_PASSWD PsxPasswd;
static char PsxPasswdBuffer[PSX_RECORD_SIZE];
static PSX_GROUP PsxGroup;
static char PsxGroupBuffer[PSX_RECORD_SIZE];

/**
 * @brief Copies the packed record returned by a lookup into RecordBuffer.
 *
 * @return The number of bytes copied.
 */
static
ULONG
PsxCopyRecord(
    _In_ PPSX_API_MESSAGE Message,
    _In_ PVOID SharedBuffer,
    _Out_writes_bytes_(RecordSize) PVOID RecordBuffer,
    _In_ ULONG RecordSize)
{
    ULONG Length = ((PULONG)Message->Data.Raw)[2];

    if (Length == 0 || Length > RecordSize)
        Length = RecordSize;

    RtlCopyMemory(RecordBuffer, SharedBuffer, Length);
    return Length;
}

/**
 * @brief Runs an id keyed lookup (getpwuid, getgrgid).
 *
 * @return The record length, or 0 on failure.
 */
static
ULONG
PsxQueryById(
    _In_ ULONG Api,
    _In_ ULONG Key,
    _Out_writes_bytes_(RecordSize) PVOID RecordBuffer,
    _In_ ULONG RecordSize)
{
    PSX_API_MESSAGE Message;
    PVOID SharedBuffer;
    ULONG Length;

    SharedBuffer = PsxAllocShared(PSX_RECORD_SIZE);
    if (SharedBuffer == NULL)
    {
        PsxSetErrno(PSX_ENOMEM);
        return 0;
    }

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = Key;
    ((PULONG)Message.Data.Raw)[1] = PsxServerPtr(SharedBuffer);
    if (PsxCallServer(&Message) < 0)
    {
        PsxFreeShared(SharedBuffer);
        return 0;
    }

    Length = PsxCopyRecord(&Message, SharedBuffer, RecordBuffer, RecordSize);
    PsxFreeShared(SharedBuffer);
    return Length;
}

/**
 * @brief Runs a name keyed lookup (getpwnam, getgrnam). The name is passed
 * through the shared section.
 *
 * @return The record length, or 0 on failure.
 */
static
ULONG
PsxQueryByName(
    _In_ ULONG Api,
    _In_z_ PCSTR Name,
    _Out_writes_bytes_(RecordSize) PVOID RecordBuffer,
    _In_ ULONG RecordSize)
{
    PSX_API_MESSAGE Message;
    ULONG NameLength = PsxStringLengthA(Name) + 1;
    PVOID NameBuffer = PsxAllocShared(NameLength);
    PVOID SharedBuffer = PsxAllocShared(PSX_RECORD_SIZE);
    ULONG Length;

    if (NameBuffer == NULL || SharedBuffer == NULL)
    {
        PsxFreeShared(NameBuffer);
        PsxFreeShared(SharedBuffer);
        PsxSetErrno(PSX_ENOMEM);
        return 0;
    }

    RtlCopyMemory(NameBuffer, Name, NameLength);
    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = PsxServerPtr(NameBuffer);
    ((PULONG)Message.Data.Raw)[1] = PsxServerPtr(SharedBuffer);
    if (PsxCallServer(&Message) < 0)
    {
        PsxFreeShared(NameBuffer);
        PsxFreeShared(SharedBuffer);
        return 0;
    }

    Length = PsxCopyRecord(&Message, SharedBuffer, RecordBuffer, RecordSize);
    PsxFreeShared(NameBuffer);
    PsxFreeShared(SharedBuffer);
    return Length;
}

static
VOID
PsxFixupPasswd(VOID)
{
    PULONG Fields = (PULONG)PsxPasswdBuffer;

    PsxPasswd.pw_name = PsxPasswdBuffer + Fields[0];
    PsxPasswd.pw_uid = Fields[1];
    PsxPasswd.pw_gid = Fields[2];
    PsxPasswd.pw_dir = PsxPasswdBuffer + Fields[3];
    PsxPasswd.pw_shell = PsxPasswdBuffer + Fields[4];
}

static
VOID
PsxFixupGroup(VOID)
{
    PULONG Fields = (PULONG)PsxGroupBuffer;

    PsxGroup.gr_name = PsxGroupBuffer + Fields[0];
    PsxGroup.gr_gid = Fields[1];
    PsxGroup.gr_mem = (char **)(PsxGroupBuffer + Fields[2]);
}

void *
__cdecl
getpwuid(
    _In_ ULONG Uid)
{
    if (PsxQueryById(PsxApiGetpwuid, Uid, PsxPasswdBuffer, sizeof(PsxPasswdBuffer)) == 0)
        return NULL;

    PsxFixupPasswd();
    return &PsxPasswd;
}

void *
__cdecl
getpwnam(
    _In_opt_z_ const char *Name)
{
    if (Name == NULL ||
        PsxQueryByName(PsxApiGetpwnam, Name, PsxPasswdBuffer, sizeof(PsxPasswdBuffer)) == 0)
    {
        return NULL;
    }

    PsxFixupPasswd();
    return &PsxPasswd;
}

void *
__cdecl
getgrgid(
    _In_ ULONG Gid)
{
    if (PsxQueryById(PsxApiGetgrgid, Gid, PsxGroupBuffer, sizeof(PsxGroupBuffer)) == 0)
        return NULL;

    PsxFixupGroup();
    return &PsxGroup;
}

void *
__cdecl
getgrnam(
    _In_opt_z_ const char *Name)
{
    if (Name == NULL ||
        PsxQueryByName(PsxApiGetgrnam, Name, PsxGroupBuffer, sizeof(PsxGroupBuffer)) == 0)
    {
        return NULL;
    }

    PsxFixupGroup();
    return &PsxGroup;
}
