/*
 * Wine WOW64 ReactOS port 
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64win.dll
 * FILE:            dll/wow64win/user_ros.c
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "wow64win_private.h"

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include "../../../win32ss/include/callback.h"

#define DEFINE_USER32_CALLBACK(id, value, fn) NTSTATUS WINAPI wow64win_Nt ## fn (PVOID, ULONG);
#include "../../../win32ss/include/u32cb.h"  
#undef DEFINE_USER32_CALLBACK 

NTSTATUS WINAPI (*UserCallbacks[])(PVOID Arguments, ULONG ArgumentLength) = 
{
#define DEFINE_USER32_CALLBACK(id, value, fn) wow64win_Nt ## fn,
#include "../../../win32ss/include/u32cb.h"  
#undef DEFINE_USER32_CALLBACK 
};

static ULONG     hUser32 = 0;
static PFNCLIENT apfnClientProcs32W;
static PFNCLIENT apfnClientProcs32A;

static MSG32 *msg_64to32( const MSG *msg64, MSG32 *msg32 )
{
    MSG32 msg;

    if (!msg32) return NULL;

    msg.hwnd    = HandleToLong( msg64->hwnd );
    msg.message = msg64->message;
    msg.wParam  = msg64->wParam;
    msg.lParam  = msg64->lParam;
    msg.time    = msg64->time;
    msg.pt      = msg64->pt;
    memcpy( msg32, &msg, sizeof(msg) );
    return msg32;
}

NTSTATUS WINAPI wow64_NtUserProcessConnect(UINT* pArgs)
{  
    HANDLE ProcessHandle = get_handle(&pArgs);    
    PUSERCONNECT pUserConnect = get_ptr(&pArgs);
    ULONG Size = get_ulong(&pArgs);
    /* Copy the memory manually to prevent the kernel complaining about 
       misaligned memory. */
    USERCONNECT UserConnect;
    
    NTSTATUS Status;
    
    if (Size != sizeof(USERCONNECT) || pUserConnect == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }
    
    Status = NtUserProcessConnect(ProcessHandle, &UserConnect, sizeof(UserConnect));
    
    g_ServerInfo = UserConnect.siClient.psi;
    
    *pUserConnect = UserConnect;
    return Status;
}

/* FIXME: HACK: It is very unlikely that this is how Windows implements this.
   Maybe Windows loads 64-bit user32.dll, so it can run native WndProcs? 
   
   Is there even a guarantee, that we can't get a 64 bit WndProc from places
   other than PFNCLIENT tables? */
static
ULONG
Translate64WndProc(WNDPROC WndProc)
{
    const WNDPROC* ClientA = (WNDPROC*)&g_ServerInfo->apfnClientA;
    const WNDPROC* ClientW = (WNDPROC*)&g_ServerInfo->apfnClientW;
    const size_t nWndProcsInClient = sizeof(PFNCLIENT) / sizeof(WNDPROC);
    int i;
    
    if ((ULONG_PTR)PtrToUlong(WndProc) != (ULONG_PTR)WndProc)
    {
        for (i = 0; i < nWndProcsInClient; i++)
        {
            if (ClientW[i] == WndProc)
            {
                return PtrToUlong(((WNDPROC*)&apfnClientProcs32W)[i]);
            }
            
            if (ClientA[i] == WndProc)
            {
                return PtrToUlong(((WNDPROC*)&apfnClientProcs32A)[i]);
            }
        }
    }
    
    /* No mapping found - return as is. */
    return PtrToUlong(WndProc);
}

VOID
WndProcParams64To32(PWINDOWPROC_CALLBACK_ARGUMENTS CallbackArgs, 
                    PWINDOWPROC_CALLBACK_ARGUMENTS32 Arguments32)
{
    Arguments32->Proc = Translate64WndProc(CallbackArgs->Proc);
    Arguments32->IsAnsiProc = CallbackArgs->IsAnsiProc;
    Arguments32->Wnd = HandleToUlong(CallbackArgs->Wnd);
    Arguments32->Msg = CallbackArgs->Msg;
    Arguments32->wParam = CallbackArgs->wParam;
    Arguments32->lParam = (ULONG)CallbackArgs->lParam;
    Arguments32->lParamBufferSize = CallbackArgs->lParamBufferSize;
    Arguments32->Result = CallbackArgs->Result;
}
 
