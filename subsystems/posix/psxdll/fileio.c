/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     File descriptor system calls (open, read, write, lseek, dup...)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_O_WRONLY    0x0001
#define PSX_O_CREAT     0x0100
#define PSX_O_TRUNC     0x0200

#define PSX_ENOENT      2

/**
 * @brief Opens a file. The mode argument is read only when O_CREAT is set.
 */
int
__cdecl
open(
    _In_z_ const char *Path,
    _In_ int OpenFlag,
    ...)
{
    PSX_API_MESSAGE Message;
    ULONG Mode = 0;
    LONG Result;

    if (OpenFlag & PSX_O_CREAT)
    {
        va_list Args;

        va_start(Args, OpenFlag);
        Mode = (ULONG)va_arg(Args, int);
        va_end(Args);
    }

    PsxInitMessage(&Message, PsxApiOpen, PSX_BODY_DATALEN(sizeof(PSX_OPEN_REQUEST)));
    if (!PsxMarshalPath(Path, &Message.Data.Open.Path))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }
    Message.Data.Open.OpenFlag = (ULONG)OpenFlag;
    Message.Data.Open.Mode = Mode;

    Result = PsxCallServer(&Message);
    PsxFreeMarshalledPath(&Message.Data.Open.Path);
    return (int)Result;
}

int
__cdecl
creat(
    _In_z_ const char *Path,
    _In_ int Mode)
{
    return open(Path, PSX_O_WRONLY | PSX_O_CREAT | PSX_O_TRUNC, Mode);
}

/**
 * @brief Common read/write path. The server moves the data straight from or to
 * the caller's buffer, except for the controlling terminal where it sets HasData
 * and the bytes are exchanged with the session leader instead.
 */
static
int
PsxReadWrite(
    _In_ ULONG Api,
    _In_ int FileDescriptor,
    _Inout_updates_bytes_(Count) void *Buffer,
    _In_ unsigned int Count)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(PSX_RW_REQUEST)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.ReadWrite.Buffer = (ULONG)(ULONG_PTR)Buffer;
    Message.Data.ReadWrite.Count = Count;

    Result = PsxCallServer(&Message);
    if (Result < 0)
        return -1;

    if (Message.Data.ReadWrite.HasData)
        return PsxTtyReadWrite(Api == PsxApiWrite, FileDescriptor, Buffer, Count);

    return (int)Result;
}

int
__cdecl
read(
    _In_ int FileDescriptor,
    _Out_writes_bytes_(Count) void *Buffer,
    _In_ unsigned int Count)
{
    return PsxReadWrite(PsxApiRead, FileDescriptor, Buffer, Count);
}

int
__cdecl
write(
    _In_ int FileDescriptor,
    _In_reads_bytes_(Count) const void *Buffer,
    _In_ unsigned int Count)
{
    return PsxReadWrite(PsxApiWrite, FileDescriptor, (void *)Buffer, Count);
}

long
__cdecl
lseek(
    _In_ int FileDescriptor,
    _In_ long Offset,
    _In_ int Whence)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiLseek, PSX_BODY_DATALEN(sizeof(PSX_LSEEK_REQUEST)));
    Message.Data.Lseek.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.Lseek.Whence = (ULONG)Whence;
    Message.Data.Lseek.Offset = Offset;
    return (long)PsxCallServer(&Message);
}

int
__cdecl
dup(
    _In_ int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiDup, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
dup2(
    _In_ int OldFd,
    _In_ int NewFd)
{
    PSX_API_MESSAGE Message;

    /* The new descriptor travels in the Buffer slot */
    PsxInitMessage(&Message, PsxApiDup2, PSX_BODY_DATALEN(sizeof(PSX_RW_REQUEST)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)OldFd;
    Message.Data.ReadWrite.Buffer = (ULONG)NewFd;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
fcntl(
    _In_ int FileDescriptor,
    _In_ int Cmd,
    ...)
{
    PSX_API_MESSAGE Message;
    long Arg;
    va_list Args;

    va_start(Args, Cmd);
    Arg = va_arg(Args, long);
    va_end(Args);

    PsxInitMessage(&Message, PsxApiFcntl, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Cmd;
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Arg;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
isatty(
    _In_ int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiIsatty, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return 0;

    /* The answer comes back in the second argument slot */
    return (int)((PULONG)Message.Data.Raw)[1];
}

int
__cdecl
ftruncate(
    _In_ int FileDescriptor,
    _In_ long Length)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiFtruncate, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Length;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
umask(
    _In_ int Mode)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiUmask, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Mode;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
pipe(
    _Out_writes_(2) int FileDescriptors[2])
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiPipe, PSX_BODY_DATALEN(5 * sizeof(ULONG)));
    Result = PsxCallServer(&Message);
    if (Result < 0)
        return -1;

    /* Read end in slot 3, write end in slot 4 */
    FileDescriptors[0] = (int)((PULONG)Message.Data.Raw)[3];
    FileDescriptors[1] = (int)((PULONG)Message.Data.Raw)[4];
    return 0;
}
