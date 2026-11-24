/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/console.c
 * PURPOSE:         Legacy Win32 server console functions
 * PROGRAMMERS:     James Tabor <jimtabor@adsl-64-217-116-74.dsl.hstntx.swbell.net>
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 *                  David Welch (welch@cwcom.net) (welch@mcmail.com)
 *                  Christoph von Wittich (christoph_vw@reactos.org)
 *                  Johannes Anderwald (johannes.anderwald@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

static LPCWSTR BaseConFileName       = CONSOLE_FILE_NAME;
static LPCWSTR BaseConInputFileName  = CONSOLE_INPUT_FILE_NAME;
static LPCWSTR BaseConOutputFileName = CONSOLE_OUTPUT_FILE_NAME;

/*
 * @implemented (Undocumented)
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
CloseConsoleHandle(HANDLE hHandle)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_CLOSEHANDLE CloseHandleRequest = &ApiMessage.Data.CloseHandleRequest;

    CloseHandleRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    CloseHandleRequest->Handle        = hHandle;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepCloseHandle),
                        sizeof(*CloseHandleRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented (Undocumented)
 * @note See http://undoc.airesoft.co.uk/kernel32.dll/ConsoleMenuControl.php
 */
HMENU
WINAPI
DECLSPEC_HOTPATCH
ConsoleMenuControl(HANDLE hConsoleOutput,
                   DWORD dwCmdIdLow,
                   DWORD dwCmdIdHigh)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_MENUCONTROL MenuControlRequest = &ApiMessage.Data.MenuControlRequest;

    MenuControlRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    MenuControlRequest->OutputHandle  = hConsoleOutput;
    MenuControlRequest->CmdIdLow      = dwCmdIdLow;
    MenuControlRequest->CmdIdHigh     = dwCmdIdHigh;
    MenuControlRequest->MenuHandle    = NULL;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepMenuControl),
                        sizeof(*MenuControlRequest));

    return MenuControlRequest->MenuHandle;
}

static DWORD
IntGetConsoleAliasExes(PVOID lpExeNameBuffer,
                       DWORD ExeNameBufferLength,
                       BOOLEAN bUnicode)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETALIASESEXES GetAliasesExesRequest = &ApiMessage.Data.GetAliasesExesRequest;
    PCSR_CAPTURE_BUFFER CaptureBuffer;

    GetAliasesExesRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    GetAliasesExesRequest->Length        = ExeNameBufferLength;
    GetAliasesExesRequest->Unicode       = bUnicode;

    CaptureBuffer = CsrAllocateCaptureBuffer(1, ExeNameBufferLength);
    if (!CaptureBuffer)
    {
        DPRINT1("CsrAllocateCaptureBuffer failed!\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    CsrAllocateMessagePointer(CaptureBuffer,
                              ExeNameBufferLength,
                              (PVOID*)&GetAliasesExesRequest->ExeNames);

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        CaptureBuffer,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetAliasExes),
                        sizeof(*GetAliasesExesRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        CsrFreeCaptureBuffer(CaptureBuffer);
        BaseSetLastNTError(ApiMessage.Status);
        return 0;
    }

    RtlCopyMemory(lpExeNameBuffer,
                  GetAliasesExesRequest->ExeNames,
                  GetAliasesExesRequest->Length);

    CsrFreeCaptureBuffer(CaptureBuffer);

    return GetAliasesExesRequest->Length;
}

/*
 * @implemented
 */
DWORD
WINAPI
DECLSPEC_HOTPATCH
GetConsoleAliasExesW(LPWSTR lpExeNameBuffer,
                     DWORD ExeNameBufferLength)
{
    DPRINT("GetConsoleAliasExesW called\n");
    return IntGetConsoleAliasExes(lpExeNameBuffer, ExeNameBufferLength, TRUE);
}


/*
 * @implemented
 */
DWORD
WINAPI
DECLSPEC_HOTPATCH
GetConsoleAliasExesA(LPSTR lpExeNameBuffer,
                     DWORD ExeNameBufferLength)
{
    DPRINT("GetConsoleAliasExesA called\n");
    return IntGetConsoleAliasExes(lpExeNameBuffer, ExeNameBufferLength, FALSE);
}


