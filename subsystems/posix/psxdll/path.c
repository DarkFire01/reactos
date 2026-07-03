/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL path translation + shared-section marshalling. The NT 4.0
 *              psxdll performs POSIX->NT path resolution client-side and hands
 *              the server a fully-qualified NT device path; the server opens it
 *              verbatim. Bulk arguments (paths, struct stat) live in the shared
 *              section and travel as server-relative pointers.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// Small ANSI strlen (psxdll imports only ntdll; no CRT string.h).
//
static ULONG
PsxStrLenA(PCSTR s)
{
    PCSTR p = s;
    while (*p != '\0')
        p++;
    return (ULONG)(p - s);
}

//
// Allocate/free from the marshalling heap laid over the shared section.
//
PVOID
PsxAllocShared(IN ULONG Size)
{
    if (PsxSharedHeap == NULL)
        return NULL;
    return RtlAllocateHeap(PsxSharedHeap, 0, Size);
}

VOID
PsxFreeShared(IN PVOID Block)
{
    if ((PsxSharedHeap != NULL) && (Block != NULL))
        RtlFreeHeap(PsxSharedHeap, 0, Block);
}

//
// Append a POSIX path fragment to an NT path, converting '/' to '\' and
// collapsing "." / ".." components (NtCreateFile treats them literally, so we
// resolve them here). Returns the new length.
//
static ULONG
PsxAppendResolved(PCHAR NtOut, ULONG Len, ULONG NtMax, PCSTR Frag)
{
    while (*Frag != '\0')
    {
        // Skip a run of separators.
        if (*Frag == '/' || *Frag == '\\')
        {
            Frag++;
            continue;
        }

        // Isolate the next component.
        {
            PCSTR Start = Frag;
            ULONG CompLen;
            while (*Frag != '\0' && *Frag != '/' && *Frag != '\\')
                Frag++;
            CompLen = (ULONG)(Frag - Start);

            if (CompLen == 1 && Start[0] == '.')
                continue;                       // "." -> stay put

            if (CompLen == 2 && Start[0] == '.' && Start[1] == '.')
            {
                // ".." -> drop the last component (but never above the root; the
                // root prefix "\DosDevices\X:" contains no plain component we set).
                while (Len > 0 && NtOut[Len - 1] != '\\')
                    Len--;
                if (Len > 0)
                    Len--;                      // drop the separator too
                // Do not erase the drive colon: if we backed up onto "X:" keep it.
                if (Len >= 2 && NtOut[Len] == ':' )
                    Len += 1;
                continue;
            }

            if (Len + 1 + CompLen >= NtMax)
                break;                          // truncate rather than overflow
            NtOut[Len++] = '\\';
            RtlCopyMemory(NtOut + Len, Start, CompLen);
            Len += CompLen;
        }
    }
    // A bare drive ("\DosDevices\X:") names the VOLUME device, not its root
    // directory -- stat/open of it (e.g. "." when the cwd is a drive root, or
    // "/") then behaves oddly (ls -> "permission denied"). Append a backslash so
    // it resolves to the root directory "\DosDevices\X:\".
    if (Len > 0 && NtOut[Len - 1] == ':' && Len + 1 < NtMax)
        NtOut[Len++] = '\\';
    NtOut[Len] = '\0';
    return Len;
}

