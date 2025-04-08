/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS user32.dll
 * FILE:            win32ss/user/user32/misc/misc.c
 * PURPOSE:         Misc
 * PROGRAMMER:      Thomas Weidenmueller (w3seek@users.sourceforge.net)
 */

#include <user32.h>

VOID
WINAPI
UserSetLastError(IN DWORD dwErrCode)
{
    /*
     * Equivalent of SetLastError in kernel32, but without breaking
     * into the debugger nor checking whether the last old error is
     * the same as the one we are going to set.
     */
    NtCurrentTeb()->LastErrorValue = dwErrCode;
}

VOID
WINAPI
UserSetLastNTError(IN NTSTATUS Status)
{
    /*
     * Equivalent of BaseSetLastNTError in kernel32, but using
     * UserSetLastError: convert from NT to Win32, then set.
     */
    UserSetLastError(RtlNtStatusToDosError(Status));
}


PTHREADINFO
GetW32ThreadInfo(VOID)
{
    PTHREADINFO ti;

    ti = (PTHREADINFO)NtCurrentTeb()->Win32ThreadInfo;
    if (ti == NULL)
    {
        /* create the THREADINFO structure */
        NtUserGetThreadState(THREADSTATE_GETTHREADINFO);
        ti = (PTHREADINFO)NtCurrentTeb()->Win32ThreadInfo;
    }

    return ti;
}


/*
 * GetUserObjectSecurity
 *
 * Retrieves security information for user object specified
 * with handle 'hObject'. Descriptor returned in self-relative
 * format.
 *
 * Arguments:
 *  1) hObject - handle to an object to retrieve information for
 *  2) pSecurityInfo - type of information to retrieve
 *  3) pSecurityDescriptor - buffer which receives descriptor
 *  4) dwLength - size, in bytes, of buffer 'pSecurityDescriptor'
 *  5) pdwLengthNeeded - receives actual size of the descriptor
 *
 * Return Vaules:
 *  TRUE on success
 *  FALSE on failure, call GetLastError() for more information
 */
/*
 * @implemented
 */