VOID
WndProcParams32To64(PWINDOWPROC_CALLBACK_ARGUMENTS OutArguments, 
                    PWINDOWPROC_CALLBACK_ARGUMENTS32 CallbackArgs)
{
    WINDOWPROC_CALLBACK_ARGUMENTS Data = { 0 }, *Arguments = &Data;
    
    Arguments->Proc = UlongToPtr(CallbackArgs->Proc);
    Arguments->IsAnsiProc = CallbackArgs->IsAnsiProc;
    Arguments->Wnd = UlongToHandle(CallbackArgs->Wnd);
    Arguments->Msg = CallbackArgs->Msg;
    Arguments->wParam = CallbackArgs->wParam;
    Arguments->lParam = CallbackArgs->lParam;
    Arguments->lParamBufferSize = CallbackArgs->lParamBufferSize;
    Arguments->Result = CallbackArgs->Result;
    
    *OutArguments = *Arguments;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallWindowProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    return wow64_NtUserCallWinProc(Arguments, ArgumentLength);
}

NTSTATUS
WINAPI
wow64win_NtUser32CallSendAsyncProcForKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32LoadSysMenuTemplateForKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32SetupDefaultCursors(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallHookProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallEventProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallLoadMenuFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    PLOADMENU_CALLBACK_ARGUMENTS pLoadMenu;
    PLOADMENU_CALLBACK_ARGUMENTS32 pLoadMenu32;
    NTSTATUS Status;
    PVOID pResult;
    ULONG nRetLen = 0;
    
    pLoadMenu = Arguments;
    pLoadMenu32 = Arguments;
    
    C_ASSERT(sizeof(*pLoadMenu) > sizeof(*pLoadMenu32));
    
    if (ArgumentLength < sizeof(*pLoadMenu))
    {
        return STATUS_UNSUCCESSFUL;
    }
    
    /* FIXME: check if pLoadMenu->hModule really is 64-bit USER32 */
    if ((ULONG_PTR)pLoadMenu->hModule & 0xFFFFFFFF00000000)
    {
        pLoadMenu32->hModule = hUser32;
    }
    else
    {
        pLoadMenu32->hModule = HandleToUlong(pLoadMenu->hModule);
    }
    
    pLoadMenu32->InterSource = PtrToUlong(pLoadMenu->InterSource);
    memcpy(pLoadMenu32->MenuName, 
           pLoadMenu->MenuName, ArgumentLength 
            - sizeof(pLoadMenu->InterSource) 
            - sizeof(pLoadMenu->hModule));
    
    Status = Wow64KiUserCallbackDispatcher(NumUser32CallLoadMenuFromKernel,
                                           pLoadMenu32,
                                           ArgumentLength - (sizeof(*pLoadMenu) - sizeof(*pLoadMenu32)),
                                           &pResult,
                                           &nRetLen);
    
    return NtCallbackReturn(pResult, nRetLen, Status);
}

