/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     UIXRender private definitions
 */

#pragma once

#define UIXRENDER_EXPORTS
#include <uix.h>
#include <uixptr.h>
#include <uixxml.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

#define UIX_INFINITE 1000000.0f

/* Theme */

typedef enum _UIX_BRUSH_KIND
{
    UixBrushSolid,
    UixBrushGradient,
    UixBrushImage
} UIX_BRUSH_KIND;

typedef enum _UIX_STRETCH
{
    UixStretchUniform,
    UixStretchFill,
    UixStretchUniformToFill,
    UixStretchNone
} UIX_STRETCH;

typedef struct _UIX_BRUSH_DEF
{
    UIX_BRUSH_KIND Kind;
    D2D1_COLOR_F Color;
    std::vector<D2D1_GRADIENT_STOP> Stops;
    BOOL Horizontal;
    std::wstring Image;
    UIX_STRETCH Stretch;
} UIX_BRUSH_DEF, *PUIX_BRUSH_DEF;

typedef struct _UIX_FONT_DEF
{
    std::wstring Family;
    FLOAT Size;
    DWRITE_FONT_WEIGHT Weight;
    DWRITE_FONT_STYLE Style;
} UIX_FONT_DEF;

typedef struct _UIX_IMAGE_DEF
{
    std::wstring Path;
    FLOAT Scale;
} UIX_IMAGE_DEF;

typedef struct _UIX_IMAGE_SET
{
    std::wstring Prefix;
    std::wstring Directory;
    FLOAT Scale;
} UIX_IMAGE_SET;

class CUixTheme
{
public:
    HRESULT
    Load(
        _In_z_ PCWSTR ThemeFile);

    VOID
    Clear(VOID);

    BOOL
    FindColor(
        _In_z_ PCWSTR Name,
        _Out_ D2D1_COLOR_F *Color) const;

    const UIX_BRUSH_DEF *
    FindBrush(
        _In_z_ PCWSTR Name) const;

    const UIX_FONT_DEF *
    FindFont(
        _In_z_ PCWSTR Name) const;

    BOOL
    FindImage(
        _In_z_ PCWSTR Name,
        _Out_ UIX_IMAGE_DEF *Image) const;

    BOOL
    FindMetric(
        _In_z_ PCWSTR Name,
        _Out_ FLOAT *Value) const;

    PCWSTR
    FindString(
        _In_z_ PCWSTR Name) const;

    std::wstring m_Name;

private:
    HRESULT
    LoadOne(
        _In_z_ PCWSTR ThemeFile,
        _In_ ULONG Depth);

    BOOL
    ParseColorValue(
        _In_z_ PCWSTR Value,
        _Out_ D2D1_COLOR_F *Color) const;

    std::map<std::wstring, D2D1_COLOR_F> m_Colors;
    std::map<std::wstring, UIX_BRUSH_DEF> m_Brushes;
    std::map<std::wstring, UIX_FONT_DEF> m_Fonts;
    std::map<std::wstring, UIX_IMAGE_DEF> m_Images;
    std::map<std::wstring, FLOAT> m_Metrics;
    std::map<std::wstring, std::wstring> m_Strings;
    std::vector<UIX_IMAGE_SET> m_ImageSets;
};

BOOL
UixParseColor(
    _In_z_ PCWSTR Text,
    _Out_ D2D1_COLOR_F *Color);

/* Markup elements */

typedef enum _UIX_KIND
{
    UixKindElement,
    UixKindText,
    UixKindImage
} UIX_KIND;

typedef enum _UIX_LAYOUT
{
    UixLayoutOverlay,
    UixLayoutStack,
    UixLayoutRow,
    UixLayoutWrap,
    UixLayoutTiles
} UIX_LAYOUT;

typedef enum _UIX_ALIGN
{
    UixAlignStretch,
    UixAlignStart,
    UixAlignCenter,
    UixAlignEnd
} UIX_ALIGN;

typedef enum _UIX_LENGTH_KIND
{
    UixLengthAuto,
    UixLengthFixed,
    UixLengthStar
} UIX_LENGTH_KIND;

