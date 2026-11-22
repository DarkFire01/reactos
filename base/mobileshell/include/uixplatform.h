/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Graphics headers and small Direct2D helpers that build on ReactOS and the Windows SDK
 */

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#ifdef __REACTOS__
/* ReactOS ships DirectComposition with its DWM; the interface declarations live next to dcomp.dll */
#include <dcompapi.hpp>

EXTERN_C
HRESULT
WINAPI
DCompositionCreateDevice(
    _In_opt_ IDXGIDevice *DxgiDevice,
    _In_ REFIID Iid,
    _Outptr_ VOID **Device);

EXTERN_C
HRESULT
WINAPI
DCompositionCreateDevice2(
    _In_opt_ IUnknown *RenderingDevice,
    _In_ REFIID Iid,
    _Outptr_ VOID **Device);
#else
#include <dcomp.h>
#endif

/* DComp enums travel as INT on ReactOS; this is the linear bitmap interpolation mode */
#ifdef __REACTOS__
#define UIX_BITMAP_INTERPOLATION_LINEAR 1
#else
#define UIX_BITMAP_INTERPOLATION_LINEAR DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR
#endif

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000
#endif

/*
 * Helpers in place of d2d1helper.h, which the ReactOS SDK does not have.
 * Calls use the pointer forms of the Direct2D methods, which both header sets declare.
 */
namespace UixD2D
{

inline
D2D1_COLOR_F
Color(
    _In_ FLOAT Red,
    _In_ FLOAT Green,
    _In_ FLOAT Blue,
    _In_ FLOAT Alpha = 1.0f)
{
    D2D1_COLOR_F Result = { Red, Green, Blue, Alpha };
    return Result;
}

inline
D2D1_RECT_F
Rect(
    _In_ FLOAT Left,
    _In_ FLOAT Top,
    _In_ FLOAT Right,
    _In_ FLOAT Bottom)
{
    D2D1_RECT_F Result = { Left, Top, Right, Bottom };
    return Result;
}

inline
D2D1_POINT_2F
Point(
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    D2D1_POINT_2F Result = { X, Y };
    return Result;
}

inline
D2D1_ROUNDED_RECT
RoundedRect(
    _In_ const D2D1_RECT_F &Bounds,
    _In_ FLOAT RadiusX,
    _In_ FLOAT RadiusY)
{
    D2D1_ROUNDED_RECT Result = { Bounds, RadiusX, RadiusY };
    return Result;
}

inline
D2D1_MATRIX_3X2_F
Matrix(
    _In_ FLOAT M11,
    _In_ FLOAT M12,
    _In_ FLOAT M21,
    _In_ FLOAT M22,
    _In_ FLOAT Dx,
    _In_ FLOAT Dy)
{
    D2D1_MATRIX_3X2_F Result;

    Result.m11 = M11;
    Result.m12 = M12;
    Result.m21 = M21;
    Result.m22 = M22;
    Result.dx = Dx;
    Result.dy = Dy;
    return Result;
}

inline
D2D1_MATRIX_3X2_F
Identity(VOID)
{
    return Matrix(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
}

inline
D2D1_MATRIX_3X2_F
Scale(
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    return Matrix(X, 0.0f, 0.0f, Y, 0.0f, 0.0f);
}

inline
D2D1_MATRIX_3X2_F
Translation(
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    return Matrix(1.0f, 0.0f, 0.0f, 1.0f, X, Y);
}

/* First applied, then Second, the order D2D's operator* uses */
inline
D2D1_MATRIX_3X2_F
Multiply(
    _In_ const D2D1_MATRIX_3X2_F &First,
    _In_ const D2D1_MATRIX_3X2_F &Second)
{
    return Matrix(First.m11 * Second.m11 + First.m12 * Second.m21,
                  First.m11 * Second.m12 + First.m12 * Second.m22,
                  First.m21 * Second.m11 + First.m22 * Second.m21,
                  First.m21 * Second.m12 + First.m22 * Second.m22,
                  First.dx * Second.m11 + First.dy * Second.m21 + Second.dx,
                  First.dx * Second.m12 + First.dy * Second.m22 + Second.dy);
}

/* A premultiplied BGRA bitmap that is only ever drawn into */
inline
D2D1_BITMAP_PROPERTIES1
TargetProperties(VOID)
{
    D2D1_BITMAP_PROPERTIES1 Result = {};

    Result.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Result.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    Result.dpiX = 96.0f;
    Result.dpiY = 96.0f;
    Result.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    return Result;
}

}