NTSTATUS
WINAPI
wow64win_NtUser32CallClientThreadSetupFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    ULONG nRetLen = 0;
    PVOID pResult;
    NTSTATUS Status;
    struct _CLIENTINFO32* pClientInfo32;
    PCLIENTINFO pClientInfo;
    
    Status = Wow64KiUserCallbackDispatcher(NumUser32CallClientThreadSetupFromKernel,
                                           Arguments,
                                           ArgumentLength,
                                           &pResult,
                                           &nRetLen);

    
    NtCurrentPeb32()->GdiSharedHandleTable = PtrToUlong(NtCurrentPeb()->GdiSharedHandleTable);
    NtCurrentPeb32()->GdiDCAttributeList = NtCurrentPeb()->GdiDCAttributeList;
    
    pClientInfo = (PCLIENTINFO) &NtCurrentTeb()->Win32ClientInfo;
    pClientInfo32 = (PCLIENTINFO32) &NtCurrentTeb32()->Win32ClientInfo;
    
    pClientInfo32->CI_flags = pClientInfo->CI_flags;
    pClientInfo32->cSpins = pClientInfo->cSpins;
    pClientInfo32->dwExpWinVer = pClientInfo->dwExpWinVer;
    pClientInfo32->dwCompatFlags = pClientInfo->dwCompatFlags;
    pClientInfo32->dwCompatFlags2 = pClientInfo->dwCompatFlags2;
    pClientInfo32->dwTIFlags = pClientInfo->dwTIFlags;
    pClientInfo32->pDeskInfo = PtrToUlong(pClientInfo->pDeskInfo);
    pClientInfo32->ulClientDelta = pClientInfo->ulClientDelta;
    pClientInfo32->phkCurrent = PtrToUlong(pClientInfo->phkCurrent);
    pClientInfo32->fsHooks = pClientInfo->fsHooks;
    pClientInfo32->CallbackWnd[0] = HandleToUlong(pClientInfo->CallbackWnd.hWnd);
    pClientInfo32->CallbackWnd[1] = PtrToUlong(pClientInfo->CallbackWnd.pWnd);
    pClientInfo32->dwHookCurrent = pClientInfo->dwHookCurrent;
    pClientInfo32->cInDDEMLCallback = pClientInfo->cInDDEMLCallback;
    pClientInfo32->pClientThreadInfo = PtrToUlong(pClientInfo->pClientThreadInfo);
    pClientInfo32->dwHookData = pClientInfo->dwHookData;
    pClientInfo32->dwKeyCache = pClientInfo->dwKeyCache;
    
    RtlCopyMemory(pClientInfo32->afKeyState, 
                  pClientInfo->afKeyState, 
                  sizeof(pClientInfo32->afKeyState));
    
    pClientInfo32->dwAsyncKeyCache = pClientInfo->dwAsyncKeyCache;
    
    RtlCopyMemory(pClientInfo32->afAsyncKeyState, 
                  pClientInfo->afAsyncKeyState, 
                  sizeof(pClientInfo32->afAsyncKeyState));
    
    RtlCopyMemory(pClientInfo32->afAsyncKeyStateRecentDow, 
                  pClientInfo->afAsyncKeyStateRecentDow, 
                  sizeof(pClientInfo32->afAsyncKeyStateRecentDow));
                  
    pClientInfo32->hKL = HandleToUlong(pClientInfo->hKL);
    pClientInfo32->CodePage = pClientInfo->CodePage;
    pClientInfo32->achDbcsCF[0] = pClientInfo->achDbcsCF[0];
    pClientInfo32->achDbcsCF[1] = pClientInfo->achDbcsCF[1];
    
    msg_64to32(&pClientInfo->msgDbcsCB, &pClientInfo32->msgDbcsCB);
    
    pClientInfo32->lpdwRegisteredClasses = PtrToUlong(pClientInfo->lpdwRegisteredClasses);
    pClientInfo32->ppi = PtrToUlong(pClientInfo->ppi);
    
    return Status;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallClientLoadLibraryFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallGetCharsetInfo(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallCopyImageFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallSetWndIconsFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32DeliverUserAPC(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallDDEPostFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallDDEGetFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallOBMFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallLPKFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    PLPK_CALLBACK_ARGUMENTS32 pLpk32;
    PLPK_CALLBACK_ARGUMENTS pLpk64;

    NTSTATUS Status;
    PVOID pResult;
    ULONG nRetLen;

    pLpk32 = (PLPK_CALLBACK_ARGUMENTS32)Arguments;
    pLpk64 = (PLPK_CALLBACK_ARGUMENTS)Arguments;

    pLpk32->lpString = PtrToUlong(pLpk64->lpString);
    pLpk32->hdc = HandleToUlong(pLpk64->hdc);
    pLpk32->x = pLpk64->x;
    pLpk32->y = pLpk64->y;
    pLpk32->flags = pLpk64->flags;
    pLpk32->rect = pLpk64->rect;
    pLpk32->count = pLpk64->count;
    pLpk32->bRect = pLpk64->bRect;

    Status = Wow64KiUserCallbackDispatcher(NumUser32CallLPKFromKernel,
                                           pLpk32,
                                           ArgumentLength,
                                           &pResult,
                                           &nRetLen);
    
    return NtCallbackReturn(pResult, nRetLen, Status);
}

NTSTATUS
WINAPI
wow64win_NtUser32CallUMPDFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallImmProcessKeyFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallImmLoadLayoutFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    DPRINT1("UNHANDLED USER CALLBACK " __FILE__ ":%d\n", __LINE__);
    return STATUS_NOT_IMPLEMENTED;
}

