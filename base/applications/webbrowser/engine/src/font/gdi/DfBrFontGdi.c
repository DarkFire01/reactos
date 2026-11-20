/*
 * PROJECT:     ReactOS Browser
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DefyBrowser font backend on GDI, measures and draws with installed fonts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../DfBrFont.h"

#define GDI_FAMILY_MAX      LF_FACESIZE
#define GDI_TEXT_STACK      256

struct DfBrFontFace
{
    struct DfBrFontFace *Next;
    WCHAR Family[GDI_FAMILY_MAX];
    LONG Weight;
    BOOL Italic;
    LONG Height;
    HFONT Font;
    DfBrFontMetrics Metrics;
};

struct DfBrFontEngine
{
    CRITICAL_SECTION Lock;
    HDC Dc;
    struct DfBrFontFace *Faces;

    /* Scratch surface the glyphs are drawn into before they are blended */
    HBITMAP Scratch;
    HGDIOBJ OldScratch;
    DWORD *ScratchBits;
    LONG ScratchWidth;
    LONG ScratchHeight;
};

static struct DfBrFontEngine *GdiEngine;
static LONG GdiEngineOnce;

/* CSS generic families and the installed fonts that stand in for them */
static const struct
{
    const char *Generic;
    const WCHAR *Face;
} GdiGenerics[] =
{
    { "serif", L"Times New Roman" },
    { "sans-serif", L"Tahoma" },
    { "monospace", L"Courier New" },
    { "cursive", L"Comic Sans MS" },
    { "fantasy", L"Impact" },
    { "system-ui", L"Tahoma" },
    { "ui-sans-serif", L"Tahoma" },
    { "ui-serif", L"Times New Roman" },
    { "ui-monospace", L"Courier New" },
    { "-apple-system", L"Tahoma" },
    { "blinkmacsystemfont", L"Tahoma" },
};

static
int
CALLBACK
GdiFoundFamily(
    _In_ const LOGFONTW *Font,
    _In_ const TEXTMETRICW *Metric,
    _In_ DWORD Type,
    _In_ LPARAM Param)
{
    UNREFERENCED_PARAMETER(Font);
    UNREFERENCED_PARAMETER(Metric);
    UNREFERENCED_PARAMETER(Type);

    *(BOOL *)Param = TRUE;
    return 0;
}

static
BOOL
GdiIsInstalled(
    _In_ HDC Dc,
    _In_z_ const WCHAR *Family)
{
    LOGFONTW Font;
    BOOL Found = FALSE;

    ZeroMemory(&Font, sizeof(Font));
    Font.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(Font.lfFaceName, Family, LF_FACESIZE);
    EnumFontFamiliesExW(Dc, &Font, GdiFoundFamily, (LPARAM)&Found, 0);
    return Found;
}

/* Turns one CSS family name into an installed face name, or fails */
static
BOOL
GdiPickFamily(
    _In_ HDC Dc,
    _In_z_ const char *Name,
    _Out_writes_(GDI_FAMILY_MAX) WCHAR *Family)
{
    char Clean[GDI_FAMILY_MAX];
    size_t Length, i;

    while (*Name == ' ' || *Name == '"' || *Name == '\'')
        Name++;
    Length = strlen(Name);
    while (Length && (Name[Length - 1] == ' ' || Name[Length - 1] == '"' || Name[Length - 1] == '\''))
        Length--;
    if (!Length || Length >= sizeof(Clean))
        return FALSE;

    memcpy(Clean, Name, Length);
    Clean[Length] = '\0';

    for (i = 0; i < ARRAYSIZE(GdiGenerics); i++)
    {
        if (!_stricmp(Clean, GdiGenerics[i].Generic))
        {
            lstrcpynW(Family, GdiGenerics[i].Face, GDI_FAMILY_MAX);
            return TRUE;
        }
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, Clean, -1, Family, GDI_FAMILY_MAX))
        return FALSE;

    return GdiIsInstalled(Dc, Family);
}

