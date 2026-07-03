/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL passwd/group database (getpwuid/getpwnam/getgrgid/getgrnam,
 *              ApiNumbers 0x37-0x3A). The server packs the struct into a shared-
 *              section buffer with SELF-RELATIVE offsets in the char* fields
 *              (pw_name/pw_dir/pw_shell, gr_name/gr_mem hold byte offsets); we
 *              copy it to static storage and turn the offsets into pointers. Each
 *              getter returns a pointer to a single static record (POSIX allows
 *              the result to be overwritten by the next call).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

struct psx_passwd { char *pw_name; ULONG pw_uid; ULONG pw_gid; char *pw_dir; char *pw_shell; };
struct psx_group  { char *gr_name; ULONG gr_gid; char **gr_mem; };

static struct psx_passwd s_pw;
static char s_pwbuf[256];
static struct psx_group s_gr;
static char s_grbuf[256];

static ULONG PsxStrLenA(PCSTR s) { PCSTR p = s; while (*p) p++; return (ULONG)(p - s); }

//
// Run an id-keyed query (getpwuid/getgrgid): key @0x30, shared result buffer
// @0x34, byte length back in @0x38. Copies the packed record into StaticBuf.
// Returns the length (0 on failure).
//
static ULONG
PsxQueryById(ULONG Api, ULONG Key, PVOID StaticBuf, ULONG StaticSize)
{
    PSX_API_MESSAGE Message;
    PVOID Buf = PsxAllocShared(256);
    ULONG Len;

    if (Buf == NULL) { PsxSetErrno(12 /* ENOMEM */); return 0; }
    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = Key;
    ((PULONG)Message.Data.Raw)[1] = PsxServerPtr(Buf);
    if (PsxCallServer(&Message) < 0) { PsxFreeShared(Buf); return 0; }

    Len = ((PULONG)Message.Data.Raw)[2];
    if (Len == 0 || Len > StaticSize) Len = StaticSize;
    RtlCopyMemory(StaticBuf, Buf, Len);
    PsxFreeShared(Buf);
    return Len;
}

//
// Run a name-keyed query (getpwnam/getgrnam): name @0x30 (also in the shared
// section, server-relative), result buffer @0x34.
//
static ULONG
PsxQueryByName(ULONG Api, PCSTR Name, PVOID StaticBuf, ULONG StaticSize)
{
    PSX_API_MESSAGE Message;
    ULONG NameLen = PsxStrLenA(Name) + 1;
    PVOID NameBuf = PsxAllocShared(NameLen);
    PVOID Buf = PsxAllocShared(256);
    ULONG Len;

    if (NameBuf == NULL || Buf == NULL)
    {
        if (NameBuf) PsxFreeShared(NameBuf);
        if (Buf) PsxFreeShared(Buf);
        PsxSetErrno(12 /* ENOMEM */);
        return 0;
    }
    RtlCopyMemory(NameBuf, Name, NameLen);
    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = PsxServerPtr(NameBuf);
    ((PULONG)Message.Data.Raw)[1] = PsxServerPtr(Buf);
    if (PsxCallServer(&Message) < 0) { PsxFreeShared(NameBuf); PsxFreeShared(Buf); return 0; }

    Len = ((PULONG)Message.Data.Raw)[2];
    if (Len == 0 || Len > StaticSize) Len = StaticSize;
    RtlCopyMemory(StaticBuf, Buf, Len);
    PsxFreeShared(NameBuf);
    PsxFreeShared(Buf);
    return Len;
}

static void PsxFixupPasswd(void)
{
    PULONG F = (PULONG)s_pwbuf;
    s_pw.pw_name  = s_pwbuf + F[0];
    s_pw.pw_uid   = F[1];
    s_pw.pw_gid   = F[2];
    s_pw.pw_dir   = s_pwbuf + F[3];
    s_pw.pw_shell = s_pwbuf + F[4];
}

static void PsxFixupGroup(void)
{
    PULONG F = (PULONG)s_grbuf;
    s_gr.gr_name = s_grbuf + F[0];
    s_gr.gr_gid  = F[1];
    s_gr.gr_mem  = (char **)(s_grbuf + F[2]);   // empty vector -> { NULL }
}

void * __cdecl getpwuid(ULONG Uid)
{
    if (PsxQueryById(PsxApiGetpwuid, Uid, s_pwbuf, sizeof(s_pwbuf)) == 0)
        return NULL;
    PsxFixupPasswd();
    return &s_pw;
}

void * __cdecl getpwnam(const char *Name)
{
    if (Name == NULL || PsxQueryByName(PsxApiGetpwnam, Name, s_pwbuf, sizeof(s_pwbuf)) == 0)
        return NULL;
    PsxFixupPasswd();
    return &s_pw;
}

void * __cdecl getgrgid(ULONG Gid)
{
    if (PsxQueryById(PsxApiGetgrgid, Gid, s_grbuf, sizeof(s_grbuf)) == 0)
        return NULL;
    PsxFixupGroup();
    return &s_gr;
}

void * __cdecl getgrnam(const char *Name)
{
    if (Name == NULL || PsxQueryByName(PsxApiGetgrnam, Name, s_grbuf, sizeof(s_grbuf)) == 0)
        return NULL;
    PsxFixupGroup();
    return &s_gr;
}
