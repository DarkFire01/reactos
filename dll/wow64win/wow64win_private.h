/*
 * WoW64 private definitions
 *
 * Copyright 2021 Alexandre Julliard
 * Copyright 2025 Marcin Jabłoński
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WOW64WIN_PRIVATE_H
#define __WOW64WIN_PRIVATE_H

#ifdef __REACTOS__
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

/* SDK/DDK/NDK Headers. */
#include <wingdi.h>
#include <objbase.h>
#include <imm.h>
#include <immdev.h>
#include <imm32_undoc.h>

#include <winddi.h>
#include <prntfont.h>

#include <ndk/rtlfuncs.h>
#include <ndk/mmfuncs.h>

/* Public Win32K Headers */
#include <ntuser.h>
#include <ntgdityp.h>
#include <ntgdi.h>
#include <ntgdihdl.h>

#define IS_ATOM(x) \
  (((ULONG_PTR)(x) > 0x0) && ((ULONG_PTR)(x) < 0x10000))

extern NTSTATUS WINAPI (*UserCallbacks[])(PVOID Arguments, ULONG ArgumentLength);
NTSTATUS WINAPI wow64_NtUserCallWinProc( void *arg, ULONG size );

#define DEFINE_USER32_CALLBACK(id, value, fn) static const ULONG Num ## fn = value;
#include "u32cb.h"  
#undef DEFINE_USER32_CALLBACK 

#endif

#ifndef __REACTOS__
#include "../win32u/win32syscalls.h"
#include "ntuser.h"

#define SYSCALL_ENTRY(id,name,_args) extern NTSTATUS WINAPI wow64_ ## name( UINT *args );
ALL_SYSCALLS32
#undef SYSCALL_ENTRY

extern ntuser_callback user_callbacks[];

struct object_attr64
{
    OBJECT_ATTRIBUTES   attr;
    UNICODE_STRING      str;
    SECURITY_DESCRIPTOR sd;
};

typedef struct
{
    ULONG Length;
    ULONG RootDirectory;
    ULONG ObjectName;
    ULONG Attributes;
    ULONG SecurityDescriptor;
    ULONG SecurityQualityOfService;
} OBJECT_ATTRIBUTES32;

static inline ULONG get_ulong( UINT **args ) { return *(*args)++; }
static inline HANDLE get_handle( UINT **args ) { return LongToHandle( *(*args)++ ); }
static inline void *get_ptr( UINT **args ) { return ULongToPtr( *(*args)++ ); }

static inline void **addr_32to64( void **addr, ULONG *addr32 )
{
    if (!addr32) return NULL;
    *addr = ULongToPtr( *addr32 );
    return addr;
}

static inline SIZE_T *size_32to64( SIZE_T *size, ULONG *size32 )
{
    if (!size32) return NULL;
    *size = *size32;
    return size;
}

static inline void put_addr( ULONG *addr32, void *addr )
{
    if (addr32) *addr32 = PtrToUlong( addr );
}

static inline void put_size( ULONG *size32, SIZE_T size )
{
    if (size32) *size32 = min( size, MAXDWORD );
}

static inline UNICODE_STRING *unicode_str_32to64( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    if (!str32) return NULL;
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->Buffer = ULongToPtr( str32->Buffer );
    return str;
}

static inline SECURITY_DESCRIPTOR *secdesc_32to64( SECURITY_DESCRIPTOR *out, const SECURITY_DESCRIPTOR *in )
{
    /* relative descr has the same layout for 32 and 64 */
    const SECURITY_DESCRIPTOR_RELATIVE *sd = (const SECURITY_DESCRIPTOR_RELATIVE *)in;

    if (!in) return NULL;
    out->Revision = sd->Revision;
    out->Sbz1     = sd->Sbz1;
    out->Control  = sd->Control & ~SE_SELF_RELATIVE;
    if (sd->Control & SE_SELF_RELATIVE)
    {
        if (sd->Owner) out->Owner = (PSID)((BYTE *)sd + sd->Owner);
        if (sd->Group) out->Group = (PSID)((BYTE *)sd + sd->Group);
        if ((sd->Control & SE_SACL_PRESENT) && sd->Sacl) out->Sacl = (PSID)((BYTE *)sd + sd->Sacl);
        if ((sd->Control & SE_DACL_PRESENT) && sd->Dacl) out->Dacl = (PSID)((BYTE *)sd + sd->Dacl);
    }
    else
    {
        out->Owner = ULongToPtr( sd->Owner );
        out->Group = ULongToPtr( sd->Group );
        if (sd->Control & SE_SACL_PRESENT) out->Sacl = ULongToPtr( sd->Sacl );
        if (sd->Control & SE_DACL_PRESENT) out->Dacl = ULongToPtr( sd->Dacl );
    }
    return out;
}

