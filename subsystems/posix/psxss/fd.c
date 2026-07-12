/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The per-process POSIX descriptor table + the file-related syscall
 *              handlers (open/close/lseek/umask/dup2). Disk files are serviced
 *              directly via the NT I/O manager; pipe/tty objects route elsewhere.
 *              Models the NT 4.0 server's fd table + file-object vtable
 *              (psxss sub_1F42364 lookup, sub_1F4230E release, the 0x18/0x2A/0x2E
 *              handlers).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/iofuncs.h>

//
// FILE_NAMES_INFORMATION is declared kernel-mode-only in the NDK; the readdir
// handler needs it in user mode, so mirror its layout here.
//
typedef struct _PSX_FILE_NAMES_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
} PSX_FILE_NAMES_INFORMATION;

//
// FileBothDirectoryInformation layout -- gives us FileAttributes (for d_type)
// alongside the name, in one single-entry NtQueryDirectoryFile call.
//
typedef struct _PSX_FILE_BOTH_DIR_INFORMATION
{
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    CCHAR         ShortNameLength;
    WCHAR         ShortName[12];
    WCHAR         FileName[1];
} PSX_FILE_BOTH_DIR_INFORMATION;

//
// POSIX errno values that cross the LPC wire (subset; see psx/errno.h).
//
#define PSX_ENOENT   2
#define PSX_EBADF    9
#define PSX_ENOMEM   12
#define PSX_EACCES   13
#define PSX_EEXIST   17
#define PSX_ENOTDIR  20
#define PSX_EISDIR   21
#define PSX_EINVAL   22
#define PSX_EMFILE   24
#define PSX_ENOSPC   28
#define PSX_ESPIPE   29

// POSIX whence (see unistd.h).
#define PSX_SEEK_SET 0
#define PSX_SEEK_CUR 1
#define PSX_SEEK_END 2

//
// Map an NTSTATUS to a POSIX errno. Mirrors the client-side table
// (psxdll sub_753911C4) and the server's sub_1F45F20.
//
ULONG
PsxErrnoFromStatus(IN NTSTATUS Status)
{
    switch (Status)
    {
        case STATUS_SUCCESS:                 return 0;
        case STATUS_NO_MEMORY:               return PSX_ENOMEM;
        case STATUS_INVALID_PARAMETER:       return PSX_EINVAL;
        case STATUS_OBJECT_NAME_NOT_FOUND:   return PSX_ENOENT;
        case STATUS_OBJECT_PATH_NOT_FOUND:   return PSX_ENOENT;
        case STATUS_OBJECT_NAME_INVALID:     return PSX_ENOENT;
        case STATUS_OBJECT_NAME_COLLISION:   return PSX_EEXIST;
        case STATUS_NOT_A_DIRECTORY:         return PSX_ENOTDIR;
        case STATUS_FILE_IS_A_DIRECTORY:     return PSX_EISDIR;
        case STATUS_DISK_FULL:               return PSX_ENOSPC;
        default:                             return PSX_EACCES;
    }
}

//
// Install a file object into the lowest free descriptor (POSIX semantics).
// Returns the fd, or -1 if the table is full.
//
INT
PsxAllocateFd(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File)
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
PsxGetFile(IN PPSX_PROCESS Process, IN INT FileDescriptor)
{
    if ((FileDescriptor < 0) || (FileDescriptor >= PSX_OPEN_MAX))
        return NULL;
    return Process->FdTable[FileDescriptor];
}

//
// Drop a reference to a file object, releasing it (and its NT handle) at zero.
//
static VOID
PsxDereferenceFile(IN PPSX_FILE_OBJECT File)
{
    if (InterlockedDecrement(&File->RefCount) != 0)
        return;

    if ((File->FileType == PSX_FILE_PIPE) && (File->Pipe != NULL))
        PsxPipeCloseEnd(File);          // drop the pipe end, free the pipe if last
    else if (((File->FileType == PSX_FILE_PTMX) || (File->FileType == PSX_FILE_PTS)) && (File->Pty != NULL))
        PsxPtyClose(File);              // drop the pty end, free the pty if last
    else if (File->FileType == PSX_FILE_XCONN)
        PsxXConnClose(File);            // close the X server pipe -> server drops client
    else if (File->NtHandle != NULL)
        NtClose(File->NtHandle);

    RtlFreeHeap(RtlGetProcessHeap(), 0, File);
}

INT
PsxCloseFd(IN PPSX_PROCESS Process, IN INT FileDescriptor)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, FileDescriptor);

    if (File == NULL)
        return -PSX_EBADF;

    Process->FdTable[FileDescriptor] = NULL;
    PsxDereferenceFile(File);
    return 0;
}

VOID
PsxCloseAllFds(IN PPSX_PROCESS Process)
{
    INT Fd;

    for (Fd = 0; Fd < PSX_OPEN_MAX; Fd++)
    {
        if (Process->FdTable[Fd] != NULL)
            PsxCloseFd(Process, Fd);
    }
}