static
VOID
GdiReadMetrics(
    _In_ HDC Dc,
    _Inout_ struct DfBrFontFace *Face)
{
    OUTLINETEXTMETRICW Outline;
    TEXTMETRICW Text;
    f32 Size = (f32)Face->Height;

    GetTextMetricsW(Dc, &Text);
    Face->Metrics.AscentPx = (f32)Text.tmAscent;
    Face->Metrics.DescentPx = (f32)Text.tmDescent;
    Face->Metrics.LineGapPx = (f32)Text.tmExternalLeading;
    Face->Metrics.XHeightPx = Size * 0.5f;
    Face->Metrics.CapHeightPx = Size * 0.7f;

    ZeroMemory(&Outline, sizeof(Outline));
    Outline.otmSize = sizeof(Outline);
    if (GetOutlineTextMetricsW(Dc, sizeof(Outline), &Outline))
    {
        if (Outline.otmsXHeight > 0)
            Face->Metrics.XHeightPx = (f32)Outline.otmsXHeight;
        if (Outline.otmsCapEmHeight > 0)
            Face->Metrics.CapHeightPx = (f32)Outline.otmsCapEmHeight;
    }
}

static
struct DfBrFontEngine *
GdiCreateEngine(VOID)
{
    struct DfBrFontEngine *Engine = calloc(1, sizeof(*Engine));

    if (!Engine)
        return NULL;

    Engine->Dc = CreateCompatibleDC(NULL);
    if (!Engine->Dc)
    {
        free(Engine);
        return NULL;
    }

    InitializeCriticalSection(&Engine->Lock);
    SetBkMode(Engine->Dc, TRANSPARENT);
    SetTextColor(Engine->Dc, RGB(255, 255, 255));
    SetTextAlign(Engine->Dc, TA_LEFT | TA_BASELINE | TA_NOUPDATECP);
    return Engine;
}

DfBrFontEngine *
DfBrFontGetDefaultEngine(void)
{
    struct DfBrFontEngine *Engine;

    if (GdiEngine)
        return GdiEngine;

    /* The first caller builds it, any other caller waits for that one */
    if (InterlockedCompareExchange(&GdiEngineOnce, 1, 0) == 0)
    {
        Engine = GdiCreateEngine();
        InterlockedExchangePointer((PVOID *)&GdiEngine, Engine);
        InterlockedExchange(&GdiEngineOnce, Engine ? 2 : 0);
        return Engine;
    }

    while (GdiEngineOnce == 1)
        Sleep(0);
    return GdiEngine;
}

DfBrFontFace *
DfBrFontEngineResolve(
    DfBrFontEngine *Engine,
    const char *const *Families,
    usize FamilyCount,
    i32 Weight,
    DfBrFontStyle Style,
    f32 SizePx)
{
    struct DfBrFontFace *Face;
    WCHAR Family[GDI_FAMILY_MAX];
    LOGFONTW Font;
    LONG Height;
    BOOL Italic = (Style != DFBR_FONT_STYLE_NORMAL);
    BOOL Picked = FALSE;
    usize i;
    HGDIOBJ Old;

    if (!Engine)
        return NULL;

    Height = (LONG)(SizePx + 0.5f);
    if (Height < 1)
        Height = 1;
    if (Weight < 100)
        Weight = 400;

    EnterCriticalSection(&Engine->Lock);

    for (i = 0; i < FamilyCount && !Picked; i++)
    {
        if (Families[i])
            Picked = GdiPickFamily(Engine->Dc, Families[i], Family);
    }
    if (!Picked)
        lstrcpynW(Family, L"Tahoma", GDI_FAMILY_MAX);

    for (Face = Engine->Faces; Face; Face = Face->Next)
    {
        if (Face->Height == Height && Face->Weight == Weight && Face->Italic == Italic &&
            !lstrcmpiW(Face->Family, Family))
        {
            LeaveCriticalSection(&Engine->Lock);
            return Face;
        }
    }

    Face = calloc(1, sizeof(*Face));
    if (!Face)
    {
        LeaveCriticalSection(&Engine->Lock);
        return NULL;
    }

    ZeroMemory(&Font, sizeof(Font));
    Font.lfHeight = -Height;
    Font.lfWeight = Weight;
    Font.lfItalic = (BYTE)Italic;
    Font.lfCharSet = DEFAULT_CHARSET;
    Font.lfOutPrecision = OUT_TT_PRECIS;
    Font.lfQuality = ANTIALIASED_QUALITY;
    lstrcpynW(Font.lfFaceName, Family, LF_FACESIZE);

    Face->Font = CreateFontIndirectW(&Font);
    if (!Face->Font)
    {
        free(Face);
        LeaveCriticalSection(&Engine->Lock);
        return NULL;
    }

    lstrcpynW(Face->Family, Family, GDI_FAMILY_MAX);
    Face->Weight = Weight;
    Face->Italic = Italic;
    Face->Height = Height;

    Old = SelectObject(Engine->Dc, Face->Font);
    GdiReadMetrics(Engine->Dc, Face);
    SelectObject(Engine->Dc, Old);

    Face->Next = Engine->Faces;
    Engine->Faces = Face;

    LeaveCriticalSection(&Engine->Lock);
    return Face;
}