BOOL
WINAPI
GetUserObjectSecurity(
    IN HANDLE hObject,
    IN PSECURITY_INFORMATION pSecurityInfo,
    OUT PSECURITY_DESCRIPTOR pSecurityDescriptor,
    IN DWORD dwLength,
    OUT PDWORD pdwLengthNeeded
)
{
    NTSTATUS Status;

    Status = NtQuerySecurityObject(hObject,
                                   *pSecurityInfo,
                                   pSecurityDescriptor,
                                   dwLength,
                                   pdwLengthNeeded);
    if (!NT_SUCCESS(Status))
    {
        UserSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}


/*
 * SetUserObjectSecurity
 *
 * Sets new security descriptor to user object specified by
 * handle 'hObject'. Descriptor must be in self-relative format.
 *
 * Arguments:
 *  1) hObject - handle to an object to set information for
 *  2) pSecurityInfo - type of information to apply
 *  3) pSecurityDescriptor - buffer which descriptor to set
 *
 * Return Vaules:
 *  TRUE on success
 *  FALSE on failure, call GetLastError() for more information
 */
/*
 * @implemented
 */
BOOL
WINAPI
SetUserObjectSecurity(
    IN HANDLE hObject,
    IN PSECURITY_INFORMATION pSecurityInfo,
    IN PSECURITY_DESCRIPTOR pSecurityDescriptor
)
{
    NTSTATUS Status;

    Status = NtSetSecurityObject(hObject,
                                 *pSecurityInfo,
                                 pSecurityDescriptor);
    if (!NT_SUCCESS(Status))
    {
        UserSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsGUIThread(
    BOOL bConvert)
{
  PTHREADINFO ti = (PTHREADINFO)NtCurrentTeb()->Win32ThreadInfo;
  if (ti == NULL)
  {
    if(bConvert)
    {
      NtUserGetThreadState(THREADSTATE_GETTHREADINFO);
      if ((PTHREADINFO)NtCurrentTeb()->Win32ThreadInfo) return TRUE;
      else
         SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return FALSE;
  }
  else
    return TRUE;
}

BOOL
FASTCALL
TestWindowProcess(PWND Wnd)
{
   if (WOW64_CAST_TO_PTR(Wnd->head.pti) == (PTHREADINFO)NtCurrentTeb()->Win32ThreadInfo)
      return TRUE;
   else
      return (NtUserQueryWindow(WOW64_CAST_TO_HANDLE(Wnd->head.h), QUERY_WINDOW_UNIQUE_PROCESS_ID) ==
              (DWORD_PTR)NtCurrentTeb()->ClientId.UniqueProcess );
}

BOOL
FASTCALL
TestState(PWND pWnd, UINT Flag)
{
    UINT bit;
    bit = 1 << LOWORD(Flag);
    switch(HIWORD(Flag))
    {
       case 0: 
          return (pWnd->state & bit); 
       case 1:
          return (pWnd->state2 & bit);
       case 2:
          return (pWnd->ExStyle2 & bit);
    }
    return FALSE;
}

#if !(defined(_WOW64) && defined(_M_IX86))
PUSER_HANDLE_ENTRY
FASTCALL
GetUser32Handle(HANDLE handle)
{
    INT Index;
    USHORT generation;

    if (!handle) return NULL;

    Index = (((UINT_PTR)handle & 0xffff) - FIRST_USER_HANDLE) >> 1;

    if (Index < 0 || Index >= gHandleTable->nb_handles)
        return NULL;

    if (!gHandleEntries[Index].type || !gHandleEntries[Index].ptr)
        return NULL;

    generation = (UINT_PTR)handle >> 16;

    if (generation == gHandleEntries[Index].generation || !generation || generation == 0xffff)
        return &gHandleEntries[Index];

    return NULL;
}
#else
UINT64
FASTCALL
GetUser32Handle(HANDLE handle)
{
    INT Index;
    USHORT generation;

    UINT64 elementAddress;

    if (!handle) return 0;

    Index = (((UINT_PTR)handle & 0xffff) - FIRST_USER_HANDLE) >> 1;
    elementAddress = gHandleEntries + Index * sizeof(USER_HANDLE_ENTRY);

    if (Index < 0 || Index >= WOW64_READ_ULONG_FIELD(gHandleTable, USER_HANDLE_TABLE, nb_handles))
        return 0;

    if (!WOW64_READ_BYTE_FIELD(elementAddress, USER_HANDLE_ENTRY, type) || 
        !WOW64_READ_PTR_FIELD(elementAddress, USER_HANDLE_ENTRY, ptr))
        return 0;

    generation = (UINT_PTR)handle >> 16;

    if (generation == WOW64_READ_WORD_FIELD(elementAddress, 
                                            USER_HANDLE_ENTRY, 
                                            generation) || 
        !generation || generation == 0xffff)
        return elementAddress;

    return 0;
}
#endif

/*
 * Decide whether an object is located on the desktop or shared heap
 */
static const BOOL g_ObjectHeapTypeShared[TYPE_CTYPES] =
{
    FALSE, /* TYPE_FREE (not used) */
    FALSE, /* TYPE_WINDOW */
    FALSE, /* TYPE_MENU */
    TRUE,  /* TYPE_CURSOR */
    TRUE,  /* TYPE_SETWINDOWPOS */
    FALSE, /* TYPE_HOOK */
    TRUE,  /* TYPE_CLIPDATA */
    FALSE, /* TYPE_CALLPROC */
    TRUE,  /* TYPE_ACCELTABLE */
    FALSE, /* TYPE_DDEACCESS */
    FALSE, /* TYPE_DDECONV */
    FALSE, /* TYPE_DDEXACT */
    TRUE,  /* TYPE_MONITOR */
    TRUE,  /* TYPE_KBDLAYOUT */
    TRUE,  /* TYPE_KBDFILE */
    TRUE,  /* TYPE_WINEVENTHOOK */
    TRUE,  /* TYPE_TIMER */
    FALSE, /* TYPE_INPUTCONTEXT */
    FALSE, /* TYPE_HIDDATA */
    FALSE, /* TYPE_DEVICEINFO */
    FALSE, /* TYPE_TOUCHINPUTINFO */
    FALSE, /* TYPE_GESTUREINFOOBJ */
};

//
// Validate Handle and return the pointer to the object.
//
PVOID
FASTCALL
ValidateHandle(HANDLE handle, UINT uType)
{
  PVOID ret;
#if !(defined(_WOW64) && defined(_M_IX86))
  PUSER_HANDLE_ENTRY pEntry;
#else
  UINT64 pEntry;
#endif

  ASSERT(uType < TYPE_CTYPES);

  pEntry = GetUser32Handle(handle);

#if !(defined(_WOW64) && defined(_M_IX86))
  if (pEntry && uType == 0)
      uType = pEntry->type;

// Must have an entry and must be the same type!
  if ( (!pEntry) ||
        (pEntry->type != uType) ||
        !pEntry->ptr ||
        (pEntry->flags & HANDLEENTRY_DESTROY) || (pEntry->flags & HANDLEENTRY_INDESTROY) )
#else
  if (pEntry && uType == 0)
      uType = WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, type);

// Must have an entry and must be the same type!
  if ( (!pEntry) ||
        (WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, type) != uType) ||
        !WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr) ||
        (WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, flags) & HANDLEENTRY_DESTROY) || 
        (WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, flags) & HANDLEENTRY_INDESTROY) )
