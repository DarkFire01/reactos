/*
 * PROJECT:     ReactOS Browser
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Touch web browser over the DefyBrowser engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

#include "common/DfBrTypes.h"
#include "core/DfBrBitmap.h"
#include "html/DfBrHtml.h"
#include "css/DfBrCss.h"
#include "layout/DfBrLayout.h"
#include "paint/DfBrPaint.h"
#include "net/DfBrNet.h"

#define BR_HOME_URL             "about:home"

/* Tall pages stop here, the page is one bitmap */
#define BR_MAX_PAGE_HEIGHT      16384

/* One loaded document, styled, laid out and painted at one viewport */
typedef struct _BR_PAGE
{
    DfBrHtmlDocument *Document;
    DfBrCssStylesheet **Sheets;
    ULONG SheetCount;
    ULONG SheetCapacity;
    DfBrCssStyleContext *Style;
    DfBrLayoutEngine *Layout;

    HBITMAP Bitmap;
    DfBrBitmap Canvas;
    ULONG ViewportWidth;
    ULONG ViewportHeight;

    PSTR Url;
    PSTR Title;
    ULONG Status;
} BR_PAGE, *PBR_PAGE;

DWORD
BrPageLoad(
    _In_z_ PCSTR Target,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _Outptr_result_maybenull_ PBR_PAGE *Page);

BOOL
BrPageRelayout(
    _Inout_ PBR_PAGE Page,
    _In_ ULONG Width,
    _In_ ULONG Height);

VOID
BrPageDestroy(
    _In_opt_ PBR_PAGE Page);

PSTR
BrPageLinkAt(
    _In_ const BR_PAGE *Page,
    _In_ LONG X,
    _In_ LONG Y);

PSTR
BrStringDuplicate(
    _In_z_ PCSTR Text);
