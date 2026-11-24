/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/iocompl.c
 * PURPOSE:         Io Completion functions
 * PROGRAMMERS:     Ariadne (ariadne@xs4all.nl)
 *                  Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES ******************************************************************/

#include <k32.h>

/* FUNCTIONS *****************************************************************/


/*
 * @implemented
 */
BOOL
WINAPI
BindIoCompletionCallback(IN HANDLE FileHandle,
                         IN LPOVERLAPPED_COMPLETION_ROUTINE Function,
                         IN ULONG Flags)
{
    NTSTATUS Status;

    /* Call RTL */
    Status = RtlSetIoCompletionCallback(FileHandle,
                                        (PIO_APC_ROUTINE)Function,
                                        Flags);
    if (!NT_SUCCESS(Status))
    {
        /* Set error and fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CallNamedPipeA(LPCSTR lpNamedPipeName,
               LPVOID lpInBuffer,
               DWORD nInBufferSize,
               LPVOID lpOutBuffer,
               DWORD nOutBufferSize,
               LPDWORD lpBytesRead,
               DWORD nTimeOut)
{
    PUNICODE_STRING PipeName = &NtCurrentTeb()->StaticUnicodeString;
    ANSI_STRING AnsiPipe;

    /* Initialize the string as ANSI_STRING and convert to Unicode */
    RtlInitAnsiString(&AnsiPipe, (LPSTR)lpNamedPipeName);
    RtlAnsiStringToUnicodeString(PipeName, &AnsiPipe, FALSE);

    /* Call the Unicode function */
    return CallNamedPipeW(PipeName->Buffer,
                          lpInBuffer,
                          nInBufferSize,
                          lpOutBuffer,
                          nOutBufferSize,
                          lpBytesRead,
                          nTimeOut);
}


/*
 * @implemented
 */
BOOL
WINAPI
CheckNameLegalDOS8Dot3W(
    LPCWSTR lpName,
    LPSTR lpOemName OPTIONAL,
    DWORD OemNameSize OPTIONAL,
    PBOOL pbNameContainsSpaces OPTIONAL,
    PBOOL pbNameLegal
    )
{
    UNICODE_STRING Name;
    ANSI_STRING AnsiName;
    BOOLEAN NameContainsSpaces;

    if(lpName == NULL ||
       (lpOemName == NULL && OemNameSize != 0) ||
       pbNameLegal == NULL)
    {
      SetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
    }

    if(lpOemName != NULL)
    {
      AnsiName.Buffer = lpOemName;
      AnsiName.MaximumLength = (USHORT)OemNameSize * sizeof(CHAR);
      AnsiName.Length = 0;
    }

    RtlInitUnicodeString(&Name, lpName);

    *pbNameLegal = RtlIsNameLegalDOS8Dot3(&Name,
                                          (lpOemName ? &AnsiName : NULL),
                                          &NameContainsSpaces);
    if (*pbNameLegal && pbNameContainsSpaces)
        *pbNameContainsSpaces = NameContainsSpaces;

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
CheckNameLegalDOS8Dot3A(
    LPCSTR lpName,
    LPSTR lpOemName OPTIONAL,
    DWORD OemNameSize OPTIONAL,
    PBOOL pbNameContainsSpaces OPTIONAL,
    PBOOL pbNameLegal
    )
{
    UNICODE_STRING Name;
    ANSI_STRING AnsiName, AnsiInputName;
    NTSTATUS Status;
    BOOLEAN NameContainsSpaces;

    if(lpName == NULL ||
       (lpOemName == NULL && OemNameSize != 0) ||
       pbNameLegal == NULL)
    {
      SetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
    }

    if(lpOemName != NULL)
    {
      AnsiName.Buffer = lpOemName;
      AnsiName.MaximumLength = (USHORT)OemNameSize * sizeof(CHAR);
      AnsiName.Length = 0;
    }

    RtlInitAnsiString(&AnsiInputName, (LPSTR)lpName);
    if(bIsFileApiAnsi)
      Status = RtlAnsiStringToUnicodeString(&Name, &AnsiInputName, TRUE);
    else
      Status = RtlOemStringToUnicodeString(&Name, &AnsiInputName, TRUE);

    if(!NT_SUCCESS(Status))
    {
      BaseSetLastNTError(Status);
      return FALSE;
    }

    *pbNameLegal = RtlIsNameLegalDOS8Dot3(&Name,
                                          (lpOemName ? &AnsiName : NULL),
                                          &NameContainsSpaces);
    if (*pbNameLegal && pbNameContainsSpaces)
        *pbNameContainsSpaces = NameContainsSpaces;

    RtlFreeUnicodeString(&Name);

    return TRUE;
}



/*
 * @implemented
 */
BOOL
WINAPI
WaitNamedPipeA(LPCSTR lpNamedPipeName,
               DWORD nTimeOut)
{
    BOOL r = FALSE;
    UNICODE_STRING NameU;

    /* Convert the name to Unicode */
    if (Basep8BitStringToDynamicUnicodeString(&NameU, lpNamedPipeName))
    {
        /* Call the Unicode API */
        r = WaitNamedPipeW(NameU.Buffer, nTimeOut);

        /* Free the Unicode string */
        RtlFreeUnicodeString(&NameU);
    }

    /* Return result */
    return r;
}


/*
 * @implemented
 */
HANDLE
WINAPI
CreateNamedPipeA(LPCSTR lpName,
                 DWORD dwOpenMode,
                 DWORD dwPipeMode,
                 DWORD nMaxInstances,
                 DWORD nOutBufferSize,
                 DWORD nInBufferSize,
                 DWORD nDefaultTimeOut,
                 LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    /* Call the W(ide) function */
    ConvertWin32AnsiChangeApiToUnicodeApi(CreateNamedPipe,
                                          lpName,
                                          dwOpenMode,
                                          dwPipeMode,
                                          nMaxInstances,
                                          nOutBufferSize,
                                          nInBufferSize,
                                          nDefaultTimeOut,
                                          lpSecurityAttributes);
}

/*
 * @implemented
 */
HANDLE
NTAPI
CreateFileMappingA(IN HANDLE hFile,
                   IN LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                   IN DWORD flProtect,
                   IN DWORD dwMaximumSizeHigh,
                   IN DWORD dwMaximumSizeLow,
                   IN LPCSTR lpName)
{
    /* Call the W(ide) function */
    ConvertWin32AnsiObjectApiToUnicodeApi(FileMapping,
                                          lpName,
                                          hFile,
                                          lpFileMappingAttributes,
                                          flProtect,
                                          dwMaximumSizeHigh,
                                          dwMaximumSizeLow);
}


/*
 * @implemented
 */
BOOL
IsThisARootDirectory(IN HANDLE VolumeHandle,
                     IN PUNICODE_STRING NtPathName)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    struct
    {
        FILE_NAME_INFORMATION;
        WCHAR Buffer[MAX_PATH];
    } FileNameInfo;

    /* If we have a handle, query the name */
    if (VolumeHandle)
    {
        Status = NtQueryInformationFile(VolumeHandle, &IoStatusBlock, &FileNameInfo, sizeof(FileNameInfo), FileNameInformation);
        if (!NT_SUCCESS(Status))
        {
            return FALSE;
        }

        /* Check we properly end with a \ */
        if (FileNameInfo.FileName[FileNameInfo.FileNameLength / sizeof(WCHAR) - 1] != L'\\')
        {
            return FALSE;
        }
    }

    /* If we have a path */
    if (NtPathName != NULL)
    {
        HANDLE LinkHandle;
        WCHAR Buffer[512];
        ULONG ReturnedLength;
        UNICODE_STRING LinkTarget;
        OBJECT_ATTRIBUTES ObjectAttributes;

        NtPathName->Length -= sizeof(WCHAR);

        InitializeObjectAttributes(&ObjectAttributes, NtPathName,
                                   OBJ_CASE_INSENSITIVE,
                                   NULL, NULL);

        /* Try to see whether that's a symbolic name */
        Status = NtOpenSymbolicLinkObject(&LinkHandle, SYMBOLIC_LINK_QUERY, &ObjectAttributes);
        NtPathName->Length += sizeof(WCHAR);
        if (!NT_SUCCESS(Status))
        {
            return FALSE;
        }

        /* If so, query the target */
        LinkTarget.Buffer = Buffer;
        LinkTarget.Length = 0;
        LinkTarget.MaximumLength = sizeof(Buffer);

        Status = NtQuerySymbolicLinkObject(LinkHandle, &LinkTarget, &ReturnedLength);
        NtClose(LinkHandle);
        /* A root directory (NtName) is a symbolic link */
        if (!NT_SUCCESS(Status))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetVolumeLabelA(IN LPCSTR lpRootPathName,
                IN LPCSTR lpVolumeName OPTIONAL) /* NULL if deleting label */
{
    BOOL Ret;
    UNICODE_STRING VolumeNameU;
    PUNICODE_STRING RootPathNameU;

    if (lpRootPathName == NULL)
    {
        lpRootPathName = "\\";
    }

    RootPathNameU = Basep8BitStringToStaticUnicodeString(lpRootPathName);
    if (RootPathNameU == NULL)
    {
        return FALSE;
    }

    if (lpVolumeName != NULL)
    {
        if (!Basep8BitStringToDynamicUnicodeString(&VolumeNameU, lpVolumeName))
        {
            return FALSE;
        }
    }
    else
    {
        VolumeNameU.Buffer = NULL;
    }

    Ret = SetVolumeLabelW(RootPathNameU->Buffer, VolumeNameU.Buffer);
    RtlFreeUnicodeString(&VolumeNameU);
    return Ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetVolumeLabelW(IN LPCWSTR lpRootPathName,
                IN LPCWSTR lpVolumeName OPTIONAL) /* NULL if deleting label */
{
    BOOL Ret;
    NTSTATUS Status;
    PWSTR VolumeRoot;
    HANDLE VolumeHandle;
    WCHAR VolumeGuid[MAX_PATH];
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PFILE_FS_LABEL_INFORMATION FsLabelInfo;
    UNICODE_STRING VolumeName, NtVolumeName;

    /* If no root path provided, default to \ */
    VolumeRoot = L"\\";

    /* If user wants to set a label, make it a string */
    if (lpVolumeName != NULL)
    {
        RtlInitUnicodeString(&VolumeName, lpVolumeName);
    }
    else
    {
        VolumeName.Length = 0;
        VolumeName.MaximumLength = 0;
        VolumeName.Buffer = NULL;
    }

    /* If we received a volume, try to get its GUID name */
    if (lpRootPathName != NULL)
    {
        Ret = GetVolumeNameForVolumeMountPointW(lpRootPathName, VolumeGuid, MAX_PATH);
    }
    else
    {
        Ret = FALSE;
    }

    /* If we got the GUID name, use it */
    if (Ret)
    {
        VolumeRoot = VolumeGuid;
    }
    else
    {
        /* Otherwise, use the name provided by the caller */
        if (lpRootPathName != NULL)
        {
            VolumeRoot = (PWSTR)lpRootPathName;
        }
    }

    /* Convert to a NT path */
    if (!RtlDosPathNameToNtPathName_U(VolumeRoot, &NtVolumeName, NULL, NULL))
    {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }


    /* Check we really end with a backslash */
    if (NtVolumeName.Buffer[(NtVolumeName.Length / sizeof(WCHAR)) - 1] != L'\\')
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, NtVolumeName.Buffer);
        BaseSetLastNTError(STATUS_OBJECT_NAME_INVALID);
        return FALSE;
    }

    /* Try to open the root directory */
    InitializeObjectAttributes(&ObjectAttributes, &NtVolumeName,
                               OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenFile(&VolumeHandle, SYNCHRONIZE | FILE_WRITE_DATA,
                        &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, NtVolumeName.Buffer);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Validate it's really a root path */
    if (!IsThisARootDirectory(VolumeHandle, NULL))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, NtVolumeName.Buffer);
        NtClose(VolumeHandle);
        SetLastError(ERROR_DIR_NOT_ROOT);
        return FALSE;
    }

    /* Done */
    NtClose(VolumeHandle);

    /* Now, open the volume to perform the label change */
    NtVolumeName.Length -= sizeof(WCHAR);
    InitializeObjectAttributes(&ObjectAttributes, &NtVolumeName,
                               OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenFile(&VolumeHandle, SYNCHRONIZE | FILE_WRITE_DATA,
                        &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);

    RtlFreeHeap(RtlGetProcessHeap(), 0, NtVolumeName.Buffer);

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Assume success */
    Ret = TRUE;

    /* Allocate a buffer that can hold new label and its size */
    FsLabelInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(FILE_FS_LABEL_INFORMATION) + VolumeName.Length);
    if (FsLabelInfo != NULL)
    {
        /* Copy name and set its size */
        RtlCopyMemory(FsLabelInfo->VolumeLabel, VolumeName.Buffer, VolumeName.Length);
        FsLabelInfo->VolumeLabelLength = VolumeName.Length;

        /* And finally, set new label */
        Status = NtSetVolumeInformationFile(VolumeHandle, &IoStatusBlock, FsLabelInfo, sizeof(FILE_FS_LABEL_INFORMATION) + VolumeName.Length, FileFsLabelInformation);
    }
    else
    {
        /* Allocation failed */
        Status = STATUS_NO_MEMORY;
    }

    /* In case of failure, set status and mark failure */
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        Ret = FALSE;
    }

    /* We're done */
    NtClose(VolumeHandle);

    /* Free buffer if required */
    if (FsLabelInfo != NULL)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, FsLabelInfo);
    }

    return Ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