//
// Translate POSIX open() flags into an NtCreateFile call and open the file.
//
static NTSTATUS
PsxOpenNtFile(IN PPSX_PROCESS Process, IN PPSX_API_MESSAGE Message,
             IN PUNICODE_STRING NtPath, IN ULONG OpenFlags, OUT PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    ACCESS_MASK DesiredAccess;
    ULONG Disposition;
    NTSTATUS Status;

    switch (OpenFlags & 7 /* O_ACCMODE */)
    {
        case 1:  DesiredAccess = FILE_GENERIC_WRITE; break;                       // O_WRONLY
        case 2:  DesiredAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE; break;   // O_RDWR
        default: DesiredAccess = FILE_GENERIC_READ; break;                        // O_RDONLY
    }
    DesiredAccess |= SYNCHRONIZE;
    if (OpenFlags & 8 /* O_APPEND */)
        DesiredAccess |= FILE_APPEND_DATA;

    if (OpenFlags & 0x100 /* O_CREAT */)
    {
        if (OpenFlags & 0x400 /* O_EXCL */)       Disposition = FILE_CREATE;
        else if (OpenFlags & 0x200 /* O_TRUNC */) Disposition = FILE_OVERWRITE_IF;
        else                                      Disposition = FILE_OPEN_IF;
    }
    else
    {
        if (OpenFlags & 0x200 /* O_TRUNC */)      Disposition = FILE_OVERWRITE;
        else                                      Disposition = FILE_OPEN;
    }

    InitializeObjectAttributes(&ObjectAttributes, NtPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    // Do not force FILE_NON_DIRECTORY_FILE: opendir() opens the directory with a
    // plain open() (O_RDONLY) and then enumerates it via readdir (op 0x3C), so a
    // directory must be openable here. FILE_GENERIC_READ carries FILE_LIST_DIRECTORY.
    //
    // Impersonate the client so the create/open runs under ITS token: the file is
    // owned by the POSIX user and access checks use its rights, matching mkdir and
    // the path.c ops. Creating as bare psxss (no impersonation) was both wrong
    // (ownership) and a source of ACCESS_DENIED on user-owned directories -- the
    // EACCES bash saw writing here-document temp files.
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

//
// Recognise the synthetic /dev nodes (Phase 2 extension). The client translates a
// POSIX path against the root drive, so open("/dev/null") arrives as an NT path
// like "\DosDevices\X:\dev\null". Match the "\dev\<name>" tail case-insensitively
// (treating '/' == '\'); returns a PSX_FILE_* type, or 0xFFFFFFFF if not a device.
//
static ULONG
PsxClassifyDeviceNode(IN PUNICODE_STRING Path)
{
    static const struct { PCWSTR Name; ULONG Type; } Table[] = {
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
    ULONG Chars = Path->Length / sizeof(WCHAR);
    ULONG i, n, Start;

    if ((Path->Buffer == NULL) || (Chars < 5 /* \dev\ */)) return 0xFFFFFFFF;

    for (i = 0; i < RTL_NUMBER_OF(Table); i++)
    {
        n = (ULONG)wcslen(Table[i].Name);
        if (Chars < n + 5) continue;                 // need "\dev\" + name
        Start = Chars - n;

        // The name must be the final path component preceded by "\dev\".
        {
            WCHAR d0 = Path->Buffer[Start - 5], d1 = Path->Buffer[Start - 4],
                  d2 = Path->Buffer[Start - 3], d3 = Path->Buffer[Start - 2],
                  d4 = Path->Buffer[Start - 1];
            BOOLEAN Sep0 = (d0 == L'\\') || (d0 == L'/');
            BOOLEAN Sep4 = (d4 == L'\\') || (d4 == L'/');
            ULONG k;
            BOOLEAN NameOk = TRUE;

            if (!Sep0 || (RtlUpcaseUnicodeChar(d1) != L'D') ||
                (RtlUpcaseUnicodeChar(d2) != L'E') || (RtlUpcaseUnicodeChar(d3) != L'V') || !Sep4)
                continue;

            for (k = 0; k < n; k++)
            {
                if (RtlUpcaseUnicodeChar(Path->Buffer[Start + k]) !=
                    RtlUpcaseUnicodeChar(Table[i].Name[k]))
                {
                    NameOk = FALSE;
                    break;
                }
            }
            if (NameOk) return Table[i].Type;
        }
    }
    return 0xFFFFFFFF;
}

//
// Recognise "\dev\pts\<N>" (the pty slave nodes) and extract the index N. The
// number is a variable trailing component, so this is separate from the fixed
// device table above. Returns TRUE + *Index on a match.
//
static BOOLEAN
PsxClassifyPts(IN PUNICODE_STRING Path, OUT PULONG Index)
{
    ULONG Chars = Path->Length / sizeof(WCHAR);
    PWCHAR B = Path->Buffer;
    LONG i, dstart;
    ULONG val = 0;
#define PSX_SEP(c) (((c) == L'\\') || ((c) == L'/'))

    if ((B == NULL) || (Chars < 9)) return FALSE;       // "\dev\pts\N" is 9 chars min
    i = (LONG)Chars - 1;
    while ((i >= 0) && (B[i] >= L'0') && (B[i] <= L'9')) i--;
    dstart = i + 1;
    if (dstart > (LONG)Chars - 1) return FALSE;          // no trailing digits
    if (i < 8) return FALSE;                             // need "\dev\pts\" before them
    if (!PSX_SEP(B[i])) return FALSE;
    if ((RtlUpcaseUnicodeChar(B[i-3]) != L'P') || (RtlUpcaseUnicodeChar(B[i-2]) != L'T') ||
        (RtlUpcaseUnicodeChar(B[i-1]) != L'S') || !PSX_SEP(B[i-4]))
        return FALSE;
    if ((RtlUpcaseUnicodeChar(B[i-7]) != L'D') || (RtlUpcaseUnicodeChar(B[i-6]) != L'E') ||
        (RtlUpcaseUnicodeChar(B[i-5]) != L'V') || !PSX_SEP(B[i-8]))
        return FALSE;
    for (i = dstart; i < (LONG)Chars; i++)
        val = val * 10 + (ULONG)(B[i] - L'0');
    *Index = val;
    return TRUE;
#undef PSX_SEP
}

// RtlRandomEx (ntdll) isn't declared by the NDK headers this build pulls in.
ULONG NTAPI RtlRandomEx(IN OUT PULONG Seed);

//
// Fill a buffer with pseudo-random bytes for /dev/random and /dev/urandom. Uses
// ntdll's RtlRandomEx stream seeded from the tick count -- functional, not
// cryptographic (a real CSPRNG would be the follow-up).
//
static VOID
PsxFillRandom(OUT PUCHAR Buffer, IN ULONG Count)
{
    static ULONG Seed = 0;
    ULONG i = 0;

    if (Seed == 0)
        Seed = GetTickCount() ^ 0x9E3779B9u;

    while (i < Count)
    {
        ULONG r = RtlRandomEx(&Seed);
        INT b;
        for (b = 0; (b < 4) && (i < Count); b++, i++)
            Buffer[i] = (UCHAR)(r >> (8 * b));
    }
}

//
// Serve a read from /dev/zero, /dev/full (zeros) or /dev/random, /dev/urandom
// (random bytes): fill the caller's buffer and report the full count.
//
static VOID
PsxDevRead(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message, IN ULONG DevType)
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
        RtlZeroMemory(Bounce, Count);       // /dev/zero, /dev/full

    Status = NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                  Bounce, Count, NULL);
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

//
// Create a fd bound to a synthetic device object (no NT handle). Returns the fd
// or a negative errno.
//
static INT
PsxOpenDeviceFd(IN PPSX_PROCESS Process, IN ULONG DevType, IN ULONG OpenFlags)
{
    PPSX_FILE_OBJECT File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY,
                                            sizeof(PSX_FILE_OBJECT));
    INT Fd;

    if (File == NULL) return -PSX_ENOMEM;
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

//
// open(path, oflag, mode) -- ApiNumber 0x18. The path is a UNICODE_STRING at the
// message body; its Buffer is a pointer into the client's shared section (already
// translated to our address space), so we bounds-check it before use.
//
VOID
PsxSrvOpen(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = Message->Data.Open.Path;
    ULONG OpenFlags = Message->Data.Open.OpenFlag;
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

    // Synthetic /dev nodes (Phase 2 extension): served entirely in psxss, no NT
    // file. /dev/tty maps to a controlling-terminal object (two-phase like fds 0-2).
    {
        ULONG DevType = PsxClassifyDeviceNode(&Path);
        ULONG PtsIndex = 0;
        BOOLEAN IsDevice = TRUE;

        if (DevType == PSX_FILE_PTMX)
        {
            // /dev/ptmx: allocate a fresh pty pair, return its master.
            Fd = PsxPtyOpenMaster(Process, OpenFlags);
        }
        else if (PsxClassifyPts(&Path, &PtsIndex))
        {
            // /dev/pts/N: the slave the child shell runs on.
            Fd = PsxPtyOpenSlave(Process, PtsIndex, OpenFlags);
        }
        else if (DevType != 0xFFFFFFFF)
        {
            // /dev/x11 is a live connection to the companion X server, not a plain
            // no-handle device: dial the named pipe instead of allocating a stub fd.
            Fd = (DevType == PSX_FILE_XCONN)
                     ? PsxOpenXConnFd(Process, OpenFlags)
                     : PsxOpenDeviceFd(Process, DevType, OpenFlags);
        }
        else
        {
            IsDevice = FALSE;
        }

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
    }

    Status = PsxOpenNtFile(Process, Message, &Path, OpenFlags, &Handle);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    File = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
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

//
// close(fd) -- ApiNumber 0x2A.
//
VOID
PsxSrvClose(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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

//
// lseek(fd, offset, whence) -- ApiNumber 0x2E. The new position is returned in
// the same body slot the client passed the offset in.
//
VOID
PsxSrvLseek(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.Lseek.FileDescriptor);
    ULONG Whence = Message->Data.Lseek.Whence;
    LONG Offset = Message->Data.Lseek.Offset;
    LONGLONG Base;
    LONGLONG NewPosition;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }
    if (File->FileType != PSX_FILE_DISK)
    {
        Message->Errno = PSX_ESPIPE;        // pipes/ttys are not seekable
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
        {
            FILE_STANDARD_INFORMATION StandardInfo;
            IO_STATUS_BLOCK IoStatusBlock;
            if (!NT_SUCCESS(NtQueryInformationFile(File->NtHandle, &IoStatusBlock,
                                                   &StandardInfo, sizeof(StandardInfo),
                                                   FileStandardInformation)))
            {
                Message->Errno = PSX_EINVAL;
                Message->ReturnValue = -1;
                return;
            }
            Base = StandardInfo.EndOfFile.QuadPart;
            break;
        }
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
    Message->Data.Lseek.Offset = (LONG)NewPosition;     // reply slot
    Message->Errno = 0;
    Message->ReturnValue = (LONG)NewPosition;
}

//
// umask(mask) -- ApiNumber 0x19. Swap the process file-creation mask, returning
// the previous value.
//
VOID
PsxSrvUmask(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    ULONG Previous = Process->Umask;

    Process->Umask = ((PULONG)Message->Data.Raw)[0] & 0x1FF;     // low 9 mode bits (body +0x30)
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Previous;
}

//
// The 40-byte POSIX struct stat, laid out 1:1 with psx/sys/stat.h. The client
// allocates it in the shared section and sends us a (server-relative) pointer;
// we fill it in place.
//
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

// POSIX st_mode bits we use (octal in the header; hex here).
#define PSX_S_IFDIR   0x4000
#define PSX_S_IFREG   0x8000

static LONG
PsxFileTimeToUnix(IN PLARGE_INTEGER Time)
{
    ULONG Seconds = 0;
    if (Time->QuadPart != 0)
        RtlTimeToSecondsSince1970(Time, &Seconds);
    return (LONG)Seconds;
}

//
// Fill a struct stat from an open NT file handle (NT file info -> POSIX fields).
//
static NTSTATUS
PsxFillStat(IN HANDLE Handle, IN PPSX_PROCESS Process, OUT PPSX_STAT Stat)
{
    FILE_BASIC_INFORMATION BasicInfo;
    FILE_STANDARD_INFORMATION StandardInfo;
    FILE_INTERNAL_INFORMATION InternalInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    ULONG Mode;

    Status = NtQueryInformationFile(Handle, &IoStatusBlock, &BasicInfo,
                                    sizeof(BasicInfo), FileBasicInformation);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&StandardInfo, sizeof(StandardInfo));
    RtlZeroMemory(&InternalInfo, sizeof(InternalInfo));
    NtQueryInformationFile(Handle, &IoStatusBlock, &StandardInfo,
                           sizeof(StandardInfo), FileStandardInformation);
    NtQueryInformationFile(Handle, &IoStatusBlock, &InternalInfo,
                           sizeof(InternalInfo), FileInternalInformation);

    if (BasicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        Mode = PSX_S_IFDIR | ((BasicInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0755);
    else
        /* Regular files report x: NT has no execute bit short of ACLs (which
         * we don't map -- see the Uid TODO below), and a mode without x makes
         * every shell's PATH search reject every binary (bash executable_file
         * refused the whole read-only CD /bin). Interix did the same when ACL
         * data was absent. */
        Mode = PSX_S_IFREG | ((BasicInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0777);

    Stat->st_mode  = Mode;
    Stat->st_ino   = InternalInfo.IndexNumber.LowPart;
    Stat->st_dev   = 0;
    Stat->st_nlink = StandardInfo.NumberOfLinks ? StandardInfo.NumberOfLinks : 1;
    Stat->st_uid   = Process->Uid;          // TODO: map the file owner SID
    Stat->st_gid   = Process->Gid;
    Stat->st_size  = StandardInfo.EndOfFile.LowPart;
    Stat->st_atime = PsxFileTimeToUnix(&BasicInfo.LastAccessTime);
    Stat->st_mtime = PsxFileTimeToUnix(&BasicInfo.LastWriteTime);
    Stat->st_ctime = PsxFileTimeToUnix(&BasicInfo.ChangeTime);
    return STATUS_SUCCESS;
}

//
// stat(path, buf) -- ApiNumber 0x1F. Both the path (Data.Stat.Path.Buffer) and
// the result buffer (Data.Stat.StatBuffer) are server-relative pointers into the
// client's shared section; we bounds-check both, then fill the stat in place.
//
VOID
PsxSrvStat(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path = Message->Data.Stat.Path;
    ULONG_PTR StatPtr = Message->Data.Stat.StatBuffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxValidateClientPointer(Process, (ULONG_PTR)Path.Buffer, Path.Length) ||
        !PsxValidateClientPointer(Process, StatPtr, sizeof(PSX_STAT)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    // Synthetic /dev nodes: report a character device without touching the fs.
    {
        ULONG DevType = PsxClassifyDeviceNode(&Path);
        if (DevType != 0xFFFFFFFF)
        {
            PPSX_STAT Stat = (PPSX_STAT)StatPtr;
            RtlZeroMemory(Stat, sizeof(*Stat));
            Stat->st_mode  = (DevType == PSX_FILE_TTY) ? (0x2000 | 0700) : (0x2000 | 0666);
            Stat->st_nlink = 1;
            Stat->st_uid   = Process->Uid;
            Stat->st_gid   = Process->Gid;
            Message->Errno = 0;
            Message->ReturnValue = 0;
            return;
        }
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &IoStatusBlock, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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

//
// access(path, mode) -- ApiNumber 0x21. Try to open the path with the access the
// mode bits ask for (R_OK=4/W_OK=2/X_OK=1, F_OK=0 just tests existence). The
// path is a server-relative pointer in the shared section. (Reuses the open
// request layout: Path@0x30, mode@0x38.)
//
VOID
PsxSrvAccess(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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

    if (ModeMask == 0)
        DesiredAccess |= FILE_READ_ATTRIBUTES;      // F_OK: existence only
    if (ModeMask & 4)
        DesiredAccess |= FILE_READ_DATA;            // R_OK
    if (ModeMask & 2)
        DesiredAccess |= FILE_WRITE_DATA;           // W_OK
    if (ModeMask & 1)
        DesiredAccess |= FILE_EXECUTE;              // X_OK

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, DesiredAccess, &ObjectAttributes, &IoStatusBlock,
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

//
// fstat(fd, buf) -- ApiNumber 0x20.
//
VOID
PsxSrvFstat(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_FILE_OBJECT File = PsxGetFile(Process, (INT)Message->Data.Fstat.FileDescriptor);
    ULONG_PTR StatPtr = Message->Data.Fstat.StatBuffer;
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

    // Objects with no NT handle: report the right POSIX file type so isatty()/
    // ttyname()/S_ISCHR checks work. tty + /dev char nodes are S_IFCHR (the tty
    // keeps the real 0x21C0); pipe ends are S_IFIFO.
    if (File->FileType != PSX_FILE_DISK)
    {
        PPSX_STAT Stat = (PPSX_STAT)StatPtr;
        RtlZeroMemory(Stat, sizeof(*Stat));
        if (File->FileType == PSX_FILE_PIPE)
            Stat->st_mode = 0x1000 | 0600;          // S_IFIFO
        else if (File->FileType == PSX_FILE_TTY)
            Stat->st_mode = 0x2000 | 0700;          // S_IFCHR | rwx (0x21C0)
        else
            Stat->st_mode = 0x2000 | 0666;          // S_IFCHR (/dev/null, zero, random, full)
        Stat->st_nlink = 1;
        Stat->st_uid   = Process->Uid;
        Stat->st_gid   = Process->Gid;
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

//
// read(fd, buf, n) -- ApiNumber 0x2B. For a tty descriptor psxss only flags the
// data phase (HasData) and the bytes move client<->posix.exe over the session
// port. Pipe reads are served by PsxPipeRead. For a disk file the client's raw
// buffer lives in ITS address space; psxss reads the file into a bounce buffer
// and pushes the bytes into the client via NtWriteVirtualMemory. (Re-derived from
// the server's file read op sub_1F42874: NtReadFile -> NtWriteVirtualMemory.)
//
VOID
PsxSrvRead(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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
    // Controlling terminal: psxss does not move tty bytes. Flag the data phase so
    // the client exchanges the bytes directly with posix.exe over the session
    // port (posix.exe owns the console). HasData is body +0x44.
    if (File->FileType == PSX_FILE_TTY)
    {
        Message->Data.ReadWrite.HasData = 1;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    if ((File->OpenFlags & 7) == 1 /* O_WRONLY */)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }
    Message->Data.ReadWrite.HasData = 0;    // fully served here; no posix.exe phase
    if (File->FileType == PSX_FILE_DEVNULL)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;           // always EOF
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
        // read(pollfd, buf, count) = wait for this process's /dev/x11 connection
        // to become readable -- the select() primitive real Xlib/Xt needs,
        // carried over plain read() because the unmodified MS psxdll rejects
        // unknown opcodes and private fcntl cmds client-side. count encodes the
        // timeout: 1 = non-blocking probe, N = wait N-1 ms. One byte lands in
        // the client buffer: '1' readable, '0' timed out; read returns 1.
        PPSX_FILE_OBJECT Xconn = NULL;
        UCHAR Result = '0';
        INT i;

        for (i = 0; i < PSX_OPEN_MAX; i++)
        {
            if ((Process->FdTable[i] != NULL) &&
                (Process->FdTable[i]->FileType == PSX_FILE_XCONN))
            {
                Xconn = Process->FdTable[i];
                break;
            }
        }
        if ((Xconn != NULL) && (Count != 0) &&
            (PsxPollWait(Xconn, Count - 1) == 1))
        {
            Result = '1';
        }
        NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                             &Result, 1, NULL);
        Message->Errno = 0;
        Message->ReturnValue = 1;
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
    Status = NtReadFile(File->NtHandle, NULL, NULL, NULL, &IoStatusBlock,
                        Bounce, Count, &Offset, NULL);
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
        Status = NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                      Bounce, Transferred, NULL);
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

//
// write(fd, buf, n) -- ApiNumber 0x2C (disk files). Mirror of read: pull the
// client's bytes via NtReadVirtualMemory, then NtWriteFile. (Re-derived from the
// server's file write op sub_1F426EA: NtReadVirtualMemory(client) -> NtWriteFile.)
//
VOID
PsxSrvWrite(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
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
    // Controlling terminal: flag the data phase; the bytes flow client<->posix.exe
    // (see PsxSrvRead). HasData is body +0x44.
    if (File->FileType == PSX_FILE_TTY)
    {
        Message->Data.ReadWrite.HasData = 1;
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    if ((File->OpenFlags & 7) == 0 /* O_RDONLY */)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }
    Message->Data.ReadWrite.HasData = 0;    // fully served here; no posix.exe phase
    if (File->FileType == PSX_FILE_DEVFULL)
    {
        Message->Errno = PSX_ENOSPC;        // /dev/full: writes always fail
        Message->ReturnValue = -1;
        return;
    }
    if ((File->FileType == PSX_FILE_DEVNULL) ||
        (File->FileType == PSX_FILE_DEVZERO) ||
        (File->FileType == PSX_FILE_DEVRANDOM))
    {
        Message->Errno = 0;
        Message->ReturnValue = (LONG)Count; // discard, report all written
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

    Status = NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                 Bounce, Count, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    // O_APPEND -> write at end of file; else at our tracked offset.
    if (File->OpenFlags & 8 /* O_APPEND */)
        Offset.QuadPart = -1;           // FILE_WRITE_TO_END_OF_FILE
    else
        Offset = File->Offset;

    Status = NtWriteFile(File->NtHandle, NULL, NULL, NULL, &IoStatusBlock,
                         Bounce, Count, &Offset, NULL);
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

//
// dup2(oldfd, newfd) -- ApiNumber 0x29. Make newfd refer to oldfd's file object.
//
VOID
PsxSrvDup2(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT OldFd = (INT)Message->Data.ReadWrite.FileDescriptor;     // body +0x30
    INT NewFd = (INT)Message->Data.ReadWrite.Buffer;             // body +0x34
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

//
// isatty(fd) -- ApiNumber 0x16 (op 22). Reports whether a descriptor refers to a
// terminal. The controlling-tty descriptors (0/1/2) are not yet wired to
// posix.exe's session, so for now every fd reports "not a terminal" (0), which
// keeps line-oriented clients (ls, sh) on their simple non-tty output path.
// errno stays 0: the descriptor is valid, it just is not a tty. TODO: once the
// session tty fds exist, return 1 for PSX_FILE_TTY and drive the data-channel
// sub-op 6/7 against posix.exe.
//
VOID
PsxSrvIsatty(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];       // body +0x30
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    // A pseudo-terminal master/slave is a terminal too -- without this, a shell
    // on a pty gets isatty()==0, decides it is non-interactive, and prints no
    // prompt (it reads its own stdin as a script). That was dtterm's dead shell.
    ULONG IsTty = ((File != NULL) &&
                   ((File->FileType == PSX_FILE_TTY) ||
                    (File->FileType == PSX_FILE_PTS) ||
                    (File->FileType == PSX_FILE_PTMX))) ? 1 : 0;

    // Phase 1: report whether the fd is a terminal in the is-tty flag at body
    // +0x34. If set, the client confirms with posix.exe (session data sub-op 6);
    // if clear, isatty() returns 0. psxss owns the descriptor table; posix.exe
    // owns the console.
    ((PULONG)Message->Data.Raw)[1] = IsTty;
    Message->Errno = 0;
    Message->ReturnValue = (LONG)IsTty;
}

//
// fcntl(fd, cmd, arg) -- ApiNumber 0x2d (op 45). fd at body +0x30, cmd at +0x40,
// arg at +0x44 (NT-ABI F_* values from psx/fcntl.h: DUPFD=0 GETFD=1 GETLK=2
// SETFD=3 GETFL=4 SETFL=5 SETLK=6 SETLKW=7). Minimal but real: F_DUPFD dups the
// descriptor, the descriptor/status-flag gets and sets are acknowledged, and
// record locks are treated as always-grantable no-ops. The not-yet-wired std fds
// (0/1/2) are tolerated so early client setup proceeds. TODO: track FD_CLOEXEC
// and real O_NONBLOCK once the tty fds exist.
//
VOID
PsxSrvFcntl(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT  Fd  = (INT)((PULONG)Message->Data.Raw)[0];     // msg +0x30 (v14)
    LONG Cmd = (LONG)((PULONG)Message->Data.Raw)[1];    // msg +0x34 (v15)
    LONG Arg = (LONG)((PULONG)Message->Data.Raw)[2];    // msg +0x38 (v16)
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);


    Message->Errno = 0;

    switch (Cmd)
    {
        case 0:     // F_DUPFD: duplicate to lowest free fd >= Arg
        {
            INT NewFd;
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
        }

        case 1:     // F_GETFD: descriptor flags (FD_CLOEXEC) -- not tracked yet
        case 3:     // F_SETFD
        case 5:     // F_SETFL
            Message->ReturnValue = 0;
            return;

        case 4:     // F_GETFL: file status flags
            Message->ReturnValue = (File != NULL) ? (LONG)File->OpenFlags : 0x02 /* O_RDWR */;
            return;

        case 2:     // F_GETLK
        case 6:     // F_SETLK
        case 7:     // F_SETLKW
            // No record locking yet: report the lock as available / granted.
            Message->ReturnValue = 0;
            return;

        case PSX_FCNTL_POLLRD:  // psxext.h: readability poll tunneled via fcntl
        {                       // (the real MS psxdll marshals unknown cmds through)
            if (File == NULL)
            {
                Message->Errno = PSX_EBADF;
                Message->ReturnValue = -1;
                return;
            }
            Message->ReturnValue = PsxPollWait(File, (ULONG)Arg);
            return;
        }

        default:
            Message->Errno = PSX_EINVAL;
            Message->ReturnValue = -1;
            return;
    }
}

//
// readdir(fd) -- ApiNumber 0x3C. One directory entry per call. The client passes
// its DIR name-buffer pointer (body +0x34) and a rewind flag (body +0x3C, one
// byte); psxss does a single-entry NtQueryDirectoryFile on the fd's handle and
// pokes the ANSI filename straight into the client buffer via NtWriteVirtualMemory,
// returning the name length in ReturnValue (0 == end of directory). The client
// filters "." / ".." itself. Faithful to sub_1F42874.
//
VOID
PsxSrvReaddir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];               // +0x30
    ULONG_PTR ClientDirent = ((PULONG)Message->Data.Raw)[1];    // +0x34 &struct dirent
    BOOLEAN Restart = (Message->Data.Raw[0x0C] != 0);           // +0x3C byte (rewind)
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
    NTSTATUS Status;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtQueryDirectoryFile(File->NtHandle, NULL, NULL, NULL, &IoStatusBlock,
                                  &DirInfo, sizeof(DirInfo), FileBothDirectoryInformation,
                                  TRUE /* ReturnSingleEntry */, NULL, Restart);
    if (Status == STATUS_NO_MORE_FILES)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;               // end of directory
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

    /* Fill the client's struct dirent. d_name is at offset 0 (kept there for ABI
     * compatibility with the original name-only struct); d_ino/d_type follow at
     * offset (NAME_MAX+1) = 0x100 / 0x104. d_ino must be non-zero (glob/fts treat
     * 0 as a stale entry) -- a stable per-name FNV-1a hash. d_type from the file
     * attributes. The name write must succeed; the trailer is best-effort. */
    Status = NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientDirent,
                                  AnsiName.Buffer, AnsiName.Length, NULL);
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = 5;                     // EIO
        Message->ReturnValue = -1;
        return;
    }
    {
        ULONG i, ino = 2166136261u;             // FNV-1a offset basis
        UCHAR Trailer[sizeof(ULONG) + 1];       // [d_ino:4][d_type:1]

        for (i = 0; i < AnsiName.Length; i++)
            ino = (ino ^ (UCHAR)AnsiName.Buffer[i]) * 16777619u;
        if (ino == 0) ino = 1;                  // never 0
        *(PULONG)&Trailer[0] = ino;             // d_ino

        if (DirInfo.Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            Trailer[sizeof(ULONG)] = 10;        // DT_LNK
        else if (DirInfo.Info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            Trailer[sizeof(ULONG)] = 4;         // DT_DIR
        else
            Trailer[sizeof(ULONG)] = 8;         // DT_REG

        // d_ino at d_name + (NAME_MAX+1) = +0x100, d_type at +0x104
        NtWriteVirtualMemory(Process->ProcessHandle,
                             (PVOID)(ClientDirent + 256),
                             Trailer, sizeof(Trailer), NULL);
    }

    Message->Errno = 0;
    Message->ReturnValue = (LONG)AnsiName.Length;   // name length (client NUL-terminates d_name)
}

//
// Wire the controlling-terminal descriptors for a freshly spawned image. The
// spawn request carries InheritedFdCount (usually 3); NT 4.0 binds descriptors
// 0..count-1 to one controlling-tty object (the sub_1F418AF fd loop). Tty bytes
// flow client<->posix.exe, so the object needs no NT handle -- only its type, so
// read/write/isatty route to the session data phase.
//
VOID
PsxWireControllingTty(IN PPSX_PROCESS Process, IN ULONG FdCount)
{
    PPSX_FILE_OBJECT Tty;
    ULONG i;

    if (FdCount == 0)
        return;
    if (FdCount > PSX_OPEN_MAX)
        FdCount = PSX_OPEN_MAX;

    Tty = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
    if (Tty == NULL)
        return;
    Tty->FileType = PSX_FILE_TTY;
    Tty->OpenFlags = 0x02;              // O_RDWR
    Tty->RefCount = (LONG)FdCount;

    for (i = 0; i < FdCount; i++)
        Process->FdTable[i] = Tty;
}

//
// dup(fd) -- ApiNumber 0x28. Duplicate a descriptor into the lowest free slot.
// (The client's dup() also routes here; dup2 is the sibling at 0x29.) Faithful
// to sub_1F48AFD.
//
VOID
PsxSrvDup(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];       // +0x30
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

//
// ftruncate(fd, length) -- ApiNumber 0x3D. length at Data[1]. A pipe/fifo yields
// ESPIPE; a disk file is resized via FileEndOfFileInformation. Returns 0/-1.
// Faithful to sub_1F4A252.
//
VOID
PsxSrvFtruncate(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];       // +0x30
    LONG Length = (LONG)((PULONG)Message->Data.Raw)[1]; // +0x34
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
    Status = NtSetInformationFile(File->NtHandle, &IoStatusBlock, &EofInfo, sizeof(EofInfo),
                                  FileEndOfFileInformation);
    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// Shared pathconf/fpathconf resolver. Returns the configurable limit for `Name`
// on the (already-open) handle, or -1 with *Errno set. Faithful to the switch in
// sub_1F490A4 / sub_1F4927A: LINK_MAX and NAME_MAX come from the volume, the rest
// are fixed POSIX minimums.
//
LONG
PsxQueryPathconf(IN HANDLE Handle, IN ULONG Name, OUT PLONG Errno)
{
    UCHAR Buffer[128];
    PFILE_FS_ATTRIBUTE_INFORMATION FsAttr = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
    IO_STATUS_BLOCK IoStatusBlock;
    BOOLEAN Ntfs;

    *Errno = 0;
    switch (Name)
    {
        case 1:  // _PC_LINK_MAX -- hard links only on NTFS/OFS
            RtlZeroMemory(Buffer, sizeof(Buffer));
            if (NT_SUCCESS(NtQueryVolumeInformationFile(Handle, &IoStatusBlock, FsAttr,
                                                        sizeof(Buffer),
                                                        FileFsAttributeInformation)))
            {
                Ntfs = (FsAttr->FileSystemNameLength >= 6 * sizeof(WCHAR)) &&
                       (FsAttr->FileSystemName[0] == L'N' || FsAttr->FileSystemName[0] == L'O');
                return Ntfs ? 1024 : 1;
            }
            return 1;

        case 4:  // _PC_NAME_MAX -- volume's max component length
            RtlZeroMemory(Buffer, sizeof(Buffer));
            if (NT_SUCCESS(NtQueryVolumeInformationFile(Handle, &IoStatusBlock, FsAttr,
                                                        sizeof(Buffer),
                                                        FileFsAttributeInformation)))
                return (LONG)FsAttr->MaximumComponentNameLength;
            return 255;

        case 5:  return 512;    // _PC_PATH_MAX
        case 6:  return 512;    // _PC_PIPE_BUF
        case 7:  return 1;      // _PC_CHOWN_RESTRICTED
        case 8:  return 1;      // _PC_NO_TRUNC

        default:                // 2,3,9 and everything else are not queryable
            *Errno = PSX_EINVAL;
            return -1;
    }
}

//
// fpathconf(fd, name) -- ApiNumber 0x26. fd at Data[0], name at Data[1].
//
VOID
PsxSrvFpathconf(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];       // +0x30
    ULONG Name = ((PULONG)Message->Data.Raw)[1];        // +0x34
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);
    LONG Errno = 0;

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;
        Message->ReturnValue = -1;
        return;
    }

    // Volume-derived limits need a disk handle; other names are pure constants.
    if (((Name == 1) || (Name == 4)) && (File->FileType != PSX_FILE_DISK))
    {
        Message->Errno = 0;
        Message->ReturnValue = (Name == 4) ? 255 : 1;
        return;
    }
    Message->ReturnValue = PsxQueryPathconf(File->NtHandle, Name, &Errno);
    Message->Errno = (ULONG)Errno;
}

//
// The terminal-control ops (tcgetattr/tcsetattr at 0x2F/0x30). psxss owns the
// descriptor table but the actual termios lives in posix.exe (the terminal
// server). Like read/write/isatty, the tty vtable op here does NOT move the
// termios itself: for a terminal fd it flags the phase-2 session exchange
// (HasData at body +0x44) and returns 0, so the client then ships the termios
// block to/from posix.exe's DispatchTcSetAttr (session op 2, req[0]=0 tcgetattr
// / req[0]=1 tcsetattr -> SetConsoleMode). Without this flag the client treats
// tcsetattr as a local no-op and posix.exe never leaves canonical mode, so an
// interactive shell's raw-mode line editor never engages. A non-terminal fd is
// ENOTTY; a bad fd is EBADF. Faithful to sub_1F4BCBE / sub_1F4BCFE + the
// session data-phase pattern (HasData@+0x44) shared with read/write.
//
VOID
PsxSrvTtyQuery(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];
    PPSX_FILE_OBJECT File = PsxGetFile(Process, Fd);

    if (File == NULL)
    {
        Message->Errno = PSX_EBADF;      // 9
        Message->ReturnValue = -1;
        return;
    }
    if (File->FileType == PSX_FILE_TTY)
    {
        // Signal the phase-2 termios exchange with posix.exe (body +0x44).
        ((PULONG)Message->Data.Raw)[5] = 1;   // HasData
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    ((PULONG)Message->Data.Raw)[5] = 0;
    Message->Errno = 25;                  // ENOTTY
    Message->ReturnValue = -1;
}

//
// The remaining terminal-control ops (0x15 and tcsendbreak/tcdrain/tcflush/tcflow/
// tcgetpgrp/tcsetpgrp at 0x31-0x36). psxss cannot apply termios, so a valid fd
// yields ENOTTY and a bad fd yields EBADF. Faithful to sub_1F4BD3E..sub_1F4BE15.
//
VOID
PsxSrvTtyStub(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    INT Fd = (INT)((PULONG)Message->Data.Raw)[0];

    if (PsxGetFile(Process, Fd) == NULL)
    {
        Message->Errno = PSX_EBADF;      // 9
        Message->ReturnValue = -1;
        return;
    }
    Message->Errno = 25;                  // ENOTTY
    Message->ReturnValue = -1;
}