ATOM
WINAPI
wow64_NtUserGetClassInfo(UINT* pArgs)
{
    HINSTANCE hInstance = get_ptr(&pArgs);
    UNICODE_STRING32* ClassName32 = get_ptr(&pArgs);
    WNDCLASSEXW32* lpWndClassEx32 = get_ptr(&pArgs);
    ULONG *ppszMenuName32 = get_ptr(&pArgs);
    BOOL bAnsi = get_ulong(&pArgs);
    
    UNICODE_STRING ClassName = { 0 };
    LPWSTR pszMenuName = NULL;
    WNDCLASSEXW wndClassEx = { 0 };
    
    ATOM Atom;

    wndClassEx.cbSize = sizeof(wndClassEx);
    
    Atom = NtUserGetClassInfo(hInstance,
                              unicode_str_32to64( &ClassName, ClassName32 ), 
                              &wndClassEx,
                              &pszMenuName,
                              bAnsi);
    if (Atom == 0)
    {
        return Atom;
    }

    lpWndClassEx32->style = wndClassEx.style;
    lpWndClassEx32->lpfnWndProc = PtrToUlong(wndClassEx.lpfnWndProc);
    lpWndClassEx32->cbClsExtra = wndClassEx.cbClsExtra;
    lpWndClassEx32->cbWndExtra = wndClassEx.cbWndExtra;
    lpWndClassEx32->hInstance = PtrToUlong(wndClassEx.hInstance);
    lpWndClassEx32->hIcon = HandleToUlong(wndClassEx.hIcon);
    lpWndClassEx32->hCursor = HandleToUlong(wndClassEx.hCursor);
    lpWndClassEx32->hbrBackground = HandleToUlong(wndClassEx.hbrBackground);
    lpWndClassEx32->lpszMenuName = PtrToUlong(wndClassEx.lpszMenuName);
    lpWndClassEx32->lpszClassName = PtrToUlong(wndClassEx.lpszClassName);
    lpWndClassEx32->hIconSm = HandleToUlong(wndClassEx.hIconSm);

    if (ppszMenuName32 != NULL)
    {
        *ppszMenuName32 = PtrToUlong(pszMenuName);
    }
    
    return Atom;
}

NTSTATUS 
WINAPI 
wow64_NtUserCallHwndLock(UINT *pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    DWORD Code = get_ulong(&pArgs);

    return NtUserCallHwndLock(hWnd, Code);
}

ULONG_PTR 
WINAPI 
wow64_NtUserGetThreadState(UINT *pArgs)
{
    DWORD Routine = get_ulong(&pArgs);

    NTSTATUS Status = NtUserGetThreadState(Routine);
        
    NtCurrentTeb32()->Win32ThreadInfo = PtrToUlong(NtCurrentTeb()->Win32ThreadInfo);
    return Status;
}

NTSTATUS 
WINAPI 
wow64_NtUserInitializeClientPfnArrays(UINT *pArgs)
{
    PPFNCLIENT pProcsA32 = get_ptr(&pArgs);
    PPFNCLIENT pProcsW32 = get_ptr(&pArgs);
    PVOID      pWorkers  = get_ptr(&pArgs);
    
    hUser32 = get_ulong(&pArgs);
    
    apfnClientProcs32A = *pProcsA32;
    apfnClientProcs32W = *pProcsW32;
    
    UNREFERENCED_PARAMETER(pWorkers);
    
    /* Do not bother with calling win32k - highly unlikely that a WOW64 process 
       is the first one to call this, so this syscall is essentialy a no-op 
       anyway. */
    return STATUS_SUCCESS;
}

NTSTATUS 
WINAPI 
wow64_NtUserBuildHwndList(UINT *pArgs)
{
    HDESK hDesktop   = get_handle(&pArgs);
    HWND hParent = get_handle(&pArgs);
    BOOLEAN bChildren = get_ulong(&pArgs);
    ULONG dwThreadId = get_ulong(&pArgs);
    ULONG cHwnd = get_ulong(&pArgs);
    ULONG *phwndList32 = get_ptr(&pArgs);
    ULONG *pcHwndNeeded = get_ptr(&pArgs);

    HWND *phwndList = NULL;
    ULONG i;
    NTSTATUS Status;

    if (phwndList32 != NULL)
    {
        phwndList = Wow64AllocateTemp(cHwnd * sizeof(*phwndList));
    }
    
    Status = NtUserBuildHwndList(hDesktop, 
                                 hParent,
                                 bChildren, 
                                 dwThreadId, 
                                 cHwnd, 
                                 phwndList, 
                                 pcHwndNeeded);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if (phwndList32 != NULL)
    {
        for (i = 0; i < *pcHwndNeeded; i++)
        {
            phwndList32[i] = HandleToUlong(phwndList[i]);
        }
    }
    
    return Status;
}

BOOL 
WINAPI
wow64_NtUserSetWindowFNID(ULONG* pArgs)
{
    HANDLE hWnd = get_handle(&pArgs);
    WORD fnId = get_ulong(&pArgs);
    
    return NtUserSetWindowFNID(hWnd, fnId);
}