typedef struct _UIX_LENGTH
{
    UIX_LENGTH_KIND Kind;
    FLOAT Value;
} UIX_LENGTH;

typedef struct _UIX_THICKNESS
{
    FLOAT Left;
    FLOAT Top;
    FLOAT Right;
    FLOAT Bottom;
} UIX_THICKNESS;

typedef struct _UIX_ELEMENT
{
    UIX_KIND Kind;
    std::wstring Name;
    std::vector<std::pair<std::wstring, std::wstring>> Attributes;
    std::vector<std::wstring> Bindings;
    std::vector<struct _UIX_ELEMENT *> Children;
    struct _UIX_ELEMENT *Parent;
    BOOL Visible;
    BOOL IsLayer;
    BOOL Interactive;
    BOOL Selectable;
    BOOL Momentary;
    BOOL Scroll;
    FLOAT ScrollOffset;
    FLOAT Extent;

    /* Repeaters keep their template, generated items keep their values */
    std::shared_ptr<UIX_XML_NODE> Template;
    std::map<std::wstring, std::wstring> Item;
    BOOL Generated;

    /* Resolved each layout pass */
    UIX_LAYOUT Layout;
    UIX_LENGTH Width;
    UIX_LENGTH Height;
    FLOAT OffsetX;
    FLOAT OffsetY;
    UIX_THICKNESS Margin;
    UIX_THICKNESS Padding;
    UIX_THICKNESS Border;
    UIX_ALIGN HAlign;
    UIX_ALIGN VAlign;
    FLOAT DesiredWidth;
    FLOAT DesiredHeight;
    UIX_RECT Rect;

    /* Cell and span inside a Tiles parent */
    ULONG TileX;
    ULONG TileY;
    ULONG SpanX;
    ULONG SpanY;

    /* Composition state, shell mode only */
    UixPtr<IDCompositionVisual> Visual;
    UixPtr<IDCompositionSurface> Surface;
    UixPtr<ID3D11Texture2D> Scratch;
    UixPtr<ID2D1Bitmap1> ScratchTarget;
    UixPtr<IDCompositionEffectGroup> Effect;
    UixPtr<IDCompositionRotateTransform> Rotation;
    LONG PixelLeft;
    LONG PixelTop;
    LONG PixelWidth;
    LONG PixelHeight;
    LONG AppliedX;
    LONG AppliedY;
    BOOL AppliedShown;
    BOOL Dirty;

    PCWSTR
    Get(
        _In_z_ PCWSTR Key) const
    {
        for (const auto &Pair : Attributes)
        {
            if (_wcsicmp(Pair.first.c_str(), Key) == 0)
                return Pair.second.c_str();
        }
        return NULL;
    }
} UIX_ELEMENT, *PUIX_ELEMENT;

class CUixEngine;

class CUixScene : public IUixScene
{
public:
    CUixScene(
        _In_ CUixEngine *Engine);
    ~CUixScene();

    HRESULT
    Load(
        _In_z_ PCWSTR MarkupFile);

    ULONG STDMETHODCALLTYPE AddRef(VOID) override;
    ULONG STDMETHODCALLTYPE Release(VOID) override;

    HRESULT STDMETHODCALLTYPE Attach(
        _In_ IDCompositionVisual *Parent) override;

    VOID STDMETHODCALLTYPE Layout(
        _In_ FLOAT Width,
        _In_ FLOAT Height) override;

    VOID STDMETHODCALLTYPE SetBinding(
        _In_z_ PCWSTR Name,
        _In_opt_z_ PCWSTR Value) override;

    BOOL STDMETHODCALLTYPE SetVisible(
        _In_z_ PCWSTR Element,
        _In_ BOOL Visible) override;

    BOOL STDMETHODCALLTYPE IsVisible(
        _In_z_ PCWSTR Element) override;

    BOOL STDMETHODCALLTYPE GetElementRect(
        _In_z_ PCWSTR Element,
        _Out_ PUIX_RECT Rect) override;

