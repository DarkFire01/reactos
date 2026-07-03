/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Path-based filesystem syscalls: unlink/rmdir/rename/link. Each
 *              takes server-relative NT path(s) in the client's shared section
 *              and runs the NT file op while impersonating the client (so access
 *              is checked against the caller's token). Faithful to the NT 4.0
 *              handlers sub_1F47605 (unlink), sub_1F471E1 (rmdir), sub_1F4780D
 *              (rename), sub_1F47B8D (link).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/iofuncs.h>
#include <ndk/kefuncs.h>    // NtQuerySystemTime (utime)

#define PSX_ENOENT   2
#define PSX_EINVAL   22

// NtOpenFile access/options reused below.
#define PSX_DELETE_ACCESS   (DELETE | SYNCHRONIZE)

//
// Read a path UNICODE_STRING out of the message at byte offset DataOffset within
// the body Data area (0 == body 0x30), and verify its Buffer lies in the client's
// shared section. The path bytes themselves are in our mapped view.
//
static BOOLEAN
PsxGetClientPath(IN PPSX_PROCESS Process,
                 IN PPSX_API_MESSAGE Message,
                 IN ULONG DataOffset,
                 OUT PUNICODE_STRING Path)
{
    *Path = *(PUNICODE_STRING)(Message->Data.Raw + DataOffset);
    return PsxValidateClientPointer(Process, (ULONG_PTR)Path->Buffer, Path->Length);
}

//
// Run the file op as the calling POSIX process so NT checks access against the
// client's token, not psxss's.
//
VOID
PsxImpersonateClient(IN PPSX_PROCESS Process, IN PPSX_API_MESSAGE Message)
{
    if (Process->ClientPort != NULL)
        NtImpersonateClientOfPort(Process->ClientPort, &Message->Header);
}

VOID
PsxRevertToSelf(VOID)
{
    HANDLE NullToken = NULL;
    NtSetInformationThread(NtCurrentThread(), ThreadImpersonationToken,
                           &NullToken, sizeof(HANDLE));
}

//
// Open a path for deletion (impersonating the client), then mark it for delete.
// Shared by unlink (files) and rmdir (directories) via the open-option flag.
//
static NTSTATUS
PsxDeletePath(IN PPSX_PROCESS Process,
              IN PPSX_API_MESSAGE Message,
              IN PUNICODE_STRING Path,
              IN ULONG OpenOptions)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_DISPOSITION_INFORMATION Disposition;
    HANDLE Handle;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, Path, OBJ_CASE_INSENSITIVE, NULL, NULL);

    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, PSX_DELETE_ACCESS, &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | OpenOptions);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
        return Status;

    Disposition.DeleteFile = TRUE;
    Status = NtSetInformationFile(Handle, &IoStatusBlock, &Disposition,
                                  sizeof(Disposition), FileDispositionInformation);
    NtClose(Handle);
    return Status;
}

//
// unlink(path) -- ApiNumber 0x1D.
//
VOID
PsxSrvUnlink(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    Status = PsxDeletePath(Process, Message, &Path, FILE_NON_DIRECTORY_FILE);
    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// rmdir(path) -- ApiNumber 0x3B. Rejects a trailing path separator (EINVAL),
// like the NT 4.0 handler.
//
VOID
PsxSrvRmdir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path) || (Path.Length < sizeof(WCHAR)))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    if (Path.Buffer[(Path.Length / sizeof(WCHAR)) - 1] == L'\\')
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    Status = PsxDeletePath(Process, Message, &Path, FILE_DIRECTORY_FILE);
    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// rename(old, new) -- ApiNumber 0x1E. Source path at body 0x30, target at 0x38.
//
VOID
PsxSrvRename(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING OldPath, NewPath;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_RENAME_INFORMATION RenameInfo;
    ULONG InfoLength;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &OldPath) ||
        !PsxGetClientPath(Process, Message, sizeof(UNICODE_STRING), &NewPath))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &OldPath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, PSX_DELETE_ACCESS, &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    InfoLength = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + NewPath.Length;
    RenameInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, InfoLength);
    if (RenameInfo == NULL)
    {
        NtClose(Handle);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }
    RenameInfo->ReplaceIfExists = TRUE;
    RenameInfo->RootDirectory = NULL;
    RenameInfo->FileNameLength = NewPath.Length;
    RtlCopyMemory(RenameInfo->FileName, NewPath.Buffer, NewPath.Length);

    Status = NtSetInformationFile(Handle, &IoStatusBlock, RenameInfo, InfoLength,
                                  FileRenameInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, RenameInfo);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// link(existing, new) -- ApiNumber 0x1A. NTFS hard link.
