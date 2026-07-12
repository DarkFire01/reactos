/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL directory streams. opendir wraps open(); readdir sends the
 *              fd plus a raw pointer to the DIR's name buffer (DIR+9), which the
 *              server fills one entry at a time (ApiReaddir 0x3C). The 0x10C-byte
 *              DIR layout matches <dirent.h>.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// DIR layout (1:1 with the SDK <dirent.h>). The embedded struct dirent keeps
// d_name FIRST at DIR+9 (ABI-compatible with the original name-only struct);
// the server appends d_ino (at d_name+0x100) and d_type (+0x104) after it.
// Name[] is sized to hold the whole struct dirent { d_name[256]; d_ino; d_type; }.
//
typedef struct _PSX_DIR
{
    int   Fd;
    ULONG Index;
    char  RestartScan;
    char  Name[256 + sizeof(ULONG) + 1 + 3];   // d_name[256] + d_ino + d_type + pad
} PSX_DIR;

#define PSX_O_RDONLY  0x0000

int __cdecl open(const char *Path, int OpenFlag, ...);
int __cdecl close(int FileDescriptor);

//
// opendir(path) -- open the directory and wrap it in a DIR.
//
void * __cdecl
opendir(const char *Path)
{
    PSX_DIR *Dir;
    int Fd = open(Path, PSX_O_RDONLY);
    if (Fd < 0)
        return NULL;

    Dir = (PSX_DIR *)RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(PSX_DIR));
    if (Dir == NULL)
    {
        close(Fd);
        return NULL;
    }
    Dir->Fd = Fd;
    Dir->Index = 0;
    Dir->RestartScan = 0;
    Dir->Name[0] = '\0';
    return Dir;
}

//
// readdir(dir) -- ApiNumber 0x3C. The server pokes the next entry's name into
// DIR->Name (a raw client pointer) and returns >0; 0 signals end of directory.
//
void * __cdecl
readdir(void *Directory)
{
    PSX_DIR *Dir = (PSX_DIR *)Directory;
    PSX_API_MESSAGE Message;
    LONG Result;

    if (Dir == NULL) { PsxSetErrno(9 /* EBADF */); return NULL; }

    PsxInitMessage(&Message, PsxApiReaddir, PSX_BODY_DATALEN(4 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Dir->Fd;                     // +0x30
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)Dir->Name;        // +0x34 &struct dirent (d_name)
    Message.Data.Raw[0x0C] = Dir->RestartScan;                         // +0x3C rewind

    Result = PsxCallServer(&Message);
    Dir->RestartScan = 0;
    if (Result <= 0)            // 0 = end of directory, -1 = error
        return NULL;

    // The server wrote d_name (ReturnValue = name length) plus d_ino/d_type after
    // it, but no name terminator; the DIR buffer is not zero-filled, so terminate
    // d_name here. Cap to d_name[255] (d_ino/d_type live beyond that).
    if (Result > 255)
        Result = 255;
    Dir->Name[Result] = '\0';

    Dir->Index++;
    return Dir->Name;           // &struct dirent (d_name at DIR+9, d_ino/d_type after)
}

//
// rewinddir(dir) -- the next readdir restarts the scan.
//
void __cdecl
rewinddir(void *Directory)
{
    PSX_DIR *Dir = (PSX_DIR *)Directory;
    if (Dir != NULL)
    {
        Dir->RestartScan = 1;
        Dir->Index = 0;
    }
}

//
// closedir(dir) -- close the fd and free the DIR.
//
int __cdecl
closedir(void *Directory)
{
    PSX_DIR *Dir = (PSX_DIR *)Directory;
    int Result;

    if (Dir == NULL) { PsxSetErrno(9 /* EBADF */); return -1; }
    Result = close(Dir->Fd);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Dir);
    return Result;
}