/*
 * @implemented (Undocumented)
 * @note See https://cboard.cprogramming.com/windows-programming/102187-console-font-size.html
 */
DWORD
WINAPI
GetConsoleFontInfo(IN HANDLE hConsoleOutput,
                   IN BOOL bMaximumWindow,
                   IN DWORD nFontCount,
                   OUT PCONSOLE_FONT_INFO lpConsoleFontInfo)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETFONTINFO GetFontInfoRequest = &ApiMessage.Data.GetFontInfoRequest;
    PCSR_CAPTURE_BUFFER CaptureBuffer;

    GetFontInfoRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    GetFontInfoRequest->OutputHandle  = hConsoleOutput;
    GetFontInfoRequest->MaximumWindow = bMaximumWindow;
    GetFontInfoRequest->NumFonts      = nFontCount;

    CaptureBuffer = CsrAllocateCaptureBuffer(1, nFontCount * sizeof(CONSOLE_FONT_INFO));
    if (CaptureBuffer == NULL)
    {
        DPRINT1("CsrAllocateCaptureBuffer failed!\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    CsrAllocateMessagePointer(CaptureBuffer,
                              nFontCount * sizeof(CONSOLE_FONT_INFO),
                              (PVOID*)&GetFontInfoRequest->FontInfo);

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        CaptureBuffer,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetFontInfo),
                        sizeof(*GetFontInfoRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
    }
    else
    {
        RtlCopyMemory(lpConsoleFontInfo,
                      GetFontInfoRequest->FontInfo,
                      GetFontInfoRequest->NumFonts * sizeof(CONSOLE_FONT_INFO));
    }

    CsrFreeCaptureBuffer(CaptureBuffer);
    return GetFontInfoRequest->NumFonts;
}


/*
 * @implemented (Undocumented)
 */
BOOL
WINAPI
GetConsoleHardwareState(HANDLE hConsoleOutput,
                        PDWORD Flags,
                        PDWORD State)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETSETHWSTATE HardwareStateRequest = &ApiMessage.Data.HardwareStateRequest;

    DPRINT1("GetConsoleHardwareState(%lu, 0x%p) UNIMPLEMENTED!\n", Flags, State);

    if (Flags == NULL || State == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HardwareStateRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    HardwareStateRequest->OutputHandle  = hConsoleOutput;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetHardwareState),
                        sizeof(*HardwareStateRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    *Flags = HardwareStateRequest->Flags;
    *State = HardwareStateRequest->State;

    return TRUE;
}

/*
 * @implemented (Undocumented)
 */
HANDLE
WINAPI
GetConsoleInputWaitHandle(VOID)
{
    return GetStdHandle(STD_INPUT_HANDLE);
}

/*
 * @implemented (Undocumented)
 * @note See http://undoc.airesoft.co.uk/kernel32.dll/SetConsoleCursor.php
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleCursor(HANDLE  hConsoleOutput,
                 HCURSOR hCursor)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SETCURSOR SetCursorRequest = &ApiMessage.Data.SetCursorRequest;

    SetCursorRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    SetCursorRequest->OutputHandle  = hConsoleOutput;
    SetCursorRequest->CursorHandle  = hCursor;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetCursor),
                        sizeof(*SetCursorRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented (Undocumented)
 * @note See https://cboard.cprogramming.com/windows-programming/102187-console-font-size.html
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleFont(IN HANDLE hConsoleOutput,
               IN DWORD nFont)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SETFONT SetFontRequest = &ApiMessage.Data.SetFontRequest;

    SetFontRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    SetFontRequest->OutputHandle  = hConsoleOutput;
    SetFontRequest->FontIndex     = nFont;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetFont),
                        sizeof(*SetFontRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}


/*
 * @implemented (Undocumented)
 */
BOOL
WINAPI
SetConsoleHardwareState(HANDLE hConsoleOutput,
                        DWORD Flags,
                        DWORD State)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETSETHWSTATE HardwareStateRequest = &ApiMessage.Data.HardwareStateRequest;

    DPRINT1("SetConsoleHardwareState(%lu, %lu) UNIMPLEMENTED!\n", Flags, State);

    HardwareStateRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    HardwareStateRequest->OutputHandle  = hConsoleOutput;
    HardwareStateRequest->Flags         = Flags;
    HardwareStateRequest->State         = State;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetHardwareState),
                        sizeof(*HardwareStateRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}