    PCWSTR STDMETHODCALLTYPE GetAttribute(
        _In_opt_z_ PCWSTR Element,
        _In_z_ PCWSTR Attribute) override;

    BOOL STDMETHODCALLTYPE SetSelected(
        _In_opt_z_ PCWSTR Element) override;

    BOOL STDMETHODCALLTYPE MoveSelection(
        _In_ INT Delta) override;

    PCWSTR STDMETHODCALLTYPE GetSelectedCommand(VOID) override;

    BOOL STDMETHODCALLTYPE PointerEvent(
        _In_ UIX_POINTER Kind,
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _Outptr_result_maybenull_z_ PCWSTR *Command) override;

    HRESULT STDMETHODCALLTYPE RenderTo(
        _In_ ID2D1DeviceContext *Context,
        _In_ FLOAT Scale) override;

    VOID STDMETHODCALLTYPE Invalidate(VOID) override;

    HRESULT STDMETHODCALLTYPE SetItems(
        _In_z_ PCWSTR Repeater,
        _In_ ULONG Count,
        _In_ ULONG FieldCount,
        _In_reads_(FieldCount) PCWSTR const *Fields,
        _In_reads_opt_(Count * FieldCount) PCWSTR const *Values) override;

    BOOL STDMETHODCALLTYPE Wheel(
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_ FLOAT Delta) override;

    VOID STDMETHODCALLTYPE ResetScroll(
        _In_z_ PCWSTR Element) override;

    VOID STDMETHODCALLTYPE ScrollTo(
        _In_z_ PCWSTR Element,
        _In_ FLOAT Offset) override;

    /* Engine side */
    HRESULT
    RenderLayers(VOID);

    BOOL
    HasDirtyLayers(VOID);

    VOID
    Detach(VOID);

private:
    PUIX_ELEMENT
    Build(
        _In_ const UIX_XML_NODE &Node,
        _In_opt_ PUIX_ELEMENT Parent);

    PUIX_ELEMENT
    Find(
        _In_z_ PCWSTR Name) const;

    std::wstring
    Expand(
        _In_opt_ PUIX_ELEMENT Context,
        _In_opt_z_ PCWSTR Raw) const;

    PUIX_ELEMENT
    ScrollerAt(
        _In_ PUIX_ELEMENT Element,
        _In_ FLOAT X,
        _In_ FLOAT Y) const;

    PUIX_ELEMENT
    SwiperAt(
        _In_ PUIX_ELEMENT Element,
        _In_ FLOAT X,
        _In_ FLOAT Y) const;

    BOOL
    ScrollBy(
        _In_ PUIX_ELEMENT Scroller,
        _In_ FLOAT Delta);

    VOID
    EnsureVisible(
        _In_ PUIX_ELEMENT Element);

    VOID
    ApplyEffects(
        _In_ PUIX_ELEMENT Element,
        _In_ BOOL Revealed);

    std::wstring
    Value(
        _In_ PUIX_ELEMENT Element,
        _In_z_ PCWSTR Key,
        _In_ BOOL Inherit) const;

    VOID
    Update(VOID);

    VOID
    Resolve(
        _In_ PUIX_ELEMENT Element);

    VOID
    Measure(
        _In_ PUIX_ELEMENT Element,
        _In_ FLOAT AvailableWidth,
        _In_ FLOAT AvailableHeight);

    VOID
    MeasureContent(
        _In_ PUIX_ELEMENT Element,
        _In_ FLOAT Width,
        _In_ FLOAT Height,
        _Out_ FLOAT *ContentWidth,
        _Out_ FLOAT *ContentHeight);

    VOID
    Arrange(
        _In_ PUIX_ELEMENT Element,
        _In_ const UIX_RECT &Slot);

    ULONG
    PlaceTiles(
        _In_ PUIX_ELEMENT Element,
        _Out_ ULONG *Columns);

    FLOAT
    TileCell(
        _In_ PUIX_ELEMENT Element,
        _In_ ULONG Columns,
        _In_ FLOAT Width);