SetFileShortNameW(
    HANDLE hFile,
    LPCWSTR lpShortName)
{
    NTSTATUS Status;
    ULONG NeededSize;
    UNICODE_STRING ShortName;
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_NAME_INFORMATION FileNameInfo;

    if(IsConsoleHandle(hFile))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if(!lpShortName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlInitUnicodeString(&ShortName, lpShortName);

    NeededSize = sizeof(FILE_NAME_INFORMATION) + ShortName.Length + sizeof(WCHAR);
    if(!(FileNameInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, NeededSize)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    FileNameInfo->FileNameLength = ShortName.Length;
    RtlCopyMemory(FileNameInfo->FileName, ShortName.Buffer, ShortName.Length);

    Status = NtSetInformationFile(hFile,
                                  &IoStatusBlock, //out
                                  FileNameInfo,
                                  NeededSize,
                                  FileShortNameInformation);

    RtlFreeHeap(RtlGetProcessHeap(), 0, FileNameInfo);
    if(!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetFileShortNameA(
    HANDLE hFile,
    LPCSTR lpShortName)
{
    PWCHAR ShortNameW;

    if(IsConsoleHandle(hFile))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if(!lpShortName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(ShortNameW = FilenameA2W(lpShortName, FALSE)))
        return FALSE;

    return SetFileShortNameW(hFile, ShortNameW);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNamedPipeHandleStateA(HANDLE hNamedPipe,
                         LPDWORD lpState,
                         LPDWORD lpCurInstances,
                         LPDWORD lpMaxCollectionCount,
                         LPDWORD lpCollectDataTimeout,
                         LPSTR lpUserName,
                         DWORD nMaxUserNameSize)
{
    UNICODE_STRING UserNameW = { 0, 0, NULL };
    ANSI_STRING UserNameA;
    BOOL Ret;

    if (lpUserName != NULL)
    {
        UserNameW.MaximumLength = (USHORT)nMaxUserNameSize * sizeof(WCHAR);
        UserNameW.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, UserNameW.MaximumLength);
        if (UserNameW.Buffer == NULL)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }

        UserNameA.Buffer = lpUserName;
        UserNameA.Length = 0;
        UserNameA.MaximumLength = (USHORT)nMaxUserNameSize;
    }

    Ret = GetNamedPipeHandleStateW(hNamedPipe,
                                   lpState,
                                   lpCurInstances,
                                   lpMaxCollectionCount,
                                   lpCollectDataTimeout,
                                   UserNameW.Buffer,
                                   nMaxUserNameSize);
    if (Ret && lpUserName != NULL)
    {
        NTSTATUS Status;

        RtlInitUnicodeString(&UserNameW, UserNameW.Buffer);
        Status = RtlUnicodeStringToAnsiString(&UserNameA, &UserNameW, FALSE);
        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            Ret = FALSE;
        }
    }

    if (UserNameW.Buffer != NULL)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, UserNameW.Buffer);
    }

    return Ret;
}

PWCHAR
FilenameA2W(LPCSTR NameA, BOOL alloc)
{
   ANSI_STRING str;
   UNICODE_STRING strW;
   PUNICODE_STRING pstrW;
   NTSTATUS Status;

   //ASSERT(NtCurrentTeb()->StaticUnicodeString.Buffer == NtCurrentTeb()->StaticUnicodeBuffer);
   ASSERT(NtCurrentTeb()->StaticUnicodeString.MaximumLength == sizeof(NtCurrentTeb()->StaticUnicodeBuffer));

   RtlInitAnsiString(&str, NameA);
   pstrW = alloc ? &strW : &NtCurrentTeb()->StaticUnicodeString;

   if (AreFileApisANSI())
        Status= RtlAnsiStringToUnicodeString( pstrW, &str, (BOOLEAN)alloc );
   else
        Status= RtlOemStringToUnicodeString( pstrW, &str, (BOOLEAN)alloc );

    if (NT_SUCCESS(Status))
       return pstrW->Buffer;

    if (Status== STATUS_BUFFER_OVERFLOW)
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
    else
        BaseSetLastNTError(Status);

    return NULL;
}