/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleIcon(HICON hIcon)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SETICON SetIconRequest = &ApiMessage.Data.SetIconRequest;

    SetIconRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    SetIconRequest->IconHandle    = hIcon;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetIcon),
                        sizeof(*SetIconRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @unimplemented (Undocumented)
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleKeyShortcuts(
    _In_ BOOL bSet,
    _In_ BYTE bReserveKeys,
    _In_reads_(dwNumAppKeys) LPAPPKEY lpAppKeys,
    _In_ DWORD dwNumAppKeys)
{
    DPRINT1("SetConsoleKeyShortcuts(%lu, 0x%x, 0x%p, 0x%x) UNIMPLEMENTED!\n",
            bSet, bReserveKeys, lpAppKeys, dwNumAppKeys);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/*
 * @implemented (Undocumented)
 * @note See http://undoc.airesoft.co.uk/kernel32.dll/SetConsoleMaximumWindowSize.php
 *       Does nothing, returns TRUE only. Checked on Windows Server 2003.
 */
BOOL
WINAPI
SetConsoleMaximumWindowSize(HANDLE hConsoleOutput,
                            COORD dwMaximumSize)
{
    DPRINT1("SetConsoleMaximumWindowSize(0x%p, {%d, %d}) does nothing\n",
            hConsoleOutput, dwMaximumSize.X, dwMaximumSize.Y);
    return TRUE;
}

/*
 * @implemented (Undocumented)
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleMenuClose(BOOL bEnable)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SETMENUCLOSE SetMenuCloseRequest = &ApiMessage.Data.SetMenuCloseRequest;

    SetMenuCloseRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    SetMenuCloseRequest->Enable        = bEnable;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetMenuClose),
                        sizeof(*SetMenuCloseRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented (Undocumented)
 * @note See http://comments.gmane.org/gmane.comp.lang.harbour.devel/27844 (DEAD_LINK)
 *       Usage example: https://github.com/harbour/core/commit/d79a1b7b812cbde6ddf718ebfd6939a24f633e52
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsolePalette(HANDLE hConsoleOutput,
                  HPALETTE hPalette,
                  UINT dwUsage)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SETPALETTE SetPaletteRequest = &ApiMessage.Data.SetPaletteRequest;

    SetPaletteRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    SetPaletteRequest->OutputHandle  = hConsoleOutput;
    SetPaletteRequest->PaletteHandle = hPalette;
    SetPaletteRequest->Usage         = dwUsage;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetPalette),
                        sizeof(*SetPaletteRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

static
BOOL
IntWriteConsoleInput(IN HANDLE hConsoleInput,
                     IN PINPUT_RECORD lpBuffer,
                     IN DWORD nLength,
                     OUT LPDWORD lpNumberOfEventsWritten,
                     IN BOOLEAN bUnicode,
                     IN BOOLEAN bAppendToEnd)
{
    BOOL Success;
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_WRITEINPUT WriteInputRequest = &ApiMessage.Data.WriteInputRequest;
    PCSR_CAPTURE_BUFFER CaptureBuffer = NULL;

    DPRINT("IntWriteConsoleInput: %lx %p\n", nLength, lpNumberOfEventsWritten);

    /* Set up the data to send to the Console Server */
    WriteInputRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    WriteInputRequest->InputHandle   = hConsoleInput;
    WriteInputRequest->NumRecords    = nLength;
    WriteInputRequest->Unicode       = bUnicode;
    WriteInputRequest->AppendToEnd   = bAppendToEnd;

    /*
     * For optimization purposes, Windows (and hence ReactOS, too, for
     * compatibility reasons) uses a static buffer if no more than five
     * input records are written. Otherwise a new buffer is allocated.
     * This behaviour is also expected in the server-side.
     */
    if (nLength <= sizeof(WriteInputRequest->RecordStaticBuffer)/sizeof(INPUT_RECORD))
    {
        WriteInputRequest->RecordBufPtr = WriteInputRequest->RecordStaticBuffer;
        // CaptureBuffer = NULL;

        _SEH2_TRY
        {
            RtlCopyMemory(WriteInputRequest->RecordBufPtr,
                          lpBuffer,
                          nLength * sizeof(INPUT_RECORD));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastError(ERROR_INVALID_ACCESS);
            _SEH2_YIELD(return FALSE);
        }
        _SEH2_END;
    }
    else
    {
        ULONG Size = nLength * sizeof(INPUT_RECORD);

        /* Allocate a Capture Buffer */
        CaptureBuffer = CsrAllocateCaptureBuffer(1, Size);
        if (CaptureBuffer == NULL)
        {
            DPRINT1("CsrAllocateCaptureBuffer failed!\n");
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }

        /* Capture the user buffer */
        CsrCaptureMessageBuffer(CaptureBuffer,
                                lpBuffer,
                                Size,
                                (PVOID*)&WriteInputRequest->RecordBufPtr);
    }

    /* Call the server */
    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        CaptureBuffer,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepWriteConsoleInput),
                        sizeof(*WriteInputRequest));

    /* Check for success */
    Success = NT_SUCCESS(ApiMessage.Status);

    /* Release the capture buffer if needed */
    if (CaptureBuffer) CsrFreeCaptureBuffer(CaptureBuffer);

    /* Retrieve the results */
    _SEH2_TRY
    {
        DPRINT("Events written: %lx\n", WriteInputRequest->NumRecords);
        *lpNumberOfEventsWritten = WriteInputRequest->NumRecords;

        if (!Success)
            BaseSetLastNTError(ApiMessage.Status);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastError(ERROR_INVALID_ACCESS);
        Success = FALSE;
    }
    _SEH2_END;

    /* Return success status */
    return Success;
}