    VOID
    UpdatePixels(
        _In_ PUIX_ELEMENT Element);

    VOID
    MarkLayerDirty(
        _In_opt_ PUIX_ELEMENT Element);

    VOID
    MarkAllDirty(
        _In_ PUIX_ELEMENT Element);

    BOOL
    EffectivelyVisible(
        _In_ PUIX_ELEMENT Element) const;

    PUIX_ELEMENT
    HitTest(
        _In_ PUIX_ELEMENT Element,
        _In_ FLOAT X,
        _In_ FLOAT Y) const;

    VOID
    CollectSelectable(
        _In_ PUIX_ELEMENT Element,
        _Inout_ std::vector<PUIX_ELEMENT> &List) const;

    HRESULT
    CreateLayerVisuals(
        _In_ PUIX_ELEMENT Element,
        _In_ IDCompositionVisual *Parent,
        _Inout_ IDCompositionVisual **Previous);

    HRESULT
    RenderLayer(
        _In_ PUIX_ELEMENT Element,
        _In_ LONG ParentX,
        _In_ LONG ParentY,
        _In_ BOOL ParentShown);

    HRESULT
    DrawLayer(
        _In_ PUIX_ELEMENT Element);

    VOID
    Draw(
        _In_ ID2D1DeviceContext *Context,
        _In_ PUIX_ELEMENT Element,
        _In_opt_ PUIX_ELEMENT Owner);

    VOID
    FillBackground(
        _In_ ID2D1DeviceContext *Context,
        _In_ const std::wstring &Value,
        _In_ const D2D1_RECT_F &Rect,
        _In_ FLOAT Radius);

    VOID
    DrawTextElement(
        _In_ ID2D1DeviceContext *Context,
        _In_ PUIX_ELEMENT Element,
        _In_ const D2D1_RECT_F &Rect);

    VOID
    DrawImageElement(
        _In_ ID2D1DeviceContext *Context,
        _In_ PUIX_ELEMENT Element,
        _In_ const D2D1_RECT_F &Rect);

    VOID
    Animate(
        _In_ PUIX_ELEMENT Element,
        _In_ LONG X,
        _In_ LONG Y);

    VOID
    DeleteTree(
        _In_ PUIX_ELEMENT Element);

    LONG m_Refs;
    CUixEngine *m_Engine;
    PUIX_ELEMENT m_Root;
    std::map<std::wstring, std::wstring> m_Bindings;
    std::wstring m_ScratchValue;
    FLOAT m_Width;
    FLOAT m_Height;
    BOOL m_NeedsLayout;
    LONG m_Generation;
    PUIX_ELEMENT m_Selected;
    PUIX_ELEMENT m_Pressed;
    PUIX_ELEMENT m_SelectedBeforePress;
    PUIX_ELEMENT m_Scroller;
    BOOL m_Dragging;
    PUIX_ELEMENT m_Swiper;
    BOOL m_Swiping;
    FLOAT m_DownX;
    FLOAT m_DownY;
    FLOAT m_LastY;
    UixPtr<IDCompositionVisual> m_Visual;
    UixPtr<IDCompositionVisual> m_Parent;
    BOOL m_Attached;
};

class CUixEngine : public IUixEngine
{
public:
    CUixEngine();
    ~CUixEngine();

    ULONG STDMETHODCALLTYPE AddRef(VOID) override;
    ULONG STDMETHODCALLTYPE Release(VOID) override;

    HRESULT STDMETHODCALLTYPE Startup(
        _In_opt_ HWND Window) override;

    VOID STDMETHODCALLTYPE SetScale(
        _In_ FLOAT Scale) override;

    FLOAT STDMETHODCALLTYPE GetScale(VOID) override;

    HRESULT STDMETHODCALLTYPE LoadTheme(
        _In_z_ PCWSTR ThemeFile) override;

    HRESULT STDMETHODCALLTYPE CreateScene(
        _In_z_ PCWSTR MarkupFile,
        _Outptr_ IUixScene **Scene) override;

