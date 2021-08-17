/*
 * COPYRIGHT: See COPYING in the top level directory
 * PROJECT  : ReactOS system libraries
 * MODULE   : kernel32.dll
 * FILE     : reactos/dll/win32/kernel32/misc/ldr.c
 * AUTHOR   : Aleksey Bragin <aleksey@reactos.org>
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

NTSTATUS
WINAPI
BasepInitializeTermsrvFpns(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

DWORD
WINAPI
BasepGetModuleHandleExParameterValidation(DWORD dwFlags,
                                          LPCWSTR lpwModuleName,
                                          HMODULE *phModule)
{
    /* Set phModule to 0 if it's not a NULL pointer */
    if (phModule) *phModule = 0;

    /* Check for invalid flags combination */
    if (dwFlags & ~(GET_MODULE_HANDLE_EX_FLAG_PIN |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ||
        ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
         (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) ||
         (!lpwModuleName && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
        )
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER_1);
        return BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_ERROR;
    }

    /* Check 2nd parameter */
    if (!phModule)
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER_2);
        return BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_ERROR;
    }

    /* Return what we have according to the module name */
    if (lpwModuleName)
    {
        return BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_CONTINUE;
    }

    /* No name given, so put ImageBaseAddress there */
    *phModule = (HMODULE)NtCurrentPeb()->ImageBaseAddress;

    return BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_SUCCESS;
}

PVOID
WINAPI
BasepMapModuleHandle(HMODULE hModule, BOOLEAN AsDataFile)
{
    /* If no handle is provided - use current image base address */
    if (!hModule) return NtCurrentPeb()->ImageBaseAddress;

    /* Check if it's a normal or a datafile one */
    if (LDR_IS_DATAFILE(hModule) && !AsDataFile)
        return NULL;

    /* It's a normal DLL, just return its handle */
    return hModule;
}

/*
 * @implemented
 */
BOOL
WINAPI
DisableThreadLibraryCalls(
    IN HMODULE hLibModule)
{
    NTSTATUS Status;

    /* Disable thread library calls */
    Status = LdrDisableThreadCalloutsForDll((PVOID)hLibModule);

    /* If it wasn't success - set last error and return failure */
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}


/*
 * @implemented
 */
HINSTANCE
WINAPI
DECLSPEC_HOTPATCH
LoadLibraryA(LPCSTR lpLibFileName)
{
    static const CHAR TwainDllName[] = "twain_32.dll";
    LPSTR PathBuffer;
    UINT Len;
    HINSTANCE Result;

    /* Treat twain_32.dll in a special way (what a surprise...) */
    if (lpLibFileName && !_strcmpi(lpLibFileName, TwainDllName))
    {
        /* Allocate space for the buffer */
        PathBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, MAX_PATH + sizeof(ANSI_NULL));
        if (PathBuffer)
        {
            /* Get windows dir in this buffer */
            Len = GetWindowsDirectoryA(PathBuffer, MAX_PATH);
            if ((Len != 0) && (Len < (MAX_PATH - sizeof(TwainDllName) - sizeof('\\'))))
            {
                /* We successfully got windows directory. Concatenate twain_32.dll to it */
                PathBuffer[Len] = '\\';
                strcpy(&PathBuffer[Len + 1], TwainDllName);

                /* And recursively call ourselves with a new string */
                Result = LoadLibraryA(PathBuffer);

                /* If it was successful -  free memory and return result */
                if (Result)
                {
                    RtlFreeHeap(RtlGetProcessHeap(), 0, PathBuffer);
                    return Result;
                }
            }

            /* Free allocated buffer */
            RtlFreeHeap(RtlGetProcessHeap(), 0, PathBuffer);
        }
    }

    /* Call the Ex version of the API */
    return LoadLibraryExA(lpLibFileName, 0, 0);
}

/*
 * @implemented
 */
HINSTANCE
WINAPI
DECLSPEC_HOTPATCH
LoadLibraryExA(LPCSTR lpLibFileName,
               HANDLE hFile,
               DWORD dwFlags)
{
    PUNICODE_STRING FileNameW;

    /* Convert file name to unicode */
    if (!(FileNameW = Basep8BitStringToStaticUnicodeString(lpLibFileName)))
        return NULL;

    /* And call W version of the API */
    return LoadLibraryExW(FileNameW->Buffer, hFile, dwFlags);
}

/*
 * @implemented
 */
HINSTANCE
WINAPI
DECLSPEC_HOTPATCH
LoadLibraryW(LPCWSTR lpLibFileName)
{
    /* Call Ex version of the API */
    return LoadLibraryExW(lpLibFileName, 0, 0);
}


