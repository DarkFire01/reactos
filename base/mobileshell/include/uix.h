/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     UIX markup runtime and co-render engine
 */

#pragma once

#include <windows.h>
#include "uixplatform.h"

#ifdef UIXRENDER_EXPORTS
#define UIX_API __declspec(dllexport)
#else
#define UIX_API __declspec(dllimport)
#endif

typedef struct _UIX_RECT
{
    FLOAT Left;
    FLOAT Top;
    FLOAT Right;
    FLOAT Bottom;
} UIX_RECT, *PUIX_RECT;

typedef enum _UIX_POINTER
{
    UixPointerDown,
    UixPointerMove,
    UixPointerUp,
    UixPointerCancel,

    /* Pressed and held without moving: cancels the tap, reports HoldCommand */
    UixPointerHold
} UIX_POINTER;

class IUixScene
{
public:
    virtual ULONG STDMETHODCALLTYPE AddRef(VOID) = 0;
    virtual ULONG STDMETHODCALLTYPE Release(VOID) = 0;

    /* Shell mode: builds the layer visuals under Parent */
    virtual HRESULT STDMETHODCALLTYPE Attach(
        _In_ IDCompositionVisual *Parent) = 0;

    /* Size of the scene in logical units */
    virtual VOID STDMETHODCALLTYPE Layout(
        _In_ FLOAT Width,
        _In_ FLOAT Height) = 0;

    virtual VOID STDMETHODCALLTYPE SetBinding(
        _In_z_ PCWSTR Name,
        _In_opt_z_ PCWSTR Value) = 0;

    virtual BOOL STDMETHODCALLTYPE SetVisible(
        _In_z_ PCWSTR Element,
        _In_ BOOL Visible) = 0;

    virtual BOOL STDMETHODCALLTYPE IsVisible(
        _In_z_ PCWSTR Element) = 0;

    virtual BOOL STDMETHODCALLTYPE GetElementRect(
        _In_z_ PCWSTR Element,
        _Out_ PUIX_RECT Rect) = 0;

    /* Value of an attribute on a named element, or on the scene root when Element is NULL */
    virtual PCWSTR STDMETHODCALLTYPE GetAttribute(
        _In_opt_z_ PCWSTR Element,
        _In_z_ PCWSTR Attribute) = 0;

    virtual BOOL STDMETHODCALLTYPE SetSelected(
        _In_opt_z_ PCWSTR Element) = 0;

    /* Moves the selection through Selectable elements, returns FALSE at the ends */
    virtual BOOL STDMETHODCALLTYPE MoveSelection(
        _In_ INT Delta) = 0;

    virtual PCWSTR STDMETHODCALLTYPE GetSelectedCommand(VOID) = 0;

    /*
     * Press feedback and command routing. Returns TRUE when an interactive element
     * took the pointer. Command is set on the release that completes a tap.
     */
    virtual BOOL STDMETHODCALLTYPE PointerEvent(
        _In_ UIX_POINTER Kind,
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _Outptr_result_maybenull_z_ PCWSTR *Command) = 0;

    /* Draws the whole tree flat into Context, used by apps and snapshots */
    virtual HRESULT STDMETHODCALLTYPE RenderTo(
        _In_ ID2D1DeviceContext *Context,
        _In_ FLOAT Scale) = 0;

    virtual VOID STDMETHODCALLTYPE Invalidate(VOID) = 0;

    /*
     * Fills a Repeater with Count copies of its template. Item I field F is
     * Values[I * FieldCount + F], visible to the template as {Item.F}. {Item.Index} is implied.
     */
    virtual HRESULT STDMETHODCALLTYPE SetItems(
        _In_z_ PCWSTR Repeater,
        _In_ ULONG Count,
        _In_ ULONG FieldCount,
        _In_reads_(FieldCount) PCWSTR const *Fields,
        _In_reads_opt_(Count * FieldCount) PCWSTR const *Values) = 0;

    /* Scrolls the Scroll element under the point, Delta in wheel notches */
    virtual BOOL STDMETHODCALLTYPE Wheel(
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_ FLOAT Delta) = 0;

    virtual VOID STDMETHODCALLTYPE ResetScroll(
        _In_z_ PCWSTR Element) = 0;

    /* Offset is clamped, so a large value scrolls to the end */
    virtual VOID STDMETHODCALLTYPE ScrollTo(
        _In_z_ PCWSTR Element,
        _In_ FLOAT Offset) = 0;

    /* Bounds of the pressed or selected element, FALSE when there is none */
    virtual BOOL STDMETHODCALLTYPE GetSelectedRect(
        _Out_ PUIX_RECT Rect) = 0;
};

class IUixEngine
{
public:
    virtual ULONG STDMETHODCALLTYPE AddRef(VOID) = 0;
    virtual ULONG STDMETHODCALLTYPE Release(VOID) = 0;

    /* Window is NULL for an offscreen engine (apps) */
    virtual HRESULT STDMETHODCALLTYPE Startup(
        _In_opt_ HWND Window) = 0;

    virtual VOID STDMETHODCALLTYPE SetScale(
        _In_ FLOAT Scale) = 0;

    virtual FLOAT STDMETHODCALLTYPE GetScale(VOID) = 0;

    /* Loads a theme.xml, its Base chain first */
    virtual HRESULT STDMETHODCALLTYPE LoadTheme(
        _In_z_ PCWSTR ThemeFile) = 0;

    virtual HRESULT STDMETHODCALLTYPE CreateScene(
        _In_z_ PCWSTR MarkupFile,
        _Outptr_ IUixScene **Scene) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetRootVisual(
        _Outptr_ IDCompositionVisual **Visual) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetCompositionDevice(
        _Outptr_ IDCompositionDevice **Device) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetDirect3DDevice(
        _Outptr_ ID3D11Device **Device) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetDirect2DContext(
        _Outptr_ ID2D1DeviceContext **Context) = 0;

    /* Copies a texture into a composition surface at its origin */
    virtual HRESULT STDMETHODCALLTYPE CopyToSurface(
        _In_ IDCompositionSurface *Surface,
        _In_ ID3D11Texture2D *Source,
        _In_ UINT Width,
        _In_ UINT Height) = 0;

    /* Co-render frame, driven by the host in this order */
    virtual HRESULT STDMETHODCALLTYPE PreRender(VOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginRender(VOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE Render(VOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndRender(VOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE PostPresent(VOID) = 0;

    /* TRUE when some attached scene has dirty layers */
    virtual BOOL STDMETHODCALLTYPE ShouldRender(VOID) = 0;
};

EXTERN_C
UIX_API
HRESULT
WINAPI
UixCreateEngine(
    _Outptr_ IUixEngine **Engine);
