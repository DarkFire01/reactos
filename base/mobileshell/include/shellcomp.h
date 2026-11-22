/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Shell host and component contracts
 */

#pragma once

#include "coremsg.h"
#include "uix.h"

#ifdef SHELLCOMP_EXPORTS
#define SHELLCOMP_API __declspec(dllexport)
#else
#define SHELLCOMP_API __declspec(dllimport)
#endif

/* Z ordered composition bands the host owns, bottom first */
typedef enum _SHELL_BAND
{
    ShellBandContent = 0,
    ShellBandChrome,
    ShellBandOverlay,
    ShellBandCount
} SHELL_BAND;

class IShellComponent;

class IShellHost
{
public:
    virtual IMessageSession *STDMETHODCALLTYPE GetSession(VOID) = 0;
    virtual IUixEngine *STDMETHODCALLTYPE GetEngine(VOID) = 0;

    virtual IDCompositionVisual *STDMETHODCALLTYPE GetBand(
        _In_ SHELL_BAND Band) = 0;

    /* Full path of a markup file in the active skin */
    virtual BOOL STDMETHODCALLTYPE GetSkinFile(
        _In_z_ PCWSTR Name,
        _Out_writes_z_(Count) PWSTR Path,
        _In_ SIZE_T Count) = 0;

    virtual PCWSTR STDMETHODCALLTYPE GetThemeFile(VOID) = 0;
    virtual PCWSTR STDMETHODCALLTYPE GetConfigFile(VOID) = 0;
    virtual PCWSTR STDMETHODCALLTYPE GetBinDirectory(VOID) = 0;

    virtual VOID STDMETHODCALLTYPE GetCanvas(
        _Out_ FLOAT *Width,
        _Out_ FLOAT *Height) = 0;

    virtual VOID STDMETHODCALLTYPE RequestFrame(VOID) = 0;

    virtual VOID STDMETHODCALLTYPE ExecuteCommand(
        _In_z_ PCWSTR Command) = 0;

    /* Posts Data to Component->UiMessage on the UI thread, safe from any thread */
    virtual BOOL STDMETHODCALLTYPE PostUiMessage(
        _In_ IShellComponent *Component,
        _In_ ULONG Code,
        _In_opt_ PVOID Data) = 0;

    /* The shell window, native app windows are stacked right below it */
    virtual HWND STDMETHODCALLTYPE GetWindow(VOID) = 0;
};

class IShellComponent
{
public:
    virtual VOID STDMETHODCALLTYPE Release(VOID) = 0;

    virtual HRESULT STDMETHODCALLTYPE Initialize(
        _In_ IShellHost *Host) = 0;

    /* Canvas or scale changed */
    virtual VOID STDMETHODCALLTYPE Layout(VOID) = 0;

    /* Skin, theme or markup reloaded */
    virtual HRESULT STDMETHODCALLTYPE Reload(VOID) = 0;

    virtual BOOL STDMETHODCALLTYPE PointerEvent(
        _In_ UIX_POINTER Kind,
        _In_ FLOAT X,
        _In_ FLOAT Y) = 0;

    /* Delta in wheel notches, positive away from the user */
    virtual BOOL STDMETHODCALLTYPE Wheel(
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_ FLOAT Delta) = 0;

    virtual BOOL STDMETHODCALLTYPE Command(
        _In_z_ PCWSTR Command) = 0;

    /* Once a second */
    virtual VOID STDMETHODCALLTYPE Tick(VOID) = 0;

    virtual VOID STDMETHODCALLTYPE UiMessage(
        _In_ ULONG Code,
        _In_opt_ PVOID Data) = 0;

    virtual VOID STDMETHODCALLTYPE Shutdown(VOID) = 0;
};

typedef HRESULT (WINAPI *PSHELL_CREATE_COMPONENT)(
    _Outptr_ IShellComponent **Component);

#define SHELL_CREATE_COMPONENT_NAME "ShellCreateComponent"