//
VOID
PsxSrvLink(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING OldPath, NewPath;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_LINK_INFORMATION LinkInfo;
    ULONG InfoLength;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &OldPath) ||
        !PsxGetClientPath(Process, Message, sizeof(UNICODE_STRING), &NewPath))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &OldPath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    InfoLength = FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) + NewPath.Length;
    LinkInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, InfoLength);
    if (LinkInfo == NULL)
    {
        NtClose(Handle);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }
    LinkInfo->ReplaceIfExists = FALSE;
    LinkInfo->RootDirectory = NULL;
    LinkInfo->FileNameLength = NewPath.Length;
    RtlCopyMemory(LinkInfo->FileName, NewPath.Buffer, NewPath.Length);

    Status = NtSetInformationFile(Handle, &IoStatusBlock, LinkInfo, InfoLength,
                                  FileLinkInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, LinkInfo);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// mkdir(path, mode) -- ApiNumber 0x1B. Path UNICODE_STRING at body 0x30; the
// POSIX mode at body 0x38 is not represented on NTFS and is ignored (as NT 4.0
// does). Creates the directory as the calling process.
//
VOID
PsxSrvMkdir(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtCreateFile(&Handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &ObjectAttributes,
                          &IoStatusBlock, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    PsxRevertToSelf();
    if (NT_SUCCESS(Status))
        NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// chmod(path, mode) -- ApiNumber 0x22. NTFS only represents the read-only
// attribute, so the POSIX owner-write bit (S_IWUSR, 0200) drives it: writable
// clears FILE_ATTRIBUTE_READONLY, non-writable sets it.
//
VOID
PsxSrvChmod(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG Mode = ((PULONG)Message->Data.Raw)[2];        // body +0x38
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION BasicInfo;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
    Status = NtQueryInformationFile(Handle, &IoStatusBlock, &BasicInfo, sizeof(BasicInfo),
                                    FileBasicInformation);
    if (NT_SUCCESS(Status))
    {
        if (Mode & 0200)
            BasicInfo.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        else
            BasicInfo.FileAttributes |= FILE_ATTRIBUTE_READONLY;
        if (BasicInfo.FileAttributes == 0)
            BasicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        // Leave the timestamps untouched (0 == "do not change").
        BasicInfo.CreationTime.QuadPart = 0;
        BasicInfo.LastAccessTime.QuadPart = 0;
        BasicInfo.LastWriteTime.QuadPart = 0;
        BasicInfo.ChangeTime.QuadPart = 0;
        Status = NtSetInformationFile(Handle, &IoStatusBlock, &BasicInfo, sizeof(BasicInfo),
                                      FileBasicInformation);
    }
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// utime(path, times) -- ApiNumber 0x24. The NULL-flag at body +0x38 (0 => set
// both to "now"); otherwise actime/modtime (POSIX time_t seconds) at +0x3C/+0x40.
//
VOID
PsxSrvUtime(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG TimesPtr   = ((PULONG)Message->Data.Raw)[2];  // +0x38 (0 => now)
    ULONG ActimeSec  = ((PULONG)Message->Data.Raw)[3];  // +0x3C
    ULONG ModtimeSec = ((PULONG)Message->Data.Raw)[4];  // +0x40
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION BasicInfo;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &IoStatusBlock, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
    if (TimesPtr == 0)
    {
        LARGE_INTEGER Now;
        NtQuerySystemTime(&Now);
        BasicInfo.LastAccessTime = Now;
        BasicInfo.LastWriteTime = Now;
    }
    else
    {
        RtlSecondsSince1970ToTime(ActimeSec, &BasicInfo.LastAccessTime);
        RtlSecondsSince1970ToTime(ModtimeSec, &BasicInfo.LastWriteTime);
    }
    Status = NtSetInformationFile(Handle, &IoStatusBlock, &BasicInfo, sizeof(BasicInfo),
                                  FileBasicInformation);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// mkfifo(path, mode) -- ApiNumber 0x1C. NT has no FIFO object type; the NT 4.0
// subsystem records a POSIX special file as a regular node tagged
// FILE_ATTRIBUTE_SYSTEM. Faithful to sub_1F48D71.
//
VOID
PsxSrvMkfifo(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtCreateFile(&Handle, FILE_GENERIC_WRITE | SYNCHRONIZE, &ObjectAttributes,
                          &IoStatusBlock, NULL, FILE_ATTRIBUTE_SYSTEM,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                          FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    PsxRevertToSelf();
    if (NT_SUCCESS(Status))
        NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

//
// pathconf(path, name) -- ApiNumber 0x25. Opens the path (as the caller) and
// returns the queried limit via the shared resolver. Faithful to sub_1F490A4.
//
VOID
PsxSrvPathconf(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG Name = ((PULONG)Message->Data.Raw)[2];        // body +0x38
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;
    LONG Errno = 0;

    if (!PsxGetClientPath(Process, Message, 0, &Path))
    {
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &IoStatusBlock, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
    {
        Message->Errno = PsxErrnoFromStatus(Status);
        Message->ReturnValue = -1;
        return;
    }

    Message->ReturnValue = PsxQueryPathconf(Handle, Name, &Errno);
    Message->Errno = (ULONG)Errno;
    NtClose(Handle);
}