/* UTF-8 to UTF-16, on the stack when the run is short */
static
WCHAR *
GdiWiden(
    _In_reads_bytes_(Length) const char *Utf8,
    _In_ usize Length,
    _Out_writes_(GDI_TEXT_STACK) WCHAR *Stack,
    _Out_ int *Count)
{
    WCHAR *Text = Stack;
    int Needed;

    *Count = 0;
    if (!Length || Length > 0x7FFFFFFF)
        return NULL;

    Needed = MultiByteToWideChar(CP_UTF8, 0, Utf8, (int)Length, NULL, 0);
    if (Needed <= 0)
        return NULL;

    if (Needed > GDI_TEXT_STACK)
    {
        Text = malloc(Needed * sizeof(*Text));
        if (!Text)
            return NULL;
    }

    *Count = MultiByteToWideChar(CP_UTF8, 0, Utf8, (int)Length, Text, Needed);
    return Text;
}

f32
DfBrFontFaceMeasureUtf8(
    DfBrFontFace *Face,
    const char *Utf8,
    usize Len)
{
    struct DfBrFontEngine *Engine = GdiEngine;
    WCHAR Stack[GDI_TEXT_STACK];
    WCHAR *Text;
    SIZE Extent = { 0, 0 };
    HGDIOBJ Old;
    int Count;

    if (!Face || !Engine || !Utf8)
        return 0.0f;

    Text = GdiWiden(Utf8, Len, Stack, &Count);
    if (!Text)
        return 0.0f;

    EnterCriticalSection(&Engine->Lock);
    Old = SelectObject(Engine->Dc, Face->Font);
    GetTextExtentPoint32W(Engine->Dc, Text, Count, &Extent);
    SelectObject(Engine->Dc, Old);
    LeaveCriticalSection(&Engine->Lock);

    if (Text != Stack)
        free(Text);
    return (f32)Extent.cx;
}

void
DfBrFontFaceGetMetrics(
    const DfBrFontFace *Face,
    DfBrFontMetrics *Out)
{
    if (!Out)
        return;

    if (!Face)
    {
        memset(Out, 0, sizeof(*Out));
        return;
    }

    *Out = Face->Metrics;
}

