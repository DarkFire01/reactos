/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Per-process descriptor table and file-related API handlers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/iofuncs.h>

/* FILE_NAMES_INFORMATION is kernel-mode only in the NDK, so mirror its layout here */
typedef struct _PSX_FILE_NAMES_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
} PSX_FILE_NAMES_INFORMATION;

/* FileBothDirectoryInformation layout, used by readdir to get the name and attributes in one call */
typedef struct _PSX_FILE_BOTH_DIR_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    WCHAR FileName[1];
} PSX_FILE_BOTH_DIR_INFORMATION;

/* POSIX errno values returned to the client (subset of psx/errno.h) */
#define PSX_ENOENT   2
#define PSX_EIO      5
#define PSX_EBADF    9
#define PSX_ENOMEM   12
#define PSX_EACCES   13
#define PSX_EEXIST   17
#define PSX_ENOTDIR  20
#define PSX_EISDIR   21
#define PSX_EINVAL   22
#define PSX_EMFILE   24
#define PSX_ENOTTY   25
#define PSX_ENOSPC   28
#define PSX_ESPIPE   29

/* lseek whence values */
#define PSX_SEEK_SET 0
#define PSX_SEEK_CUR 1
#define PSX_SEEK_END 2

/* Returned by PsxClassifyDeviceNode when the path is not a /dev node */
#define PSX_NOT_A_DEVICE 0xFFFFFFFF

/**
 * @brief Map an NTSTATUS to a POSIX errno value.
 */
ULONG
PsxErrnoFromStatus(
    _In_ NTSTATUS Status)
{
    switch (Status)
    {
        case STATUS_SUCCESS:
            return 0;

        case STATUS_NO_MEMORY:
            return PSX_ENOMEM;

        case STATUS_INVALID_PARAMETER:
            return PSX_EINVAL;

        case STATUS_OBJECT_NAME_NOT_FOUND:
        case STATUS_OBJECT_PATH_NOT_FOUND:
        case STATUS_OBJECT_NAME_INVALID:
            return PSX_ENOENT;

        case STATUS_OBJECT_NAME_COLLISION:
            return PSX_EEXIST;

        case STATUS_NOT_A_DIRECTORY:
            return PSX_ENOTDIR;

        case STATUS_FILE_IS_A_DIRECTORY:
            return PSX_EISDIR;

        case STATUS_DISK_FULL:
            return PSX_ENOSPC;

        default:
            return PSX_EACCES;
    }
}

/**
 * @brief Install a file object in the lowest free descriptor slot.
 * @return The descriptor, or -1 if the table is full.
 */
INT
PsxAllocateFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File)
{
    INT Fd;

    for (Fd = 0; Fd < PSX_OPEN_MAX; Fd++)
    {
        if (Process->FdTable[Fd] == NULL)
        {
            Process->FdTable[Fd] = File;
            return Fd;
        }
    }

    return -1;
}

PPSX_FILE_OBJECT
PsxGetFile(
    _In_ PPSX_PROCESS Process,
    _In_ INT FileDescriptor)
{
    if ((FileDescriptor < 0) || (FileDescriptor >= PSX_OPEN_MAX))
        return NULL;

    return Process->FdTable[FileDescriptor];
}

/**
 * @brief Drop a reference to a file object and release it when the count reaches zero.
 */
static
VOID
PsxDereferenceFile(
    _In_ PPSX_FILE_OBJECT File)
{
    if (InterlockedDecrement(&File->RefCount) != 0)
        return;

    if ((File->FileType == PSX_FILE_PIPE) && (File->Pipe != NULL))
        PsxPipeCloseEnd(File);
    else if (((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS)) && (File->Pty != NULL))
        PsxPtyClose(File);
    else if (File->FileType == PSX_FILE_XCONN)
        PsxXConnClose(File);
    else if (File->NtHandle != NULL)
        NtClose(File->NtHandle);

    RtlFreeHeap(RtlGetProcessHeap(), 0, File);
}

INT
PsxCloseFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ INT FileDescriptor)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, FileDescriptor);

    if (File == NULL)
        return -PSX_EBADF;

    Process->FdTable[FileDescriptor] = NULL;
    PsxDereferenceFile(File);
    return 0;
}

VOID
PsxCloseAllFds(
    _Inout_ PPSX_PROCESS Process)
{
    INT Fd;

    for (Fd = 0; Fd < PSX_OPEN_MAX; Fd++)
    {
        if (Process->FdTable[Fd] != NULL)
            PsxCloseFd(Process, Fd);
    }
}

/**
 * @brief Translate POSIX open() flags into an NtCreateFile call.
 */
