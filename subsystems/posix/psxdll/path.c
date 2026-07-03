/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX to NT path translation and shared section marshalling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_DOSDEVICES_PREFIX       "\\DosDevices\\"
#define PSX_DOSDEVICES_PREFIX_LEN   12

ULONG
PsxStringLengthA(
    _In_z_ PCSTR String)
{
    PCSTR End = String;

    while (*End != '\0')
        End++;

    return (ULONG)(End - String);
}

PVOID
PsxAllocShared(
    _In_ ULONG Size)
{
    if (PsxSharedHeap == NULL)
        return NULL;

    return RtlAllocateHeap(PsxSharedHeap, 0, Size);
}

VOID
PsxFreeShared(
    _In_opt_ PVOID Block)
{
    if (PsxSharedHeap != NULL && Block != NULL)
        RtlFreeHeap(PsxSharedHeap, 0, Block);
}

static
BOOLEAN
PsxIsDriveLetter(
    _In_ CHAR Char)
{
    return (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z');
}

/**
 * @brief Appends a POSIX path fragment to an NT path, converting separators and
 * resolving "." and ".." components.
 *
 * @return The new length of NtOut.
 */
static
ULONG
PsxAppendResolved(
    _Inout_updates_z_(NtMax) PCHAR NtOut,
    _In_ ULONG Length,
    _In_ ULONG NtMax,
    _In_z_ PCSTR Fragment)
{
    PCSTR Start;
    ULONG ComponentLength;

    while (*Fragment != '\0')
    {
        if (*Fragment == '/' || *Fragment == '\\')
        {
            Fragment++;
            continue;
        }

        Start = Fragment;
        while (*Fragment != '\0' && *Fragment != '/' && *Fragment != '\\')
            Fragment++;
        ComponentLength = (ULONG)(Fragment - Start);

        if (ComponentLength == 1 && Start[0] == '.')
            continue;

        if (ComponentLength == 2 && Start[0] == '.' && Start[1] == '.')
        {
            /* Drop the last component and its separator, but keep a drive colon */
            while (Length > 0 && NtOut[Length - 1] != '\\')
                Length--;
            if (Length > 0)
                Length--;
            if (Length >= 2 && NtOut[Length] == ':')
                Length += 1;
            continue;
        }

        /* Truncate rather than overflow */
        if (Length + 1 + ComponentLength >= NtMax)
            break;

        NtOut[Length++] = '\\';
        RtlCopyMemory(NtOut + Length, Start, ComponentLength);
        Length += ComponentLength;
    }

    /* A bare "\DosDevices\X:" names the volume, so point it at the root directory */
    if (Length > 0 && NtOut[Length - 1] == ':' && Length + 1 < NtMax)
        NtOut[Length++] = '\\';

    NtOut[Length] = '\0';
    return Length;
}

/**
 * @brief Writes "\DosDevices\X:" for the given drive letter into NtOut.
 *
 * @return The prefix length.
 */
static
ULONG
PsxWriteDrivePrefix(
    _Out_writes_z_(PSX_DOSDEVICES_PREFIX_LEN + 3) PCHAR NtOut,
    _In_ CHAR Drive)
{
    ULONG Length = PSX_DOSDEVICES_PREFIX_LEN;

    if (Drive >= 'a' && Drive <= 'z')
        Drive = (CHAR)(Drive - 'a' + 'A');

    RtlCopyMemory(NtOut, PSX_DOSDEVICES_PREFIX, PSX_DOSDEVICES_PREFIX_LEN);
    NtOut[Length++] = Drive;
    NtOut[Length++] = ':';
    NtOut[Length] = '\0';
    return Length;
}

/**
 * @brief Resolves a POSIX path to an absolute NT device path.
 *
 * "//X/a" and "X:/a" name drive X directly, "/a" is relative to the POSIX root
 * and anything else is relative to the current directory.
 *
 * @return The resulting length, or 0 on failure.
 */
ULONG
PsxBuildNtPath(
    _In_opt_z_ PCSTR PosixPath,
    _Out_writes_z_(NtMax) PCHAR NtOut,
    _In_ ULONG NtMax)
{
    ULONG Length;

    if (PosixPath == NULL || PosixPath[0] == '\0')
        return 0;

    /* "//X/..." drive notation */
    if (PosixPath[0] == '/' && PosixPath[1] == '/' &&
        PsxIsDriveLetter(PosixPath[2]) &&
        (PosixPath[3] == '/' || PosixPath[3] == '\0'))
    {
        if (NtMax < 16)
            return 0;

        Length = PsxWriteDrivePrefix(NtOut, PosixPath[2]);
        return PsxAppendResolved(NtOut, Length, NtMax, PosixPath + 3);
    }

    /* "X:/..." or "X:\..." Windows style drive path, not relative to the cwd */
    if (PsxIsDriveLetter(PosixPath[0]) && PosixPath[1] == ':')
    {
        if (NtMax < 16)
            return 0;

        Length = PsxWriteDrivePrefix(NtOut, PosixPath[0]);
        return PsxAppendResolved(NtOut, Length, NtMax, PosixPath + 2);
    }

    /* Absolute POSIX path: root prefix (no trailing separator) plus the path */
    if (PosixPath[0] == '/')
    {
        Length = PsxStartupRootLen ? PsxStartupRootLen : PsxStringLengthA(PsxStartupRoot);
        if (Length >= NtMax)
            return 0;

        RtlCopyMemory(NtOut, PsxStartupRoot, Length);
        NtOut[Length] = '\0';
        return PsxAppendResolved(NtOut, Length, NtMax, PosixPath);
    }

    /* Relative path: current directory plus the path */
    Length = PsxStartupCwdLen ? PsxStartupCwdLen : PsxStringLengthA(PsxStartupCwd);
    if (Length >= NtMax)
        return 0;

    RtlCopyMemory(NtOut, PsxStartupCwd, Length);

    /* Drop the trailing separator so exactly one is added back */
    if (Length > 0 && (NtOut[Length - 1] == '\\' || NtOut[Length - 1] == '/'))
        Length--;

    NtOut[Length] = '\0';
    return PsxAppendResolved(NtOut, Length, NtMax, PosixPath);
}

/**
 * @brief Translates a POSIX path and copies it into the shared section as a
 * UNICODE_STRING whose Buffer is a server-relative address.
 *
 * @return FALSE if translation or allocation fails.
 */
BOOLEAN
PsxMarshalPath(
    _In_opt_z_ PCSTR PosixPath,
    _Out_ PUNICODE_STRING NtPath)
{
    CHAR NtAnsi[PSX_PATH_MAX * 2];
    ULONG AnsiLength;
    PWSTR Wide;
    ULONG i;

    NtPath->Length = 0;
    NtPath->MaximumLength = 0;
    NtPath->Buffer = NULL;

    AnsiLength = PsxBuildNtPath(PosixPath, NtAnsi, sizeof(NtAnsi));
    if (AnsiLength == 0)
        return FALSE;

    Wide = (PWSTR)PsxAllocShared((AnsiLength + 1) * sizeof(WCHAR));
    if (Wide == NULL)
        return FALSE;

    /* NT device paths are ASCII */
    for (i = 0; i < AnsiLength; i++)
        Wide[i] = (WCHAR)(UCHAR)NtAnsi[i];
    Wide[AnsiLength] = L'\0';

    NtPath->Length = (USHORT)(AnsiLength * sizeof(WCHAR));
    NtPath->MaximumLength = (USHORT)((AnsiLength + 1) * sizeof(WCHAR));
    NtPath->Buffer = (PWSTR)(ULONG_PTR)PsxServerPtr(Wide);
    return TRUE;
}

VOID
PsxFreeMarshalledPath(
    _Inout_ PUNICODE_STRING NtPath)
{
    PVOID ClientBuffer;

    if (NtPath->Buffer == NULL)
        return;

    /* Convert the server-relative Buffer back to a client address */
    ClientBuffer = (PVOID)((ULONG_PTR)NtPath->Buffer - (LONG_PTR)PsxClientToServer);
    PsxFreeShared(ClientBuffer);
    NtPath->Buffer = NULL;
}
