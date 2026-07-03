/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Path based filesystem calls, run while impersonating the client
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/iofuncs.h>
#include <ndk/kefuncs.h>

#define PSX_ENOENT   2
#define PSX_EINVAL   22

#define PSX_DELETE_ACCESS   (DELETE | SYNCHRONIZE)

/**
 * @brief Read a path UNICODE_STRING at DataOffset in the message body and validate its buffer.
 */
static
BOOLEAN
PsxGetClientPath(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_API_MESSAGE Message,
    _In_ ULONG DataOffset,
    _Out_ PUNICODE_STRING Path)
{
    *Path = *(PUNICODE_STRING)(Message->Data.Raw + DataOffset);
    return PsxValidateClientPointer(Process, (ULONG_PTR)Path->Buffer, Path->Length);
}

/**
 * @brief Impersonate the calling client so NT checks access against its token.
 */
VOID
PsxImpersonateClient(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_API_MESSAGE Message)
{
    if (Process->ClientPort != NULL)
        NtImpersonateClientOfPort(Process->ClientPort, &Message->Header);
}

VOID
PsxRevertToSelf(VOID)
{
    HANDLE NullToken = NULL;

    NtSetInformationThread(NtCurrentThread(),
                           ThreadImpersonationToken,
                           &NullToken,
                           sizeof(NullToken));
}

/**
 * @brief Open a path as the client and mark it for deletion. Used by unlink and rmdir.
 */
static
NTSTATUS
PsxDeletePath(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_API_MESSAGE Message,
    _In_ PUNICODE_STRING Path,
    _In_ ULONG OpenOptions)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_DISPOSITION_INFORMATION Disposition;
    HANDLE Handle;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, Path, OBJ_CASE_INSENSITIVE, NULL, NULL);

    PsxImpersonateClient(Process, Message);
    Status = NtOpenFile(&Handle,
                        PSX_DELETE_ACCESS,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | OpenOptions);
    PsxRevertToSelf();
    if (!NT_SUCCESS(Status))
        return Status;

    Disposition.DeleteFile = TRUE;
    Status = NtSetInformationFile(Handle,
                                  &IoStatusBlock,
                                  &Disposition,
                                  sizeof(Disposition),
                                  FileDispositionInformation);
    NtClose(Handle);
    return Status;
}

/**
 * @brief unlink(path) (ApiNumber 0x1D).
 */
VOID
PsxSrvUnlink(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
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

/**
 * @brief rmdir(path) (ApiNumber 0x3B). A trailing path separator fails with EINVAL.
 */
VOID
PsxSrvRmdir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
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

/**
 * @brief rename(old, new) (ApiNumber 0x1E). Source path at Data offset 0, target right after it.
 */
VOID
PsxSrvRename(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING OldPath;
    UNICODE_STRING NewPath;
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
    Status = NtOpenFile(&Handle,
                        PSX_DELETE_ACCESS,
                        &ObjectAttributes,
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

    Status = NtSetInformationFile(Handle,
                                  &IoStatusBlock,
                                  RenameInfo,
                                  InfoLength,
                                  FileRenameInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, RenameInfo);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief link(existing, new) (ApiNumber 0x1A). Creates an NTFS hard link.
 */
VOID
PsxSrvLink(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING OldPath;
    UNICODE_STRING NewPath;
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
    Status = NtOpenFile(&Handle,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
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

    Status = NtSetInformationFile(Handle,
                                  &IoStatusBlock,
                                  LinkInfo,
                                  InfoLength,
                                  FileLinkInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, LinkInfo);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief mkdir(path, mode) (ApiNumber 0x1B). The POSIX mode has no NTFS equivalent and is ignored.
 */
VOID
PsxSrvMkdir(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
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
    Status = NtCreateFile(&Handle,
                          FILE_LIST_DIRECTORY | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_CREATE,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    PsxRevertToSelf();
    if (NT_SUCCESS(Status))
        NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief chmod(path, mode) (ApiNumber 0x22).
 *
 * Only the owner write bit (0200) is represented, as the absence of FILE_ATTRIBUTE_READONLY.
 */
VOID
PsxSrvChmod(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG Mode = ((PULONG)Message->Data.Raw)[2];
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
    Status = NtOpenFile(&Handle,
                        FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
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

    RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
    Status = NtQueryInformationFile(Handle,
                                    &IoStatusBlock,
                                    &BasicInfo,
                                    sizeof(BasicInfo),
                                    FileBasicInformation);
    if (NT_SUCCESS(Status))
    {
        if (Mode & 0200)
            BasicInfo.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        else
            BasicInfo.FileAttributes |= FILE_ATTRIBUTE_READONLY;
        if (BasicInfo.FileAttributes == 0)
            BasicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;

        /* Zero timestamps are left unchanged */
        BasicInfo.CreationTime.QuadPart = 0;
        BasicInfo.LastAccessTime.QuadPart = 0;
        BasicInfo.LastWriteTime.QuadPart = 0;
        BasicInfo.ChangeTime.QuadPart = 0;
        Status = NtSetInformationFile(Handle,
                                      &IoStatusBlock,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      FileBasicInformation);
    }
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief utime(path, times) (ApiNumber 0x24).
 *
 * Data[2] is zero to set both times to now; otherwise Data[3] and Data[4] hold
 * actime and modtime in seconds since 1970.
 */
VOID
PsxSrvUtime(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG TimesPtr = ((PULONG)Message->Data.Raw)[2];
    ULONG ActimeSec = ((PULONG)Message->Data.Raw)[3];
    ULONG ModtimeSec = ((PULONG)Message->Data.Raw)[4];
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION BasicInfo;
    LARGE_INTEGER Now;
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
    Status = NtOpenFile(&Handle,
                        FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
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

    RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
    if (TimesPtr == 0)
    {
        NtQuerySystemTime(&Now);
        BasicInfo.LastAccessTime = Now;
        BasicInfo.LastWriteTime = Now;
    }
    else
    {
        RtlSecondsSince1970ToTime(ActimeSec, &BasicInfo.LastAccessTime);
        RtlSecondsSince1970ToTime(ModtimeSec, &BasicInfo.LastWriteTime);
    }
    Status = NtSetInformationFile(Handle,
                                  &IoStatusBlock,
                                  &BasicInfo,
                                  sizeof(BasicInfo),
                                  FileBasicInformation);
    NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief mkfifo(path, mode) (ApiNumber 0x1C).
 *
 * NT has no FIFO object, so the node is a regular file tagged FILE_ATTRIBUTE_SYSTEM.
 */
VOID
PsxSrvMkfifo(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
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
    Status = NtCreateFile(&Handle,
                          FILE_GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_SYSTEM,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_CREATE,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    PsxRevertToSelf();
    if (NT_SUCCESS(Status))
        NtClose(Handle);

    Message->Errno = NT_SUCCESS(Status) ? 0 : PsxErrnoFromStatus(Status);
    Message->ReturnValue = NT_SUCCESS(Status) ? 0 : -1;
}

/**
 * @brief pathconf(path, name) (ApiNumber 0x25). Name is at Data[2].
 */
VOID
PsxSrvPathconf(
    _In_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    UNICODE_STRING Path;
    ULONG Name = ((PULONG)Message->Data.Raw)[2];
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
    Status = NtOpenFile(&Handle,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
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

    Message->ReturnValue = PsxQueryPathconf(Handle, Name, &Errno);
    Message->Errno = (ULONG)Errno;
    NtClose(Handle);
}