static
NTSTATUS
BasepLoadLibraryAsDatafile(PWSTR Path, LPCWSTR Name, HMODULE *hModule)
{
    WCHAR FilenameW[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping;
    NTSTATUS Status;
    PVOID lpBaseAddress = NULL;
    SIZE_T ViewSize = 0;
    //PUNICODE_STRING OriginalName;
    //UNICODE_STRING dotDLL = RTL_CONSTANT_STRING(L".DLL");

    /* Zero out handle value */
    *hModule = 0;

    DPRINT("BasepLoadLibraryAsDatafile(%S %S %p)\n", Path, Name, hModule);

    /*Status = RtlDosApplyFileIsolationRedirection_Ustr(TRUE,
                                                      Name,
                                                      &dotDLL,
                                                      RedirName,
                                                      RedirName2,
                                                      &OriginalName2,
                                                      NULL,
                                                      NULL,
                                                      NULL);*/

    /* Try to search for it */
    if (!SearchPathW(Path,
                     Name,
                     L".DLL",
                     sizeof(FilenameW) / sizeof(FilenameW[0]),
                     FilenameW,
                     NULL))
    {
        /* Return last status value directly */
        return NtCurrentTeb()->LastStatusValue;
    }

    /* Open this file we found */
    hFile = CreateFileW(FilenameW,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        0);

    /* If opening failed - return last status value */
    if (hFile == INVALID_HANDLE_VALUE) return NtCurrentTeb()->LastStatusValue;

    /* Create file mapping */
    hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);

    /* Close the file handle */
    CloseHandle(hFile);

    /* If creating file mapping failed - return last status value */
    if (!hMapping) return NtCurrentTeb()->LastStatusValue;

    /* Map view of section */
    Status = NtMapViewOfSection(hMapping,
                                NtCurrentProcess(),
                                &lpBaseAddress,
                                0,
                                0,
                                0,
                                &ViewSize,
                                ViewShare,
                                0,
                                PAGE_READONLY);

    /* Close handle to the section */
    CloseHandle(hMapping);

    /* If mapping view of section failed - return last status value */
    if (!NT_SUCCESS(Status)) return NtCurrentTeb()->LastStatusValue;

    /* Make sure it's a valid PE file */
    if (!RtlImageNtHeader(lpBaseAddress))
    {
        /* Unmap the view and return failure status */
        UnmapViewOfFile(lpBaseAddress);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* Set low bit of handle to indicate datafile module */
    *hModule = (HMODULE)((ULONG_PTR)lpBaseAddress | 1);

    /* Load alternate resource module */
    //LdrLoadAlternateResourceModule(*hModule, FilenameW);

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
HINSTANCE
WINAPI
DECLSPEC_HOTPATCH
LoadLibraryExW(LPCWSTR lpLibFileName,
               HANDLE hFile,
               DWORD dwFlags)
{
    UNICODE_STRING DllName;
    HINSTANCE hInst;
    NTSTATUS Status;
    PWSTR SearchPath;
    ULONG DllCharacteristics = 0;
    BOOL FreeString = FALSE;

    /* Check for any flags LdrLoadDll might be interested in */
    if (dwFlags & DONT_RESOLVE_DLL_REFERENCES)
    {
        /* Tell LDR to treat it as an EXE */
        DllCharacteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
    }

    /* Build up a unicode dll name from null-terminated string */
    RtlInitUnicodeString(&DllName, (LPWSTR)lpLibFileName);

    /* Lazy-initialize BasepExeLdrEntry */
    if (!BasepExeLdrEntry)
        LdrEnumerateLoadedModules(0, BasepLocateExeLdrEntry, NtCurrentPeb()->ImageBaseAddress);

    /* Check if that module is our exe*/
    if (BasepExeLdrEntry && !(dwFlags & LOAD_LIBRARY_AS_DATAFILE) &&
        DllName.Length == BasepExeLdrEntry->FullDllName.Length)
    {
        /* Lengths match and it's not a datafile, so perform name comparison */
        if (RtlEqualUnicodeString(&DllName, &BasepExeLdrEntry->FullDllName, TRUE))
        {
            /* That's us! */
            return BasepExeLdrEntry->DllBase;
        }
    }

    /* Check for trailing spaces and remove them if necessary */
    if (DllName.Buffer[DllName.Length/sizeof(WCHAR) - 1] == L' ')
    {
        RtlCreateUnicodeString(&DllName, (LPWSTR)lpLibFileName);
        while (DllName.Length > sizeof(WCHAR) &&
            DllName.Buffer[DllName.Length/sizeof(WCHAR) - 1] == L' ')
        {
            DllName.Length -= sizeof(WCHAR);
        }
        DllName.Buffer[DllName.Length/sizeof(WCHAR)] = UNICODE_NULL;
        FreeString = TRUE;
    }

    /* Compute the load path */
    SearchPath = BaseComputeProcessDllPath((dwFlags & LOAD_WITH_ALTERED_SEARCH_PATH) ?
                                           DllName.Buffer : NULL,
                                           NULL);
    if (!SearchPath)
    {
        /* Getting DLL path failed, so set last error, free mem and return */
        BaseSetLastNTError(STATUS_NO_MEMORY);
        if (FreeString) RtlFreeUnicodeString(&DllName);
        return NULL;
    }

    _SEH2_TRY
    {
        if (dwFlags & LOAD_LIBRARY_AS_DATAFILE)
        {
            /* If the image is loaded as a datafile, try to get its handle */
            Status = LdrGetDllHandleEx(0, SearchPath, NULL, &DllName, (PVOID*)&hInst);
            if (!NT_SUCCESS(Status))
            {
                /* It's not loaded yet - so load it up */
                Status = BasepLoadLibraryAsDatafile(SearchPath, DllName.Buffer, &hInst);
            }
            _SEH2_YIELD(goto done;)
        }

        /* Call the API Properly */
        Status = LdrLoadDll(SearchPath,
                            &DllCharacteristics,
                            &DllName,
                            (PVOID*)&hInst);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    } _SEH2_END;


done:
    /* Free SearchPath buffer */
    RtlFreeHeap(RtlGetProcessHeap(), 0, SearchPath);

    /* Free DllName string if it was dynamically allocated */
    if (FreeString) RtlFreeUnicodeString(&DllName);

    /* Set last error in failure case */
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LoadLibraryExW(%ls) failing with status %lx\n", lpLibFileName, Status);
        BaseSetLastNTError(Status);
        return NULL;
    }

    /* Return loaded module handle */
    return hInst;
}


/*
 * @implemented
 */
FARPROC
WINAPI
GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    ANSI_STRING ProcedureName, *ProcNamePtr = NULL;
    FARPROC fnExp = NULL;
    NTSTATUS Status;
    PVOID hMapped;
    ULONG Ordinal = 0;

    if ((ULONG_PTR)lpProcName > MAXUSHORT)
    {
        /* Look up by name */
        RtlInitAnsiString(&ProcedureName, (LPSTR)lpProcName);
        ProcNamePtr = &ProcedureName;
    }
    else
    {
        /* Look up by ordinal */
        Ordinal = PtrToUlong(lpProcName);
    }

    /* Map provided handle */
    hMapped = BasepMapModuleHandle(hModule, FALSE);

    /* Get the proc address */
    Status = LdrGetProcedureAddress(hMapped,
                                    ProcNamePtr,
                                    Ordinal,
                                    (PVOID*)&fnExp);

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    /* Check for a special case when returned pointer is
       the same as image's base address */
    if (fnExp == hMapped)
    {
        /* Set correct error code */
        if (HIWORD(lpProcName) != 0)
            BaseSetLastNTError(STATUS_ENTRYPOINT_NOT_FOUND);
        else
            BaseSetLastNTError(STATUS_ORDINAL_NOT_FOUND);

        return NULL;
    }

    /* All good, return procedure pointer */
    return fnExp;
}