BOOL
WINAPI
wow64_NtUserDefSetText(ULONG* pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    PLARGE_STRING32 pWindowText32 = get_ptr(&pArgs);

    LARGE_STRING WindowText;

    return NtUserDefSetText(hWnd, large_str_32to64(&WindowText, pWindowText32));
}

HBRUSH
WINAPI
wow64_NtUserGetControlColor(ULONG *pArgs)
{
    HWND hwndParent = get_handle(&pArgs);
    HWND hwnd = get_handle(&pArgs);
    HDC hdc = get_handle(&pArgs);
    UINT CtlMsg = get_ulong(&pArgs);
    
    return NtUserGetControlColor(hwndParent, hwnd, hdc, CtlMsg);
}

HBRUSH
WINAPI
wow64_NtUserGetControlBrush(ULONG *pArgs)
{
    HWND hwnd = get_handle(&pArgs);
    HDC hdc = get_handle(&pArgs);
    UINT CtlMsg = get_ulong(&pArgs);
    
    return NtUserGetControlBrush(hwnd, hdc, CtlMsg);
}

NTSTATUS
WINAPI
wow64_NtUserAlterWindowStyle(ULONG* pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    DWORD Index = get_ulong(&pArgs);
    LONG NewValue = get_ulong(&pArgs);
    
    return NtUserAlterWindowStyle(hWnd, Index, NewValue);
}

NTSTATUS 
WINAPI 
wow64_NtUserSetCursorIconData(UINT *pArgs)
{
    HCURSOR hCursor = get_handle(&pArgs);
    PUNICODE_STRING32 pusModule32 = get_ptr(&pArgs);
    PUNICODE_STRING32 pusResName32 = get_ptr(&pArgs);
    PCURSORDATA32 pCursorData32 = get_ptr(&pArgs);

    UNICODE_STRING usModule, usResName;
    CURSORDATA CursorData;

    CursorData.lpName = UlongToPtr(pCursorData32->lpName);
    CursorData.lpModName = UlongToPtr(pCursorData32->lpModName);
    CursorData.rt = pCursorData32->rt;
    CursorData.dummy = pCursorData32->dummy;
    CursorData.CURSORF_flags = pCursorData32->CURSORF_flags;
    CursorData.xHotspot = pCursorData32->xHotspot;
    CursorData.yHotspot = pCursorData32->yHotspot;
    CursorData.hbmMask = UlongToHandle(pCursorData32->hbmMask);
    CursorData.hbmColor = UlongToHandle(pCursorData32->hbmColor);
    CursorData.hbmAlpha = UlongToHandle(pCursorData32->hbmAlpha);
    CursorData.rcBounds = pCursorData32->rcBounds;
    CursorData.hbmUserAlpha = UlongToHandle(pCursorData32->hbmUserAlpha);
    CursorData.bpp = pCursorData32->bpp;
    CursorData.cx = pCursorData32->cx;
    CursorData.cy = pCursorData32->cy;
    CursorData.cpcur = pCursorData32->cpcur;
    CursorData.cicur = pCursorData32->cicur;
    CursorData.aspcur = UlongToPtr(pCursorData32->aspcur);
    CursorData.aicur = UlongToPtr(pCursorData32->aicur);
    CursorData.ajifRate = UlongToPtr(pCursorData32->ajifRate);
    CursorData.iicur = pCursorData32->iicur;

    return NtUserSetCursorIconData(hCursor,
                                   unicode_str_32to64(&usModule, pusModule32),
                                   unicode_str_32to64(&usResName, pusResName32), 
                                   &CursorData);
}

HMONITOR 
WINAPI
wow64_NtUserMonitorFromRect(UINT *pArgs)
{
    LPCRECT pRect = get_ptr(&pArgs);
    DWORD dwFlags = get_ulong(&pArgs);
    
    return NtUserMonitorFromRect(pRect, dwFlags);
}

NTSTATUS 
WINAPI
wow64_NtUserGetMonitorInfo(UINT *pArgs)
{
    HANDLE hMonitor = get_handle(&pArgs);
    LPMONITORINFO lpMi = get_ptr(&pArgs);
    
    return NtUserGetMonitorInfo(hMonitor, lpMi);
}

NTSTATUS 
WINAPI 
wow64_NtUserSetScrollBarInfo(UINT *pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    LONG id = get_ulong(&pArgs);
    SETSCROLLBARINFO *pInfo = get_ptr(&pArgs);

    return NtUserSetScrollBarInfo(hWnd, id, pInfo);
}

