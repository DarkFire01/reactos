/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/moveansi.c
 * PURPOSE:         Directory functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 *                  Gerhard W. Gruber (sparhawk_at_gmx.at)
 *                  Dmitry Philippov (shedon@mail.ru)
 *                  Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
MoveFileA(IN LPCSTR lpExistingFileName,
          IN LPCSTR lpNewFileName)
{
    return MoveFileWithProgressA(lpExistingFileName,
                                 lpNewFileName,
                                 NULL,
                                 NULL,
                                 MOVEFILE_COPY_ALLOWED);
}


/*
 * @implemented
 */
BOOL
WINAPI
MoveFileExA(IN LPCSTR lpExistingFileName,
            IN LPCSTR lpNewFileName OPTIONAL,
            IN DWORD dwFlags)
{
    return MoveFileWithProgressA(lpExistingFileName,
                                 lpNewFileName,
                                 NULL,
                                 NULL,
                                 dwFlags);
}

/*
 * @implemented
 */
BOOL
WINAPI
ReplaceFileA(IN LPCSTR lpReplacedFileName,
             IN LPCSTR lpReplacementFileName,
             IN LPCSTR lpBackupFileName OPTIONAL,
             IN DWORD dwReplaceFlags,
             IN LPVOID lpExclude,
             IN LPVOID lpReserved)
{
    BOOL Ret;
    UNICODE_STRING ReplacedFileNameW, ReplacementFileNameW, BackupFileNameW;

    if (!lpReplacedFileName || !lpReplacementFileName || lpExclude || lpReserved ||
        (dwReplaceFlags & ~(REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!Basep8BitStringToDynamicUnicodeString(&ReplacedFileNameW, lpReplacedFileName))
    {
        return FALSE;
    }

    if (!Basep8BitStringToDynamicUnicodeString(&ReplacementFileNameW, lpReplacementFileName))
    {
        RtlFreeUnicodeString(&ReplacedFileNameW);
        return FALSE;
    }

    if (lpBackupFileName)
    {
        if (!Basep8BitStringToDynamicUnicodeString(&BackupFileNameW, lpBackupFileName))
        {
            RtlFreeUnicodeString(&ReplacementFileNameW);
            RtlFreeUnicodeString(&ReplacedFileNameW);
            return FALSE;
        }
    }
    else
    {
        BackupFileNameW.Buffer = NULL;
    }

    Ret = ReplaceFileW(ReplacedFileNameW.Buffer,
                       ReplacementFileNameW.Buffer,
                       BackupFileNameW.Buffer,
                       dwReplaceFlags, 0, 0);

    if (lpBackupFileName)
    {
        RtlFreeUnicodeString(&BackupFileNameW);
    }
    RtlFreeUnicodeString(&ReplacementFileNameW);
    RtlFreeUnicodeString(&ReplacedFileNameW);

    return Ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
MoveFileWithProgressA(IN LPCSTR lpExistingFileName,
                      IN LPCSTR lpNewFileName OPTIONAL,
                      IN LPPROGRESS_ROUTINE lpProgressRoutine OPTIONAL,
                      IN LPVOID lpData OPTIONAL,
                      IN DWORD dwFlags)
{
    BOOL Ret;
    UNICODE_STRING ExistingFileNameW, NewFileNameW;

    if (!Basep8BitStringToDynamicUnicodeString(&ExistingFileNameW, lpExistingFileName))
    {
        return FALSE;
    }

    if (lpNewFileName)
    {
        if (!Basep8BitStringToDynamicUnicodeString(&NewFileNameW, lpNewFileName))
        {
            RtlFreeUnicodeString(&ExistingFileNameW);
            return FALSE;
        }
    }
    else
    {
        NewFileNameW.Buffer = NULL;
    }

    Ret = MoveFileWithProgressW(ExistingFileNameW.Buffer,
                                NewFileNameW.Buffer,
                                lpProgressRoutine,
                                lpData,
                                dwFlags);

    RtlFreeUnicodeString(&ExistingFileNameW);
    RtlFreeUnicodeString(&NewFileNameW);

    return Ret;
}


/*
 * @implemented
 */
DWORD
WINAPI
BasepGetComputerNameFromNtPath(IN PUNICODE_STRING NewPath,
                               IN HANDLE NewHandle,
                               OUT PWSTR ComputerName,
                               IN OUT PULONG ComputerNameLength)
{
    BOOL Query = FALSE;
    WCHAR Letter;
    PWSTR AbsolutePath, EndOfName;
    USHORT AbsolutePathLength, NameLength;
    WCHAR TargetDevice[MAX_PATH + 1];
    WCHAR DeviceName[] = {'A', ':', '\0'}; /* Init to something, will be set later */
    UNICODE_STRING UncString = RTL_CONSTANT_STRING(L"\\??\\UNC\\");
    UNICODE_STRING GlobalString = RTL_CONSTANT_STRING(L"\\??\\");

    /* If it's an UNC path */
    if (RtlPrefixUnicodeString(&UncString, NewPath, TRUE))
    {
        /* Check for broken caller */
        if (NewPath->Length <= UncString.Length)
        {
            return ERROR_BAD_PATHNAME;
        }

        /* Skip UNC prefix */
        AbsolutePath = &NewPath->Buffer[UncString.Length / sizeof(WCHAR)];
        AbsolutePathLength = NewPath->Length - UncString.Length;

        /* And query DFS */
        Query = TRUE;
    }
    /* Otherwise, we have to be in global (NT path!), with drive letter */
    else if (RtlPrefixUnicodeString(&GlobalString, NewPath, TRUE) && NewPath->Buffer[5] == ':')
    {
        /* Path is like that: \??\C:\Complete Path\To File.ext */
        /* Get the letter and upcase it if required */
        Letter = NewPath->Buffer[4];
        if (Letter >= 'a' && Letter <= 'z')
        {
            Letter -= ('a' - 'A');
        }
        DeviceName[0] = Letter;

        /* Query the associated DOS device */
        if (!QueryDosDeviceW(DeviceName, TargetDevice, ARRAYSIZE(TargetDevice)))
        {
            return GetLastError();
        }

        /* If that's a network share */
        if (TargetDevice == wcsstr(TargetDevice, L"\\Device\\LanmanRedirector\\;"))
        {
            /* Path is like that: \Device\LanmanRedirector\;C:0000000000000000\Complete Path\To File.ext */
            /* Check we have the correct drive letter */
            if (TargetDevice[26] == DeviceName[0] &&
                TargetDevice[27] == ':')
            {
                /* Check for the path begin, computer name is before */
                PWSTR Path = wcschr(&TargetDevice[28], L'\\');
                if (Path == NULL)
                {
                    return ERROR_BAD_PATHNAME;
                }

                AbsolutePath = Path + 1;
                AbsolutePathLength = sizeof(WCHAR) * (ARRAYSIZE(TargetDevice) - (AbsolutePath - TargetDevice));
            }
            else
            {
                return ERROR_BAD_PATHNAME;
            }
        }
        /* If it's a local device */
        else if (TargetDevice == wcsstr(TargetDevice, L"\\Device\\Harddisk")
                 || TargetDevice == wcsstr(TargetDevice, L"\\Device\\CdRom")
                 || TargetDevice == wcsstr(TargetDevice, L"\\Device\\Floppy"))
        {
            /* Just query the computer name */
            if (!GetComputerNameW(ComputerName, ComputerNameLength))
            {
                return GetLastError();
            }

            return ERROR_SUCCESS;
        }
        /* If it's a DFS share */
        else if (TargetDevice == wcsstr(TargetDevice, L"\\Device\\WinDfs\\"))
        {
            /* Obviously, query DFS */
            Query = TRUE;
        }
        else
        {
            return ERROR_BAD_PATHNAME;
        }
    }
    else
    {
        return ERROR_BAD_PATHNAME;
    }

    /* Query DFS, currently not implemented - shouldn't be missing in ReactOS yet ;-) */
    if (Query)
    {
        OutputDebugStringA("Querying DFS not implemented!\n");
        __debugbreak();
        AbsolutePath = NULL;
        AbsolutePathLength = 0;
    }

    /* Now, properly extract the computer name from the full path */
    EndOfName = AbsolutePath;
    if (AbsolutePathLength)
    {
        for (NameLength = 0; NameLength < AbsolutePathLength; NameLength += sizeof(WCHAR))
        {
            /* Look for the next \, it will be the end of computer name */
            if (EndOfName[0] == L'\\')
            {
                break;
            }
            /* Computer name cannot contain ., if we get to that point, something went wrong... */
            else if (EndOfName[0] == L'.')
            {
                return ERROR_BAD_PATHNAME;
            }

            ++EndOfName;
        }
    }

    NameLength = EndOfName - AbsolutePath;
    /* Check we didn't overflow and that our computer name isn't ill-formed */
    if (NameLength >= AbsolutePathLength || NameLength >= MAX_COMPUTERNAME_LENGTH * sizeof(WCHAR))
    {
        return ERROR_BAD_PATHNAME;
    }

    /* Check we can fit */
    if (NameLength + sizeof(UNICODE_NULL) > *ComputerNameLength * sizeof(WCHAR))
    {
        return ERROR_BUFFER_OVERFLOW;
    }

    /* Write, zero and done! */
    RtlCopyMemory(ComputerName, AbsolutePath, NameLength);
    *ComputerNameLength = NameLength / sizeof(WCHAR);
    ComputerName[NameLength / sizeof(WCHAR)] = UNICODE_NULL;

    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
WINAPI
BasepNotifyTrackingService(IN OUT PHANDLE ExistingHandle,
                           IN POBJECT_ATTRIBUTES ObjectAttributes,
                           IN HANDLE NewHandle,
                           IN PUNICODE_STRING NewPath)
{
    NTSTATUS Status;
    ULONG ComputerNameLength, FileAttributes;
    WCHAR ComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    OEM_STRING ComputerNameStringA;
    CHAR ComputerNameStringBuffer[MAX_PATH + 1];
    UNICODE_STRING ComputerNameStringW;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION FileBasicInfo;
    HANDLE hFullWrite;
    struct
    {
        FILE_TRACKING_INFORMATION;
        CHAR Buffer[(MAX_COMPUTERNAME_LENGTH + 1) * sizeof(WCHAR)];
    } FileTrackingInfo;

    Status = STATUS_SUCCESS;
    ComputerNameLength = ARRAYSIZE(ComputerName);

    /* Attempt to get computer name of target handle */
    if (BasepGetComputerNameFromNtPath(NewPath, NewHandle, ComputerName, &ComputerNameLength))
    {
        /* If we failed to get it, we will just notify with the handle */
        FileTrackingInfo.ObjectInformationLength = 0;
    }
    else
    {
        /* Convert the retrieved computer name to ANSI and attach it to the notification */
        RtlInitEmptyAnsiString(&ComputerNameStringA,
                               ComputerNameStringBuffer,
                               sizeof(ComputerNameStringBuffer));

        RtlInitUnicodeString(&ComputerNameStringW, ComputerName);
        Status = RtlUnicodeStringToOemString(&ComputerNameStringA, &ComputerNameStringW, FALSE);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        RtlCopyMemory(FileTrackingInfo.ObjectInformation, ComputerNameStringA.Buffer, ComputerNameStringA.Length);
        FileTrackingInfo.ObjectInformation[ComputerNameStringA.Length] = ANSI_NULL;
        FileTrackingInfo.ObjectInformationLength = ComputerNameStringA.Length + 1;
    }

    /* Attach the handle we moved */
    FileTrackingInfo.DestinationFile = NewHandle;

    /* Final, notify */
    Status = NtSetInformationFile(*ExistingHandle,
                                  &IoStatusBlock,
                                  &FileTrackingInfo,
                                  sizeof(FileTrackingInfo),
                                  FileTrackingInformation);
    if (Status != STATUS_ACCESS_DENIED)
    {
        return Status;
    }

    /* If we get here, we got access denied error, this comes from a
     * read-only flag. So, close the file, in order to reopen it with enough
     * rights to remove said flag and reattempt notification.
     */
    CloseHandle(*ExistingHandle);

    /* Reopen it, to be able to change the destination file attributes */
    Status = NtOpenFile(ExistingHandle,
                        SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                        ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        *ExistingHandle = INVALID_HANDLE_VALUE;
        return Status;
    }

    /* Get the file attributes */
    Status = NtQueryInformationFile(*ExistingHandle,
                                    &IoStatusBlock,
                                    &FileBasicInfo,
                                    sizeof(FileBasicInfo),
                                    FileBasicInformation);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Get rid of the read only flag */
    FileAttributes = FileBasicInfo.FileAttributes & ~FILE_ATTRIBUTE_READONLY;
    RtlZeroMemory(&FileBasicInfo, sizeof(FileBasicInfo));
    FileBasicInfo.FileAttributes = FileAttributes;

    /* Attempt... */
    Status = NtSetInformationFile(*ExistingHandle,
                                  &IoStatusBlock,
                                  &FileBasicInfo,
                                  sizeof(FileBasicInfo),
                                  FileBasicInformation);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Now, reopen with maximum accesses to notify */
    Status = NtOpenFile(&hFullWrite,
                        GENERIC_WRITE | SYNCHRONIZE,
                        ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(Status))
    {
        NtClose(*ExistingHandle);
        *ExistingHandle = hFullWrite;

        /* Full success, notify! */
        Status = NtSetInformationFile(*ExistingHandle,
                                      &IoStatusBlock,
                                      &FileTrackingInfo,
                                      sizeof(FileTrackingInfo),
                                      FileTrackingInformation);
    }

    /* If opening with full access failed or if notify failed, restore read-only */
    if (!NT_SUCCESS(Status))
    {
        FileBasicInfo.FileAttributes |= FILE_ATTRIBUTE_READONLY;

        Status = NtSetInformationFile(*ExistingHandle,
                                      &IoStatusBlock,
                                      &FileBasicInfo,
                                      sizeof(FileBasicInfo),
                                      FileBasicInformation);
    }

    /* We're done */
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
WINAPI
BasepOpenFileForMove(IN LPCWSTR File,
                     OUT PUNICODE_STRING RelativeNtName,
                     OUT LPWSTR * NtName,
                     OUT PHANDLE FileHandle,
                     OUT POBJECT_ATTRIBUTES ObjectAttributes,
                     IN ACCESS_MASK DesiredAccess,
                     IN ULONG ShareAccess,
                     IN ULONG OpenOptions)
{
    RTL_RELATIVE_NAME_U RelativeName;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_ATTRIBUTE_TAG_INFORMATION TagInfo;
    ULONG IntShareAccess;
    BOOLEAN HasRelative = FALSE;

    _SEH2_TRY
    {
        /* Zero output */
        RtlInitEmptyUnicodeString(RelativeNtName, NULL, 0);
        *NtName = NULL;

        if (!RtlDosPathNameToRelativeNtPathName_U(File, RelativeNtName, NULL, &RelativeName))
        {
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            _SEH2_LEAVE;
        }

        HasRelative = TRUE;
        *NtName = RelativeNtName->Buffer;

        if (RelativeName.RelativeName.Length)
        {
            RelativeNtName->Length = RelativeName.RelativeName.Length;
            RelativeNtName->MaximumLength = RelativeName.RelativeName.MaximumLength;
            RelativeNtName->Buffer = RelativeName.RelativeName.Buffer;
        }
        else
        {
            RelativeName.ContainingDirectory = NULL;
        }

        InitializeObjectAttributes(ObjectAttributes,
                                   RelativeNtName,
                                   OBJ_CASE_INSENSITIVE,
                                   RelativeName.ContainingDirectory,
                                   NULL);
        /* Force certain flags here, given ops we'll do */
        IntShareAccess = ShareAccess | FILE_SHARE_READ | FILE_SHARE_WRITE;
        OpenOptions |= FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT;

        /* We'll try to read reparse tag */
        Status = NtOpenFile(FileHandle,
                            DesiredAccess | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                            ObjectAttributes,
                            &IoStatusBlock,
                            IntShareAccess,
                            OpenOptions | FILE_OPEN_REPARSE_POINT);
        if (NT_SUCCESS(Status))
        {
            /* Attempt the read */
            Status = NtQueryInformationFile(*FileHandle,
                                            &IoStatusBlock,
                                            &TagInfo,
                                            sizeof(TagInfo),
                                            FileAttributeTagInformation);

            /* Return if failure with a status that wouldn't mean the FSD cannot support reparse points */
            if (!NT_SUCCESS(Status) &&
                (Status != STATUS_NOT_IMPLEMENTED && Status != STATUS_INVALID_PARAMETER))
            {
                _SEH2_LEAVE;
            }

            if (NT_SUCCESS(Status))
            {
                /* This cannot happen on mount points */
                if (TagInfo.FileAttributes & FILE_ATTRIBUTE_DEVICE ||
                    TagInfo.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
                {
                    _SEH2_LEAVE;
                }
            }

            NtClose(*FileHandle);
            *FileHandle = INVALID_HANDLE_VALUE;

            IntShareAccess = ShareAccess | FILE_SHARE_READ | FILE_SHARE_DELETE;
        }
        else if (Status == STATUS_INVALID_PARAMETER)
        {
            IntShareAccess = ShareAccess | FILE_SHARE_READ | FILE_SHARE_WRITE;
        }
        else
        {
            _SEH2_LEAVE;
        }

        /* Reattempt to open normally, following reparse point if needed */
        Status = NtOpenFile(FileHandle,
                            DesiredAccess | SYNCHRONIZE,
                            ObjectAttributes,
                            &IoStatusBlock,
                            IntShareAccess,
                            OpenOptions);
    }
    _SEH2_FINALLY
    {
        if (HasRelative)
        {
            RtlReleaseRelativeName(&RelativeName);
        }
    }
    _SEH2_END;

    return Status;
}

/*
 * @implemented
 */
BOOL
WINAPI
PrivMoveFileIdentityW(IN LPCWSTR lpSource, IN LPCWSTR lpDestination, IN DWORD dwFlags)
{
    ACCESS_MASK SourceAccess;
    UNICODE_STRING NtSource, NtDestination;
    LPWSTR RelativeSource, RelativeDestination;
    HANDLE SourceHandle, DestinationHandle;
    OBJECT_ATTRIBUTES ObjectAttributesSource, ObjectAttributesDestination;
    NTSTATUS Status, OldStatus = STATUS_SUCCESS;
    ACCESS_MASK DestAccess;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION SourceInformation, DestinationInformation;
    FILE_DISPOSITION_INFORMATION FileDispositionInfo;

    SourceHandle = INVALID_HANDLE_VALUE;
    RtlInitEmptyUnicodeString(&NtSource, NULL, 0);
    RelativeSource = NULL;
    DestinationHandle = INVALID_HANDLE_VALUE;
    RtlInitEmptyUnicodeString(&NtDestination, NULL, 0);
    RelativeDestination = NULL;

    /* FILE_WRITE_DATA is required for later on notification */
    SourceAccess = FILE_READ_ATTRIBUTES | FILE_WRITE_DATA;
    if (dwFlags & PRIV_DELETE_ON_SUCCESS)
    {
        SourceAccess |= DELETE;
    }

    _SEH2_TRY
    {
        /* We will loop twice:
         * First we attempt to open with FILE_WRITE_DATA for notification.
         * If it fails and we have flag for non-trackable files, we retry
         * without FILE_WRITE_DATA.
         * If that one fails, then, we quit for real.
         */
        while (TRUE)
        {
            Status = BasepOpenFileForMove(lpSource,
                                          &NtSource,
                                          &RelativeSource,
                                          &SourceHandle,
                                          &ObjectAttributesSource,
                                          SourceAccess,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          FILE_OPEN_NO_RECALL);
            if (NT_SUCCESS(Status))
            {
                break;
            }

            /* If we already attempted the opening without FILE_WRITE_DATA
             * or if we cannot move on non-trackable files, fail.
             */
            if (!(SourceAccess & FILE_WRITE_DATA) || !(dwFlags & PRIV_ALLOW_NON_TRACKABLE))
            {
                _SEH2_LEAVE;
            }

            if (RelativeSource)
            {
                RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeSource);
                RelativeSource = NULL;
            }

            if (SourceHandle != INVALID_HANDLE_VALUE)
            {
                NtClose(SourceHandle);
                SourceHandle = INVALID_HANDLE_VALUE;
            }

            SourceAccess &= ~FILE_WRITE_DATA;

            /* Remember fist failure in the path */
            if (NT_SUCCESS(OldStatus))
            {
                OldStatus = Status;
            }
        }

        DestAccess = FILE_WRITE_ATTRIBUTES;
        /* If we could preserve FILE_WRITE_DATA for source, attempt to
         * get it for destination, still for notification purposes. */
        if (SourceAccess & FILE_WRITE_DATA)
        {
            DestAccess |= FILE_WRITE_DATA;
        }

        /* cf comment for first loop */
        while (TRUE)
        {
            Status = BasepOpenFileForMove(lpDestination,
                                          &NtDestination,
                                          &RelativeDestination,
                                          &DestinationHandle,
                                          &ObjectAttributesDestination,
                                          DestAccess,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          FILE_OPEN_NO_RECALL);
            if (NT_SUCCESS(Status))
            {
                break;
            }

            /* If we already attempted the opening without FILE_WRITE_DATA
             * or if we cannot move on non-trackable files, fail.
             */
            if (!(DestAccess & FILE_WRITE_DATA) || !(dwFlags & PRIV_ALLOW_NON_TRACKABLE))
            {
                _SEH2_LEAVE;
            }

            if (RelativeDestination)
            {
                RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeDestination);
                RelativeDestination = NULL;
            }

            if (DestinationHandle != INVALID_HANDLE_VALUE)
            {
                NtClose(DestinationHandle);
                DestinationHandle = INVALID_HANDLE_VALUE;
            }

            DestAccess &= ~FILE_WRITE_DATA;

            /* Remember fist failure in the path */
            if (NT_SUCCESS(OldStatus))
            {
                OldStatus = Status;
            }
        }

        /* Get the creation time from source */
        Status = NtQueryInformationFile(SourceHandle,
                                        &IoStatusBlock,
                                        &SourceInformation,
                                        sizeof(SourceInformation),
                                        FileBasicInformation);
        if (NT_SUCCESS(Status))
        {
            /* Then, prepare to set it for destination */
            RtlZeroMemory(&DestinationInformation, sizeof(DestinationInformation));
            DestinationInformation.CreationTime.QuadPart = SourceInformation.CreationTime.QuadPart;

            /* And set it, that's all folks! */
            Status = NtSetInformationFile(DestinationHandle,
                                          &IoStatusBlock,
                                          &DestinationInformation,
                                          sizeof(DestinationInformation),
                                          FileBasicInformation);
        }

        if (!NT_SUCCESS(Status))
        {
            if (!(dwFlags & PRIV_ALLOW_NON_TRACKABLE))
            {
                _SEH2_LEAVE;
            }

            /* Remember the failure for later notification */
            if (NT_SUCCESS(OldStatus))
            {
                OldStatus = Status;
            }
        }

        /* If we could open with FILE_WRITE_DATA both source and destination,
         * then, notify
         */
        if (DestAccess & FILE_WRITE_DATA && SourceAccess & FILE_WRITE_DATA)
        {
            Status = BasepNotifyTrackingService(&SourceHandle,
                                                &ObjectAttributesSource,
                                                DestinationHandle,
                                                &NtDestination);
            if (!NT_SUCCESS(Status))
            {
                if (dwFlags & PRIV_ALLOW_NON_TRACKABLE)
                {
                    if (NT_SUCCESS(OldStatus))
                        OldStatus = Status;

                    /* Reset status, we allow non trackable files */
                    Status = STATUS_SUCCESS;
                }
            }
        }
    }
    _SEH2_FINALLY
    {
        if (RelativeSource)
            RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeSource);

        if (RelativeDestination)
            RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeDestination);
    }
    _SEH2_END;

    /* If caller asked for source deletion, if everything succeed, proceed */
    if (NT_SUCCESS(Status) && dwFlags & PRIV_DELETE_ON_SUCCESS)
    {
        FileDispositionInfo.DeleteFile = TRUE;

        Status = NtSetInformationFile(SourceHandle,
                                      &IoStatusBlock,
                                      &FileDispositionInfo,
                                      sizeof(FileDispositionInfo),
                                      FileDispositionInformation);
    }

    /* Cleanup/close portion */
    if (DestinationHandle != INVALID_HANDLE_VALUE)
    {
        NtClose(DestinationHandle);
    }

    if (SourceHandle != INVALID_HANDLE_VALUE)
    {
        NtClose(SourceHandle);
    }

    /* Set last error if any, and quit */
    if (NT_SUCCESS(Status))
    {
        if (!NT_SUCCESS(OldStatus))
        {
            BaseSetLastNTError(OldStatus);
        }
    }
    else
    {
        BaseSetLastNTError(Status);
    }

    return NT_SUCCESS(Status);
}

/* EOF */