#endif
  {
     switch ( uType )
     {  // Test (with wine too) confirms these results!
        case TYPE_WINDOW:
          SetLastError(ERROR_INVALID_WINDOW_HANDLE);
          break;
        case TYPE_MENU:
          SetLastError(ERROR_INVALID_MENU_HANDLE);
          break;
        case TYPE_CURSOR:
          SetLastError(ERROR_INVALID_CURSOR_HANDLE);
          break;
        case TYPE_SETWINDOWPOS:
          SetLastError(ERROR_INVALID_DWP_HANDLE);
          break;
        case TYPE_HOOK:
          SetLastError(ERROR_INVALID_HOOK_HANDLE);
          break;
        case TYPE_ACCELTABLE:
          SetLastError(ERROR_INVALID_ACCEL_HANDLE);
          break;
        default:
          SetLastError(ERROR_INVALID_HANDLE);
          break;
    }
    return NULL;
  }

#if !(defined(_WOW64) && defined(_M_IX86))
  if (g_ObjectHeapTypeShared[uType])
    ret = SharedPtrToUser(pEntry->ptr);
  else
    ret = DesktopPtrToUser(pEntry->ptr);
#else
  if (g_ObjectHeapTypeShared[uType])
    /* FIXME: truncation */
    ret = (PVOID)(ULONG_PTR)SharedPtrToUser(WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr));
  else
    ret = DesktopPtrToUser((PVOID)(ULONG_PTR)WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr)); 
#endif

  return ret;
}

//
// Validate Handle and return the pointer to the object.
//
PVOID
FASTCALL
ValidateHandleNoErr(HANDLE handle, UINT uType)
{
  PVOID ret;
#if !(defined(_WOW64) && defined(_M_IX86))
  PUSER_HANDLE_ENTRY pEntry;
#else
  UINT64 pEntry;
#endif

  ASSERT(uType < TYPE_CTYPES);

  pEntry = GetUser32Handle(handle);

#if !(defined(_WOW64) && defined(_M_IX86))
  if (pEntry && uType == 0)
    uType = pEntry->type;

// Must have an entry and must be the same type!
  if ( (!pEntry) || (pEntry->type != uType) || !pEntry->ptr )
    return NULL;

  if (g_ObjectHeapTypeShared[uType])
    ret = SharedPtrToUser(pEntry->ptr);
  else
    ret = DesktopPtrToUser(pEntry->ptr);
#else
  if (pEntry && uType == 0)
    uType = WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, type);

  if ( (!pEntry) || (WOW64_READ_BYTE_FIELD(pEntry, USER_HANDLE_ENTRY, type) != uType) ||
       !WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr) )
    return NULL;    

  /* FIXME: address truncation */
  if (g_ObjectHeapTypeShared[uType])
    ret = (PVOID)(ULONG_PTR)SharedPtrToUser(WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr));
  else
    ret = DesktopPtrToUser((PVOID)(ULONG_PTR)WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ptr));
#endif

  return ret;
}

//
// Validate a callproc handle and return the pointer to the object.
//
PCALLPROCDATA
FASTCALL
ValidateCallProc(HANDLE hCallProc)
{
#if !(defined(_WOW64) && defined(_M_IX86))
  PUSER_HANDLE_ENTRY pEntry;
#else
  UINT64 pEntry;
#endif

  PCALLPROCDATA CallProc = ValidateHandle(hCallProc, TYPE_CALLPROC);

  pEntry = GetUser32Handle(hCallProc);
#if !(defined(_WOW64) && defined(_M_IX86))
  if (CallProc != NULL && pEntry->ppi == g_ppi)
#else
  if (CallProc != NULL && WOW64_READ_PTR_FIELD(pEntry, USER_HANDLE_ENTRY, ppi) == (ULONG_PTR)g_ppi)
#endif
     return CallProc;

  return NULL;
}