/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
FreeLibrary(HINSTANCE hLibModule)
{
    NTSTATUS Status;
    PIMAGE_NT_HEADERS NtHeaders;

    if (LDR_IS_DATAFILE(hLibModule))
    {
        /* This is a LOAD_LIBRARY_AS_DATAFILE module, check if it's a valid one */
        NtHeaders = RtlImageNtHeader((PVOID)((ULONG_PTR)hLibModule & ~1));

        if (NtHeaders)
        {
            /* Unmap view */
            Status = NtUnmapViewOfSection(NtCurrentProcess(), (PVOID)((ULONG_PTR)hLibModule & ~1));

            /* Unload alternate resource module */
            LdrUnloadAlternateResourceModule(hLibModule);
        }
        else
            Status = STATUS_INVALID_IMAGE_FORMAT;
    }
    else
    {
        /* Just unload it */
        Status = LdrUnloadDll((PVOID)hLibModule);
    }

    /* Check what kind of status we got */
    if (!NT_SUCCESS(Status))
    {
        /* Set last error */
        BaseSetLastNTError(Status);

        /* Return failure */
        return FALSE;
    }

    /* Return success */
    return TRUE;
}


/*
 * @implemented
 */
VOID
WINAPI
FreeLibraryAndExitThread(HMODULE hLibModule,
                         DWORD dwExitCode)
{

    if (LDR_IS_DATAFILE(hLibModule))
    {
        /* This is a LOAD_LIBRARY_AS_DATAFILE module */
        if (RtlImageNtHeader((PVOID)((ULONG_PTR)hLibModule & ~1)))
        {
            /* Unmap view */
            NtUnmapViewOfSection(NtCurrentProcess(), (PVOID)((ULONG_PTR)hLibModule & ~1));

            /* Unload alternate resource module */
            LdrUnloadAlternateResourceModule(hLibModule);
        }
    }
    else
    {
        /* Just unload it */
        LdrUnloadDll((PVOID)hLibModule);
    }

    /* Exit thread */
    ExitThread(dwExitCode);
}


/*
 * @implemented
 */
DWORD
WINAPI
GetModuleFileNameA(HINSTANCE hModule,
                   LPSTR lpFilename,
                   DWORD nSize)
{
    UNICODE_STRING FilenameW;
    ANSI_STRING FilenameA;
    NTSTATUS Status;
    DWORD Length = 0, LengthToCopy;

    /* Allocate a unicode buffer */
    FilenameW.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, nSize * sizeof(WCHAR));
    if (!FilenameW.Buffer)
    {
        BaseSetLastNTError(STATUS_NO_MEMORY);
        return 0;
    }

    /* Call unicode API */
    FilenameW.Length = (USHORT)GetModuleFileNameW(hModule, FilenameW.Buffer, nSize) * sizeof(WCHAR);
    FilenameW.MaximumLength = FilenameW.Length + sizeof(WCHAR);

    if (FilenameW.Length)
    {
        /* Convert to ansi string */
        Status = BasepUnicodeStringTo8BitString(&FilenameA, &FilenameW, TRUE);
        if (!NT_SUCCESS(Status))
        {
            /* Set last error, free string and return failure */
            BaseSetLastNTError(Status);
            RtlFreeUnicodeString(&FilenameW);
            return 0;
        }

        /* Calculate size to copy */
        Length = min(nSize, FilenameA.Length);

        /* Include terminating zero */
        if (nSize > Length)
            LengthToCopy = Length + 1;
        else
            LengthToCopy = nSize;

        /* Now copy back to the caller amount he asked */
        RtlMoveMemory(lpFilename, FilenameA.Buffer, LengthToCopy);

        /* Free ansi filename */
        RtlFreeAnsiString(&FilenameA);
    }

    /* Free unicode filename */
    RtlFreeHeap(RtlGetProcessHeap(), 0, FilenameW.Buffer);

    /* Return length copied */
    return Length;
}

/*
 * @implemented
 */
