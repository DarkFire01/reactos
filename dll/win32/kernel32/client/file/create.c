/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/create.c
 * PURPOSE:         Directory functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 * UPDATE HISTORY:
 *                  Created 01/11/98
 *                  Removed use of SearchPath (not used by Windows)
 *                  18/08/2002: CreateFileW mess cleaned up (KJK::Hyperion)
 *                  24/08/2002: removed superfluous DPRINTs (KJK::Hyperion)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>
#define NDEBUG
#include <debug.h>

#if DBG
DEBUG_CHANNEL(kernel32file);
#endif

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
HFILE WINAPI
OpenFile(LPCSTR lpFileName,
	 LPOFSTRUCT lpReOpenBuff,
	 UINT uStyle)
{
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK IoStatusBlock;
	UNICODE_STRING FileNameString;
	UNICODE_STRING FileNameU;
	ANSI_STRING FileName;
	WCHAR PathNameW[MAX_PATH];
	HANDLE FileHandle = NULL;
	NTSTATUS errCode;
	PWCHAR FilePart;
	ULONG Len;

	TRACE("OpenFile('%s', lpReOpenBuff %p, uStyle %x)\n", lpFileName, lpReOpenBuff, uStyle);

	if (lpReOpenBuff == NULL)
	{
		return HFILE_ERROR;
	}

    lpReOpenBuff->nErrCode = 0;

	if (uStyle & OF_REOPEN) lpFileName = lpReOpenBuff->szPathName;

	if (!lpFileName)
	{
		return HFILE_ERROR;
	}

	if (!GetFullPathNameA(lpFileName,
						  sizeof(lpReOpenBuff->szPathName),
						  lpReOpenBuff->szPathName,
						  NULL))
	{
	    lpReOpenBuff->nErrCode = (WORD)GetLastError();
		return HFILE_ERROR;
	}

    if (uStyle & OF_PARSE)
    {
        lpReOpenBuff->fFixedDisk = (GetDriveTypeA(lpReOpenBuff->szPathName) != DRIVE_REMOVABLE);
        TRACE("(%s): OF_PARSE, res = '%s'\n", lpFileName, lpReOpenBuff->szPathName);
        return 0;
    }

    if ((uStyle & OF_EXIST) && !(uStyle & OF_CREATE))
    {
        DWORD dwAttributes = GetFileAttributesA(lpReOpenBuff->szPathName);

        switch (dwAttributes)
        {
            case INVALID_FILE_ATTRIBUTES: /* File does not exist */
                SetLastError(ERROR_FILE_NOT_FOUND);
                lpReOpenBuff->nErrCode = (WORD) ERROR_FILE_NOT_FOUND;
                return -1;

            case FILE_ATTRIBUTE_DIRECTORY:
                SetLastError(ERROR_ACCESS_DENIED);
                lpReOpenBuff->nErrCode = (WORD) ERROR_ACCESS_DENIED;
                return -1;

            default:
                lpReOpenBuff->cBytes = sizeof(OFSTRUCT);
                return 1;
        }
    }
    lpReOpenBuff->cBytes = sizeof(OFSTRUCT);
	if ((uStyle & OF_CREATE) == OF_CREATE)
	{
		DWORD Sharing;
		switch (uStyle & 0x70)
		{
			case OF_SHARE_EXCLUSIVE: Sharing = 0; break;
			case OF_SHARE_DENY_WRITE: Sharing = FILE_SHARE_READ; break;
			case OF_SHARE_DENY_READ: Sharing = FILE_SHARE_WRITE; break;
			case OF_SHARE_DENY_NONE:
			case OF_SHARE_COMPAT:
			default:
				Sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
		}
		return (HFILE)(ULONG_PTR) CreateFileA (lpFileName,
		                            GENERIC_READ | GENERIC_WRITE,
		                            Sharing,
		                            NULL,
		                            CREATE_ALWAYS,
		                            FILE_ATTRIBUTE_NORMAL,
		                            0);
	}

	RtlInitAnsiString (&FileName, (LPSTR)lpFileName);

	/* convert ansi (or oem) string to unicode */
	if (bIsFileApiAnsi)
		RtlAnsiStringToUnicodeString (&FileNameU, &FileName, TRUE);
	else
		RtlOemStringToUnicodeString (&FileNameU, &FileName, TRUE);

	Len = SearchPathW (NULL,
	                   FileNameU.Buffer,
        	           NULL,
	                   OFS_MAXPATHNAME,
	                   PathNameW,
        	           &FilePart);

	RtlFreeUnicodeString(&FileNameU);

	if (Len == 0 || Len > OFS_MAXPATHNAME)
	{
		lpReOpenBuff->nErrCode = (WORD)GetLastError();
		return HFILE_ERROR;
	}

    if (uStyle & OF_DELETE)
    {
        if (!DeleteFileW(PathNameW))
		{
			lpReOpenBuff->nErrCode = (WORD)GetLastError();
			return HFILE_ERROR;
		}
        TRACE("(%s): OF_DELETE return = OK\n", lpFileName);
        return TRUE;
    }

	FileName.Buffer = lpReOpenBuff->szPathName;
	FileName.Length = 0;
	FileName.MaximumLength = OFS_MAXPATHNAME;

	RtlInitUnicodeString(&FileNameU, PathNameW);

	/* convert unicode string to ansi (or oem) */
	if (bIsFileApiAnsi)
		RtlUnicodeStringToAnsiString (&FileName, &FileNameU, FALSE);
	else
		RtlUnicodeStringToOemString (&FileName, &FileNameU, FALSE);

	if (!RtlDosPathNameToNtPathName_U (PathNameW,
					   &FileNameString,
					   NULL,
					   NULL))
	{
		return HFILE_ERROR;
	}

	// FILE_SHARE_READ
	// FILE_NO_INTERMEDIATE_BUFFERING

    InitializeObjectAttributes(&ObjectAttributes,
                               &FileNameString,
                               OBJ_CASE_INSENSITIVE | OBJ_INHERIT,
                               NULL,
                               NULL);

	errCode = NtOpenFile (&FileHandle,
	                      GENERIC_READ | SYNCHRONIZE,
	                      &ObjectAttributes,
	                      &IoStatusBlock,
	                      FILE_SHARE_READ,
	                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);

	RtlFreeHeap(RtlGetProcessHeap(), 0, FileNameString.Buffer);

	lpReOpenBuff->nErrCode = (WORD)RtlNtStatusToDosError(errCode);

	if (!NT_SUCCESS(errCode))
	{
		BaseSetLastNTError (errCode);
		return HFILE_ERROR;
	}

	if (uStyle & OF_EXIST)
	{
		NtClose(FileHandle);
		return (HFILE)1;
	}

	return (HFILE)(ULONG_PTR)FileHandle;
}