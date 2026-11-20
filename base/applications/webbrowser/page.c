/*
 * PROJECT:     ReactOS Browser
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fetches a document and runs it through the engine into a bitmap
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "webbrowser.h"
#include <shlwapi.h>

/* A growable byte buffer for a fetched body or a generated page */
typedef struct _BR_BUFFER
{
    PSTR Data;
    SIZE_T Length;
    SIZE_T Capacity;
} BR_BUFFER, *PBR_BUFFER;

static const char BrHomePage[] =
    "<!doctype html><html><head><title>ReactOS Browser</title>"
    "<style>"
    "body{font-family:Tahoma,sans-serif;margin:0;background:#121214;color:#f2f2f4}"
    "header{padding:28px 20px 18px;background:#3a96dd}"
    "h1{margin:0;font-size:30px;font-weight:600;color:#fff}"
    "p.sub{margin:6px 0 0;font-size:15px;color:#eaf4fc}"
    "ul{list-style:none;margin:0;padding:16px}"
    "li{margin:0 0 12px}"
    "a{display:block;padding:16px 18px;background:#24252a;border-radius:10px;"
    "color:#3a96dd;text-decoration:none;font-size:18px;font-weight:600}"
    "a span{display:block;font-size:13px;color:#9a9ba3;font-weight:400;margin-top:3px}"
    "</style></head><body>"
    "<header><h1>ReactOS Browser</h1>"
    "<p class=sub>DefyBrowser engine, HTML, CSS, layout and paint written from scratch</p></header>"
    "<ul>"
    "<li><a href=\"https://example.com/\">example.com<span>The canonical test page</span></a></li>"
    "<li><a href=\"https://reactos.org/\">ReactOS<span>reactos.org</span></a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/ReactOS\">Wikipedia: ReactOS<span>en.wikipedia.org</span></a></li>"
    "<li><a href=\"https://info.cern.ch/\">info.cern.ch<span>The first website</span></a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite<span>Search without scripts</span></a></li>"
    "</ul></body></html>";

PSTR
BrStringDuplicate(
    _In_z_ PCSTR Text)
{
    SIZE_T Length = strlen(Text) + 1;
    PSTR Copy = malloc(Length);

    if (Copy)
        memcpy(Copy, Text, Length);
    return Copy;
}

static
BOOL
BrBufferAppend(
    _Inout_ PBR_BUFFER Buffer,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ SIZE_T Length)
{
    if (Buffer->Length + Length + 1 > Buffer->Capacity)
    {
        SIZE_T Capacity = max(Buffer->Capacity * 2, Buffer->Length + Length + 1);
        PSTR Grown = realloc(Buffer->Data, Capacity);

        if (!Grown)
            return FALSE;
        Buffer->Data = Grown;
        Buffer->Capacity = Capacity;
    }

    memcpy(Buffer->Data + Buffer->Length, Data, Length);
    Buffer->Length += Length;
    Buffer->Data[Buffer->Length] = '\0';
    return TRUE;
}

static
BOOL
BrBufferAppendText(
    _Inout_ PBR_BUFFER Buffer,
    _In_z_ PCSTR Text)
{
    return BrBufferAppend(Buffer, Text, strlen(Text));
}

/* Copies text into markup with the characters HTML gives meaning escaped */
static
BOOL
BrBufferAppendEscaped(
    _Inout_ PBR_BUFFER Buffer,
    _In_reads_bytes_(Length) PCSTR Text,
    _In_ SIZE_T Length)
{
    SIZE_T i;

    for (i = 0; i < Length; i++)
    {
        PCSTR Entity = NULL;

        switch (Text[i])
        {
            case '&': Entity = "&amp;"; break;
            case '<': Entity = "&lt;"; break;
            case '>': Entity = "&gt;"; break;
            case '"': Entity = "&quot;"; break;
        }

        if (Entity ? !BrBufferAppendText(Buffer, Entity) : !BrBufferAppend(Buffer, &Text[i], 1))
            return FALSE;
    }

    return TRUE;
}

