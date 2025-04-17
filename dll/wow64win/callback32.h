/*
 * Wine WOW64 ReactOS port 
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64win.dll
 * FILE:            dll/wow64win/callback32.h
 * PROGRAMMER:      Marcin Jabłoński
 */

#pragma once

typedef struct _WINDOWPROC_CALLBACK_ARGUMENTS32
{
    ULONG Proc;
    ULONG IsAnsiProc;
    ULONG Wnd;
    UINT Msg;
    ULONG wParam;
    ULONG lParam;
    INT lParamBufferSize;
    ULONG Result;
} WINDOWPROC_CALLBACK_ARGUMENTS32, *PWINDOWPROC_CALLBACK_ARGUMENTS32;

typedef struct _EVENTPROC_CALLBACK_ARGUMENTS32
{
    ULONG hook;
    DWORD event;
    ULONG hwnd; 
    LONG idObject;
    LONG idChild;
    DWORD dwEventThread;
    DWORD dwmsEventTime;
    ULONG Proc;
    INT Mod;
    ULONG offPfn;
} EVENTPROC_CALLBACK_ARGUMENTS32, *PEVENTPROC_CALLBACK_ARGUMENTS32;

typedef struct _LOADMENU_CALLBACK_ARGUMENTS32
{
    ULONG hModule;
    ULONG InterSource;
    WCHAR MenuName[1];
} LOADMENU_CALLBACK_ARGUMENTS32, *PLOADMENU_CALLBACK_ARGUMENTS32;

typedef struct _COPYIMAGE_CALLBACK_ARGUMENTS32
{
    ULONG hImage;
    UINT uType;
    int cxDesired;
    int cyDesired;
    UINT fuFlags;
} COPYIMAGE_CALLBACK_ARGUMENTS32, *PCOPYIMAGE_CALLBACK_ARGUMENTS32;

typedef struct _CLIENT_LOAD_LIBRARY_ARGUMENTS32
{
    UNICODE_STRING32 strLibraryName;
    UNICODE_STRING32 strInitFuncName;
    BOOL Unload;
    BOOL ApiHook;
} CLIENT_LOAD_LIBRARY_ARGUMENTS32, *PCLIENT_LOAD_LIBRARY_ARGUMENTS32;

typedef struct _LPK_CALLBACK_ARGUMENTS32
{
    ULONG lpString;
    ULONG hdc;
    INT x;
    INT y;
    UINT flags;
    RECT rect;
    UINT count;
    BOOL bRect;
} LPK_CALLBACK_ARGUMENTS32, *PLPK_CALLBACK_ARGUMENTS32;