/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
WriteConsoleInputVDMW(IN HANDLE hConsoleInput,
                      IN CONST INPUT_RECORD *lpBuffer,
                      IN DWORD nLength,
                      OUT LPDWORD lpNumberOfEventsWritten)
{
    return IntWriteConsoleInput(hConsoleInput,
                                (PINPUT_RECORD)lpBuffer,
                                nLength,
                                lpNumberOfEventsWritten,
                                TRUE,
                                FALSE);
}


/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
WriteConsoleInputVDMA(IN HANDLE hConsoleInput,
                      IN CONST INPUT_RECORD *lpBuffer,
                      IN DWORD nLength,
                      OUT LPDWORD lpNumberOfEventsWritten)
{
    return IntWriteConsoleInput(hConsoleInput,
                                (PINPUT_RECORD)lpBuffer,
                                nLength,
                                lpNumberOfEventsWritten,
                                FALSE,
                                FALSE);
}


static
BOOL
IntGetConsoleKeyboardLayoutName(OUT PVOID pszLayoutName,
                                IN BOOL bAnsi)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETKBDLAYOUTNAME GetKbdLayoutNameRequest = &ApiMessage.Data.GetKbdLayoutNameRequest;

    /* Set up the data to send to the Console Server */
    GetKbdLayoutNameRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    GetKbdLayoutNameRequest->Ansi          = bAnsi;

    /* Call the server */
    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetKeyboardLayoutName),
                        sizeof(*GetKbdLayoutNameRequest));

    /* Check for success */
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    /* Retrieve the results */
    _SEH2_TRY
    {
        /* Copy only KL_NAMELENGTH == 9 characters, ANSI or UNICODE */
        if (bAnsi)
            strncpy(pszLayoutName, (PCHAR)GetKbdLayoutNameRequest->LayoutBuffer, KL_NAMELENGTH);
        else
            wcsncpy(pszLayoutName, (PWCHAR)GetKbdLayoutNameRequest->LayoutBuffer, KL_NAMELENGTH);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastError(ERROR_INVALID_ACCESS);
        _SEH2_YIELD(return FALSE);
    }
    _SEH2_END;

    return TRUE;
}

/*
 * @implemented (undocumented)
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetConsoleKeyboardLayoutNameA(OUT LPSTR pszLayoutName)
{
    return IntGetConsoleKeyboardLayoutName(pszLayoutName, TRUE);
}

/*
 * @implemented (undocumented)
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetConsoleKeyboardLayoutNameW(OUT LPWSTR pszLayoutName)
{
    return IntGetConsoleKeyboardLayoutName(pszLayoutName, FALSE);
}

/*
 * @implemented (Undocumented)
 * @note See http://undoc.airesoft.co.uk/kernel32.dll/ShowConsoleCursor.php
 */