static
BOOL
BrIsHttp(
    _In_z_ PCSTR Url)
{
    return !_strnicmp(Url, "http://", 7) || !_strnicmp(Url, "https://", 8);
}

static
VOID
BrMakeErrorPage(
    _Inout_ PBR_BUFFER Body,
    _In_z_ PCSTR Url,
    _In_z_ PCSTR Reason)
{
    BrBufferAppendText(Body,
        "<!doctype html><html><head><title>Could not open the page</title>"
        "<style>body{font-family:Tahoma,sans-serif;margin:24px;background:#121214;color:#f2f2f4}"
        "h1{font-size:26px;color:#3a96dd}p{color:#9a9ba3;font-size:16px}</style></head><body>"
        "<h1>Could not open the page</h1><p>");
    BrBufferAppendEscaped(Body, Url, strlen(Url));
    BrBufferAppendText(Body, "</p><p>");
    BrBufferAppendEscaped(Body, Reason, strlen(Reason));
    BrBufferAppendText(Body, "</p><p><a href=\"" BR_HOME_URL "\">Start page</a></p></body></html>");
}

static
BOOL
BrLoadFile(
    _In_z_ PCSTR Path,
    _Inout_ PBR_BUFFER Body)
{
    WCHAR WidePath[MAX_PATH];
    BYTE Chunk[4096];
    HANDLE File;
    DWORD Read;
    BOOL Ok = TRUE;

    if (!_strnicmp(Path, "file:///", 8))
        Path += 8;
    if (!MultiByteToWideChar(CP_UTF8, 0, Path, -1, WidePath, ARRAYSIZE(WidePath)))
        return FALSE;

    File = CreateFileW(WidePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    while (Ok && ReadFile(File, Chunk, sizeof(Chunk), &Read, NULL) && Read)
        Ok = BrBufferAppend(Body, Chunk, Read);

    CloseHandle(File);
    return Ok;
}

/* Fills Body with something the HTML parser can take, whatever the resource was */
static
BOOL
BrLoadResource(
    _In_z_ PCSTR Target,
    _Inout_ PBR_BUFFER Body,
    _Outptr_ PSTR *FinalUrl,
    _Out_ PULONG Status)
{
    DfBrHttpResponse Response;

    *FinalUrl = NULL;
    *Status = 0;

    if (!_stricmp(Target, BR_HOME_URL))
    {
        BrBufferAppendText(Body, BrHomePage);
    }
    else if (!_strnicmp(Target, "about:", 6))
    {
        BrBufferAppendText(Body, "<!doctype html><html><head><title></title></head><body></body></html>");
    }
    else if (BrIsHttp(Target))
    {
        ZeroMemory(&Response, sizeof(Response));
        if (DfBrHttpGet(Target, &Response) != DFBR_OK)
        {
            DfBrHttpResponseFree(&Response);
            BrMakeErrorPage(Body, Target, "The server could not be reached. Check the network connection.");
            *FinalUrl = BrStringDuplicate(Target);
            return *FinalUrl != NULL;
        }

        *Status = Response.StatusCode;
        *FinalUrl = BrStringDuplicate(Response.FinalUrl[0] ? Response.FinalUrl : Target);

        if (!_strnicmp(Response.ContentType, "image/", 6))
        {
            /* Show an image on its own the way other browsers do */
            BrBufferAppendText(Body, "<!doctype html><html><body style=\"margin:0;background:#121214\"><img src=\"");
            BrBufferAppendEscaped(Body, *FinalUrl ? *FinalUrl : Target, strlen(*FinalUrl ? *FinalUrl : Target));
            BrBufferAppendText(Body, "\" style=\"max-width:100%\"></body></html>");
        }
        else if (!_strnicmp(Response.ContentType, "text/plain", 10))
        {
            BrBufferAppendText(Body, "<!doctype html><html><body><pre style=\"white-space:pre-wrap\">");
            BrBufferAppendEscaped(Body, (PCSTR)Response.Body, Response.BodyLen);
            BrBufferAppendText(Body, "</pre></body></html>");
        }
        else if (Response.Body && Response.BodyLen)
        {
            BrBufferAppend(Body, Response.Body, Response.BodyLen);
        }

        DfBrHttpResponseFree(&Response);
    }
    else
    {
        if (!BrLoadFile(Target, Body))
            BrMakeErrorPage(Body, Target, "The file could not be opened.");
    }

    if (!*FinalUrl)
        *FinalUrl = BrStringDuplicate(Target);
    if (!Body->Data)
        BrBufferAppendText(Body, "");

    return *FinalUrl != NULL && Body->Data != NULL;
}

static
VOID
BrNodeText(
    _In_ const DfBrHtmlNode *Element,
    _Inout_ PBR_BUFFER Text)
{
    const DfBrHtmlNode *Child;

    for (Child = Element->FirstChild; Child; Child = Child->NextSibling)
    {
        if (Child->Kind == DFBR_HTML_NODE_TEXT && Child->TextLen)
            BrBufferAppend(Text, Child->Text, Child->TextLen);
    }
}

static
VOID
BrAddSheet(
    _Inout_ PBR_PAGE Page,
    _In_reads_bytes_(Length) PCSTR Css,
    _In_ SIZE_T Length)
{
    DfBrCssStylesheet *Sheet;

    if (Page->SheetCount == Page->SheetCapacity)
    {
        ULONG Capacity = Page->SheetCapacity ? Page->SheetCapacity * 2 : 8;
        DfBrCssStylesheet **Grown = realloc(Page->Sheets, Capacity * sizeof(*Grown));

        if (!Grown)
            return;
        Page->Sheets = Grown;
        Page->SheetCapacity = Capacity;
    }

    Sheet = DfBrCssParse(Css, Length, NULL);
    if (Sheet)
        Page->Sheets[Page->SheetCount++] = Sheet;
}

/* Every <style> body, then every linked stylesheet fetched against the page URL */
static
VOID
BrCollectSheets(
    _Inout_ PBR_PAGE Page)
{
    const DfBrHtmlNode *Root = DfBrHtmlDocumentRoot(Page->Document);
    const DfBrHtmlNode *Node;

    if (!Root)
        return;

    for (Node = DfBrHtmlNodeNextPreorder(Root, NULL); Node; Node = DfBrHtmlNodeNextPreorder(Root, Node))
    {
        if (Node->Kind != DFBR_HTML_NODE_ELEMENT)
            continue;

        if (Node->Tag == DFBR_HTML_TAG_STYLE)
        {
            BR_BUFFER Css = { NULL, 0, 0 };

            BrNodeText(Node, &Css);
            if (Css.Length)
                BrAddSheet(Page, Css.Data, Css.Length);
            free(Css.Data);
        }
        else if (Node->Tag == DFBR_HTML_TAG_LINK)
        {
            PCSTR Rel = DfBrHtmlNodeGetAttrValue(Node, "rel");
            PCSTR Href = DfBrHtmlNodeGetAttrValue(Node, "href");
            DfBrHttpResponse Response;
            PSTR Absolute;

            if (!Rel || !Href || !Href[0] || !StrStrIA(Rel, "stylesheet"))
                continue;

            Absolute = DfBrUrlResolve(Page->Url, Href);
            if (!Absolute)
                continue;

            ZeroMemory(&Response, sizeof(Response));
            if (BrIsHttp(Absolute) && DfBrHttpGet(Absolute, &Response) == DFBR_OK &&
                Response.Body && Response.BodyLen)
            {
                BrAddSheet(Page, (PCSTR)Response.Body, Response.BodyLen);
            }

            DfBrHttpResponseFree(&Response);
            free(Absolute);
        }
    }
}

static
f32
BrContentBottom(
    _In_ const DfBrLayoutEngine *Layout)
{
    const DfBrLayoutBox *Root = DfBrLayoutEngineRoot(Layout);
    const DfBrLayoutBox *Box;
    f32 Deepest = 0.0f;

    for (Box = DfBrLayoutBoxNextPreorder(Root, NULL); Box; Box = DfBrLayoutBoxNextPreorder(Root, Box))
    {
        f32 Bottom = Box->ContentRect.Y + Box->ContentRect.H +
                     Box->Padding.Bottom + Box->Border.Bottom + Box->Margin.Bottom;

        if (Bottom > Deepest)
            Deepest = Bottom;
    }

    return Deepest;
}

static
VOID
BrFreeCanvas(
    _Inout_ PBR_PAGE Page)
{
    if (Page->Bitmap)
        DeleteObject(Page->Bitmap);
    Page->Bitmap = NULL;
    ZeroMemory(&Page->Canvas, sizeof(Page->Canvas));
}

/* The engine paints straight into a DIB section, so the window can blit it as is */
static
BOOL
BrAllocateCanvas(
    _Inout_ PBR_PAGE Page,
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    BITMAPINFO Info;
    PVOID Bits;

    BrFreeCanvas(Page);

    ZeroMemory(&Info, sizeof(Info));
    Info.bmiHeader.biSize = sizeof(Info.bmiHeader);
    Info.bmiHeader.biWidth = (LONG)Width;
    Info.bmiHeader.biHeight = -(LONG)Height;
    Info.bmiHeader.biPlanes = 1;
    Info.bmiHeader.biBitCount = 32;
    Info.bmiHeader.biCompression = BI_RGB;

    Page->Bitmap = CreateDIBSection(NULL, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
    if (!Page->Bitmap)
        return FALSE;

    Page->Canvas.Width = Width;
    Page->Canvas.Height = Height;
    Page->Canvas.Pixels = Bits;
    Page->Canvas.OwnsPixels = DFBR_FALSE;
    return TRUE;
}

BOOL
BrPageRelayout(
    _Inout_ PBR_PAGE Page,
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    const DfBrCssStylesheet **Sheets;
    DfBrLayoutViewport Viewport;
    DfBrPaintOptions Options;
    ULONG PageHeight, i;
    f32 Deepest;

    if (!Page->Document || Width < 8 || Height < 8)
        return FALSE;

    if (Page->Layout)
        DfBrLayoutEngineDestroy(Page->Layout);
    if (Page->Style)
        DfBrCssStyleContextDestroy(Page->Style);
    Page->Layout = NULL;
    Page->Style = NULL;
    Page->ViewportWidth = Width;
    Page->ViewportHeight = Height;

    Sheets = malloc((Page->SheetCount + 1) * sizeof(*Sheets));
    if (!Sheets)
        return FALSE;

    Sheets[0] = DfBrCssUserAgentStylesheet();
    for (i = 0; i < Page->SheetCount; i++)
        Sheets[i + 1] = Page->Sheets[i];

    Page->Style = DfBrCssApplyToDocumentEx(Page->Document, Sheets, Page->SheetCount + 1, (f32)Width, (f32)Height);
    free((PVOID)Sheets);
    if (!Page->Style)
        return FALSE;

    Page->Layout = DfBrLayoutBuildWithBase(Page->Document, Page->Style, Page->Url);
    if (!Page->Layout)
        return FALSE;

    Viewport.WidthPx = (f32)Width;
    Viewport.HeightPx = (f32)Height;
    Viewport.DefaultFontSizePx = 16.0f;
    DfBrLayoutRun(Page->Layout, &Viewport);

    Deepest = BrContentBottom(Page->Layout);
    PageHeight = Height;
    if (Deepest > (f32)PageHeight)
        PageHeight = min((ULONG)(Deepest + 0.999f), BR_MAX_PAGE_HEIGHT);

    if (!BrAllocateCanvas(Page, Width, PageHeight))
        return FALSE;

    Options = DfBrPaintDefaultOptions();
    Options.CanvasColor = DfBrRgb(255, 255, 255);
    Options.ClearCanvas = DFBR_TRUE;
    DfBrPaintRender(Page->Layout, &Page->Canvas, &Options);
    GdiFlush();
    return TRUE;
}

DWORD
BrPageLoad(
    _In_z_ PCSTR Target,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _Outptr_result_maybenull_ PBR_PAGE *Result)
{
    BR_BUFFER Body = { NULL, 0, 0 };
    BR_BUFFER Title = { NULL, 0, 0 };
    const DfBrHtmlNode *Root, *TitleNode;
    PBR_PAGE Page;

    *Result = NULL;

    Page = calloc(1, sizeof(*Page));
    if (!Page)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!BrLoadResource(Target, &Body, &Page->Url, &Page->Status))
    {
        free(Body.Data);
        BrPageDestroy(Page);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Page->Document = DfBrHtmlParse(Body.Data, Body.Length, NULL);
    free(Body.Data);
    if (!Page->Document)
    {
        BrPageDestroy(Page);
        return ERROR_INVALID_DATA;
    }

    BrCollectSheets(Page);

    Root = DfBrHtmlDocumentRoot(Page->Document);
    TitleNode = Root ? DfBrHtmlNodeFindFirstByTag(Root, DFBR_HTML_TAG_TITLE) : NULL;
    if (TitleNode)
        BrNodeText(TitleNode, &Title);
    Page->Title = Title.Data ? Title.Data : BrStringDuplicate("");

    if (!BrPageRelayout(Page, Width, Height))
    {
        BrPageDestroy(Page);
        return ERROR_INVALID_DATA;
    }

    *Result = Page;
    return ERROR_SUCCESS;
}

VOID
BrPageDestroy(
    _In_opt_ PBR_PAGE Page)
{
    ULONG i;

    if (!Page)
        return;

    if (Page->Layout)
        DfBrLayoutEngineDestroy(Page->Layout);
    if (Page->Style)
        DfBrCssStyleContextDestroy(Page->Style);
    for (i = 0; i < Page->SheetCount; i++)
        DfBrCssStylesheetDestroy(Page->Sheets[i]);
    free(Page->Sheets);
    if (Page->Document)
        DfBrHtmlDocumentDestroy(Page->Document);

    BrFreeCanvas(Page);
    free(Page->Url);
    free(Page->Title);
    free(Page);
}

/* The URL of the link under a point in page coordinates, resolved against the page */
PSTR
BrPageLinkAt(
    _In_ const BR_PAGE *Page,
    _In_ LONG X,
    _In_ LONG Y)
{
    const DfBrLayoutBox *Box;
    const DfBrHtmlNode *Node = NULL;
    PCSTR Href = NULL;
    PSTR Absolute;

    if (!Page->Layout)
        return NULL;

    /* Inline content sits in anonymous boxes, climb to one that has a DOM node */
    for (Box = DfBrLayoutHitTest(Page->Layout, (f32)X, (f32)Y); Box && !Node; Box = Box->Parent)
        Node = Box->DomNode;

    for (; Node && !Href; Node = Node->Parent)
    {
        if (Node->Kind == DFBR_HTML_NODE_ELEMENT && Node->Tag == DFBR_HTML_TAG_A)
        {
            Href = DfBrHtmlNodeGetAttrValue(Node, "href");
            if (Href && !Href[0])
                Href = NULL;
        }
    }

    /* No script engine here, and a fragment only moves within the page */
    if (!Href || !_strnicmp(Href, "javascript:", 11) || Href[0] == '#')
        return NULL;

    if (strstr(Href, "://") || !_strnicmp(Href, "about:", 6))
        return BrStringDuplicate(Href);

    Absolute = DfBrUrlResolve(Page->Url ? Page->Url : "about:blank", Href);
    return Absolute ? Absolute : BrStringDuplicate(Href);
}