//
// Resolve a POSIX path to an absolute NT device path.
//   "//X/a/b" (drive notation) -> "\DosDevices\X:\a\b"
//   "/a/b"    (absolute)       -> <root>\a\b
//   "a/b"     (relative)       -> <cwd>a\b  (cwd carries a trailing '\')
// Returns the resulting length (0 on failure).
//
ULONG
PsxBuildNtPath(IN PCSTR PosixPath, OUT PCHAR NtOut, IN ULONG NtMax)
{
    ULONG Len = 0;

    if (PosixPath == NULL || PosixPath[0] == '\0')
        return 0;

    // "//<drive>/..." explicit-drive notation.
    if (PosixPath[0] == '/' && PosixPath[1] == '/' &&
        (((PosixPath[2] >= 'A') && (PosixPath[2] <= 'Z')) ||
         ((PosixPath[2] >= 'a') && (PosixPath[2] <= 'z'))) &&
        (PosixPath[3] == '/' || PosixPath[3] == '\0'))
    {
        CHAR Drive = PosixPath[2];
        if (Drive >= 'a' && Drive <= 'z')
            Drive = (CHAR)(Drive - 'a' + 'A');
        if (NtMax < 16) return 0;
        RtlCopyMemory(NtOut, "\\DosDevices\\", 12);
        Len = 12;
        NtOut[Len++] = Drive;
        NtOut[Len++] = ':';
        NtOut[Len] = '\0';
        return PsxAppendResolved(NtOut, Len, NtMax, PosixPath + 3);
    }

    // "<drive>:/..." / "<drive>:\..." Windows-style drive path. Some POSIX
    // programs (e.g. vi/elvis' temp file) build paths from a Windows-derived
    // environment; treat a leading drive letter as an absolute drive path so it
    // does NOT get appended to the cwd (which produced a doubled "C:\C:\...").
    if (((PosixPath[0] >= 'A' && PosixPath[0] <= 'Z') ||
         (PosixPath[0] >= 'a' && PosixPath[0] <= 'z')) &&
        PosixPath[1] == ':')
    {
        CHAR Drive = PosixPath[0];
        if (Drive >= 'a' && Drive <= 'z')
            Drive = (CHAR)(Drive - 'a' + 'A');
        if (NtMax < 16) return 0;
        RtlCopyMemory(NtOut, "\\DosDevices\\", 12);
        Len = 12;
        NtOut[Len++] = Drive;
        NtOut[Len++] = ':';
        NtOut[Len] = '\0';
        return PsxAppendResolved(NtOut, Len, NtMax, PosixPath + 2);  // skip "X:"
    }

    // Absolute POSIX path -> root prefix (no trailing slash) + the path.
    if (PosixPath[0] == '/')
    {
        Len = PsxStartupRootLen ? PsxStartupRootLen
                                : (ULONG)PsxStrLenA(PsxStartupRoot);
        if (Len >= NtMax) return 0;
        RtlCopyMemory(NtOut, PsxStartupRoot, Len);
        NtOut[Len] = '\0';
        return PsxAppendResolved(NtOut, Len, NtMax, PosixPath);
    }

    // Relative path -> current directory (carries a trailing '\') + the path.
    Len = PsxStartupCwdLen ? PsxStartupCwdLen : (ULONG)PsxStrLenA(PsxStartupCwd);
    if (Len >= NtMax) return 0;
    RtlCopyMemory(NtOut, PsxStartupCwd, Len);
    // Drop a trailing separator so PsxAppendResolved re-adds exactly one.
    if (Len > 0 && (NtOut[Len - 1] == '\\' || NtOut[Len - 1] == '/'))
        Len--;
    NtOut[Len] = '\0';
    return PsxAppendResolved(NtOut, Len, NtMax, PosixPath);
}

//
// Translate a POSIX path and marshal it into the shared section as a wide
// UNICODE_STRING whose Buffer is the server-relative address the server reads.
// Returns FALSE (ENOENT-ish) if translation or allocation fails.
//
BOOLEAN
PsxMarshalPath(IN PCSTR PosixPath, OUT PUNICODE_STRING NtPath)
{
    CHAR NtAnsi[PSX_PATH_MAX * 2];
    ULONG AnsiLen;
    PWSTR Wide;
    ULONG i;

    NtPath->Length = 0;
    NtPath->MaximumLength = 0;
    NtPath->Buffer = NULL;

    AnsiLen = PsxBuildNtPath(PosixPath, NtAnsi, sizeof(NtAnsi));
    if (AnsiLen == 0)
        return FALSE;

    Wide = (PWSTR)PsxAllocShared((AnsiLen + 1) * sizeof(WCHAR));
    if (Wide == NULL)
        return FALSE;

    // NT device paths are ASCII; widen byte-by-byte.
    for (i = 0; i < AnsiLen; i++)
        Wide[i] = (WCHAR)(UCHAR)NtAnsi[i];
    Wide[AnsiLen] = L'\0';

    NtPath->Length = (USHORT)(AnsiLen * sizeof(WCHAR));
    NtPath->MaximumLength = (USHORT)((AnsiLen + 1) * sizeof(WCHAR));
    NtPath->Buffer = (PWSTR)(ULONG_PTR)PsxServerPtr(Wide);   // server-relative
    return TRUE;
}

VOID
PsxFreeMarshalledPath(IN PUNICODE_STRING NtPath)
{
    if (NtPath->Buffer != NULL)
    {
        // Convert the server-relative Buffer back to our client pointer to free.
        PVOID Client = (PVOID)((ULONG_PTR)NtPath->Buffer - (LONG_PTR)PsxClientToServer);
        PsxFreeShared(Client);
        NtPath->Buffer = NULL;
    }
}