DWORD
WINAPI
GetModuleFileNameW(HINSTANCE hModule,
                   LPWSTR lpFilename,
                   DWORD nSize)
{
    PLIST_ENTRY ModuleListHead, Entry;
    PLDR_DATA_TABLE_ENTRY Module;
    ULONG Length = 0;
    ULONG_PTR Cookie;
    PPEB Peb;

    hModule = BasepMapModuleHandle(hModule, FALSE);

    /* Upscale nSize from chars to bytes */
    nSize *= sizeof(WCHAR);

    _SEH2_TRY
    {
        /* We don't use per-thread cur dir now */
        //PRTL_PERTHREAD_CURDIR PerThreadCurdir = (PRTL_PERTHREAD_CURDIR)teb->NtTib.SubSystemTib;

        Peb = NtCurrentPeb ();

        /* Acquire a loader lock */
        LdrLockLoaderLock(LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS, NULL, &Cookie);

        /* Traverse the module list */
        ModuleListHead = &Peb->Ldr->InLoadOrderModuleList;
        Entry = ModuleListHead->Flink;
        while (Entry != ModuleListHead)
        {
            Module = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

            /* Check if this is the requested module */
            if (Module->DllBase == (PVOID)hModule)
            {
                /* Calculate size to copy */
                Length = min(nSize, Module->FullDllName.MaximumLength);

                /* Copy contents */
                RtlMoveMemory(lpFilename, Module->FullDllName.Buffer, Length);

                /* Subtract a terminating zero */
                if (Length == Module->FullDllName.MaximumLength)
                    Length -= sizeof(WCHAR);

                /* Break out of the loop */
                break;
            }

            /* Advance to the next entry */
            Entry = Entry->Flink;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        BaseSetLastNTError(_SEH2_GetExceptionCode());
        Length = 0;
    } _SEH2_END

    /* Release the loader lock */
    LdrUnlockLoaderLock(LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS, Cookie);

    return Length / sizeof(WCHAR);
}

HMODULE
WINAPI
GetModuleHandleForUnicodeString(PUNICODE_STRING ModuleName)
{
    NTSTATUS Status;
    PVOID Module;
    LPWSTR DllPath;

    /* Try to get a handle with a magic value of 1 for DllPath */
    Status = LdrGetDllHandle((LPWSTR)1, NULL, ModuleName, &Module);

    /* If that succeeded - we're done */
    if (NT_SUCCESS(Status)) return Module;

    /* If not, then the path should be computed */
    DllPath = BaseComputeProcessDllPath(NULL, 0);
    if (!DllPath)
    {
        Status = STATUS_NO_MEMORY;
    }
    else
    {
        _SEH2_TRY
        {
            Status = LdrGetDllHandle(DllPath, NULL, ModuleName, &Module);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Fail with the SEH error */
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    /* Free the DllPath */
    RtlFreeHeap(RtlGetProcessHeap(), 0, DllPath);

    /* In case of error set last win32 error and return NULL */
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Failure acquiring DLL module '%wZ' handle, Status 0x%08X\n", ModuleName, Status);
        BaseSetLastNTError(Status);
        Module = 0;
    }

    /* Return module */
    return (HMODULE)Module;
}

BOOLEAN
WINAPI
BasepGetModuleHandleExW(BOOLEAN NoLock, DWORD dwPublicFlags, LPCWSTR lpwModuleName, HMODULE *phModule)
{
    ULONG_PTR Cookie;
    NTSTATUS Status = STATUS_SUCCESS, Status2;
    HANDLE hModule = NULL;
    UNICODE_STRING ModuleNameU;
    DWORD dwValid;
    BOOLEAN Redirected = FALSE; // FIXME

    /* Validate parameters */
    dwValid = BasepGetModuleHandleExParameterValidation(dwPublicFlags, lpwModuleName, phModule);
    ASSERT(dwValid == BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_CONTINUE);

    /* Acquire lock if necessary */
    if (!NoLock)
    {
        Status = LdrLockLoaderLock(0, NULL, &Cookie);
        if (!NT_SUCCESS(Status))
        {
            /* Fail */
            BaseSetLastNTError(Status);
            if (phModule) *phModule = NULL;
            return NT_SUCCESS(Status);
        }
    }

    if (!(dwPublicFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
    {
        /* Create a unicode string out of module name */
        RtlInitUnicodeString(&ModuleNameU, lpwModuleName);

        // FIXME: Do some redirected DLL stuff?
        if (Redirected)
        {
            UNIMPLEMENTED;
        }

        if (!hModule)
        {
            hModule = GetModuleHandleForUnicodeString(&ModuleNameU);
            if (!hModule)
            {
                /* Last error is already set, so just return failure by setting status */
                Status = STATUS_DLL_NOT_FOUND;
                goto quickie;
            }
        }
    }
    else
    {
        /* Perform Pc to file header to get module instance */
        hModule = (HMODULE)RtlPcToFileHeader((PVOID)lpwModuleName,
                                             (PVOID*)&hModule);

        /* Check if it succeeded */
        if (!hModule)
        {
            /* Set "dll not found" status and quit */
            Status = STATUS_DLL_NOT_FOUND;
            goto quickie;
        }
    }

    /* Check if changing reference is not forbidden */
    if (!(dwPublicFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
    {
        /* Add reference to this DLL */
        Status = LdrAddRefDll((dwPublicFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) ? LDR_ADDREF_DLL_PIN : 0,
                              hModule);
    }

quickie:
    /* Set last error in case of failure */
    if (!NT_SUCCESS(Status))
        BaseSetLastNTError(Status);

    /* Unlock loader lock if it was acquired */
    if (!NoLock)
    {
        Status2 = LdrUnlockLoaderLock(0, Cookie);
        ASSERT(NT_SUCCESS(Status2));
    }

    /* Set the module handle to the caller */
    if (phModule) *phModule = hModule;

    /* Return TRUE on success and FALSE otherwise */
    return NT_SUCCESS(Status);
}

/*
 * @implemented
 */
HMODULE
WINAPI
DECLSPEC_HOTPATCH
GetModuleHandleA(LPCSTR lpModuleName)
{
    PUNICODE_STRING ModuleNameW;
    PTEB pTeb = NtCurrentTeb();

    /* Check if we have no name to convert */
    if (!lpModuleName)
        return ((HMODULE)pTeb->ProcessEnvironmentBlock->ImageBaseAddress);

    /* Convert module name to unicode */
    ModuleNameW = Basep8BitStringToStaticUnicodeString(lpModuleName);

    /* Call W version if conversion was successful */
    if (ModuleNameW)
        return GetModuleHandleW(ModuleNameW->Buffer);

    /* Return failure */
    return 0;
}


/*
 * @implemented
 */
HMODULE
WINAPI
GetModuleHandleW(LPCWSTR lpModuleName)
{
    HMODULE hModule;
    BOOLEAN Success;

    /* If current module is requested - return it right away */
    if (!lpModuleName)
        return ((HMODULE)NtCurrentPeb()->ImageBaseAddress);

    /* Use common helper routine */
    Success = BasepGetModuleHandleExW(TRUE,
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      lpModuleName,
                                      &hModule);

    /* If it wasn't successful - return NULL */
    if (!Success) hModule = NULL;

    /* Return the handle */
    return hModule;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetModuleHandleExW(IN DWORD dwFlags,
                   IN LPCWSTR lpwModuleName  OPTIONAL,
                   OUT HMODULE* phModule)
{
    DWORD dwValid;
    BOOL Ret;

    /* Validate parameters */
    dwValid = BasepGetModuleHandleExParameterValidation(dwFlags, lpwModuleName, phModule);

    /* If result is invalid parameter - return failure */
    if (dwValid == BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_ERROR) return FALSE;

    /* If result is 2, there is no need to do anything - return success. */
    if (dwValid == BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_SUCCESS) return TRUE;

    /* Use common helper routine */
    Ret = BasepGetModuleHandleExW(FALSE,
                                  dwFlags,
                                  lpwModuleName,
                                  phModule);

    return Ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetModuleHandleExA(IN DWORD dwFlags,
                   IN LPCSTR lpModuleName OPTIONAL,
                   OUT HMODULE* phModule)
{
    PUNICODE_STRING lpModuleNameW;
    DWORD dwValid;
    BOOL Ret;

    /* Validate parameters */
    dwValid = BasepGetModuleHandleExParameterValidation(dwFlags, (LPCWSTR)lpModuleName, phModule);

    /* If result is invalid parameter - return failure */
    if (dwValid == BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_ERROR) return FALSE;

    /* If result is 2, there is no need to do anything - return success. */
    if (dwValid == BASEP_GET_MODULE_HANDLE_EX_PARAMETER_VALIDATION_SUCCESS) return TRUE;

    /* Check if we don't need to convert the name */
    if (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
    {
        /* Call the extended version of the API without conversion */
        Ret = BasepGetModuleHandleExW(FALSE,
                                      dwFlags,
                                      (LPCWSTR)lpModuleName,
                                      phModule);
    }
    else
    {
        /* Convert module name to unicode */
        lpModuleNameW = Basep8BitStringToStaticUnicodeString(lpModuleName);

        /* Return FALSE if conversion failed */
        if (!lpModuleNameW) return FALSE;

        /* Call the extended version of the API */
        Ret = BasepGetModuleHandleExW(FALSE,
                                      dwFlags,
                                      lpModuleNameW->Buffer,
                                      phModule);
    }

    /* Return result */
    return Ret;
}


/*
 * @implemented
 */
DWORD
WINAPI
LoadModule(LPCSTR lpModuleName,
           LPVOID lpParameterBlock)
{
    STARTUPINFOA StartupInfo;
    PROCESS_INFORMATION ProcessInformation;
    LOADPARMS32 *LoadParams;
    char FileName[MAX_PATH];
    LPSTR CommandLine;
    DWORD Length, Error;
    BOOL ProcessStatus;
    ANSI_STRING AnsiStr;
    UNICODE_STRING UnicStr;
    RTL_PATH_TYPE PathType;
    HANDLE Handle;

    LoadParams = (LOADPARMS32*)lpParameterBlock;

    /* Check load parameters */
    if (LoadParams->dwReserved || LoadParams->wMagicValue != 2)
    {
        /* Fail with invalid param error */
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return 0;
    }

    /* Search path */
    Length = SearchPathA(NULL, lpModuleName, ".exe", MAX_PATH, FileName, NULL);

    /* Check if path was found */
    if (Length && Length < MAX_PATH)
    {
        /* Build StartupInfo */
        RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));

        StartupInfo.cb = sizeof(STARTUPINFOA);
        StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        StartupInfo.wShowWindow = LoadParams->wCmdShow;

        /* Allocate command line buffer */
        CommandLine = RtlAllocateHeap(RtlGetProcessHeap(),
                                      HEAP_ZERO_MEMORY,
                                      (ULONG)LoadParams->lpCmdLine[0] + Length + 2);

        /* Put module name there, then a space, and then copy provided command line,
           and null-terminate it */
        RtlCopyMemory(CommandLine, FileName, Length);
        CommandLine[Length] = ' ';
        RtlCopyMemory(&CommandLine[Length + 1], &LoadParams->lpCmdLine[1], (ULONG)LoadParams->lpCmdLine[0]);
        CommandLine[Length + 1 + (ULONG)LoadParams->lpCmdLine[0]] = 0;

        /* Create the process */
        ProcessStatus = CreateProcessA(FileName,
                                       CommandLine,
                                       NULL,
                                       NULL,
                                       FALSE,
                                       0,
                                       LoadParams->lpEnvAddress,
                                       NULL,
                                       &StartupInfo,
                                       &ProcessInformation);

        /* Free the command line buffer */
        RtlFreeHeap(RtlGetProcessHeap(), 0, CommandLine);

        if (!ProcessStatus)
        {
            /* Creating process failed, return right error code */
            Error = GetLastError();
            switch(Error)
            {
            case ERROR_BAD_EXE_FORMAT:
               return ERROR_BAD_FORMAT;

            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return Error;
            }

            /* Return 0 otherwise */
            return 0;
        }

        /* Wait up to 30 seconds for the process to become idle */
        if (UserWaitForInputIdleRoutine)
        {
            UserWaitForInputIdleRoutine(ProcessInformation.hProcess, 30000);
        }

        /* Close handles */
        NtClose(ProcessInformation.hThread);
        NtClose(ProcessInformation.hProcess);

        /* Return magic success value (33) */
        return 33;
    }

    /* The path was not found, create an ansi string from
        the module name and convert it to unicode */
    RtlInitAnsiString(&AnsiStr, lpModuleName);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&UnicStr,&AnsiStr,TRUE)))
        return ERROR_FILE_NOT_FOUND;

    /* Determine path type */
    PathType = RtlDetermineDosPathNameType_U(UnicStr.Buffer);

    /* Free the unicode module name */
    RtlFreeUnicodeString(&UnicStr);

    /* If it's a relative path, return file not found */
    if (PathType == RtlPathTypeRelative)
        return ERROR_FILE_NOT_FOUND;

    /* If not, try to open it */
    Handle = CreateFile(lpModuleName,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);

    if (Handle != INVALID_HANDLE_VALUE)
    {
        /* Opening file succeeded for some reason, close the handle and return file not found anyway */
        CloseHandle(Handle);
        return ERROR_FILE_NOT_FOUND;
    }

    /* Return last error which CreateFile set during an attempt to open it */
    return GetLastError();
}

/*
 * @unimplemented
 */
FARPROC WINAPI DelayLoadFailureHook(LPCSTR pszDllName, LPCSTR pszProcName)
{
    STUB;
    return NULL;
}

/*
 * @unimplemented
 */
BOOL WINAPI UTRegister( HMODULE hModule, LPSTR lpsz16BITDLL,
                        LPSTR lpszInitName, LPSTR lpszProcName,
                        FARPROC *ppfn32Thunk, FARPROC pfnUT32CallBack,
                        LPVOID lpBuff )
{
    STUB;
    return 0;
}

/*
 * @unimplemented
 */
VOID WINAPI UTUnRegister( HMODULE hModule )
{
    STUB;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
BaseQueryModuleData(IN LPSTR ModuleName,
                    IN LPSTR Unknown,
                    IN PVOID Unknown2,
                    IN PVOID Unknown3,
                    IN PVOID Unknown4)
{
    DPRINT1("BaseQueryModuleData called: %s %s %p %p %p\n",
            ModuleName,
            Unknown,
            Unknown2,
            Unknown3,
            Unknown4);
    return FALSE;
}

/*
 * @implemented
 */
NTSTATUS
WINAPI
BaseProcessInitPostImport(VOID)
{
    DPRINT("Post-init called\n");

    /* Check if this is a terminal server */
    if (SharedUserData->SuiteMask & VER_SUITE_TERMINAL)
    {
        /* Initialize TS pointers */
        return BasepInitializeTermsrvFpns();
    }

    /* FIXME: Initialize TS pointers */
    return STATUS_SUCCESS;
}


typedef struct _PEBB32
{
    BOOLEAN InheritedAddressSpace;
    BOOLEAN ReadImageFileExecOptions;
    BOOLEAN BeingDebugged;
    BOOLEAN SpareBool;
    DWORD   Mutant;
    DWORD   ImageBaseAddress;
    DWORD   LdrData;
} PEBB32;
/*
typedef struct _LIST_ENTRY32
{
  DWORD Flink;
  DWORD Blink;
} LIST_ENTRY32;
*/
typedef struct _PEB_LDR_DATA32
{
    ULONG        Length;
    BOOLEAN      Initialized;
    DWORD        SsHandle;
    LIST_ENTRY32 InLoadOrderModuleList;
} PEB_LDR_DATA32;
/*
typedef *struct _UNICODE_STRING32
{
  USHORT Length;
  USHORT MaximumLength;
  DWORD  Buffer;
} UNICODE_STRING32;
*/

typedef struct _LDR_MODULE
{
    LIST_ENTRY          InLoadOrderModuleList;
    LIST_ENTRY          InMemoryOrderModuleList;
    LIST_ENTRY          InInitializationOrderModuleList;
    void*               BaseAddress;
    void*               EntryPoint;
    ULONG               SizeOfImage;
    UNICODE_STRING      FullDllName;
    UNICODE_STRING      BaseDllName;
    ULONG               Flags;
    SHORT               LoadCount;
    SHORT               TlsIndex;
    HANDLE              SectionHandle;
    ULONG               CheckSum;
    ULONG               TimeDateStamp;
    HANDLE              ActivationContext;
} LDR_MODULE, *PLDR_MODULE;


typedef struct _LDR_MODULE32
{
    LIST_ENTRY32        InLoadOrderModuleList;
    LIST_ENTRY32        InMemoryOrderModuleList;
    LIST_ENTRY32        InInitializationOrderModuleList;
    DWORD               BaseAddress;
    DWORD               EntryPoint;
    ULONG               SizeOfImage;
    UNICODE_STRING32    FullDllName;
    UNICODE_STRING32    BaseDllName;
} LDR_MODULE32;

typedef struct {
    HANDLE process;
    PLIST_ENTRY head, current;
    LDR_MODULE ldr_module;
    BOOL wow64;
    LDR_MODULE32 ldr_module32;
} MODULE_ITERATOR;

static BOOL init_module_iterator(MODULE_ITERATOR *iter, HANDLE process)
{
    PROCESS_BASIC_INFORMATION pbi;
    PPEB_LDR_DATA ldr_data;
    NTSTATUS status;

    if (!IsWow64Process(process, &iter->wow64))
        return FALSE;

    /* Get address of PEB */
    status = NtQueryInformationProcess(process, ProcessBasicInformation,
                                       &pbi, sizeof(pbi), NULL);
    if (status != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }

    if (sizeof(void *) == 8 && iter->wow64)
    {
        PEB_LDR_DATA32 *ldr_data32_ptr;
        DWORD ldr_data32, first_module;
        PEBB32 *peb32;

        peb32 = (PEBB32 *)(DWORD_PTR)pbi.PebBaseAddress;

        if (!ReadProcessMemory(process, &peb32->LdrData, &ldr_data32,
                               sizeof(ldr_data32), NULL))
            return FALSE;
        ldr_data32_ptr = (PEB_LDR_DATA32 *)(DWORD_PTR) ldr_data32;

        if (!ReadProcessMemory(process,
                               &ldr_data32_ptr->InLoadOrderModuleList.Flink,
                               &first_module, sizeof(first_module), NULL))
            return FALSE;
        iter->head = (LIST_ENTRY *)&ldr_data32_ptr->InLoadOrderModuleList;
        iter->current = (LIST_ENTRY *)(DWORD_PTR) first_module;
        iter->process = process;

        return TRUE;
    }

    /* Read address of LdrData from PEB */
    if (!ReadProcessMemory(process, &pbi.PebBaseAddress->Ldr,
                           &ldr_data, sizeof(ldr_data), NULL))
        return FALSE;

    /* Read address of first module from LdrData */
    if (!ReadProcessMemory(process,
                           &ldr_data->InLoadOrderModuleList.Flink,
                           &iter->current, sizeof(iter->current), NULL))
        return FALSE;

    iter->head = &ldr_data->InLoadOrderModuleList;
    iter->process = process;

    return TRUE;
}

static int module_iterator_next(MODULE_ITERATOR *iter)
{
    if (iter->current == iter->head)
        return 0;

    if (sizeof(void *) == 8 && iter->wow64)
    {
        LIST_ENTRY32 *entry32 = (LIST_ENTRY32 *)iter->current;

        if (!ReadProcessMemory(iter->process,
                               CONTAINING_RECORD(entry32, LDR_MODULE32, InLoadOrderModuleList),
                               &iter->ldr_module32, sizeof(iter->ldr_module32), NULL))
            return -1;

        iter->current = (LIST_ENTRY *)(DWORD_PTR) iter->ldr_module32.InLoadOrderModuleList.Flink;
        return 1;
    }

    if (!ReadProcessMemory(iter->process,
                           CONTAINING_RECORD(iter->current, LDR_MODULE, InLoadOrderModuleList),
                           &iter->ldr_module, sizeof(iter->ldr_module), NULL))
         return -1;

    iter->current = iter->ldr_module.InLoadOrderModuleList.Flink;
    return 1;
}

static BOOL get_ldr_module(HANDLE process, HMODULE module, LDR_MODULE *ldr_module)
{
    MODULE_ITERATOR iter;
    INT ret;

    if (!init_module_iterator(&iter, process))
        return FALSE;

    while ((ret = module_iterator_next(&iter)) > 0)
        /* When hModule is NULL we return the process image - which will be
         * the first module since our iterator uses InLoadOrderModuleList */
        if (!module || module == iter.ldr_module.BaseAddress)
        {
            *ldr_module = iter.ldr_module;
            return TRUE;
        }

    if (ret == 0)
        SetLastError(ERROR_INVALID_HANDLE);

    return FALSE;
}

static BOOL get_ldr_module32(HANDLE process, HMODULE module, LDR_MODULE32 *ldr_module)
{
    MODULE_ITERATOR iter;
    INT ret;

    if (!init_module_iterator(&iter, process))
        return FALSE;

    while ((ret = module_iterator_next(&iter)) > 0)
        /* When hModule is NULL we return the process image - which will be
         * the first module since our iterator uses InLoadOrderModuleList */
        if (!module || (DWORD)(DWORD_PTR) module == iter.ldr_module32.BaseAddress)
        {
            *ldr_module = iter.ldr_module32;
            return TRUE;
        }

    if (ret == 0)
        SetLastError(ERROR_INVALID_HANDLE);

    return FALSE;
}

/***********************************************************************
 *           K32EnumProcessModules (KERNEL32.@)
 *
 * NOTES
 *  Returned list is in load order.
 */
BOOL WINAPI K32EnumProcessModules(HANDLE process, HMODULE *lphModule,
                                  DWORD cb, DWORD *needed)
{
    MODULE_ITERATOR iter;
    DWORD size = 0;
    INT ret;

    if (!init_module_iterator(&iter, process))
        return FALSE;

    if (cb && !lphModule)
    {
        SetLastError(ERROR_NOACCESS);
        return FALSE;
    }

    while ((ret = module_iterator_next(&iter)) > 0)
    {
        if (cb >= sizeof(HMODULE))
        {
            if (sizeof(void *) == 8 && iter.wow64)
                *lphModule++ = (HMODULE) (DWORD_PTR)iter.ldr_module32.BaseAddress;
            else
                *lphModule++ = iter.ldr_module.BaseAddress;
            cb -= sizeof(HMODULE);
        }
        size += sizeof(HMODULE);
    }

    if (!needed)
    {
        SetLastError(ERROR_NOACCESS);
        return FALSE;
    }
    *needed = size;

    return ret == 0;
}

/***********************************************************************
 *           K32EnumProcessModulesEx (KERNEL32.@)
 *
 * NOTES
 *  Returned list is in load order.
 */
BOOL WINAPI K32EnumProcessModulesEx(HANDLE process, HMODULE *lphModule,
                                    DWORD cb, DWORD *needed, DWORD filter)
{
    //FIXME("(%p, %p, %d, %p, %d) semi-stub\n",
      //    process, lphModule, cb, needed, filter);
    return K32EnumProcessModules(process, lphModule, cb, needed);
}

/***********************************************************************
 *           K32GetModuleBaseNameW (KERNEL32.@)
 */
DWORD WINAPI K32GetModuleBaseNameW(HANDLE process, HMODULE module,
                                   LPWSTR base_name, DWORD size)
{
    LDR_MODULE ldr_module;
    BOOL wow64;

    if (!IsWow64Process(process, &wow64))
        return 0;

    if (sizeof(void *) == 8 && wow64)
    {
        LDR_MODULE32 ldr_module32;

        if (!get_ldr_module32(process, module, &ldr_module32))
            return 0;

        size = min(ldr_module32.BaseDllName.Length / sizeof(WCHAR), size);
        if (!ReadProcessMemory(process, (void *)(DWORD_PTR)ldr_module32.BaseDllName.Buffer,
                               base_name, size * sizeof(WCHAR), NULL))
            return 0;
    }
    else
    {
        if (!get_ldr_module(process, module, &ldr_module))
            return 0;

        size = min(ldr_module.BaseDllName.Length / sizeof(WCHAR), size);
        if (!ReadProcessMemory(process, ldr_module.BaseDllName.Buffer,
                               base_name, size * sizeof(WCHAR), NULL))
            return 0;
    }

    base_name[size] = 0;
    return size;
}

/***********************************************************************
 *           K32GetModuleBaseNameA (KERNEL32.@)
 */
DWORD WINAPI K32GetModuleBaseNameA(HANDLE process, HMODULE module,
                                   LPSTR base_name, DWORD size)
{
    WCHAR *base_name_w;
    DWORD len, ret = 0;

    if(!base_name || !size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    base_name_w = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * size);
    if(!base_name_w)
        return 0;

    len = K32GetModuleBaseNameW(process, module, base_name_w, size);
    //TRACE("%d, %s\n", len, debugstr_w(base_name_w));
    if (len)
    {
        ret = WideCharToMultiByte(CP_ACP, 0, base_name_w, len,
                                  base_name, size, NULL, NULL);
        if (ret < size) base_name[ret] = 0;
    }
    HeapFree(GetProcessHeap(), 0, base_name_w);
    return ret;
}

/***********************************************************************
 *           K32GetModuleFileNameExW (KERNEL32.@)
 */
DWORD WINAPI K32GetModuleFileNameExW(HANDLE process, HMODULE module,
                                     LPWSTR file_name, DWORD size)
{
    LDR_MODULE ldr_module;
    BOOL wow64;
    DWORD len;

    if (!size) return 0;

    if (!IsWow64Process(process, &wow64))
        return 0;

    if (sizeof(void *) == 8 && wow64)
    {
        LDR_MODULE32 ldr_module32;

        if (!get_ldr_module32(process, module, &ldr_module32))
            return 0;

        len = ldr_module32.FullDllName.Length / sizeof(WCHAR);
        if (!ReadProcessMemory(process, (void *)(DWORD_PTR)ldr_module32.FullDllName.Buffer,
                               file_name, min( len, size ) * sizeof(WCHAR), NULL))
            return 0;
    }
    else
    {
        if (!get_ldr_module(process, module, &ldr_module))
            return 0;

        len = ldr_module.FullDllName.Length / sizeof(WCHAR);
        if (!ReadProcessMemory(process, ldr_module.FullDllName.Buffer,
                               file_name, min( len, size ) * sizeof(WCHAR), NULL))
            return 0;
    }

    if (len < size)
    {
        file_name[len] = 0;
        return len;
    }
    else
    {
        file_name[size - 1] = 0;
        return size;
    }
}

/***********************************************************************
 *           K32GetModuleFileNameExA (KERNEL32.@)
 */
DWORD WINAPI K32GetModuleFileNameExA(HANDLE process, HMODULE module,
                                     LPSTR file_name, DWORD size)
{
    WCHAR *ptr;
    DWORD len;

    //TRACE("(hProcess=%p, hModule=%p, %p, %d)\n", process, module, file_name, size);

    if (!file_name || !size)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }

    if ( process == GetCurrentProcess() )
    {
        len = GetModuleFileNameA( module, file_name, size );
        if (size) file_name[size - 1] = '\0';
        return len;
    }

    if (!(ptr = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR)))) return 0;

    len = K32GetModuleFileNameExW(process, module, ptr, size);
    if (!len)
    {
        file_name[0] = '\0';
    }
    else
    {
        if (!WideCharToMultiByte( CP_ACP, 0, ptr, -1, file_name, size, NULL, NULL ))
        {
            file_name[size - 1] = 0;
            len = size;
        }
        else if (len < size) len = strlen( file_name );
    }

    HeapFree(GetProcessHeap(), 0, ptr);
    return len;
}

typedef struct _MODULEINFO {
  LPVOID lpBaseOfDll;
  DWORD SizeOfImage;
  LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;

/***********************************************************************
 *           K32GetModuleInformation (KERNEL32.@)
 */
BOOL WINAPI K32GetModuleInformation(HANDLE process, HMODULE module,
                                    MODULEINFO *modinfo, DWORD cb)
{
    LDR_MODULE ldr_module;
    BOOL wow64;

    if (cb < sizeof(MODULEINFO))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    if (!IsWow64Process(process, &wow64))
        return FALSE;

    if (sizeof(void *) == 8 && wow64)
    {
        LDR_MODULE32 ldr_module32;

        if (!get_ldr_module32(process, module, &ldr_module32))
            return FALSE;

        modinfo->lpBaseOfDll = (void *)(DWORD_PTR)ldr_module32.BaseAddress;
        modinfo->SizeOfImage = ldr_module32.SizeOfImage;
        modinfo->EntryPoint  = (void *)(DWORD_PTR)ldr_module32.EntryPoint;
    }
    else
    {
        if (!get_ldr_module(process, module, &ldr_module))
            return FALSE;

        modinfo->lpBaseOfDll = ldr_module.BaseAddress;
        modinfo->SizeOfImage = ldr_module.SizeOfImage;
        modinfo->EntryPoint  = ldr_module.EntryPoint;
    }
    return TRUE;
}


/* EOF */