static inline OBJECT_ATTRIBUTES *objattr_32to64( struct object_attr64 *out, const OBJECT_ATTRIBUTES32 *in )
{
    memset( out, 0, sizeof(*out) );
    if (!in) return NULL;
    if (in->Length != sizeof(*in)) return &out->attr;

    out->attr.Length = sizeof(out->attr);
    out->attr.RootDirectory = LongToHandle( in->RootDirectory );
    out->attr.Attributes = in->Attributes;
    out->attr.ObjectName = unicode_str_32to64( &out->str, ULongToPtr( in->ObjectName ));
    out->attr.SecurityQualityOfService = ULongToPtr( in->SecurityQualityOfService );
    out->attr.SecurityDescriptor = secdesc_32to64( &out->sd, ULongToPtr( in->SecurityDescriptor ));
    return &out->attr;
}

static inline void set_last_error32( DWORD err )
{
    TEB *teb = NtCurrentTeb();
    TEB32 *teb32 = (TEB32 *)((char *)teb + teb->WowTebOffset);
    teb32->LastErrorValue = err;
}

#else

#include "../wow64/ros_wow64_private.h"

static HANDLE UlongToHandleNoSignExtend(ULONG ulong)
{
    if ((LONG)ulong == -1L)
    {
        return INVALID_HANDLE_VALUE;
    }
    else if (ulong == HandleToUlong(HWND_MESSAGE))
    {
        return HWND_MESSAGE;
    }
    else if (ulong == HandleToUlong(HWND_NOTOPMOST))
    {
        return HWND_NOTOPMOST;
    }
    return (HANDLE)(ULONG_PTR)ulong;
}

#define UlongToHandle(ulong) UlongToHandleNoSignExtend(ulong)
#define get_handle(ppArgs) UlongToHandle(get_ulong(ppArgs))

typedef struct
{
    DWORD cbSize;
    DWORD fMask;
    DWORD dwStyle;
    UINT  cyMax;
    ULONG hbrBack;
    DWORD dwContextHelpID;
    ULONG dwMenuData;
} MENUINFO32;

typedef struct
{
    UINT    cbSize;
    UINT    fMask;
    UINT    fType;
    UINT    fState;
    UINT    wID;
    UINT32  hSubMenu;
    UINT32  hbmpChecked;
    UINT32  hbmpUnchecked;
    UINT32  dwItemData;
    UINT32  dwTypeData;
    UINT    cch;
    UINT32  hbmpItem;
} MENUITEMINFOW32;

typedef struct
{
    UINT32  hwnd;
    UINT    message;
    UINT32  wParam;
    UINT32  lParam;
    DWORD   time;
    POINT   pt;
} MSG32;

typedef struct
{
    DWORD dwType;
    DWORD dwSize;
    UINT32 hDevice;
    UINT32 wParam;
} RAWINPUTHEADER32;

typedef struct
{
    USHORT usUsagePage;
    USHORT usUsage;
    DWORD dwFlags;
    UINT32 hwndTarget;
} RAWINPUTDEVICE32;

typedef struct
{
    UINT32 hDevice;
    DWORD dwType;
} RAWINPUTDEVICELIST32;

typedef struct
{
    LONG    dx;
    LONG    dy;
    DWORD   mouseData;
    DWORD   dwFlags;
    DWORD   time;
    ULONG   dwExtraInfo;
} MOUSEINPUT32;

typedef struct
{
    WORD    wVk;
    WORD    wScan;
    DWORD   dwFlags;
    DWORD   time;
    ULONG   dwExtraInfo;
} KEYBDINPUT32;

typedef struct
{
    DWORD type;
    union
    {
        MOUSEINPUT32   mi;
        KEYBDINPUT32   ki;
        HARDWAREINPUT  hi;
    } DUMMYUNIONNAME;
} INPUT32;

typedef struct
{
    int x;
    int y;
    DWORD time;
    ULONG dwExtraInfo;
} MOUSEMOVEPOINT32;

typedef struct
{
    UINT32 hdc;
    BOOL   fErase;
    RECT   rcPaint;
    BOOL   fRestore;
    BOOL   fIncUpdate;
    BYTE   rgbReserved[32];
} PAINTSTRUCT32;

typedef struct
{
    ULONG lpCreateParams;
    ULONG hInstance;
    ULONG hMenu;
    ULONG hwndParent;
    INT   cy;
    INT   cx;
    INT   y;
    INT   x;
    LONG  style;
    ULONG lpszName;
    ULONG lpszClass;
    DWORD dwExStyle;
} CREATESTRUCT32;