INT
WINAPI
DECLSPEC_HOTPATCH
ShowConsoleCursor(HANDLE hConsoleOutput,
                  BOOL bShow)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_SHOWCURSOR ShowCursorRequest = &ApiMessage.Data.ShowCursorRequest;

    ShowCursorRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    ShowCursorRequest->OutputHandle  = hConsoleOutput;
    ShowCursorRequest->Show          = bShow;
    ShowCursorRequest->RefCount      = 0;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepShowCursor),
                        sizeof(*ShowCursorRequest));

    return ShowCursorRequest->RefCount;
}

/*
 * FUNCTION: Checks whether the given handle is a valid console handle.
 *
 * ARGUMENTS:
 *      hIoHandle - Handle to be checked.
 *
 * RETURNS:
 *      TRUE : Handle is a valid console handle.
 *      FALSE: Handle is not a valid console handle.
 *
 * STATUS: Officially undocumented
 *
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
VerifyConsoleIoHandle(HANDLE hIoHandle)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_VERIFYHANDLE VerifyHandleRequest = &ApiMessage.Data.VerifyHandleRequest;

    VerifyHandleRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    VerifyHandleRequest->Handle        = hIoHandle;
    VerifyHandleRequest->IsValid       = FALSE;

    /* If the process is not attached to a console, return invalid handle */
    if (VerifyHandleRequest->ConsoleHandle == NULL) return FALSE;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepVerifyIoHandle),
                        sizeof(*VerifyHandleRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return VerifyHandleRequest->IsValid;
}

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
DuplicateConsoleHandle(HANDLE hSourceHandle,
                       DWORD dwDesiredAccess,
                       BOOL bInheritHandle,
                       DWORD dwOptions)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_DUPLICATEHANDLE DuplicateHandleRequest = &ApiMessage.Data.DuplicateHandleRequest;

    if ( (dwOptions & ~(DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) ||
         (!(dwOptions & DUPLICATE_SAME_ACCESS) &&
           (dwDesiredAccess & ~(GENERIC_READ | GENERIC_WRITE))) )
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    DuplicateHandleRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    DuplicateHandleRequest->SourceHandle  = hSourceHandle;
    DuplicateHandleRequest->DesiredAccess = dwDesiredAccess;
    DuplicateHandleRequest->InheritHandle = bInheritHandle;
    DuplicateHandleRequest->Options       = dwOptions;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepDuplicateHandle),
                        sizeof(*DuplicateHandleRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return INVALID_HANDLE_VALUE;
    }

    return DuplicateHandleRequest->TargetHandle;
}
static BOOL
IntRegisterConsoleIME(
    _In_ HWND hWnd,
    _In_ DWORD dwThreadId,
    _In_opt_ SIZE_T cbDesktop,
    _In_opt_ PWSTR pDesktop,
    _Out_opt_ PDWORD pdwAttachToThreadId)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_REGISTERCONSOLEIME RegisterConsoleIME = &ApiMessage.Data.RegisterConsoleIME;
    PCSR_CAPTURE_BUFFER CaptureBuffer;

    if (!cbDesktop || !pDesktop)
    {
        pDesktop = NtCurrentPeb()->ProcessParameters->DesktopInfo.Buffer;
        cbDesktop = NtCurrentPeb()->ProcessParameters->DesktopInfo.Length;
    }

    cbDesktop = min(cbDesktop, (MAX_PATH + 1) * sizeof(WCHAR));

    RegisterConsoleIME->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    RegisterConsoleIME->hWnd = hWnd;
    RegisterConsoleIME->dwThreadId = dwThreadId;
    RegisterConsoleIME->cbDesktop = cbDesktop;

    CaptureBuffer = CsrAllocateCaptureBuffer(1, cbDesktop);
    if (!CaptureBuffer)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    CsrCaptureMessageBuffer(CaptureBuffer,
                            pDesktop,
                            cbDesktop,
                            (PVOID*)&RegisterConsoleIME->pDesktop);

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        CaptureBuffer,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepRegisterConsoleIME),
                        sizeof(*RegisterConsoleIME));

    CsrFreeCaptureBuffer(CaptureBuffer);

    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    if (pdwAttachToThreadId)
    {
        _SEH2_TRY
        {
            *pdwAttachToThreadId = RegisterConsoleIME->dwAttachToThreadId;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            BaseSetLastNTError(STATUS_ACCESS_VIOLATION);
            _SEH2_YIELD(return FALSE);
        }
        _SEH2_END;
    }

    return TRUE;
}