BOOL
WINAPI
wow64_NtUserSBGetParms(UINT *pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    int fnBar = get_ulong(&pArgs);
    PSBDATA pSBData = get_ptr(&pArgs);
    LPSCROLLINFO lpSi = get_ptr(&pArgs);
    
    return NtUserSBGetParms(hWnd, fnBar, pSBData, lpSi);
}

HMONITOR
NTAPI
wow64_NtUserMonitorFromWindow(UINT *pArgs)
{
    HWND hWnd = get_handle(&pArgs);
    DWORD dwFlags = get_ulong(&pArgs);
    
    return NtUserMonitorFromWindow(hWnd, dwFlags);
}

ULONG 
WINAPI 
wow64_NtUserDeferWindowPos(UINT *pArgs)
{
    HDWP WinPosInfo = get_handle(&pArgs);
    HWND Wnd = get_handle(&pArgs);
    HWND WndInsertAfter = get_handle(&pArgs);
    int x = get_ulong(&pArgs);
    int y = get_ulong(&pArgs);
    int cx = get_ulong(&pArgs);
    int cy = get_ulong(&pArgs);
    UINT Flags = get_ulong(&pArgs);
    HDWP Result;

    Result = NtUserDeferWindowPos(WinPosInfo,
                                  Wnd,
                                  WndInsertAfter,
                                  x,
                                  y,
                                  cx,
                                  cy,
                                  Flags);
    
    return HandleToUlong(Result);
}

UINT
NTAPI
wow64_NtUserRegisterWindowMessage(UINT* pArgs)
{
    PUNICODE_STRING32 pMessageName32 = get_ptr(&pArgs);
    UNICODE_STRING MessageName;
    
    return NtUserRegisterWindowMessage(unicode_str_32to64(&MessageName, 
                                                          pMessageName32));
}

DWORD
NTAPI
wow64_NtUserQueryWindow(UINT* pArgs)
{
    HANDLE hWindow = get_handle(&pArgs);
    DWORD nIndex = get_ulong(&pArgs);

    return (DWORD)NtUserQueryWindow(hWindow, nIndex);
}

NTSTATUS
NTAPI
wow64_NtUserGetGuiResources(UINT* pArgs)
{
    HANDLE hProcess = get_handle(&pArgs);
    DWORD uiFlags = get_ulong(&pArgs);

    return NtUserGetGuiResources(hProcess, uiFlags);
}

HANDLE
NTAPI
wow64_NtUserGetClipboardData(UINT *pArgs)
{
    ULONG ulFormat = get_ulong(&pArgs);
    PGETCLIPBDATA32 pClipData32 = get_ptr(&pArgs);

    GETCLIPBDATA ClipData = { 0 };
    HANDLE Result;

    ClipData.uFmtRet = pClipData32->uFmtRet;
    ClipData.fGlobalHandle = pClipData32->fGlobalHandle;
    ClipData.hLocale = UlongToHandle(pClipData32->hLocale);

    Result = NtUserGetClipboardData(ulFormat, &ClipData);

    pClipData32->uFmtRet = ClipData.uFmtRet;
    pClipData32->fGlobalHandle = ClipData.fGlobalHandle;
    pClipData32->hLocale = HandleToUlong(ClipData.hLocale);

    return Result;
}

HANDLE
NTAPI
wow64_NtUserSetClipboardData(UINT *pArgs)
{
    ULONG ulFormat = get_ulong(&pArgs);
    HANDLE hMem = get_handle(&pArgs);
    PSETCLIPBDATA pClipData = get_ptr(&pArgs);

    return NtUserSetClipboardData(ulFormat, hMem, pClipData);
}

NTSTATUS
NTAPI
wow64_NtUserCreateLocalMemHandle(UINT* pArgs)
{
    HANDLE hMem = get_handle(&pArgs);
    PVOID pData32 = get_ptr(&pArgs);
    DWORD cbData32 = get_ulong(&pArgs);
    PDWORD32 pcbData = get_ptr(&pArgs);

    return NtUserCreateLocalMemHandle(hMem, pData32, cbData32, pcbData);
}

HANDLE
NTAPI
wow64_NtUserConvertMemHandle(UINT* pArgs)
{
    PVOID pData = get_ptr(&pArgs);
    DWORD cbData = get_ulong(&pArgs);

    return NtUserConvertMemHandle(pData, cbData);
}