typedef struct
{
    ULONG szClass;
    ULONG szTitle;
    ULONG hOwner;
    INT   x;
    INT   y;
    INT   cx;
    INT   cy;
    DWORD style;
    ULONG lParam;
} MDICREATESTRUCT32;

typedef struct
{
    ULONG hmenuIn;
    ULONG hmenuNext;
    ULONG hwndNext;
} MDINEXTMENU32;

typedef struct
{
    LONG  lResult;
    LONG  lParam;
    LONG  wParam;
    DWORD message;
    ULONG hwnd;
} CWPRETSTRUCT32;

typedef struct
{
    ULONG hwnd;
    ULONG hwndInsertAfter;
    INT   x;
    INT   y;
    INT   cx;
    INT   cy;
    UINT  flags;
} WINDOWPOS32;

typedef struct
{
    RECT  rgrc[3];
    ULONG lppos;
} NCCALCSIZE_PARAMS32;

typedef struct
{
    UINT  CtlType;
    UINT  CtlID;
    ULONG hwndItem;
    UINT  itemID1;
    ULONG itemData1;
    UINT  itemID2;
    ULONG itemData2;
    DWORD dwLocaleId;
} COMPAREITEMSTRUCT32;

typedef struct
{
    ULONG dwData;
    DWORD cbData;
    ULONG lpData;
} COPYDATASTRUCT32;

typedef struct
{
    UINT   cbSize;
    INT    iContextType;
    INT    iCtrlId;
    ULONG  hItemHandle;
    DWORD  dwContextId;
    POINT  MousePos;
} HELPINFO32;

typedef struct
{
    UINT  CtlType;
    UINT  CtlID;
    UINT  itemID;
    UINT  itemWidth;
    UINT  itemHeight;
    ULONG itemData;
} MEASUREITEMSTRUCT32;

typedef struct
{
    UINT  CtlType;
    UINT  CtlID;
    UINT  itemID;
    UINT  itemAction;
    UINT  itemState;
    ULONG hwndItem;
    ULONG hDC;
    RECT  rcItem;
    ULONG itemData;
} DRAWITEMSTRUCT32;

typedef struct
{
    DWORD cbSize;
    RECT  rcItem;
    RECT  rcButton;
    DWORD stateButton;
    ULONG hwndCombo;
    ULONG hwndItem;
    ULONG hwndList;
} COMBOBOXINFO32;

typedef struct
{
    ULONG lParam;
    ULONG wParam;
    UINT  message;
    ULONG hwnd;
} CWPSTRUCT32;

typedef struct
{
    POINT pt;
    ULONG hwnd;
    UINT  wHitTestCode;
    ULONG dwExtraInfo;
    DWORD mouseData;
} MOUSEHOOKSTRUCTEX32;

typedef struct
{
    POINT pt;
    DWORD mouseData;
    DWORD flags;
    DWORD time;
    ULONG dwExtraInfo;
} MSLLHOOKSTRUCT32;

typedef struct
{
    DWORD  vkCode;
    DWORD  scanCode;
    DWORD  flags;
    DWORD  time;
    ULONG  dwExtraInfo;
} KBDLLHOOKSTRUCT32;

typedef struct
{
    UINT  message;
    UINT  paramL;
    UINT  paramH;
    DWORD time;
    ULONG hwnd;
} EVENTMSG32;

typedef struct
{
    BOOL  fMouse;
    ULONG hWndActive;
} CBTACTIVATESTRUCT32;

typedef struct
{
    UINT  CtlType;
    UINT  CtlID;
    UINT  itemID;
    ULONG hwndItem;
    ULONG itemData;
} DELETEITEMSTRUCT32;

typedef struct
{
    UINT   cbSize;
    UINT   style;
    ULONG  lpfnWndProc;
    INT    cbClsExtra;
    INT    cbWndExtra;
    ULONG  hInstance;
    ULONG  hIcon;
    ULONG  hCursor;
    ULONG  hbrBackground;
    ULONG  lpszMenuName;
    ULONG  lpszClassName;
    ULONG  hIconSm;
} WNDCLASSEXW32;