static BOOL
IntUnregisterConsoleIME(
    _In_ DWORD dwThreadId)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_UNREGISTERCONSOLEIME UnregisterConsoleIME = &ApiMessage.Data.UnregisterConsoleIME;

    UnregisterConsoleIME->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    UnregisterConsoleIME->dwThreadId = dwThreadId;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepUnregisterConsoleIME),
                        sizeof(*UnregisterConsoleIME));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

/* This function is called by CONIME.EXE */
BOOL
WINAPI
DECLSPEC_HOTPATCH
RegisterConsoleIME(
    _In_ HWND hWnd,
    _Out_opt_ LPDWORD pdwAttachToThreadId)
{
    return IntRegisterConsoleIME(hWnd,
                                 GetCurrentThreadId(),
                                 0,
                                 NULL,
                                 pdwAttachToThreadId);
}

/* This function is called by CONIME.EXE */
BOOL
WINAPI
DECLSPEC_HOTPATCH
UnregisterConsoleIME(VOID)
{
    return IntUnregisterConsoleIME(GetCurrentThreadId());
}

/*
 * @implemented (Undocumented)
 */
HANDLE
WINAPI
OpenConsoleW(LPCWSTR wsName,
             DWORD dwDesiredAccess,
             BOOL bInheritHandle,
             DWORD dwShareMode)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_OPENCONSOLE OpenConsoleRequest = &ApiMessage.Data.OpenConsoleRequest;
    CONSOLE_HANDLE_TYPE HandleType;

    if (wsName && (_wcsicmp(wsName, BaseConInputFileName) == 0))
    {
        HandleType = HANDLE_INPUT;
    }
    else if (wsName && (_wcsicmp(wsName, BaseConOutputFileName) == 0))
    {
        HandleType = HANDLE_OUTPUT;
    }
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    if ( (dwDesiredAccess & ~(GENERIC_READ | GENERIC_WRITE)) ||
         (dwShareMode & ~(FILE_SHARE_READ | FILE_SHARE_WRITE)) )
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    OpenConsoleRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    OpenConsoleRequest->HandleType    = HandleType;
    OpenConsoleRequest->DesiredAccess = dwDesiredAccess;
    OpenConsoleRequest->InheritHandle = bInheritHandle;
    OpenConsoleRequest->ShareMode     = dwShareMode;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepOpenConsole),
                        sizeof(*OpenConsoleRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return INVALID_HANDLE_VALUE;
    }

    return OpenConsoleRequest->Handle;
}

/*
 * @implemented (Undocumented)
 * @note See http://blog.airesoft.co.uk/2012/10/things-ms-can-do-that-they-dont-tell-you-about-console-graphics/
 */
BOOL
WINAPI
InvalidateConsoleDIBits(IN HANDLE hConsoleOutput,
                        IN PSMALL_RECT lpRect)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_INVALIDATEDIBITS InvalidateDIBitsRequest = &ApiMessage.Data.InvalidateDIBitsRequest;

    if (lpRect == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    InvalidateDIBitsRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    InvalidateDIBitsRequest->OutputHandle  = hConsoleOutput;
    InvalidateDIBitsRequest->Region        = *lpRect;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepInvalidateBitMapRect),
                        sizeof(*InvalidateDIBitsRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented (Undocumented)
 * @note See https://cboard.cprogramming.com/windows-programming/102187-console-font-size.html
 */
DWORD
WINAPI
DECLSPEC_HOTPATCH
GetNumberOfConsoleFonts(VOID)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETNUMFONTS GetNumFontsRequest = &ApiMessage.Data.GetNumFontsRequest;

    GetNumFontsRequest->ConsoleHandle = NtCurrentPeb()->ProcessParameters->ConsoleHandle;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetNumberOfFonts),
                        sizeof(*GetNumFontsRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return 0;
    }

    return GetNumFontsRequest->NumFonts;
}