static
BOOL
GdiEnsureScratch(
    _Inout_ struct DfBrFontEngine *Engine,
    _In_ LONG Width,
    _In_ LONG Height)
{
    BITMAPINFO Info;
    HBITMAP Bitmap;
    PVOID Bits;

    if (Engine->Scratch && Engine->ScratchWidth >= Width && Engine->ScratchHeight >= Height)
        return TRUE;

    Width = max(Width, Engine->ScratchWidth);
    Height = max(Height, Engine->ScratchHeight);

    ZeroMemory(&Info, sizeof(Info));
    Info.bmiHeader.biSize = sizeof(Info.bmiHeader);
    Info.bmiHeader.biWidth = Width;
    Info.bmiHeader.biHeight = -Height;
    Info.bmiHeader.biPlanes = 1;
    Info.bmiHeader.biBitCount = 32;
    Info.bmiHeader.biCompression = BI_RGB;

    Bitmap = CreateDIBSection(Engine->Dc, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
    if (!Bitmap)
        return FALSE;

    if (Engine->Scratch)
    {
        SelectObject(Engine->Dc, Engine->OldScratch);
        DeleteObject(Engine->Scratch);
    }

    Engine->OldScratch = SelectObject(Engine->Dc, Bitmap);
    Engine->Scratch = Bitmap;
    Engine->ScratchBits = Bits;
    Engine->ScratchWidth = Width;
    Engine->ScratchHeight = Height;
    return TRUE;
}

/* Draws white on black, then uses the coverage as alpha for the text color */
void
DfBrFontFaceDrawUtf8(
    DfBrFontFace *Face,
    const char *Utf8,
    usize Len,
    DfBrBitmap *Target,
    f32 PenX,
    f32 BaselineY,
    DfBrColor Color)
{
    struct DfBrFontEngine *Engine = GdiEngine;
    WCHAR Stack[GDI_TEXT_STACK];
    WCHAR *Text;
    SIZE Extent;
    HGDIOBJ Old;
    LONG Width, Height, Ascent, Left, Top, X, Y;
    int Count;

    if (!Face || !Engine || !Utf8 || !Target || !Target->Pixels || !Color.A)
        return;

    Text = GdiWiden(Utf8, Len, Stack, &Count);
    if (!Text)
        return;

    EnterCriticalSection(&Engine->Lock);
    Old = SelectObject(Engine->Dc, Face->Font);
    if (!GetTextExtentPoint32W(Engine->Dc, Text, Count, &Extent))
        goto Done;

    /* Italics and overhangs reach past the advance, leave room on both sides */
    Ascent = (LONG)(Face->Metrics.AscentPx + 0.5f);
    Width = Extent.cx + Face->Height + 4;
    Height = Ascent + (LONG)(Face->Metrics.DescentPx + 0.5f) + 4;
    Left = (LONG)PenX - Face->Height / 2;
    Top = (LONG)(BaselineY + 0.5f) - Ascent - 2;

    if (Left >= (LONG)Target->Width || Top >= (LONG)Target->Height ||
        Left + Width <= 0 || Top + Height <= 0)
    {
        goto Done;
    }

    if (!GdiEnsureScratch(Engine, Width, Height))
        goto Done;

    GdiFlush();
    for (Y = 0; Y < Height; Y++)
        ZeroMemory(&Engine->ScratchBits[Y * Engine->ScratchWidth], Width * sizeof(DWORD));

    SelectObject(Engine->Dc, Face->Font);
    ExtTextOutW(Engine->Dc, (LONG)PenX - Left, Ascent + 2, 0, NULL, Text, (UINT)Count, NULL);
    GdiFlush();

    for (Y = 0; Y < Height; Y++)
    {
        LONG DestY = Top + Y;
        const DWORD *Row;
        DfBrColor *Dest;

        if (DestY < 0 || DestY >= (LONG)Target->Height)
            continue;

        Row = &Engine->ScratchBits[Y * Engine->ScratchWidth];
        Dest = &Target->Pixels[(usize)DestY * Target->Width];

        for (X = 0; X < Width; X++)
        {
            LONG DestX = Left + X;
            ULONG Coverage, Alpha;
            DfBrColor *Pixel;

            if (DestX < 0 || DestX >= (LONG)Target->Width)
                continue;

            /* Grayscale antialiasing leaves the same value in every channel */
            Coverage = (Row[X] >> 8) & 0xFF;
            if (!Coverage)
                continue;

            Alpha = Coverage * Color.A / 255;
            Pixel = &Dest[DestX];
            Pixel->R = (u8)((Color.R * Alpha + Pixel->R * (255 - Alpha)) / 255);
            Pixel->G = (u8)((Color.G * Alpha + Pixel->G * (255 - Alpha)) / 255);
            Pixel->B = (u8)((Color.B * Alpha + Pixel->B * (255 - Alpha)) / 255);
            Pixel->A = (u8)(Alpha + Pixel->A * (255 - Alpha) / 255);
        }
    }

Done:
    SelectObject(Engine->Dc, Old);
    LeaveCriticalSection(&Engine->Lock);

    if (Text != Stack)
        free(Text);
}