typedef struct _CLIENTINFO32
{
    ULONG  CI_flags;
    ULONG  cSpins;
    DWORD  dwExpWinVer;
    DWORD  dwCompatFlags;
    DWORD  dwCompatFlags2;
    DWORD  dwTIFlags;
    ULONG  pDeskInfo;
    ULONG  ulClientDelta;
    ULONG  phkCurrent;
    ULONG  fsHooks;
    ULONG  CallbackWnd[2];
    DWORD  dwHookCurrent;
    INT    cInDDEMLCallback;
    ULONG  pClientThreadInfo;
    ULONG  dwHookData;
    DWORD  dwKeyCache;
    BYTE   afKeyState[8];
    DWORD  dwAsyncKeyCache;
    BYTE   afAsyncKeyState[8];
    BYTE   afAsyncKeyStateRecentDow[8];
    ULONG  hKL;
    USHORT CodePage;
    UCHAR  achDbcsCF[2];
    MSG32  msgDbcsCB;
    ULONG  lpdwRegisteredClasses;
    ULONG  Win32ClientInfo3[26];
    ULONG  ppi;
} CLIENTINFO32, *PCLIENTINFO32;

C_ASSERT(sizeof(CLIENTINFO32) == 61 * sizeof(ULONG));

typedef struct _CURSORDATA32
{
    ULONG lpName;
    ULONG lpModName;
    USHORT rt;
    USHORT dummy;
    ULONG CURSORF_flags;
    SHORT xHotspot;
    SHORT yHotspot;
    ULONG hbmMask;
    ULONG hbmColor;
    ULONG hbmAlpha;
    RECT rcBounds;
    ULONG hbmUserAlpha;
    ULONG bpp;
    ULONG cx;
    ULONG cy;
    UINT cpcur;
    UINT cicur;
    ULONG aspcur;
    ULONG aicur;
    ULONG ajifRate;
    UINT iicur;
} CURSORDATA32, *PCURSORDATA32;

typedef struct _LARGE_STRING32
{
    ULONG Length;
    ULONG MaximumLength:31;
    ULONG bAnsi:1;
    ULONG Buffer;
} LARGE_STRING32, *PLARGE_STRING32;

static inline LARGE_STRING *large_str_32to64( LARGE_STRING *str, const LARGE_STRING32 *str32 )
{
    if (!str32) return NULL;
    if (IS_ATOM(str32)) return (PVOID)str32;
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->bAnsi = str32->bAnsi;
    str->Buffer = ULongToPtr( str32->Buffer );
    return str;
}

typedef struct _DRIVER_INFO_2W32
{
    DWORD  cVersion;
    ULONG pName;
    ULONG pEnvironment;
    ULONG pDriverPath;
    ULONG pDataFile;
    ULONG pConfigFile;
} DRIVER_INFO_2W32, *PDRIVER_INFO_2W32;

static 
inline 
PDRIVER_INFO_2W 
DriverInfo2W32To64(PDRIVER_INFO_2W         pDriverInfo64,
                   const DRIVER_INFO_2W32* pDriverInfo32)
{
    pDriverInfo64->cVersion = pDriverInfo32->cVersion;
    pDriverInfo64->pName = UlongToPtr(pDriverInfo32->pName);
    pDriverInfo64->pEnvironment = UlongToPtr(pDriverInfo32->pEnvironment);
    pDriverInfo64->pDriverPath = UlongToPtr(pDriverInfo32->pDriverPath);
    pDriverInfo64->pDataFile = UlongToPtr(pDriverInfo32->pDataFile);
    pDriverInfo64->pConfigFile = UlongToPtr(pDriverInfo32->pConfigFile);
    
    return pDriverInfo64;
}

static 
inline 
PDRIVER_INFO_2W32
DriverInfo2W64To32(const DRIVER_INFO_2W* pDriverInfo64,
                   PDRIVER_INFO_2W32     pDriverInfo32)
{
    pDriverInfo32->cVersion = pDriverInfo64->cVersion;
    pDriverInfo32->pName = PtrToUlong(pDriverInfo64->pName);
    pDriverInfo32->pEnvironment = PtrToUlong(pDriverInfo64->pEnvironment);
    pDriverInfo32->pDriverPath = PtrToUlong(pDriverInfo64->pDriverPath);
    pDriverInfo32->pDataFile = PtrToUlong(pDriverInfo64->pDataFile);
    pDriverInfo32->pConfigFile = PtrToUlong(pDriverInfo64->pConfigFile);
    
    return pDriverInfo32;
}

static inline void set_last_error32(DWORD err)
{
    NtCurrentTeb32()->LastErrorValue = err;
}

#include "callback32.h"

extern PSERVERINFO g_ServerInfo;

typedef struct tagGETCLIPBDATA32
{
    UINT uFmtRet;
    BOOL fGlobalHandle;
    union
    {
        ULONG hLocale;
        ULONG hPalette;
    };
} GETCLIPBDATA32, *PGETCLIPBDATA32;

#endif /* __REACTOS__ */
#endif /* __WOW64WIN_PRIVATE_H */
