/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Directory stream functions (opendir, readdir, rewinddir, closedir)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_O_RDONLY    0x0000
#define PSX_EBADF       9
#define PSX_NAME_MAX    255

/*
 * DIR layout shared with the SDK <dirent.h>. Name holds a struct dirent at DIR+9:
 * d_name[256] first, then d_ino and d_type written by the server.
 */
typedef struct _PSX_DIR
{
    int Fd;
    ULONG Index;
    char RestartScan;
    char Name[256 + sizeof(ULONG) + 1 + 3];
} PSX_DIR, *PPSX_DIR;

void *
__cdecl
opendir(
    _In_z_ const char *Path)
{
    PPSX_DIR Dir;
    int Fd;

    Fd = open(Path, PSX_O_RDONLY);
    if (Fd < 0)
        return NULL;

    Dir = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Dir));
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

/**
 * @brief Reads the next entry. The server writes the entry straight into
 * the Name buffer and returns the name length, or 0 at the end of the directory.
 */
void *
__cdecl
readdir(
    _Inout_opt_ void *Directory)
{
    PPSX_DIR Dir = (PPSX_DIR)Directory;
    PSX_API_MESSAGE Message;
    LONG Result;

    if (Dir == NULL)
    {
        PsxSetErrno(PSX_EBADF);
        return NULL;
    }

    PsxInitMessage(&Message, PsxApiReaddir, PSX_BODY_DATALEN(4 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Dir->Fd;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)Dir->Name;
    Message.Data.Raw[0x0C] = Dir->RestartScan;

    Result = PsxCallServer(&Message);
    Dir->RestartScan = 0;
    if (Result <= 0)
        return NULL;

    /* The server does not terminate d_name */
    if (Result > PSX_NAME_MAX)
        Result = PSX_NAME_MAX;
    Dir->Name[Result] = '\0';

    Dir->Index++;
    return Dir->Name;
}

void
__cdecl
rewinddir(
    _Inout_opt_ void *Directory)
{
    PPSX_DIR Dir = (PPSX_DIR)Directory;

    if (Dir != NULL)
    {
        Dir->RestartScan = 1;
        Dir->Index = 0;
    }
}

int
__cdecl
closedir(
    _In_opt_ void *Directory)
{
    PPSX_DIR Dir = (PPSX_DIR)Directory;
    int Result;

    if (Dir == NULL)
    {
        PsxSetErrno(PSX_EBADF);
        return -1;
    }

    Result = close(Dir->Fd);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Dir);
    return Result;
}