//
// Validate a window handle and return the pointer to the object.
//
PWND
FASTCALL
ValidateHwnd(HWND hwnd)
{
    PCLIENTINFO ClientInfo = GetWin32ClientInfo();
    ASSERT(ClientInfo != NULL);

    /* See if the window is cached */
    if (hwnd && hwnd == ClientInfo->CallbackWnd.hWnd)
        return ClientInfo->CallbackWnd.pWnd;

    return ValidateHandle((HANDLE)hwnd, TYPE_WINDOW);
}

//
// Validate a window handle and return the pointer to the object.
//
PWND
FASTCALL
ValidateHwndNoErr(HWND hwnd)
{
    PWND Wnd;
    PCLIENTINFO ClientInfo = GetWin32ClientInfo();
    ASSERT(ClientInfo != NULL);

    /* See if the window is cached */
    if (hwnd == ClientInfo->CallbackWnd.hWnd)
        return ClientInfo->CallbackWnd.pWnd;

    Wnd = ValidateHandleNoErr((HANDLE)hwnd, TYPE_WINDOW);
    if (Wnd != NULL)
    {
        return Wnd;
    }

    return NULL;
}

PWND
FASTCALL
GetThreadDesktopWnd(VOID)
{
    PWND Wnd = WOW64_CAST_TO_PTR(GetThreadDesktopInfo()->spwnd);
    if (Wnd != NULL)
        Wnd = DesktopPtrToUser(Wnd);
    return Wnd;
}

//
// Validate a window handle and return the pointer to the object.
//
PWND
FASTCALL
ValidateHwndOrDesk(HWND hwnd)
{
    if (hwnd == HWND_DESKTOP)
        return GetThreadDesktopWnd();

    return ValidateHwnd(hwnd);
}

/*
 * @implemented
 */
DWORD WINAPI WCSToMBEx(WORD CodePage,LPWSTR UnicodeString,LONG UnicodeSize,LPSTR *MBString,LONG MBSize,BOOL Allocate)
{
	DWORD Size;
	if (UnicodeSize == -1)
	{
		UnicodeSize = wcslen(UnicodeString)+1;
	}
	if (MBSize == -1)
	{
		if (!Allocate)
		{
			return 0;
		}
		MBSize = UnicodeSize * 2;
	}
	if (Allocate)
	{
		LPSTR SafeString = RtlAllocateHeap(GetProcessHeap(), 0, MBSize);
        if (SafeString == NULL)
            return 0;
        *MBString = SafeString;
	}
	if (CodePage == 0)
	{
		RtlUnicodeToMultiByteN(*MBString,MBSize,&Size,UnicodeString,UnicodeSize);
	}
	else
	{
		WideCharToMultiByte(CodePage,0,UnicodeString,UnicodeSize,*MBString,MBSize,0,0);
	}
	return UnicodeSize;
}

/*
 * @implemented
 */
DWORD WINAPI MBToWCSEx(WORD CodePage,LPSTR MBString,LONG MBSize,LPWSTR *UnicodeString,LONG UnicodeSize,BOOL Allocate)
{
	DWORD Size;
	if (MBSize == -1)
	{
		MBSize = strlen(MBString)+1;
	}
	if (UnicodeSize == -1)
	{
		if (!Allocate)
		{
			return 0;
		}
		UnicodeSize = MBSize;
	}
	if (Allocate)
	{
		LPWSTR SafeString = RtlAllocateHeap(GetProcessHeap(), 0, UnicodeSize);
        if (SafeString == NULL)
            return 0;
        *UnicodeString = SafeString;
	}
	UnicodeSize *= sizeof(WCHAR);
	if (CodePage == 0)
	{
		RtlMultiByteToUnicodeN(*UnicodeString,UnicodeSize,&Size,MBString,MBSize);
	}
	else
	{
		Size = MultiByteToWideChar(CodePage,0,MBString,MBSize,*UnicodeString,UnicodeSize);
	}
	return Size;
}