static
NTSTATUS
PsxOpenNtFile(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_API_MESSAGE Message,
    _In_ PUNICODE_STRING NtPath,
    _In_ ULONG OpenFlags,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    ACCESS_MASK DesiredAccess;
    ULONG Disposition;
    NTSTATUS Status;

    /* O_ACCMODE */
    switch (OpenFlags & 7)
    {
        case 1:
            /* O_WRONLY */
            DesiredAccess = FILE_GENERIC_WRITE;
            break;

        case 2:
            /* O_RDWR */
            DesiredAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
            break;

        default:
            /* O_RDONLY */
            DesiredAccess = FILE_GENERIC_READ;
            break;
    }

    DesiredAccess |= SYNCHRONIZE;

    /* O_APPEND */
    if (OpenFlags & 8)
        DesiredAccess |= FILE_APPEND_DATA;

    /* O_CREAT, O_EXCL and O_TRUNC select the create disposition */
    if (OpenFlags & 0x100)
    {
        if (OpenFlags & 0x400)
            Disposition = FILE_CREATE;
        else if (OpenFlags & 0x200)
            Disposition = FILE_OVERWRITE_IF;
        else
            Disposition = FILE_OPEN_IF;
    }
    else
    {
        if (OpenFlags & 0x200)
            Disposition = FILE_OVERWRITE;
        else
            Disposition = FILE_OPEN;
    }

    InitializeObjectAttributes(&ObjectAttributes, NtPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    /*
     * FILE_NON_DIRECTORY_FILE is not set because opendir() opens directories through open().
     * The open runs under the client token so ownership and access checks use the POSIX user.
     */
    PsxImpersonateClient(Process, Message);
    Status = NtCreateFile(Handle,
                          DesiredAccess,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          Disposition,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    PsxRevertToSelf();
    return Status;
}

typedef struct _PSX_DEVICE_NODE
{
    PCWSTR Name;
    ULONG Type;
} PSX_DEVICE_NODE;

static const PSX_DEVICE_NODE PsxDeviceNodes[] =
{
    { L"null",    PSX_FILE_DEVNULL },
    { L"zero",    PSX_FILE_DEVZERO },
    { L"full",    PSX_FILE_DEVFULL },
    { L"random",  PSX_FILE_DEVRANDOM },
    { L"urandom", PSX_FILE_DEVRANDOM },
    { L"tty",     PSX_FILE_TTY },
    { L"x11",     PSX_FILE_XCONN },
    { L"xpoll",   PSX_FILE_XPOLL },
    { L"ptmx",    PSX_FILE_PTMX },
};

static
BOOLEAN
PsxIsPathSeparator(
    _In_ WCHAR Char)
{
    return (Char == L'\\') || (Char == L'/');
}

/**
 * @brief Check whether the "\dev\" component ends right before Buffer[End].
 */
static
BOOLEAN
PsxHasDevPrefix(
    _In_ PCWSTR Buffer,
    _In_ ULONG End)
{
    return PsxIsPathSeparator(Buffer[End - 5]) &&
           (RtlUpcaseUnicodeChar(Buffer[End - 4]) == L'D') &&
           (RtlUpcaseUnicodeChar(Buffer[End - 3]) == L'E') &&
           (RtlUpcaseUnicodeChar(Buffer[End - 2]) == L'V') &&
           PsxIsPathSeparator(Buffer[End - 1]);
}

/**
 * @brief Identify a synthetic /dev node from a translated NT path.
 * @return A PSX_FILE_* type, or PSX_NOT_A_DEVICE.
 *
 * The path ends in "\dev\<name>"; the match is case-insensitive and accepts '/' as a separator.
 */
static
ULONG
PsxClassifyDeviceNode(
    _In_ PUNICODE_STRING Path)
{
    ULONG PathChars = Path->Length / sizeof(WCHAR);
    ULONG NameChars;
    ULONG NameStart;
    ULONG Entry;
    ULONG Char;

    if ((Path->Buffer == NULL) || (PathChars < 5))
        return PSX_NOT_A_DEVICE;

    for (Entry = 0; Entry < RTL_NUMBER_OF(PsxDeviceNodes); Entry++)
    {
        NameChars = (ULONG)wcslen(PsxDeviceNodes[Entry].Name);
        if (PathChars < NameChars + 5)
            continue;

        NameStart = PathChars - NameChars;
        if (!PsxHasDevPrefix(Path->Buffer, NameStart))
            continue;

        for (Char = 0; Char < NameChars; Char++)
        {
            if (RtlUpcaseUnicodeChar(Path->Buffer[NameStart + Char]) !=
                RtlUpcaseUnicodeChar(PsxDeviceNodes[Entry].Name[Char]))
            {
                break;
            }
        }

        if (Char == NameChars)
            return PsxDeviceNodes[Entry].Type;
    }

    return PSX_NOT_A_DEVICE;
}

/**
 * @brief Match "\dev\pts\<N>" and extract the pty index N.
 */
static
BOOLEAN
PsxClassifyPts(
    _In_ PUNICODE_STRING Path,
    _Out_ PULONG Index)
{
    ULONG PathChars = Path->Length / sizeof(WCHAR);
    PWCHAR Buffer = Path->Buffer;
    LONG Pos;
    LONG DigitStart;
    ULONG Value = 0;

    /* The shortest match, "\dev\pts\N", is 9 characters */
    if ((Buffer == NULL) || (PathChars < 9))
        return FALSE;

    Pos = (LONG)PathChars - 1;
    while ((Pos >= 0) && (Buffer[Pos] >= L'0') && (Buffer[Pos] <= L'9'))
        Pos--;

    DigitStart = Pos + 1;
    if (DigitStart > (LONG)PathChars - 1)
        return FALSE;

    if (Pos < 8)
        return FALSE;

    if (!PsxIsPathSeparator(Buffer[Pos]))
        return FALSE;

    if ((RtlUpcaseUnicodeChar(Buffer[Pos - 3]) != L'P') ||
        (RtlUpcaseUnicodeChar(Buffer[Pos - 2]) != L'T') ||
        (RtlUpcaseUnicodeChar(Buffer[Pos - 1]) != L'S') ||
        !PsxIsPathSeparator(Buffer[Pos - 4]))
    {
        return FALSE;
    }

    if (!PsxHasDevPrefix(Buffer, (ULONG)(Pos - 3)))
        return FALSE;

    for (Pos = DigitStart; Pos < (LONG)PathChars; Pos++)
        Value = Value * 10 + (ULONG)(Buffer[Pos] - L'0');

    *Index = Value;
    return TRUE;
}

/* Exported by ntdll but not declared by the NDK headers used here */
ULONG
NTAPI
RtlRandomEx(
    _Inout_ PULONG Seed);

/**
 * @brief Fill a buffer with pseudo-random bytes for /dev/random and /dev/urandom.
 *
 * Not cryptographically secure.
 */
static
VOID
PsxFillRandom(
    _Out_writes_bytes_(Count) PUCHAR Buffer,
    _In_ ULONG Count)
{
    static ULONG Seed = 0;
    ULONG Filled = 0;
    ULONG Random;
    INT Byte;

    if (Seed == 0)
        Seed = GetTickCount() ^ 0x9E3779B9u;

    while (Filled < Count)
    {
        Random = RtlRandomEx(&Seed);
        for (Byte = 0; (Byte < 4) && (Filled < Count); Byte++, Filled++)
            Buffer[Filled] = (UCHAR)(Random >> (8 * Byte));
    }
}

/**
 * @brief Serve a read from /dev/zero, /dev/full, /dev/random or /dev/urandom.
 */
static
VOID
PsxDevRead(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message,
    _In_ ULONG DevType)
{
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PVOID Bounce;
    NTSTATUS Status;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    if (DevType == PSX_FILE_DEVRANDOM)
        PsxFillRandom(Bounce, Count);
    else
        RtlZeroMemory(Bounce, Count);

    Status = NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Count, NULL);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = (LONG)Count;
}

/**
 * @brief Create a descriptor bound to a synthetic device object with no NT handle.
 * @return The descriptor, or a negative errno.
 */
static
INT
PsxOpenDeviceFd(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG DevType,
    _In_ ULONG OpenFlags)
{
    PPSX_FILE_OBJECT File;
    INT Fd;

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*File));
    if (File == NULL)
        return -PSX_ENOMEM;

    File->RefCount = 1;
    File->NtHandle = NULL;
    File->OpenFlags = OpenFlags;
    File->FileType = DevType;

    Fd = PsxAllocateFd(Process, File);
    if (Fd < 0)
    {
        PsxDereferenceFile(File);
        return -PSX_EMFILE;
    }

    return Fd;
}

