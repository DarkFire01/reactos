/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL file-descriptor syscalls (open/read/write/lseek/dup/...).
 *              Paths are translated + marshalled client-side (path.c); read and
 *              write hand the server the RAW buffer pointer, which it moves with
 *              NtRead/WriteVirtualMemory (no shared-section copy).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// POSIX O_CREAT (from the SDK <fcntl.h>): only then is the varargs mode read.
//
#define PSX_O_CREAT     0x0100

//
// open(path, flags[, mode]) -- ApiNumber 0x18. Server opens the marshalled NT
// path verbatim (POSIX->NT translation already done here).
//
int __cdecl
open(const char *Path, int OpenFlag, ...)
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
        PsxSetErrno(2 /* ENOENT */);
        return -1;
    }
    Message.Data.Open.OpenFlag = (ULONG)OpenFlag;
    Message.Data.Open.Mode = Mode;

    Result = PsxCallServer(&Message);
    PsxFreeMarshalledPath(&Message.Data.Open.Path);
    return (int)Result;
}

int __cdecl
creat(const char *Path, int Mode)
{
    // creat = open for writing, create/truncate.  O_WRONLY|O_CREAT|O_TRUNC.
    return open(Path, 0x0001 | PSX_O_CREAT | 0x0200, Mode);
}

//
// read/write(fd, buf, count) -- ApiNumbers 0x2B/0x2C. Buffer is the caller's
// raw pointer; the server bounces the bytes via NtRead/WriteVirtualMemory.
//
static int
PsxReadWrite(ULONG Api, int FileDescriptor, void *Buffer, unsigned int Count)
{
    PSX_API_MESSAGE Message;

    LONG Result;

    PsxInitMessage(&Message, Api, PSX_BODY_DATALEN(sizeof(PSX_RW_REQUEST)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.ReadWrite.Buffer = (ULONG)(ULONG_PTR)Buffer;   // raw client ptr
    Message.Data.ReadWrite.Count = Count;
    Result = PsxCallServer(&Message);
    if (Result < 0)
        return -1;

    // Controlling terminal: psxss served only the control phase and flagged that
    // the bytes flow directly between us and the session leader (posix.exe).
    if (Message.Data.ReadWrite.HasData)
        return PsxTtyReadWrite(Api == PsxApiWrite, FileDescriptor, Buffer, Count);
    return (int)Result;
}

int __cdecl
read(int FileDescriptor, void *Buffer, unsigned int Count)
{
    return PsxReadWrite(PsxApiRead, FileDescriptor, Buffer, Count);
}

int __cdecl
write(int FileDescriptor, const void *Buffer, unsigned int Count)
{
    return PsxReadWrite(PsxApiWrite, FileDescriptor, (void *)Buffer, Count);
}

//
// lseek(fd, offset, whence) -- ApiNumber 0x2E. Returns the new file position.
//
long __cdecl
lseek(int FileDescriptor, long Offset, int Whence)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiLseek, PSX_BODY_DATALEN(sizeof(PSX_LSEEK_REQUEST)));
    Message.Data.Lseek.FileDescriptor = (ULONG)FileDescriptor;
    Message.Data.Lseek.Whence = (ULONG)Whence;
    Message.Data.Lseek.Offset = Offset;
    return (long)PsxCallServer(&Message);
}

//
// dup(fd) -- 0x28 / dup2(old,new) -- 0x29.
//
int __cdecl
dup(int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiDup, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    return (int)PsxCallServer(&Message);
}

int __cdecl
dup2(int OldFd, int NewFd)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiDup2, PSX_BODY_DATALEN(sizeof(PSX_RW_REQUEST)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)OldFd;    // +0x30
    Message.Data.ReadWrite.Buffer = (ULONG)NewFd;            // +0x34
    return (int)PsxCallServer(&Message);
}

//
// fcntl(fd, cmd, arg) -- ApiNumber 0x2D.
//
int __cdecl
fcntl(int FileDescriptor, int Cmd, ...)
{
    PSX_API_MESSAGE Message;
    long Arg;
    va_list Args;

    va_start(Args, Cmd);
    Arg = va_arg(Args, long);
    va_end(Args);

    PsxInitMessage(&Message, PsxApiFcntl, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;  // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Cmd;             // +0x34
    ((PULONG)Message.Data.Raw)[2] = (ULONG)Arg;             // +0x38
    return (int)PsxCallServer(&Message);
}

//
// isatty(fd) -- ApiNumber 0x16. Server writes the result at Raw[1].
//
int __cdecl
isatty(int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiIsatty, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return 0;
    return (int)((PULONG)Message.Data.Raw)[1];
}

int __cdecl
ftruncate(int FileDescriptor, long Length)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiFtruncate, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;  // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Length;          // +0x34
    return (int)PsxCallServer(&Message);
}

//
// umask(mode) -- ApiNumber 0x19. Returns the previous mask.
//
int __cdecl
umask(int Mode)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiUmask, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Mode;
    return (int)PsxCallServer(&Message);
}

//
// pipe(int fds[2]) -- ApiNumber 0x27. Server returns the two fds at Raw[3]/[4].
//
int __cdecl
pipe(int FileDescriptors[2])
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PsxApiPipe, PSX_BODY_DATALEN(5 * sizeof(ULONG)));
    Result = PsxCallServer(&Message);
    if (Result < 0)
        return -1;
    FileDescriptors[0] = (int)((PULONG)Message.Data.Raw)[3];  // read end  (+0x3C)
    FileDescriptors[1] = (int)((PULONG)Message.Data.Raw)[4];  // write end (+0x40)
    return 0;
}