    HRESULT STDMETHODCALLTYPE GetRootVisual(
        _Outptr_ IDCompositionVisual **Visual) override;

    HRESULT STDMETHODCALLTYPE GetCompositionDevice(
        _Outptr_ IDCompositionDevice **Device) override;

    HRESULT STDMETHODCALLTYPE GetDirect3DDevice(
        _Outptr_ ID3D11Device **Device) override;

    HRESULT STDMETHODCALLTYPE GetDirect2DContext(
        _Outptr_ ID2D1DeviceContext **Context) override;

    HRESULT STDMETHODCALLTYPE CopyToSurface(
        _In_ IDCompositionSurface *Surface,
        _In_ ID3D11Texture2D *Source,
        _In_ UINT Width,
        _In_ UINT Height) override;

    HRESULT STDMETHODCALLTYPE PreRender(VOID) override;
    HRESULT STDMETHODCALLTYPE BeginRender(VOID) override;
    HRESULT STDMETHODCALLTYPE Render(VOID) override;
    HRESULT STDMETHODCALLTYPE EndRender(VOID) override;
    HRESULT STDMETHODCALLTYPE PostPresent(VOID) override;
    BOOL STDMETHODCALLTYPE ShouldRender(VOID) override;

    /* Scene side */
    VOID
    AttachScene(
        _In_ CUixScene *Scene);

    VOID
    DetachScene(
        _In_ CUixScene *Scene);

    LONG
    Generation(VOID) const
    {
        return m_Generation;
    }

    const CUixTheme &
    Theme(VOID) const
    {
        return m_Theme;
    }

    IDCompositionDevice *
    Composition(VOID) const
    {
        return m_Composition.Get();
    }

    ID2D1DeviceContext *
    Context(VOID) const
    {
        return m_Context.Get();
    }

    ID3D11Device *
    Device(VOID) const
    {
        return m_Device.Get();
    }

    FLOAT
    ResolveNumber(
        _In_ const std::wstring &Value,
        _In_ FLOAT Default) const;

    BOOL
    ResolveBrush(
        _In_ const std::wstring &Value,
        _Out_ UIX_BRUSH_DEF *Brush) const;

    BOOL
    ResolveColor(
        _In_ const std::wstring &Value,
        _Out_ D2D1_COLOR_F *Color) const;

    std::wstring
    ResolveString(
        _In_ const std::wstring &Value) const;

    IDWriteTextFormat *
    GetTextFormat(
        _In_ const std::wstring &Value);

    ID2D1Bitmap1 *
    GetImage(
        _In_ const std::wstring &Value,
        _Out_ FLOAT *ImageScale);

    IDWriteFactory *
    Write(VOID) const
    {
        return m_Write.Get();
    }

    BOOL
    TrackAnimations(VOID) const
    {
        return m_Composition.Get() != NULL;
    }

private:
    HRESULT
    LoadBitmap(
        _In_z_ PCWSTR Path,
        _Outptr_ ID2D1Bitmap1 **Bitmap);

    std::wstring
    PickFamily(
        _In_ const std::wstring &Families);

    LONG m_Refs;
    HWND m_Window;
    FLOAT m_Scale;
    LONG m_Generation;
    CUixTheme m_Theme;
    UixPtr<ID3D11Device> m_Device;
    UixPtr<ID3D11DeviceContext> m_Immediate;
    UixPtr<ID2D1Factory1> m_Factory;
    UixPtr<ID2D1Device> m_Device2D;
    UixPtr<ID2D1DeviceContext> m_Context;
    UixPtr<IDWriteFactory> m_Write;
    UixPtr<IWICImagingFactory> m_Imaging;
    UixPtr<IDCompositionDevice> m_Composition;
    UixPtr<IDCompositionTarget> m_Target;
    UixPtr<IDCompositionVisual> m_Root;
    std::vector<CUixScene *> m_Scenes;
    std::map<std::wstring, UixPtr<ID2D1Bitmap1>> m_Images;
    std::map<std::wstring, UixPtr<IDWriteTextFormat>> m_Formats;
    BOOL m_Pending;
};
