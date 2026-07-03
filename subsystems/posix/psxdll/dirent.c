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
// DIR layout (1:1 with the SDK <dirent.h>): { int fd; ULONG index; char
// restart; char d_name[256]; } -> d_name at offset 9, total padded to 0x10C.
//
typedef struct _PSX_DIR
{
    int   Fd;
    ULONG Index;
    char  RestartScan;
    char  Name[256];    // struct dirent { char d_name[NAME_MAX+1]; }
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
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)Dir->Name;        // +0x34 raw ptr
    Message.Data.Raw[0x0C] = Dir->RestartScan;                         // +0x3C rewind

    Result = PsxCallServer(&Message);
    Dir->RestartScan = 0;
    if (Result <= 0)            // 0 = end of directory, -1 = error
        return NULL;

    // The server writes the name bytes (ReturnValue = length) but no terminator;
    // the DIR buffer is not zero-filled, so terminate d_name here.
    if (Result > (LONG)sizeof(Dir->Name) - 1)
        Result = (LONG)sizeof(Dir->Name) - 1;
    Dir->Name[Result] = '\0';

    Dir->Index++;
    return Dir->Name;           // &struct dirent (d_name at DIR+9)
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