/**
 * @brief open(path, oflag, mode), API 0x18.
 *
 * The path buffer points into the client's shared section and is bounds-checked before use.
 */
VOID
PsxSrvOpen(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = Message->Data.Open.Path;
    ULONG OpenFlags = Message->Data.Open.OpenFlag;
    ULONG DevType;
    ULONG PtsIndex = 0;
    BOOLEAN IsDevice = TRUE;
    HANDLE Handle;
    PPSX_FILE_OBJECT File;
    NTSTATUS Status;
    INT Fd;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    /* Synthetic /dev nodes are served entirely in psxss */
    DevType = PsxClassifyDeviceNode(&Path);
    if (DevType == PSX_FILE_PTMX)
        Fd = PsxPtyOpenMaster(Process, OpenFlags);
    else if (PsxClassifyPts(&Path, &PtsIndex))
        Fd = PsxPtyOpenSlave(Process, PtsIndex, OpenFlags);
    else if (DevType == PSX_FILE_XCONN)
        Fd = PsxOpenXConnFd(Process, OpenFlags);
    else if (DevType != PSX_NOT_A_DEVICE)
        Fd = PsxOpenDeviceFd(Process, DevType, OpenFlags);
    else
        IsDevice = FALSE;

    if (IsDevice)
    {
        if (Fd < 0)
        {
            Message->Errno = (ULONG)(-Fd);
            Message->ReturnValue = -1;
            return;
        }

        Message->Errno = 0;
        Message->ReturnValue = Fd;
        return;
    }

    Status = PsxOpenNtFile(Process, Message, &Path, OpenFlags, &Handle);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*File));
    if (File == NULL)
    {
        NtClose(Handle);
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    File->RefCount = 1;
    File->NtHandle = Handle;
    File->OpenFlags = OpenFlags;
    File->FileType = PSX_FILE_DISK;

    Fd = PsxAllocateFd(Process, File);
    if (Fd < 0)
    {
        PsxDereferenceFile(File);
        Message->Errno = PSX_EMFILE;
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = Fd;
}

/**
 * @brief close(fd), API 0x2A.
 */
VOID
PsxSrvClose(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Result = PsxCloseFd(Process, (INT)Message->Data.ReadWrite.FileDescriptor);

    if (Result < 0)
    {
        Message->Errno = (ULONG)(-Result);
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief lseek(fd, offset, whence), API 0x2E.
 *
 * The new position is also returned in the offset slot of the message.
 */
VOID
PsxSrvLseek(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.Lseek.FileDescriptor);
    ULONG Whence = Message->Data.Lseek.Whence;
    LONG Offset = Message->Data.Lseek.Offset;
    FILE_STANDARD_INFORMATION StandardInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    LONGLONG Base;
    LONGLONG NewPosition;
    NTSTATUS Status;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    /* Pipes, ttys and devices are not seekable */
    if (File->FileType != PSX_FILE_DISK)
    {
        Message->Errno = PSX_ESPIPE;
        Message->ReturnValue = -1;
        return;
    }

    switch (Whence)
    {
        case PSX_SEEK_SET:
            Base = 0;
            break;

        case PSX_SEEK_CUR:
            Base = File->Offset.QuadPart;
            break;

        case PSX_SEEK_END:
            Status = NtQueryInformationFile(File->NtHandle,
                                            &IoStatusBlock,
                                            &StandardInfo,
                                            sizeof(StandardInfo),
                                            FileStandardInformation);
            if (!NT_SUCCESS(Status))
            {
                Message->Errno = PSX_EINVAL;
                Message->ReturnValue = -1;
                return;
            }
            Base = StandardInfo.EndOfFile.QuadPart;
            break;

        default:
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
    }

    NewPosition = Base + Offset;
    if (NewPosition < 0)
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    File->Offset.QuadPart = NewPosition;
    Message->Data.Lseek.Offset = (LONG)NewPosition;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)NewPosition;
}

/**
 * @brief umask(mask), API 0x19. Swaps the file creation mask and returns the previous one.
 */
VOID
PsxSrvUmask(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG Previous = Process->Umask;

    /* Keep the low 9 permission bits */
    Process->Umask = ((PULONG)Message->Data.Raw)[0] & 0x1FF;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Previous;
}

/* 40-byte POSIX struct stat, laid out to match psx/sys/stat.h */
typedef struct _PSX_STAT
{
    ULONG st_mode;      // 0x00
    ULONG st_ino;       // 0x04
    ULONG st_dev;       // 0x08
    ULONG st_nlink;     // 0x0C
    ULONG st_uid;       // 0x10
    ULONG st_gid;       // 0x14
    LONG  st_size;      // 0x18
    LONG  st_atime;     // 0x1C
    LONG  st_mtime;     // 0x20
    LONG  st_ctime;     // 0x24
} PSX_STAT, *PPSX_STAT;

/* st_mode file type bits */
#define PSX_S_IFIFO   0x1000
#define PSX_S_IFCHR   0x2000
#define PSX_S_IFDIR   0x4000
#define PSX_S_IFREG   0x8000

static
LONG
PsxFileTimeToUnix(
    _In_ PLARGE_INTEGER Time)
{
    ULONG Seconds = 0;

    if (Time->QuadPart != 0)
        RtlTimeToSecondsSince1970(Time, &Seconds);

    return (LONG)Seconds;
}

/**
 * @brief Fill a POSIX stat structure from an open NT file handle.
 */
static
NTSTATUS
PsxFillStat(
    _In_ HANDLE Handle,
    _In_ PPSX_PROCESS Process,
    _Out_ PPSX_STAT Stat)
{
    FILE_BASIC_INFORMATION BasicInfo;
    FILE_STANDARD_INFORMATION StandardInfo;
    FILE_INTERNAL_INFORMATION InternalInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    ULONG Mode;

    Status = NtQueryInformationFile(Handle,
                                    &IoStatusBlock,
                                    &BasicInfo,
                                    sizeof(BasicInfo),
                                    FileBasicInformation);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&StandardInfo, sizeof(StandardInfo));
    RtlZeroMemory(&InternalInfo, sizeof(InternalInfo));
    NtQueryInformationFile(Handle,
                           &IoStatusBlock,
                           &StandardInfo,
                           sizeof(StandardInfo),
                           FileStandardInformation);
    NtQueryInformationFile(Handle,
                           &IoStatusBlock,
                           &InternalInfo,
                           sizeof(InternalInfo),
                           FileInternalInformation);

    if (BasicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        Mode = PSX_S_IFDIR | ((BasicInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0755);
    }
    else
    {
        /* NT has no execute bit outside of ACLs, so report x or PATH searches reject every binary */
        Mode = PSX_S_IFREG | ((BasicInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0777);
    }

    Stat->st_mode = Mode;
    Stat->st_ino = InternalInfo.IndexNumber.LowPart;
    Stat->st_dev = 0;
    Stat->st_nlink = StandardInfo.NumberOfLinks ? StandardInfo.NumberOfLinks : 1;

    /* TODO: map the file owner SID */
    Stat->st_uid = Process->Uid;
    Stat->st_gid = Process->Gid;
    Stat->st_size = StandardInfo.EndOfFile.LowPart;
    Stat->st_atime = PsxFileTimeToUnix(&BasicInfo.LastAccessTime);
    Stat->st_mtime = PsxFileTimeToUnix(&BasicInfo.LastWriteTime);
    Stat->st_ctime = PsxFileTimeToUnix(&BasicInfo.ChangeTime);
    return STATUS_SUCCESS;
}

/**
 * @brief stat(path, buf), API 0x1F.
 *
 * Both the path and the result buffer point into the client's shared section.
 */
VOID
PsxSrvStat(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = Message->Data.Stat.Path;
    ULONG_PTR StatPtr = Message->Data.Stat.StatBuffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PPSX_STAT Stat;
    ULONG DevType;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length) ||
        !PsxValidateClientPointer(Process, StatPtr, sizeof(PSX_STAT)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    /* Synthetic /dev nodes report a character device */
    DevType = PsxClassifyDeviceNode(&Path);
    if (DevType != PSX_NOT_A_DEVICE)
    {
        Stat = (PPSX_STAT)StatPtr;
        RtlZeroMemory(Stat, sizeof(*Stat));
        Stat->st_mode = (DevType == PSX_FILE_TTY) ? (PSX_S_IFCHR | 0700) : (PSX_S_IFCHR | 0666);
        Stat->st_nlink = 1;
        Stat->st_uid = Process->Uid;
        Stat->st_gid = Process->Gid;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    Status = PsxFillStat(Handle, Process, (PPSX_STAT)StatPtr);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief access(path, mode), API 0x21.
 *
 * Opens the path with the access the mode bits request. Uses the open request layout.
 */
VOID
PsxSrvAccess(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = Message->Data.Open.Path;
    ULONG ModeMask = Message->Data.Open.OpenFlag;
    ACCESS_MASK DesiredAccess = SYNCHRONIZE;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    /* F_OK only tests for existence */
    if (ModeMask == 0)
        DesiredAccess |= FILE_READ_ATTRIBUTES;

    /* R_OK */
    if (ModeMask & 4)
        DesiredAccess |= FILE_READ_DATA;

    /* W_OK */
    if (ModeMask & 2)
        DesiredAccess |= FILE_WRITE_DATA;

    /* X_OK */
    if (ModeMask & 1)
        DesiredAccess |= FILE_EXECUTE;

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle,
                        DesiredAccess,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(Status))
    {
        NtClose(Handle);
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Message->Errno = PsxErrnoFromStatus(Status);
    Message->ReturnValue = -1;
}

/**
 * @brief fstat(fd, buf), API 0x20.
 */
VOID
PsxSrvFstat(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.Fstat.FileDescriptor);
    ULONG_PTR StatPtr = Message->Data.Fstat.StatBuffer;
    PPSX_STAT Stat;
    NTSTATUS Status;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    if (!PsxValidateClientPointer(Process, StatPtr, sizeof(PSX_STAT)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    /* Objects without an NT handle still report a file type so isatty() and S_ISCHR work */
    if (File->FileType != PSX_FILE_DISK)
    {
        Stat = (PPSX_STAT)StatPtr;
        RtlZeroMemory(Stat, sizeof(*Stat));
        if (File->FileType == PSX_FILE_PIPE)
            Stat->st_mode = PSX_S_IFIFO | 0600;
        else if (File->FileType == PSX_FILE_TTY)
            Stat->st_mode = PSX_S_IFCHR | 0700;
        else
            Stat->st_mode = PSX_S_IFCHR | 0666;

        Stat->st_nlink = 1;
        Stat->st_uid = Process->Uid;
        Stat->st_gid = Process->Gid;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Status = PsxFillStat(File->NtHandle, Process, (PPSX_STAT)StatPtr);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief Poll this process's /dev/x11 connection for readability through a read() on /dev/xpoll.
 *
 * Count encodes the timeout: 1 is a non-blocking probe, N waits N-1 ms. One byte is written
 * to the client buffer, '1' when readable or '0' on timeout, and read() returns 1.
 */
static
VOID
PsxXPollRead(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PPSX_FILE_OBJECT XConn = NULL;
    UCHAR Result = '0';
    INT Fd;

    for (Fd = 0; Fd < PSX_OPEN_MAX; Fd++)
    {
        if ((Process->FdTable[Fd] != NULL) &&
            (Process->FdTable[Fd]->FileType == PSX_FILE_XCONN))
        {
            XConn = Process->FdTable[Fd];
            break;
        }
    }

    if ((XConn != NULL) && (Count != 0) && (PsxPollWait(XConn, Count - 1) == 1))
        Result = '1';

    NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, &Result, 1, NULL);
    Message->Errno = 0;
    Message->ReturnValue = 1;
}

/**
 * @brief read(fd, buf, n), API 0x2B.
 *
 * Tty reads only set HasData so the client exchanges the bytes with posix.exe.
 * Disk reads go through a bounce buffer that is copied into the client process.
 */
VOID
PsxSrvRead(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.ReadWrite.FileDescriptor);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PVOID Bounce;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER Offset;
    NTSTATUS Status;
    ULONG Transferred = 0;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    /* posix.exe owns the console, so the client moves tty bytes with it directly */
    if (File->FileType == PSX_FILE_TTY)
    {
        Message->Data.ReadWrite.HasData = 1;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    /* O_WRONLY */
    if ((File->OpenFlags & 7) == 1)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Message->Data.ReadWrite.HasData = 0;

    /* /dev/null always reads as end of file */
    if (File->FileType == PSX_FILE_DEVNULL)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    if ((File->FileType == PSX_FILE_DEVZERO) ||
        (File->FileType == PSX_FILE_DEVFULL) ||
        (File->FileType == PSX_FILE_DEVRANDOM))
    {
        PsxDevRead(Process, Message, File->FileType);
        return;
    }

    if (File->FileType == PSX_FILE_PIPE)
    {
        PsxPipeRead(Process, File, Message);
        return;
    }

    if ((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS))
    {
        PsxPtyRead(Process, File, Message);
        return;
    }

    if (File->FileType == PSX_FILE_XCONN)
    {
        PsxXConnRead(Process, File, Message);
        return;
    }

    if (File->FileType == PSX_FILE_XPOLL)
    {
        PsxXPollRead(Process, Message);
        return;
    }

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    Offset = File->Offset;
    Status = NtReadFile(File->NtHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        Bounce,
                        Count,
                        &Offset,
                        NULL);
    if (Status == STATUS_END_OF_FILE)
    {
        Transferred = 0;
    }
    else if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }
    else
    {
        Transferred = (ULONG)IoStatusBlock.Information;
    }

    if (Transferred != 0)
    {
        Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                      (PVOID)ClientBuffer,
                                      Bounce,
                                      Transferred,
                                      NULL);
        if (!NT_SUCCESS(Status))
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
        }

        File->Offset.QuadPart += Transferred;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Transferred;
}

/**
 * @brief write(fd, buf, n), API 0x2C.
 *
 * Disk writes copy the client bytes into a bounce buffer and then call NtWriteFile.
 */
VOID
PsxSrvWrite(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.ReadWrite.FileDescriptor);
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PVOID Bounce;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER Offset;
    NTSTATUS Status;
    ULONG Transferred = 0;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    /* Tty bytes flow between the client and posix.exe, see PsxSrvRead */
    if (File->FileType == PSX_FILE_TTY)
    {
        Message->Data.ReadWrite.HasData = 1;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    /* O_RDONLY */
    if ((File->OpenFlags & 7) == 0)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Message->Data.ReadWrite.HasData = 0;

    /* Writes to /dev/full always fail */
    if (File->FileType == PSX_FILE_DEVFULL)
    {
        Message->Errno = PSX_ENOSPC;
        Message->ReturnValue = -1;
        return;
    }

    /* Discard the data and report everything as written */
    if ((File->FileType == PSX_FILE_DEVNULL) ||
        (File->FileType == PSX_FILE_DEVZERO) ||
        (File->FileType == PSX_FILE_DEVRANDOM))
    {
        Message->Errno = 0;
        Message->ReturnValue = (LONG)Count;
        return;
    }

    if (File->FileType == PSX_FILE_PIPE)
    {
        PsxPipeWrite(Process, File, Message);
        return;
    }

    if ((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS))
    {
        PsxPtyWrite(Process, File, Message);
        return;
    }

    if (File->FileType == PSX_FILE_XCONN)
    {
        PsxXConnWrite(Process, File, Message);
        return;
    }

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Count, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    /* O_APPEND writes at end of file, otherwise at the tracked offset */
    if (File->OpenFlags & 8)
        Offset.QuadPart = -1;
    else
        Offset = File->Offset;

    Status = NtWriteFile(File->NtHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         Bounce,
                         Count,
                         &Offset,
                         NULL);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    Transferred = (ULONG)IoStatusBlock.Information;
    if ((File->OpenFlags & 8) == 0)
        File->Offset.QuadPart += Transferred;

    Message->Errno = 0;
    Message->ReturnValue = (LONG)Transferred;
}

/**
 * @brief dup2(oldfd, newfd), API 0x29. Makes newfd refer to the file object of oldfd.
 */
VOID
PsxSrvDup2(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT OldFd = (INT)Message->Data.ReadWrite.FileDescriptor;
    INT NewFd = (INT)Message->Data.ReadWrite.Buffer;
    PPSX_FILE_OBJECT File = PsxGetFile(Process, OldFd);

    if ((File == NULL) || (NewFd < 0) || (NewFd >= PSX_OPEN_MAX))
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    if (OldFd != NewFd)
    {
        if (Process->FdTable[NewFd] != NULL)
            PsxCloseFd(Process, NewFd);

        InterlockedIncrement(&File->RefCount);
        Process->FdTable[NewFd] = File;
    }

    Message->Errno = 0;
    Message->ReturnValue = NewFd;
}

/**
 * @brief isatty(fd), API 0x16.
 *
 * The result goes in the is-tty flag at body offset 0x34 and in ReturnValue.
 * Pty masters and slaves count as terminals so shells on a pty run interactively.
 */
VOID
PsxSrvIsatty(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    ULONG IsTty = 0;

    if ((File != NULL) &&
        ((File->FileType == PSX_FILE_TTY) ||
         (File->FileType == PSX_FILE_PTS) ||
         (File->FileType == PSX_FILE_PTMX)))
    {
        IsTty = 1;
    }

    ((PULONG)Message->Data.Raw)[1] = IsTty;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)IsTty;
}

/**
 * @brief fcntl(fd, cmd, arg), API 0x2D.
 *
 * Command values follow psx/fcntl.h. Descriptor and status flags are accepted but not
 * tracked, and record locks are always granted.
 */
VOID
PsxSrvFcntl(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    LONG Cmd = (LONG)((PULONG)Message->Data.Raw)[1];
    LONG Arg = (LONG)((PULONG)Message->Data.Raw)[2];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    INT NewFd;

    Message->Errno = 0;

    switch (Cmd)
    {
        case 0:
            /* F_DUPFD: duplicate to the lowest free descriptor >= Arg */
            if (File == NULL)
            {
                Message->Errno = PSX_EBADF;
                Message->ReturnValue = -1;
                return;
            }

            for (NewFd = (Arg > 0) ? Arg : 0; NewFd < PSX_OPEN_MAX; NewFd++)
            {
                if (Process->FdTable[NewFd] == NULL)
                {
                    InterlockedIncrement(&File->RefCount);
                    Process->FdTable[NewFd] = File;
                    Message->ReturnValue = NewFd;
                    return;
                }
            }

            Message->Errno = PSX_EMFILE;
            Message->ReturnValue = -1;
            return;

        case 1: /* F_GETFD */
        case 3: /* F_SETFD */
        case 5: /* F_SETFL */
            Message->ReturnValue = 0;
            return;

        case 4: /* F_GETFL */
            /* Report O_RDWR when the descriptor is not open */
            Message->ReturnValue = (File != NULL) ? (LONG)File->OpenFlags : 0x02;
            return;

        case 2: /* F_GETLK */
        case 6: /* F_SETLK */
        case 7: /* F_SETLKW */
            Message->ReturnValue = 0;
            return;

        case PSX_FCNTL_POLLRD:
            /* Readability poll carried over fcntl, see psxext.h */
            if (File == NULL)
            {
                Message->Errno = PSX_EBADF;
                Message->ReturnValue = -1;
                return;
            }

            Message->ReturnValue = PsxPollWait(File, (ULONG)Arg);
            return;

        default:
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
    }
}

/**
 * @brief readdir(fd), API 0x3C. Returns one directory entry per call.
 *
 * Body offset 0x34 holds the client dirent pointer and the byte at 0x3C is the rewind flag.
 * The ANSI name is written to the client and its length returned; 0 means end of directory.
 */
VOID
PsxSrvReaddir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    ULONG_PTR ClientDirent = ((PULONG)Message->Data.Raw)[1];
    BOOLEAN Restart = (Message->Data.Raw[0x0C] != 0);
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    struct
    {
        PSX_FILE_BOTH_DIR_INFORMATION Info;
        WCHAR NameTail[256];
    } DirInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING UniName;
    ANSI_STRING AnsiName;
    CHAR AnsiBuffer[512];
    UCHAR Trailer[sizeof(ULONG) + 1];
    ULONG Inode;
    ULONG Index;
    NTSTATUS Status;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtQueryDirectoryFile(File->NtHandle,
                                  NULL,
                                  NULL,
                                  NULL,
                                  &IoStatusBlock,
                                  &DirInfo,
                                  sizeof(DirInfo),
                                  FileBothDirectoryInformation,
                                  TRUE,
                                  NULL,
                                  Restart);
    if (Status == STATUS_NO_MORE_FILES)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    if (!NT_SUCCESS(Status))
    {
        Message->Errno = (Status == STATUS_INVALID_PARAMETER) ? PSX_ENOTDIR : PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    UniName.Buffer = DirInfo.Info.FileName;
    UniName.Length = (USHORT)DirInfo.Info.FileNameLength;
    UniName.MaximumLength = UniName.Length;
    AnsiName.Buffer = AnsiBuffer;
    AnsiName.MaximumLength = sizeof(AnsiBuffer);
    AnsiName.Length = 0;
    Status = RtlUnicodeStringToAnsiString(&AnsiName, &UniName, FALSE);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    /* d_name is at offset 0 of the client dirent; this write must succeed */
    Status = NtWriteVirtualMemory(Process->ProcessHandle,
                                  (PVOID)ClientDirent,
                                  AnsiName.Buffer,
                                  AnsiName.Length,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PSX_EIO;
        Message->ReturnValue = -1;
        return;
    }

    /* d_ino is an FNV-1a hash of the name and must never be 0, since glob and fts skip those */
    Inode = 2166136261u;
    for (Index = 0; Index < AnsiName.Length; Index++)
        Inode = (Inode ^ (UCHAR)AnsiName.Buffer[Index]) * 16777619u;

    if (Inode == 0)
        Inode = 1;

    *(PULONG)&Trailer[0] = Inode;

    /* d_type: DT_LNK, DT_DIR or DT_REG */
    if (DirInfo.Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        Trailer[sizeof(ULONG)] = 10;
    else if (DirInfo.Info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        Trailer[sizeof(ULONG)] = 4;
    else
        Trailer[sizeof(ULONG)] = 8;

    /* d_ino and d_type follow d_name at offsets 0x100 and 0x104; best effort */
    NtWriteVirtualMemory(Process->ProcessHandle,
                         (PVOID)(ClientDirent + 256),
                         Trailer,
                         sizeof(Trailer),
                         NULL);

    /* The client NUL-terminates d_name */
    Message->Errno = 0;
    Message->ReturnValue = (LONG)AnsiName.Length;
}

/**
 * @brief Bind descriptors 0 to FdCount-1 of a new image to one controlling tty object.
 *
 * Tty bytes flow between the client and posix.exe, so the object has no NT handle.
 */
VOID
PsxWireControllingTty(
    _Inout_ PPSX_PROCESS Process,
    _In_ ULONG FdCount)
{
    PPSX_FILE_OBJECT Tty;
    ULONG Fd;

    if (FdCount == 0)
        return;

    if (FdCount > PSX_OPEN_MAX)
        FdCount = PSX_OPEN_MAX;

    Tty = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Tty));
    if (Tty == NULL)
        return;

    Tty->FileType = PSX_FILE_TTY;

    /* O_RDWR */
    Tty->OpenFlags = 0x02;
    Tty->RefCount = (LONG)FdCount;

    for (Fd = 0; Fd < FdCount; Fd++)
        Process->FdTable[Fd] = Tty;
}

/**
 * @brief dup(fd), API 0x28. Duplicates a descriptor into the lowest free slot.
 */
VOID
PsxSrvDup(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    INT NewFd;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    NewFd = PsxAllocateFd(Process, File);
    if (NewFd < 0)
    {
        Message->Errno = PSX_EMFILE;
        Message->ReturnValue = -1;
        return;
    }

    InterlockedIncrement(&File->RefCount);
    Message->Errno = 0;
    Message->ReturnValue = NewFd;
}

/**
 * @brief ftruncate(fd, length), API 0x3D. Only disk files can be resized.
 */
VOID
PsxSrvFtruncate(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    LONG Length = (LONG)((PULONG)Message->Data.Raw)[1];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    FILE_END_OF_FILE_INFORMATION EofInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    if (File->FileType != PSX_FILE_DISK)
    {
        Message->Errno = PSX_ESPIPE;
        Message->ReturnValue = -1;
        return;
    }

    EofInfo.EndOfFile.QuadPart = (LONGLONG)Length;
    Status = NtSetInformationFile(File->NtHandle,
                                  &IoStatusBlock,
                                  &EofInfo,
                                  sizeof(EofInfo),
                                  FileEndOfFileInformation);
    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief Resolve a pathconf/fpathconf limit for an open handle.
 * @return The limit, or -1 with Errno set.
 *
 * LINK_MAX and NAME_MAX come from the volume; the rest are fixed values.
 */
LONG
PsxQueryPathconf(
    _In_ HANDLE Handle,
    _In_ ULONG Name,
    _Out_ PLONG Errno)
{
    UCHAR Buffer[128];
    PFILE_FS_ATTRIBUTE_INFORMATION FsAttr = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    BOOLEAN HasHardLinks;

    *Errno = 0;
    switch (Name)
    {
        case 1:
            /* _PC_LINK_MAX, hard links are only supported on NTFS and OFS */
            RtlZeroMemory(Buffer, sizeof(Buffer));
            Status = NtQueryVolumeInformationFile(Handle,
                                                  &IoStatusBlock,
                                                  FsAttr,
                                                  sizeof(Buffer),
                                                  FileFsAttributeInformation);
            if (NT_SUCCESS(Status))
            {
                HasHardLinks = (FsAttr->FileSystemNameLength >= 6 * sizeof(WCHAR)) &&
                               ((FsAttr->FileSystemName[0] == L'N') || (FsAttr->FileSystemName[0] == L'O'));
                return HasHardLinks ? 1024 : 1;
            }
            return 1;

        case 4:
            /* _PC_NAME_MAX, the maximum component length of the volume */
            RtlZeroMemory(Buffer, sizeof(Buffer));
            Status = NtQueryVolumeInformationFile(Handle,
                                                  &IoStatusBlock,
                                                  FsAttr,
                                                  sizeof(Buffer),
                                                  FileFsAttributeInformation);
            if (NT_SUCCESS(Status))
                return (LONG)FsAttr->MaximumComponentNameLength;
            return 255;

        case 5:
            /* _PC_PATH_MAX */
            return 512;

        case 6:
            /* _PC_PIPE_BUF */
            return 512;

        case 7:
            /* _PC_CHOWN_RESTRICTED */
            return 1;

        case 8:
            /* _PC_NO_TRUNC */
            return 1;

        default:
            *Errno = PSX_EINVAL;
            return -1;
    }
}

/**
 * @brief fpathconf(fd, name), API 0x26.
 */
VOID
PsxSrvFpathconf(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    ULONG Name = ((PULONG)Message->Data.Raw)[1];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    LONG Errno = 0;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    /* Volume limits need a disk handle; other objects get the defaults */
    if (((Name == 1) || (Name == 4)) && (File->FileType != PSX_FILE_DISK))
    {
        Message->Errno = 0;
        Message->ReturnValue = (Name == 4) ? 255 : 1;
        return;
    }

    Message->ReturnValue = PsxQueryPathconf(File->NtHandle, Name, &Errno);
    Message->Errno = (ULONG)Errno;
}

/**
 * @brief tcgetattr/tcsetattr, API 0x2F and 0x30.
 *
 * The termios state lives in posix.exe, so a tty descriptor only sets HasData (body offset 0x44)
 * and the client exchanges the termios block with posix.exe.
 */
VOID
PsxSrvTtyQuery(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    if (File->FileType == PSX_FILE_TTY)
    {
        /* HasData */
        ((PULONG)Message->Data.Raw)[5] = 1;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    ((PULONG)Message->Data.Raw)[5] = 0;
    Message->Errno = PSX_ENOTTY;
    Message->ReturnValue = -1;
}

/**
 * @brief Remaining terminal control APIs (0x15 and 0x31 to 0x36).
 *
 * A valid descriptor yields ENOTTY and a bad one EBADF.
 */
VOID
PsxSrvTtyStub(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];

    if (PsxGetFile(Process, Fd) == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Message->Errno = PSX_ENOTTY;
    Message->ReturnValue = -1;
}
