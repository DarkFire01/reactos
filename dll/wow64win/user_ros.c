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

__declspec(allocate(".text"))
static unsigned char ReadFsDwordImpl[] =
{
    0x64, 0x8B, 0x01, /* mov eax, fs:[rcx] */
    0xC3              /* ret */
};

static ULONG __readfsdword(ULONG x)
{
    typedef ULONG(*__readfsdwordImplType)(ULONG);
    return ((__readfsdwordImplType)ReadFsDwordImpl)(x);
}

static
PULONG
GetKernelCallbackTable32()
{
    return UlongToPtr(((PPEB32)(ULONG_PTR)NtCurrentTeb32()->ProcessEnvironmentBlock)->KernelCallbackTable);
}

/* TODO: move back to wow64.dll */
NTSTATUS 
WINAPI 
Wow64KiUserCallbackDispatcher(ULONG nCallback, 
                              PVOID IN pArgs, 
                              ULONG nArgLen, 
                              PVOID* OUT ppReturn, 
                              PULONG OUT pnRetLen)
{    
    USER_CALLBACK_FRAME frame;
    ULONG Args64[2];
    
    Args64[0] = PtrToUlong(pArgs);
    Args64[1] = nArgLen;
    
    frame.prev_frame = NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA];
    frame.temp_list  = NtCurrentTeb()->TlsSlots[WOW64_TLS_TEMPLIST];
    frame.ret_ptr    = ppReturn;
    frame.ret_len    = pnRetLen;
    frame.temp_list  = NULL;
    
    NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA] = &frame;
    
    if (!setjmp(frame.jmpbuf))
    {
        Call32(GetKernelCallbackTable32()[nCallback], 2, Args64);
    }
   
    NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA] = frame.prev_frame;
    return frame.status;
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
    
    /* FIXME */
    if ((ULONG_PTR)pLoadMenu->hModule & 0xFFFFFFFF00000000)
    {
        pLoadMenu->hModule = (PVOID)0x77a20000;
    }
    
    pLoadMenu32->hModule = HandleToUlong(pLoadMenu->hModule);
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
    
    __debugbreak();
    
    typedef struct _CLIENTINFO32
    {
        ULONG CI_flags;
        ULONG cSpins;
        DWORD dwExpWinVer;
        DWORD dwCompatFlags;
        DWORD dwCompatFlags2;
        DWORD dwTIFlags; /* ThreadInfo TIF_Xxx flags for User space. */
        ULONG pDeskInfo;
        ULONG ulClientDelta;
        ULONG phkCurrent;
        ULONG fsHooks;
        ULONG CallbackWnd[2];
        DWORD dwHookCurrent;
        INT cInDDEMLCallback;
        ULONG pClientThreadInfo;
        ULONG dwHookData;
        DWORD dwKeyCache;
        BYTE afKeyState[8];
        DWORD dwAsyncKeyCache;
        BYTE afAsyncKeyState[8];
        BYTE afAsyncKeyStateRecentDow[8];
        ULONG hKL;
        USHORT CodePage;
        UCHAR achDbcsCF[2];
        MSG32 msgDbcsCB;
        ULONG lpdwRegisteredClasses;
        ULONG Win32ClientInfo3[26];
        ULONG ppi;
    } CLIENTINFO32, *PCLIENTINFO32;
    
    C_ASSERT(sizeof(CLIENTINFO32) == 61 * sizeof(ULONG));
    
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
    return STATUS_NOT_IMPLEMENTED;
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