/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Markup scene: element tree, layout, drawing and layer composition
 */

#include "uixp.h"
#include <math.h>
#include <stdlib.h>
#include <algorithm>

static
FLOAT
Max0(
    _In_ FLOAT Value)
{
    return (Value > 0.0f) ? Value : 0.0f;
}

static
BOOL
Contains(
    _In_ const UIX_RECT &Rect,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    return X >= Rect.Left && X < Rect.Right && Y >= Rect.Top && Y < Rect.Bottom;
}

static
VOID
CollectBindingNames(
    _In_ const std::wstring &Value,
    _Inout_ std::vector<std::wstring> &Names)
{
    SIZE_T Open = 0;
    SIZE_T Close;

    while ((Open = Value.find(L'{', Open)) != std::wstring::npos)
    {
        Close = Value.find(L'}', Open + 1);
        if (Close == std::wstring::npos)
            break;
        Names.push_back(Value.substr(Open + 1, Close - Open - 1));
        Open = Close + 1;
    }
}

static
UIX_ALIGN
ParseAlign(
    _In_ const std::wstring &Value)
{
    PCWSTR Text = Value.c_str();

    if (_wcsicmp(Text, L"Left") == 0 || _wcsicmp(Text, L"Top") == 0 || _wcsicmp(Text, L"Start") == 0)
        return UixAlignStart;
    if (_wcsicmp(Text, L"Center") == 0)
        return UixAlignCenter;
    if (_wcsicmp(Text, L"Right") == 0 || _wcsicmp(Text, L"Bottom") == 0 || _wcsicmp(Text, L"End") == 0)
        return UixAlignEnd;
    return UixAlignStretch;
}

static
BOOL
ParseBool(
    _In_opt_z_ PCWSTR Value,
    _In_ BOOL Default)
{
    if (!Value)
        return Default;
    return (_wcsicmp(Value, L"true") == 0 || _wcsicmp(Value, L"1") == 0);
}

CUixScene::CUixScene(
    _In_ CUixEngine *Engine)
    : m_Refs(1),
      m_Engine(Engine),
      m_Root(NULL),
      m_Width(0.0f),
      m_Height(0.0f),
      m_NeedsLayout(TRUE),
      m_Generation(0),
      m_Selected(NULL),
      m_Pressed(NULL),
      m_SelectedBeforePress(NULL),
      m_Attached(FALSE),
      m_Scroller(NULL),
      m_Dragging(FALSE),
      m_DownY(0.0f),
      m_LastY(0.0f)
{
    m_Engine->AddRef();
}

CUixScene::~CUixScene()
{
    Detach();
    if (m_Root)
        DeleteTree(m_Root);
    m_Engine->Release();
}

ULONG
STDMETHODCALLTYPE
CUixScene::AddRef(VOID)
{
    return InterlockedIncrement(&m_Refs);
}

ULONG
STDMETHODCALLTYPE
CUixScene::Release(VOID)
{
    ULONG Refs = InterlockedDecrement(&m_Refs);

    if (Refs == 0)
        delete this;
    return Refs;
}

VOID
CUixScene::DeleteTree(
    _In_ PUIX_ELEMENT Element)
{
    for (PUIX_ELEMENT Child : Element->Children)
        DeleteTree(Child);
    delete Element;
}

HRESULT
CUixScene::Load(
    _In_z_ PCWSTR MarkupFile)
{
    UIX_XML_NODE Root;
    HRESULT Status;

    Status = UixXmlLoad(MarkupFile, &Root);
    if (FAILED(Status))
        return Status;

    m_Root = Build(Root, NULL);
    return m_Root ? S_OK : E_OUTOFMEMORY;
}

PUIX_ELEMENT
CUixScene::Build(
    _In_ const UIX_XML_NODE &Node,
    _In_opt_ PUIX_ELEMENT Parent)
{
    PUIX_ELEMENT Element = new UIX_ELEMENT();

    if (Node.Name == L"Text")
        Element->Kind = UixKindText;
    else if (Node.Name == L"Image")
        Element->Kind = UixKindImage;
    else
        Element->Kind = UixKindElement;

    Element->Attributes = Node.Attributes;
    Element->Parent = Parent;
    Element->Name = Node.Attr(L"Name", L"");
    Element->Visible = ParseBool(Node.Attr(L"Visible"), TRUE);
    Element->IsLayer = ParseBool(Node.Attr(L"Layer"), FALSE);
    Element->Interactive = (Node.Attr(L"Command") != NULL);
    Element->Selectable = ParseBool(Node.Attr(L"Selectable"), FALSE);
    Element->Momentary = ParseBool(Node.Attr(L"Momentary"), FALSE);
    Element->Scroll = ParseBool(Node.Attr(L"Scroll"), FALSE);
    Element->AppliedShown = TRUE;
    Element->AppliedX = MAXLONG;
    Element->AppliedY = MAXLONG;
    Element->Dirty = TRUE;

    /* Generated items never get visuals, they draw into the layer that owns the repeater */
    for (PUIX_ELEMENT Walk = Parent; Walk; Walk = Walk->Parent)
    {
        if (Walk->Generated)
        {
            Element->Generated = TRUE;
            Element->IsLayer = FALSE;
            break;
        }
    }

    for (const auto &Pair : Node.Attributes)
        CollectBindingNames(Pair.second, Element->Bindings);

    if (Node.Name == L"Repeater")
    {
        for (const UIX_XML_NODE &Child : Node.Children)
        {
            if (Child.Name == L"Element" || Child.Name == L"Text" || Child.Name == L"Image")
            {
                Element->Template = std::make_shared<UIX_XML_NODE>(Child);
                break;
            }
        }
        return Element;
    }

    for (const UIX_XML_NODE &Child : Node.Children)
    {
        if (Child.Name == L"Element" || Child.Name == L"Text" || Child.Name == L"Image" || Child.Name == L"Repeater")
            Element->Children.push_back(Build(Child, Element));
    }
    return Element;
}

HRESULT
STDMETHODCALLTYPE
CUixScene::SetItems(
    _In_z_ PCWSTR Name,
    _In_ ULONG Count,
    _In_ ULONG FieldCount,
    _In_reads_(FieldCount) PCWSTR const *Fields,
    _In_reads_opt_(Count * FieldCount) PCWSTR const *Values)
{
    PUIX_ELEMENT Repeater = Find(Name);
    PUIX_ELEMENT Item;
    PUIX_ELEMENT Walk;
    WCHAR Index[16];
    ULONG Row;
    ULONG Field;

    if (!Repeater || !Repeater->Template)
        return E_INVALIDARG;

    /* Drop state that points into the old items */
    for (PUIX_ELEMENT *State : { &m_Selected, &m_Pressed, &m_SelectedBeforePress })
    {
        for (Walk = *State; Walk; Walk = Walk->Parent)
        {
            if (Walk == Repeater && *State != Repeater)
            {
                *State = NULL;
                break;
            }
        }
    }

    for (PUIX_ELEMENT Child : Repeater->Children)
        DeleteTree(Child);
    Repeater->Children.clear();

    for (Row = 0; Row < Count; Row++)
    {
        BOOL WasGenerated = Repeater->Generated;

        /* Mark the repeater generated while building so the copies come out flat */
        Repeater->Generated = TRUE;
        Item = Build(*Repeater->Template, Repeater);
        Repeater->Generated = WasGenerated;

        swprintf_s(Index, ARRAYSIZE(Index), L"%lu", Row);
        Item->Item[L"Index"] = Index;
        for (Field = 0; Field < FieldCount; Field++)
        {
            PCWSTR Value = Values ? Values[Row * FieldCount + Field] : NULL;

            Item->Item[Fields[Field]] = Value ? Value : L"";
        }

        /* Items can be named from their own fields, which makes them selectable by name */
        if (Item->Get(L"Name"))
            Item->Name = Expand(Item, Item->Get(L"Name"));
        Repeater->Children.push_back(Item);
    }

    m_NeedsLayout = TRUE;
    MarkLayerDirty(Repeater);
    return S_OK;
}

PUIX_ELEMENT
CUixScene::Find(
    _In_z_ PCWSTR Name) const
{
    std::vector<PUIX_ELEMENT> Pending;
    PUIX_ELEMENT Element;

    if (!m_Root || !Name || !Name[0])
        return NULL;

    Pending.push_back(m_Root);
    while (!Pending.empty())
    {
        Element = Pending.back();
        Pending.pop_back();
        if (Element->Name == Name)
            return Element;
        for (PUIX_ELEMENT Child : Element->Children)
            Pending.push_back(Child);
    }
    return NULL;
}

std::wstring
CUixScene::Expand(
    _In_opt_ PUIX_ELEMENT Context,
    _In_opt_z_ PCWSTR Raw) const
{
    std::wstring Result;
    std::wstring Name;
    PCWSTR Cursor = Raw;
    PCWSTR Close;
    PUIX_ELEMENT Walk;

    if (!Raw)
        return Result;

    while (*Cursor)
    {
        if (*Cursor == L'{' && (Close = wcschr(Cursor + 1, L'}')) != NULL)
        {
            Name.assign(Cursor + 1, Close);
            Cursor = Close + 1;

            /* Item fields come from the nearest generated item above the element */
            if (Name.compare(0, 5, L"Item.") == 0)
            {
                for (Walk = Context; Walk; Walk = Walk->Parent)
                {
                    auto Field = Walk->Item.find(Name.substr(5));

                    if (Field != Walk->Item.end())
                    {
                        Result += Field->second;
                        break;
                    }
                }
                continue;
            }

            auto Entry = m_Bindings.find(Name);
            if (Entry != m_Bindings.end())
                Result += Entry->second;
        }
        else
        {
            Result += *Cursor;
            Cursor++;
        }
    }
    return Result;
}

std::wstring
CUixScene::Value(
    _In_ PUIX_ELEMENT Element,
    _In_z_ PCWSTR Key,
    _In_ BOOL Inherit) const
{
    PUIX_ELEMENT Current = Element;
    PCWSTR Raw;

    while (Current)
    {
        Raw = Current->Get(Key);
        if (Raw)
            return Expand(Element, Raw);
        if (!Inherit)
            break;
        Current = Current->Parent;
    }
    return std::wstring();
}

VOID
CUixScene::Resolve(
    _In_ PUIX_ELEMENT Element)
{
    std::wstring Text;
    std::vector<FLOAT> Parts;
    UIX_THICKNESS *Targets[3] = { &Element->Margin, &Element->Padding, &Element->Border };
    PCWSTR Keys[3] = { L"Margin", L"Padding", L"BorderThickness" };
    UIX_LENGTH *Lengths[2] = { &Element->Width, &Element->Height };
    PCWSTR LengthKeys[2] = { L"Width", L"Height" };
    SIZE_T Index;

    Text = Value(Element, L"Layout", FALSE);
    if (_wcsicmp(Text.c_str(), L"Stack") == 0)
        Element->Layout = UixLayoutStack;
    else if (_wcsicmp(Text.c_str(), L"Row") == 0)
        Element->Layout = UixLayoutRow;
    else if (_wcsicmp(Text.c_str(), L"Wrap") == 0)
        Element->Layout = UixLayoutWrap;
    else
        Element->Layout = UixLayoutOverlay;

    for (Index = 0; Index < 2; Index++)
    {
        Text = Value(Element, LengthKeys[Index], FALSE);
        if (Text.empty() || _wcsicmp(Text.c_str(), L"auto") == 0)
        {
            Lengths[Index]->Kind = UixLengthAuto;
            Lengths[Index]->Value = 0.0f;
        }
        else if (Text.back() == L'*')
        {
            Lengths[Index]->Kind = UixLengthStar;
            Lengths[Index]->Value = (Text.size() > 1) ? static_cast<FLOAT>(_wtof(Text.c_str())) : 1.0f;
            if (Lengths[Index]->Value <= 0.0f)
                Lengths[Index]->Value = 1.0f;
        }
        else
        {
            Lengths[Index]->Kind = UixLengthFixed;
            Lengths[Index]->Value = Max0(m_Engine->ResolveNumber(Text, 0.0f));
        }
    }

    for (Index = 0; Index < 3; Index++)
    {
        SIZE_T Start = 0;
        SIZE_T Comma;

        Text = Value(Element, Keys[Index], FALSE);
        Parts.clear();
        while (!Text.empty())
        {
            Comma = Text.find(L',', Start);
            Parts.push_back(m_Engine->ResolveNumber(Text.substr(Start, Comma - Start), 0.0f));
            if (Comma == std::wstring::npos)
                break;
            Start = Comma + 1;
        }

        if (Parts.size() == 1)
            *Targets[Index] = { Parts[0], Parts[0], Parts[0], Parts[0] };
        else if (Parts.size() == 2)
            *Targets[Index] = { Parts[0], Parts[1], Parts[0], Parts[1] };
        else if (Parts.size() >= 4)
            *Targets[Index] = { Parts[0], Parts[1], Parts[2], Parts[3] };
        else
            *Targets[Index] = { 0.0f, 0.0f, 0.0f, 0.0f };
    }

    Element->OffsetX = m_Engine->ResolveNumber(Value(Element, L"X", FALSE), 0.0f);
    Element->OffsetY = m_Engine->ResolveNumber(Value(Element, L"Y", FALSE), 0.0f);
    Element->HAlign = ParseAlign(Value(Element, L"HAlign", FALSE));
    Element->VAlign = ParseAlign(Value(Element, L"VAlign", FALSE));
}

VOID
CUixScene::MeasureContent(
    _In_ PUIX_ELEMENT Element,
    _In_ FLOAT Width,
    _In_ FLOAT Height,
    _Out_ FLOAT *ContentWidth,
    _Out_ FLOAT *ContentHeight)
{
    FLOAT RowWidth;
    FLOAT RowHeight;

    *ContentWidth = 0.0f;
    *ContentHeight = 0.0f;

    if (Element->Kind == UixKindText)
    {
        UixPtr<IDWriteTextLayout> Layout;
        DWRITE_TEXT_METRICS Metrics;
        IDWriteTextFormat *Format;
        std::wstring Text = m_Engine->ResolveString(Value(Element, L"Content", FALSE));
        BOOL Wrap = ParseBool(Value(Element, L"Wrap", FALSE).c_str(), FALSE);

        Format = m_Engine->GetTextFormat(Value(Element, L"Font", TRUE));
        if (!Format)
            return;

        /* Collapse="true" lets an empty text take no room at all */
        if (Text.empty() && ParseBool(Value(Element, L"Collapse", FALSE).c_str(), FALSE))
            return;
        if (Text.empty())
            Text = L" ";

        if (FAILED(m_Engine->Write()->CreateTextLayout(Text.c_str(), static_cast<UINT32>(Text.size()), Format,
                                                       Wrap ? Max0(Width) : UIX_INFINITE, UIX_INFINITE, Layout.Put())))
        {
            return;
        }

        Layout->SetWordWrapping(Wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(Layout->GetMetrics(&Metrics)))
        {
            *ContentWidth = (Text == L" ") ? 0.0f : ceilf(Metrics.widthIncludingTrailingWhitespace);
            *ContentHeight = ceilf(Metrics.height);
        }
        return;
    }

    if (Element->Kind == UixKindImage)
    {
        FLOAT ImageScale;
        ID2D1Bitmap1 *Bitmap = m_Engine->GetImage(Value(Element, L"Source", FALSE), &ImageScale);

        if (Bitmap)
        {
            D2D1_SIZE_U Size = Bitmap->GetPixelSize();

            *ContentWidth = Size.width / ImageScale;
            *ContentHeight = Size.height / ImageScale;
        }
        return;
    }

    RowWidth = 0.0f;
    RowHeight = 0.0f;

    for (PUIX_ELEMENT Child : Element->Children)
    {
        switch (Element->Layout)
        {
            case UixLayoutStack:
                Measure(Child, Width, UIX_INFINITE);
                *ContentWidth = max(*ContentWidth, Child->DesiredWidth);
                if (Child->Height.Kind != UixLengthStar)
                    *ContentHeight += Child->DesiredHeight;
                break;

            case UixLayoutRow:
                Measure(Child, UIX_INFINITE, Height);
                *ContentHeight = max(*ContentHeight, Child->DesiredHeight);
                if (Child->Width.Kind != UixLengthStar)
                    *ContentWidth += Child->DesiredWidth;
                break;

            case UixLayoutWrap:
                Measure(Child, Width, UIX_INFINITE);
                if (RowWidth > 0.0f && RowWidth + Child->DesiredWidth > Width)
                {
                    *ContentHeight += RowHeight;
                    RowWidth = 0.0f;
                    RowHeight = 0.0f;
                }
                RowWidth += Child->DesiredWidth;
                RowHeight = max(RowHeight, Child->DesiredHeight);
                *ContentWidth = max(*ContentWidth, RowWidth);
                break;

            default:
                Measure(Child, Width, Height);
                *ContentWidth = max(*ContentWidth, Child->DesiredWidth);
                *ContentHeight = max(*ContentHeight, Child->DesiredHeight);
                break;
        }
    }

    if (Element->Layout == UixLayoutWrap)
        *ContentHeight += RowHeight;
}

VOID
CUixScene::Measure(
    _In_ PUIX_ELEMENT Element,
    _In_ FLOAT AvailableWidth,
    _In_ FLOAT AvailableHeight)
{
    FLOAT MarginWidth;
    FLOAT MarginHeight;
    FLOAT ChromeWidth;
    FLOAT ChromeHeight;
    FLOAT BoxWidth;
    FLOAT BoxHeight;
    FLOAT ContentWidth;
    FLOAT ContentHeight;
    FLOAT Width;
    FLOAT Height;

    Resolve(Element);
    if (!Element->Visible)
    {
        Element->DesiredWidth = 0.0f;
        Element->DesiredHeight = 0.0f;
        return;
    }

    MarginWidth = Element->Margin.Left + Element->Margin.Right;
    MarginHeight = Element->Margin.Top + Element->Margin.Bottom;
    ChromeWidth = Element->Padding.Left + Element->Padding.Right + Element->Border.Left + Element->Border.Right;
    ChromeHeight = Element->Padding.Top + Element->Padding.Bottom + Element->Border.Top + Element->Border.Bottom;

    BoxWidth = (Element->Width.Kind == UixLengthFixed) ? Element->Width.Value : Max0(AvailableWidth - MarginWidth);
    BoxHeight = (Element->Height.Kind == UixLengthFixed) ? Element->Height.Value : Max0(AvailableHeight - MarginHeight);

    MeasureContent(Element, Max0(BoxWidth - ChromeWidth), Max0(BoxHeight - ChromeHeight), &ContentWidth, &ContentHeight);
    if (Element->Scroll)
        Element->Extent = ContentHeight;

    if (Element->Width.Kind == UixLengthFixed)
        Width = Element->Width.Value;
    else if (Element->Width.Kind == UixLengthStar && AvailableWidth < UIX_INFINITE / 2)
        Width = BoxWidth;
    else
        Width = ContentWidth + ChromeWidth;

    if (Element->Height.Kind == UixLengthFixed)
        Height = Element->Height.Value;
    else if (Element->Height.Kind == UixLengthStar && AvailableHeight < UIX_INFINITE / 2)
        Height = BoxHeight;
    else
        Height = ContentHeight + ChromeHeight;

    Element->DesiredWidth = Width + MarginWidth;
    Element->DesiredHeight = Height + MarginHeight;
}

VOID
CUixScene::Arrange(
    _In_ PUIX_ELEMENT Element,
    _In_ const UIX_RECT &Slot)
{
    UIX_RECT Content;
    FLOAT SlotX = Slot.Left + Element->Margin.Left;
    FLOAT SlotY = Slot.Top + Element->Margin.Top;
    FLOAT SlotWidth = Max0(Slot.Right - Slot.Left - Element->Margin.Left - Element->Margin.Right);
    FLOAT SlotHeight = Max0(Slot.Bottom - Slot.Top - Element->Margin.Top - Element->Margin.Bottom);
    FLOAT Width = SlotWidth;
    FLOAT Height = SlotHeight;
    FLOAT X = SlotX;
    FLOAT Y = SlotY;
    FLOAT Fixed;
    FLOAT Stars;
    FLOAT Remain;
    FLOAT Cursor;
    FLOAT RowHeight;
    FLOAT Size;

    if (!Element->Visible)
    {
        Element->Rect = { SlotX, SlotY, SlotX, SlotY };
        return;
    }

    if (Element->Width.Kind == UixLengthFixed ||
        (Element->Width.Kind == UixLengthAuto && Element->HAlign != UixAlignStretch))
    {
        Width = Element->DesiredWidth - Element->Margin.Left - Element->Margin.Right;
        if (Element->HAlign == UixAlignCenter)
            X = SlotX + (SlotWidth - Width) / 2.0f;
        else if (Element->HAlign == UixAlignEnd)
            X = SlotX + SlotWidth - Width;
    }

    if (Element->Height.Kind == UixLengthFixed ||
        (Element->Height.Kind == UixLengthAuto && Element->VAlign != UixAlignStretch))
    {
        Height = Element->DesiredHeight - Element->Margin.Top - Element->Margin.Bottom;
        if (Element->VAlign == UixAlignCenter)
            Y = SlotY + (SlotHeight - Height) / 2.0f;
        else if (Element->VAlign == UixAlignEnd)
            Y = SlotY + SlotHeight - Height;
    }

    X += Element->OffsetX;
    Y += Element->OffsetY;
    Element->Rect = { X, Y, X + Width, Y + Height };

    Content.Left = X + Element->Border.Left + Element->Padding.Left;
    Content.Top = Y + Element->Border.Top + Element->Padding.Top;
    Content.Right = max(Content.Left, X + Width - Element->Border.Right - Element->Padding.Right);
    Content.Bottom = max(Content.Top, Y + Height - Element->Border.Bottom - Element->Padding.Bottom);

    /* A scroller lays its children out on the full extent, shifted by the offset */
    if (Element->Scroll)
    {
        FLOAT Viewport = Content.Bottom - Content.Top;
        FLOAT Limit = Max0(Element->Extent - Viewport);

        Element->ScrollOffset = min(Max0(Element->ScrollOffset), Limit);
        Content.Top -= Element->ScrollOffset;
        Content.Bottom = Content.Top + max(Viewport, Element->Extent);
    }

    switch (Element->Layout)
    {
        case UixLayoutStack:
        case UixLayoutRow:
        {
            BOOL Vertical = (Element->Layout == UixLayoutStack);

            Fixed = 0.0f;
            Stars = 0.0f;
            for (PUIX_ELEMENT Child : Element->Children)
            {
                const UIX_LENGTH &Length = Vertical ? Child->Height : Child->Width;

                if (!Child->Visible)
                    continue;
                if (Length.Kind == UixLengthStar)
                    Stars += Length.Value;
                else
                    Fixed += Vertical ? Child->DesiredHeight : Child->DesiredWidth;
            }

            Remain = Max0((Vertical ? (Content.Bottom - Content.Top) : (Content.Right - Content.Left)) - Fixed);
            Cursor = Vertical ? Content.Top : Content.Left;

            for (PUIX_ELEMENT Child : Element->Children)
            {
                const UIX_LENGTH &Length = Vertical ? Child->Height : Child->Width;

                if (!Child->Visible)
                {
                    Arrange(Child, { Content.Left, Content.Top, Content.Left, Content.Top });
                    continue;
                }

                if (Length.Kind == UixLengthStar)
                    Size = (Stars > 0.0f) ? Remain * Length.Value / Stars : 0.0f;
                else
                    Size = Vertical ? Child->DesiredHeight : Child->DesiredWidth;

                if (Vertical)
                    Arrange(Child, { Content.Left, Cursor, Content.Right, Cursor + Size });
                else
                    Arrange(Child, { Cursor, Content.Top, Cursor + Size, Content.Bottom });
                Cursor += Size;
            }
            break;
        }

        case UixLayoutWrap:
        {
            FLOAT CursorX = Content.Left;
            FLOAT CursorY = Content.Top;

            RowHeight = 0.0f;
            for (PUIX_ELEMENT Child : Element->Children)
            {
                if (!Child->Visible)
                {
                    Arrange(Child, { Content.Left, Content.Top, Content.Left, Content.Top });
                    continue;
                }

                if (CursorX > Content.Left && CursorX + Child->DesiredWidth > Content.Right + 0.01f)
                {
                    CursorX = Content.Left;
                    CursorY += RowHeight;
                    RowHeight = 0.0f;
                }

                Arrange(Child, { CursorX, CursorY, CursorX + Child->DesiredWidth, CursorY + Child->DesiredHeight });
                CursorX += Child->DesiredWidth;
                RowHeight = max(RowHeight, Child->DesiredHeight);
            }
            break;
        }

        default:
            for (PUIX_ELEMENT Child : Element->Children)
                Arrange(Child, Content);
            break;
    }
}

VOID
CUixScene::UpdatePixels(
    _In_ PUIX_ELEMENT Element)
{
    FLOAT Scale = m_Engine->GetScale();
    LONG Left = lroundf(Element->Rect.Left * Scale);
    LONG Top = lroundf(Element->Rect.Top * Scale);
    LONG Right = lroundf(Element->Rect.Right * Scale);
    LONG Bottom = lroundf(Element->Rect.Bottom * Scale);

    if (Element->IsLayer &&
        (Left != Element->PixelLeft || Top != Element->PixelTop ||
         Right - Left != Element->PixelWidth || Bottom - Top != Element->PixelHeight))
    {
        Element->Dirty = TRUE;
    }

    Element->PixelLeft = Left;
    Element->PixelTop = Top;
    Element->PixelWidth = Right - Left;
    Element->PixelHeight = Bottom - Top;

    for (PUIX_ELEMENT Child : Element->Children)
        UpdatePixels(Child);
}

VOID
CUixScene::MarkLayerDirty(
    _In_opt_ PUIX_ELEMENT Element)
{
    while (Element && !Element->IsLayer)
        Element = Element->Parent;
    if (Element)
        Element->Dirty = TRUE;
}

VOID
CUixScene::MarkAllDirty(
    _In_ PUIX_ELEMENT Element)
{
    Element->Dirty = TRUE;
    for (PUIX_ELEMENT Child : Element->Children)
        MarkAllDirty(Child);
}

VOID
CUixScene::Update(VOID)
{
    if (!m_Root)
        return;

    if (m_Generation != m_Engine->Generation())
    {
        m_Generation = m_Engine->Generation();
        m_NeedsLayout = TRUE;
        MarkAllDirty(m_Root);
    }

    if (m_NeedsLayout && m_Width > 0.0f && m_Height > 0.0f)
    {
        m_NeedsLayout = FALSE;
        Measure(m_Root, m_Width, m_Height);
        Arrange(m_Root, { 0.0f, 0.0f, m_Width, m_Height });
        UpdatePixels(m_Root);
    }
}

BOOL
CUixScene::HasDirtyLayers(VOID)
{
    std::vector<PUIX_ELEMENT> Pending;
    PUIX_ELEMENT Element;

    if (!m_Root)
        return FALSE;
    if (m_NeedsLayout || m_Generation != m_Engine->Generation())
        return TRUE;

    Pending.push_back(m_Root);
    while (!Pending.empty())
    {
        Element = Pending.back();
        Pending.pop_back();
        if (Element->IsLayer && Element->Dirty)
            return TRUE;
        for (PUIX_ELEMENT Child : Element->Children)
            Pending.push_back(Child);
    }
    return FALSE;
}

VOID
STDMETHODCALLTYPE
CUixScene::Layout(
    _In_ FLOAT Width,
    _In_ FLOAT Height)
{
    if (Width != m_Width || Height != m_Height)
    {
        m_Width = Width;
        m_Height = Height;
        m_NeedsLayout = TRUE;
    }
}

VOID
STDMETHODCALLTYPE
CUixScene::SetBinding(
    _In_z_ PCWSTR Name,
    _In_opt_z_ PCWSTR Value)
{
    std::vector<PUIX_ELEMENT> Pending;
    PUIX_ELEMENT Element;
    std::wstring Text = Value ? Value : L"";

    auto Entry = m_Bindings.find(Name);
    if (Entry != m_Bindings.end() && Entry->second == Text)
        return;
    m_Bindings[Name] = Text;

    if (!m_Root)
        return;

    Pending.push_back(m_Root);
    while (!Pending.empty())
    {
        Element = Pending.back();
        Pending.pop_back();
        for (const std::wstring &Used : Element->Bindings)
        {
            if (Used == Name)
            {
                MarkLayerDirty(Element);
                m_NeedsLayout = TRUE;
                break;
            }
        }
        for (PUIX_ELEMENT Child : Element->Children)
            Pending.push_back(Child);
    }
}

BOOL
STDMETHODCALLTYPE
CUixScene::SetVisible(
    _In_z_ PCWSTR Name,
    _In_ BOOL Visible)
{
    PUIX_ELEMENT Element = Find(Name);

    if (!Element)
        return FALSE;

    Visible = !!Visible;
    if (Element->Visible != Visible)
    {
        Element->Visible = Visible;
        m_NeedsLayout = TRUE;
        MarkLayerDirty(Element->Parent);
        MarkLayerDirty(Element);

        if (!Visible && m_Selected)
        {
            PUIX_ELEMENT Walk = m_Selected;

            while (Walk && Walk != Element)
                Walk = Walk->Parent;
            if (Walk)
                m_Selected = NULL;
        }
    }
    return TRUE;
}

BOOL
STDMETHODCALLTYPE
CUixScene::IsVisible(
    _In_z_ PCWSTR Name)
{
    PUIX_ELEMENT Element = Find(Name);

    return Element ? EffectivelyVisible(Element) : FALSE;
}

BOOL
STDMETHODCALLTYPE
CUixScene::GetElementRect(
    _In_z_ PCWSTR Name,
    _Out_ PUIX_RECT Rect)
{
    PUIX_ELEMENT Element = Find(Name);

    if (!Element)
        return FALSE;

    Update();
    *Rect = Element->Rect;
    return TRUE;
}

PCWSTR
STDMETHODCALLTYPE
CUixScene::GetAttribute(
    _In_opt_z_ PCWSTR Name,
    _In_z_ PCWSTR Attribute)
{
    PUIX_ELEMENT Element = Name ? Find(Name) : m_Root;

    return Element ? Element->Get(Attribute) : NULL;
}

BOOL
CUixScene::EffectivelyVisible(
    _In_ PUIX_ELEMENT Element) const
{
    while (Element)
    {
        if (!Element->Visible)
            return FALSE;
        Element = Element->Parent;
    }
    return TRUE;
}

BOOL
STDMETHODCALLTYPE
CUixScene::SetSelected(
    _In_opt_z_ PCWSTR Name)
{
    PUIX_ELEMENT Element = Name ? Find(Name) : NULL;

    if (Name && !Element)
        return FALSE;

    if (Element != m_Selected)
    {
        MarkLayerDirty(m_Selected);
        MarkLayerDirty(Element);
        m_Selected = Element;
    }
    if (Element)
        EnsureVisible(Element);
    return TRUE;
}

VOID
CUixScene::CollectSelectable(
    _In_ PUIX_ELEMENT Element,
    _Inout_ std::vector<PUIX_ELEMENT> &List) const
{
    if (!Element->Visible)
        return;
    if (Element->Selectable)
        List.push_back(Element);
    for (PUIX_ELEMENT Child : Element->Children)
        CollectSelectable(Child, List);
}

BOOL
STDMETHODCALLTYPE
CUixScene::MoveSelection(
    _In_ INT Delta)
{
    std::vector<PUIX_ELEMENT> List;
    INT Index = -1;
    INT Next;
    INT Count;

    if (!m_Root)
        return FALSE;

    CollectSelectable(m_Root, List);
    Count = static_cast<INT>(List.size());
    if (Count == 0)
        return FALSE;

    for (INT Search = 0; Search < Count; Search++)
    {
        if (List[Search] == m_Selected)
        {
            Index = Search;
            break;
        }
    }

    if (Index < 0)
        Next = (Delta >= 0) ? 0 : Count - 1;
    else
        Next = Index + Delta;

    if (Next < 0 || Next >= Count)
        return FALSE;

    MarkLayerDirty(m_Selected);
    m_Selected = List[Next];
    MarkLayerDirty(m_Selected);
    EnsureVisible(m_Selected);
    return TRUE;
}

PCWSTR
STDMETHODCALLTYPE
CUixScene::GetSelectedCommand(VOID)
{
    if (!m_Selected || !m_Selected->Interactive)
        return NULL;

    m_ScratchValue = Value(m_Selected, L"Command", FALSE);
    return m_ScratchValue.c_str();
}

PUIX_ELEMENT
CUixScene::HitTest(
    _In_ PUIX_ELEMENT Element,
    _In_ FLOAT X,
    _In_ FLOAT Y) const
{
    PUIX_ELEMENT Hit;
    SIZE_T Index;

    if (!Element->Visible)
        return NULL;

    /* Scrolled content outside the viewport cannot be hit */
    if (Element->Scroll && !Contains(Element->Rect, X, Y))
        return NULL;

    for (Index = Element->Children.size(); Index > 0; Index -= 1)
    {
        Hit = HitTest(Element->Children[Index - 1], X, Y);
        if (Hit)
            return Hit;
    }

    if (Element->Interactive && Contains(Element->Rect, X, Y))
        return Element;
    return NULL;
}

PUIX_ELEMENT
CUixScene::ScrollerAt(
    _In_ PUIX_ELEMENT Element,
    _In_ FLOAT X,
    _In_ FLOAT Y) const
{
    PUIX_ELEMENT Found;
    SIZE_T Index;

    if (!Element->Visible)
        return NULL;
    if (Element->Scroll && !Contains(Element->Rect, X, Y))
        return NULL;

    for (Index = Element->Children.size(); Index > 0; Index -= 1)
    {
        Found = ScrollerAt(Element->Children[Index - 1], X, Y);
        if (Found)
            return Found;
    }
    return Element->Scroll ? Element : NULL;
}

BOOL
CUixScene::ScrollBy(
    _In_ PUIX_ELEMENT Scroller,
    _In_ FLOAT Delta)
{
    FLOAT Viewport = Scroller->Rect.Bottom - Scroller->Rect.Top - Scroller->Border.Top - Scroller->Border.Bottom -
                     Scroller->Padding.Top - Scroller->Padding.Bottom;
    FLOAT Limit = Max0(Scroller->Extent - Viewport);
    FLOAT Offset = min(Max0(Scroller->ScrollOffset + Delta), Limit);

    if (Offset == Scroller->ScrollOffset)
        return FALSE;

    Scroller->ScrollOffset = Offset;
    m_NeedsLayout = TRUE;
    MarkLayerDirty(Scroller);
    return TRUE;
}

VOID
CUixScene::EnsureVisible(
    _In_ PUIX_ELEMENT Element)
{
    PUIX_ELEMENT Scroller = Element->Parent;
    FLOAT Top;
    FLOAT Bottom;

    while (Scroller && !Scroller->Scroll)
        Scroller = Scroller->Parent;
    if (!Scroller)
        return;

    Update();
    Top = Scroller->Rect.Top + Scroller->Border.Top + Scroller->Padding.Top;
    Bottom = Scroller->Rect.Bottom - Scroller->Border.Bottom - Scroller->Padding.Bottom;

    if (Element->Rect.Top < Top)
        ScrollBy(Scroller, Element->Rect.Top - Top);
    else if (Element->Rect.Bottom > Bottom)
        ScrollBy(Scroller, Element->Rect.Bottom - Bottom);
}

BOOL
STDMETHODCALLTYPE
CUixScene::Wheel(
    _In_ FLOAT X,
    _In_ FLOAT Y,
    _In_ FLOAT Delta)
{
    PUIX_ELEMENT Scroller;

    if (!m_Root)
        return FALSE;

    Update();
    Scroller = ScrollerAt(m_Root, X, Y);
    if (!Scroller)
        return FALSE;
    return ScrollBy(Scroller, -Delta * m_Engine->ResolveNumber(Value(Scroller, L"ScrollStep", FALSE), 36.0f));
}

VOID
STDMETHODCALLTYPE
CUixScene::ResetScroll(
    _In_z_ PCWSTR Name)
{
    PUIX_ELEMENT Element = Find(Name);

    if (Element && Element->ScrollOffset != 0.0f)
    {
        Element->ScrollOffset = 0.0f;
        m_NeedsLayout = TRUE;
        MarkLayerDirty(Element);
    }
}

VOID
STDMETHODCALLTYPE
CUixScene::ScrollTo(
    _In_z_ PCWSTR Name,
    _In_ FLOAT Offset)
{
    PUIX_ELEMENT Element = Find(Name);

    if (!Element || !Element->Scroll)
        return;

    /* Lay out first so the extent is current, then clamp through ScrollBy */
    Update();
    ScrollBy(Element, Offset - Element->ScrollOffset);
}

BOOL
STDMETHODCALLTYPE
CUixScene::PointerEvent(
    _In_ UIX_POINTER Kind,
    _In_ FLOAT X,
    _In_ FLOAT Y,
    _Outptr_result_maybenull_z_ PCWSTR *Command)
{
    PUIX_ELEMENT Hit;
    PUIX_ELEMENT Pressed;
    std::wstring Hold;
    BOOL Dragged;

    *Command = NULL;
    if (!m_Root)
        return FALSE;

    Update();

    switch (Kind)
    {
        case UixPointerDown:
            Hit = HitTest(m_Root, X, Y);
            m_Scroller = ScrollerAt(m_Root, X, Y);
            m_Dragging = FALSE;
            m_DownY = Y;
            m_LastY = Y;
            if (!Hit)
                return m_Scroller != NULL;

            m_Pressed = Hit;
            m_SelectedBeforePress = m_Selected;
            MarkLayerDirty(m_Selected);
            m_Selected = Hit;
            MarkLayerDirty(Hit);
            return TRUE;

        case UixPointerMove:
            if (m_Scroller)
            {
                /* Past the slop a press turns into a drag and gives up its tap */
                if (!m_Dragging && fabsf(Y - m_DownY) > m_Engine->ResolveNumber(L"@DragSlop", 6.0f))
                {
                    m_Dragging = TRUE;
                    if (m_Pressed)
                    {
                        MarkLayerDirty(m_Selected);
                        m_Selected = m_SelectedBeforePress;
                        MarkLayerDirty(m_Selected);
                        m_Pressed = NULL;
                    }
                }
                if (m_Dragging)
                    ScrollBy(m_Scroller, m_LastY - Y);
                m_LastY = Y;
                return TRUE;
            }
            return m_Pressed != NULL;

        case UixPointerHold:
            if (!m_Pressed || m_Dragging)
                return FALSE;

            /* Without a HoldCommand the press stays a normal tap */
            Hold = Value(m_Pressed, L"HoldCommand", FALSE);
            if (Hold.empty())
                return FALSE;

            m_Pressed = NULL;
            m_ScratchValue = Hold;
            *Command = m_ScratchValue.c_str();
            return TRUE;

        case UixPointerUp:
        case UixPointerCancel:
            Pressed = m_Pressed;
            Dragged = m_Dragging || m_Scroller;
            m_Scroller = NULL;
            m_Dragging = FALSE;
            if (!Pressed)
                return Dragged;

            m_Pressed = NULL;
            if (Kind == UixPointerUp && Contains(Pressed->Rect, X, Y))
            {
                m_ScratchValue = Value(Pressed, L"Command", FALSE);
                *Command = m_ScratchValue.c_str();
            }

            /* Momentary elements only show the press, they do not keep the selection */
            if (Pressed->Momentary || Kind == UixPointerCancel)
            {
                MarkLayerDirty(m_Selected);
                m_Selected = Pressed->Momentary ? m_SelectedBeforePress : NULL;
                MarkLayerDirty(m_Selected);
            }
            return TRUE;
    }
    return FALSE;
}

VOID
STDMETHODCALLTYPE
CUixScene::Invalidate(VOID)
{
    if (m_Root)
    {
        m_NeedsLayout = TRUE;
        MarkAllDirty(m_Root);
    }
}

HRESULT
STDMETHODCALLTYPE
CUixScene::Attach(
    _In_ IDCompositionVisual *Parent)
{
    IDCompositionDevice *Device = m_Engine->Composition();
    IDCompositionVisual *Previous = NULL;
    HRESULT Status;

    if (!Device || !m_Root)
        return E_NOT_VALID_STATE;
    if (m_Attached)
        return S_FALSE;

    Status = Device->CreateVisual(m_Visual.Put());
    if (FAILED(Status))
        return Status;

    Status = Parent->AddVisual(m_Visual.Get(), FALSE, NULL);
    if (FAILED(Status))
        return Status;

    Status = CreateLayerVisuals(m_Root, m_Visual.Get(), &Previous);
    if (FAILED(Status))
        return Status;

    m_Parent = Parent;
    m_Attached = TRUE;
    m_Engine->AttachScene(this);
    return S_OK;
}

VOID
CUixScene::Detach(VOID)
{
    if (!m_Attached)
        return;

    m_Engine->DetachScene(this);
    if (m_Parent && m_Visual)
        m_Parent->RemoveVisual(m_Visual.Get());
    m_Parent.Reset();
    m_Visual.Reset();
    m_Attached = FALSE;
}

HRESULT
CUixScene::CreateLayerVisuals(
    _In_ PUIX_ELEMENT Element,
    _In_ IDCompositionVisual *Parent,
    _Inout_ IDCompositionVisual **Previous)
{
    IDCompositionVisual *ChildPrevious = NULL;
    HRESULT Status;

    if (Element->IsLayer)
    {
        Status = m_Engine->Composition()->CreateVisual(Element->Visual.Put());
        if (FAILED(Status))
            return Status;

        /* Each layer goes above the one added before it, matching markup order */
        if (*Previous)
            Status = Parent->AddVisual(Element->Visual.Get(), TRUE, *Previous);
        else
            Status = Parent->AddVisual(Element->Visual.Get(), FALSE, NULL);
        if (FAILED(Status))
            return Status;

        *Previous = Element->Visual.Get();
        Parent = Element->Visual.Get();
        Previous = &ChildPrevious;
    }

    for (PUIX_ELEMENT Child : Element->Children)
    {
        Status = CreateLayerVisuals(Child, Parent, Previous);
        if (FAILED(Status))
            return Status;
    }
    return S_OK;
}

VOID
CUixScene::Animate(
    _In_ PUIX_ELEMENT Element,
    _In_ LONG X,
    _In_ LONG Y)
{
    UixPtr<IDCompositionAnimation> Animation;
    std::wstring Reveal = Value(Element, L"Reveal", FALSE);
    FLOAT FromX = 0.0f;
    FLOAT FromY = 0.0f;
    FLOAT Seconds = 0.0f;
    FLOAT Scale = m_Engine->GetScale();
    FLOAT Distance;
    LONG Target[2] = { X, Y };
    FLOAT From[2];
    ULONG Axis;

    if (swscanf(Reveal.c_str(), L"%f,%f,%f", &FromX, &FromY, &Seconds) != 3 || Seconds <= 0.0f)
    {
        Element->Visual->SetOffsetX(static_cast<FLOAT>(X));
        Element->Visual->SetOffsetY(static_cast<FLOAT>(Y));
        return;
    }

    From[0] = X + FromX * Scale;
    From[1] = Y + FromY * Scale;

    for (Axis = 0; Axis < 2; Axis++)
    {
        Distance = Target[Axis] - From[Axis];
        Animation.Reset();

        /* Ease out: p(t) = from + d(2t/T - t^2/T^2), evaluated by the compositor */
        if (Distance == 0.0f || FAILED(m_Engine->Composition()->CreateAnimation(Animation.Put())))
        {
            if (Axis == 0)
                Element->Visual->SetOffsetX(static_cast<FLOAT>(Target[Axis]));
            else
                Element->Visual->SetOffsetY(static_cast<FLOAT>(Target[Axis]));
            continue;
        }

        Animation->AddCubic(0.0, From[Axis], 2.0f * Distance / Seconds, -Distance / (Seconds * Seconds), 0.0f);
        Animation->End(Seconds, static_cast<FLOAT>(Target[Axis]));

        if (Axis == 0)
            Element->Visual->SetOffsetX(Animation.Get());
        else
            Element->Visual->SetOffsetY(Animation.Get());
    }
}

VOID
CUixScene::ApplyEffects(
    _In_ PUIX_ELEMENT Element,
    _In_ BOOL Revealed)
{
    IDCompositionDevice *Device = m_Engine->Composition();
    UixPtr<IDCompositionAnimation> Animation;
    std::wstring Reveal = Value(Element, L"Reveal", FALSE);
    std::wstring Spin = Value(Element, L"Spin", FALSE);
    FLOAT Opacity = m_Engine->ResolveNumber(Value(Element, L"Opacity", FALSE), 1.0f);
    FLOAT Ignored[2];
    FLOAT Seconds = 0.0f;
    FLOAT From = 1.0f;
    FLOAT Period;

    /* Opacity lives on an effect group, created only for layers that use it */
    if (Element->Get(L"Opacity") ||
        swscanf(Reveal.c_str(), L"%f,%f,%f,%f", &Ignored[0], &Ignored[1], &Seconds, &From) == 4)
    {
        if (!Element->Effect && SUCCEEDED(Device->CreateEffectGroup(Element->Effect.Put())))
            Element->Visual->SetEffect(Element->Effect.Get());

        if (Element->Effect)
        {
            if (Revealed && Seconds > 0.0f && SUCCEEDED(Device->CreateAnimation(Animation.Put())))
            {
                Animation->AddCubic(0.0, From, (Opacity - From) / Seconds, 0.0f, 0.0f);
                Animation->End(Seconds, Opacity);
                Element->Effect->SetOpacity(Animation.Get());
            }
            else
            {
                Element->Effect->SetOpacity(Opacity);
            }
        }
    }

    /* Spin="seconds per turn", a repeating rotation about the layer center */
    Period = m_Engine->ResolveNumber(Spin, 0.0f);
    if (Period > 0.0f)
    {
        if (!Element->Rotation && SUCCEEDED(Device->CreateRotateTransform(Element->Rotation.Put())))
            Element->Visual->SetTransform(Element->Rotation.Get());

        Animation.Reset();
        if (Element->Rotation && SUCCEEDED(Device->CreateAnimation(Animation.Put())))
        {
            Element->Rotation->SetCenterX(Element->PixelWidth / 2.0f);
            Element->Rotation->SetCenterY(Element->PixelHeight / 2.0f);
            Animation->AddCubic(0.0, 0.0f, 360.0f / Period, 0.0f, 0.0f);
            Animation->AddRepeat(Period, Period);
            Element->Rotation->SetAngle(Animation.Get());
        }
    }
}

HRESULT
CUixScene::RenderLayers(VOID)
{
    if (!m_Attached || !m_Root)
        return S_OK;

    Update();
    return RenderLayer(m_Root, 0, 0, TRUE);
}

HRESULT
CUixScene::RenderLayer(
    _In_ PUIX_ELEMENT Element,
    _In_ LONG ParentX,
    _In_ LONG ParentY,
    _In_ BOOL ParentShown)
{
    BOOL Shown = ParentShown && Element->Visible;
    BOOL Revealed = FALSE;
    HRESULT Status = S_OK;
    LONG X;
    LONG Y;

    if (Element->IsLayer && Element->Visual)
    {
        if (Shown != Element->AppliedShown)
        {
            /* An empty clip hides the layer and every layer under it */
            if (Shown)
                Element->Visual->SetClip(static_cast<IDCompositionClip *>(NULL));
            else
                Element->Visual->SetClip(UixD2D::Rect(0.0f, 0.0f, 0.0f, 0.0f));
            Revealed = Shown;
            Element->AppliedShown = Shown;
        }

        X = Element->PixelLeft - ParentX;
        Y = Element->PixelTop - ParentY;
        if (Revealed && Element->Get(L"Reveal"))
        {
            Animate(Element, X, Y);
            Element->AppliedX = X;
            Element->AppliedY = Y;
        }
        else if (X != Element->AppliedX || Y != Element->AppliedY)
        {
            Element->Visual->SetOffsetX(static_cast<FLOAT>(X));
            Element->Visual->SetOffsetY(static_cast<FLOAT>(Y));
            Element->AppliedX = X;
            Element->AppliedY = Y;
        }

        if (Shown && Element->Dirty)
            Status = DrawLayer(Element);

        /* Effects start on reveal, or once for layers that are shown from the start */
        if (Shown && (Revealed ||
                      (!Element->Effect && !Element->Rotation && (Element->Get(L"Opacity") || Element->Get(L"Spin")))))
        {
            ApplyEffects(Element, Revealed);
        }

        ParentX = Element->PixelLeft;
        ParentY = Element->PixelTop;
    }

    for (PUIX_ELEMENT Child : Element->Children)
    {
        HRESULT ChildStatus = RenderLayer(Child, ParentX, ParentY, Shown);

        if (FAILED(ChildStatus))
            Status = ChildStatus;
    }
    return Status;
}

HRESULT
CUixScene::DrawLayer(
    _In_ PUIX_ELEMENT Element)
{
    ID2D1DeviceContext *Context = m_Engine->Context();
    ID2D1RenderTarget *Target = Context;
    D2D1_MATRIX_3X2_F Transform;
    D2D1_COLOR_F Clear;
    UixPtr<IDXGISurface> Dxgi;
    D3D11_TEXTURE2D_DESC Desc;
    D2D1_BITMAP_PROPERTIES1 Properties;
    FLOAT Scale = m_Engine->GetScale();
    HRESULT Status;
    UINT Width = static_cast<UINT>(Element->PixelWidth);
    UINT Height = static_cast<UINT>(Element->PixelHeight);

    if (Element->PixelWidth <= 0 || Element->PixelHeight <= 0)
    {
        Element->Visual->SetContent(NULL);
        Element->Surface.Reset();
        Element->Dirty = FALSE;
        return S_OK;
    }

    if (Element->Scratch)
    {
        Element->Scratch->GetDesc(&Desc);
        if (Desc.Width != Width || Desc.Height != Height)
        {
            Element->ScratchTarget.Reset();
            Element->Scratch.Reset();
            Element->Surface.Reset();
        }
    }

    if (!Element->Surface)
    {
        Status = m_Engine->Composition()->CreateSurface(Width, Height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                        DXGI_ALPHA_MODE_PREMULTIPLIED, Element->Surface.Put());
        if (FAILED(Status))
            return Status;

        ZeroMemory(&Desc, sizeof(Desc));
        Desc.Width = Width;
        Desc.Height = Height;
        Desc.MipLevels = 1;
        Desc.ArraySize = 1;
        Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        Desc.SampleDesc.Count = 1;
        Desc.Usage = D3D11_USAGE_DEFAULT;
        Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        Status = m_Engine->Device()->CreateTexture2D(&Desc, NULL, Element->Scratch.Put());
        if (FAILED(Status))
            return Status;

        Status = Element->Scratch.As(&Dxgi);
        if (FAILED(Status))
            return Status;

        Properties = UixD2D::TargetProperties();
        Status = Context->CreateBitmapFromDxgiSurface(Dxgi.Get(), &Properties, Element->ScratchTarget.Put());
        if (FAILED(Status))
            return Status;

        Element->Visual->SetContent(Element->Surface.Get());
    }

    Context->SetTarget(Element->ScratchTarget.Get());
    Context->BeginDraw();
    Clear = UixD2D::Color(0.0f, 0.0f, 0.0f, 0.0f);
    Transform = UixD2D::Multiply(UixD2D::Scale(Scale, Scale),
                                 UixD2D::Translation(static_cast<FLOAT>(-Element->PixelLeft), static_cast<FLOAT>(-Element->PixelTop)));
    Target->Clear(&Clear);
    Target->SetTransform(&Transform);
    Draw(Context, Element, Element);
    Status = Context->EndDraw(NULL, NULL);
    Context->SetTarget(NULL);
    if (FAILED(Status))
        return Status;

    Status = m_Engine->CopyToSurface(Element->Surface.Get(), Element->Scratch.Get(), Width, Height);
    if (SUCCEEDED(Status))
        Element->Dirty = FALSE;
    return Status;
}

HRESULT
STDMETHODCALLTYPE
CUixScene::RenderTo(
    _In_ ID2D1DeviceContext *Context,
    _In_ FLOAT Scale)
{
    if (!m_Root)
        return E_NOT_VALID_STATE;

    ID2D1RenderTarget *Target = Context;
    D2D1_MATRIX_3X2_F Transform = UixD2D::Scale(Scale, Scale);

    Update();
    Target->SetTransform(&Transform);
    Draw(Context, m_Root, NULL);
    Transform = UixD2D::Identity();
    Target->SetTransform(&Transform);
    return S_OK;
}

VOID
CUixScene::FillBackground(
    _In_ ID2D1DeviceContext *Context,
    _In_ const std::wstring &Value,
    _In_ const D2D1_RECT_F &Rect,
    _In_ FLOAT Radius)
{
    UIX_BRUSH_DEF Brush;
    UixPtr<ID2D1Brush> Paint;
    D2D1_ROUNDED_RECT Rounded = UixD2D::RoundedRect(Rect, Radius, Radius);
    ID2D1RenderTarget *Target = Context;

    if (Value.empty() || !m_Engine->ResolveBrush(Value, &Brush))
        return;

    if (Brush.Kind == UixBrushImage)
    {
        FLOAT ImageScale;
        ID2D1Bitmap1 *Bitmap = m_Engine->GetImage(Brush.Image, &ImageScale);
        D2D1_RECT_F Place = Rect;
        D2D1_SIZE_F Size;
        FLOAT Fit;

        if (!Bitmap)
            return;

        Size = Bitmap->GetSize();
        if (Brush.Stretch == UixStretchUniformToFill && Size.width > 0.0f && Size.height > 0.0f)
        {
            Fit = max((Rect.right - Rect.left) / Size.width, (Rect.bottom - Rect.top) / Size.height);
            Place.left = (Rect.left + Rect.right - Size.width * Fit) / 2.0f;
            Place.top = (Rect.top + Rect.bottom - Size.height * Fit) / 2.0f;
            Place.right = Place.left + Size.width * Fit;
            Place.bottom = Place.top + Size.height * Fit;
        }

        Target->PushAxisAlignedClip(&Rect, D2D1_ANTIALIAS_MODE_ALIASED);
        Context->DrawBitmap(Bitmap, &Place, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, NULL, NULL);
        Target->PopAxisAlignedClip();
        return;
    }

    if (Brush.Kind == UixBrushGradient)
    {
        UixPtr<ID2D1GradientStopCollection> Stops;
        UixPtr<ID2D1LinearGradientBrush> Linear;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES Line;

        Line.startPoint = UixD2D::Point(Rect.left, Rect.top);
        Line.endPoint = Brush.Horizontal ? UixD2D::Point(Rect.right, Rect.top) : UixD2D::Point(Rect.left, Rect.bottom);
        if (FAILED(Target->CreateGradientStopCollection(Brush.Stops.data(), static_cast<UINT32>(Brush.Stops.size()),
                                                        D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, Stops.Put())))
            return;
        if (FAILED(Target->CreateLinearGradientBrush(&Line, NULL, Stops.Get(), Linear.Put())))
        {
            return;
        }
        Paint = Linear.Get();
    }
    else
    {
        UixPtr<ID2D1SolidColorBrush> Solid;

        if (FAILED(Target->CreateSolidColorBrush(&Brush.Color, NULL, Solid.Put())))
            return;
        Paint = Solid.Get();
    }

    if (Radius > 0.0f)
        Target->FillRoundedRectangle(&Rounded, Paint.Get());
    else
        Target->FillRectangle(&Rect, Paint.Get());
}

VOID
CUixScene::DrawTextElement(
    _In_ ID2D1DeviceContext *Context,
    _In_ PUIX_ELEMENT Element,
    _In_ const D2D1_RECT_F &Rect)
{
    UixPtr<IDWriteTextLayout> Layout;
    UixPtr<IDWriteInlineObject> Ellipsis;
    UixPtr<ID2D1SolidColorBrush> Brush;
    DWRITE_TRIMMING Trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    D2D1_COLOR_F Color = UixD2D::Color(0.0f, 0.0f, 0.0f);
    IDWriteTextFormat *Format;
    std::wstring Text = m_Engine->ResolveString(Value(Element, L"Content", FALSE));
    std::wstring Align = Value(Element, L"TextAlign", TRUE);
    std::wstring VAlign = Value(Element, L"TextVAlign", FALSE);
    std::wstring Foreground;
    BOOL Wrap = ParseBool(Value(Element, L"Wrap", FALSE).c_str(), FALSE);
    PUIX_ELEMENT Walk;

    if (Text.empty())
        return;

    Format = m_Engine->GetTextFormat(Value(Element, L"Font", TRUE));
    if (!Format)
        return;

    /* A selected ancestor can recolor its text */
    for (Walk = Element; Walk; Walk = Walk->Parent)
    {
        if (Walk == m_Selected && Walk->Get(L"SelectedForeground"))
        {
            Foreground = Value(Walk, L"SelectedForeground", FALSE);
            break;
        }
    }
    if (Foreground.empty())
        Foreground = Value(Element, L"Foreground", TRUE);
    m_Engine->ResolveColor(Foreground, &Color);

    if (FAILED(m_Engine->Write()->CreateTextLayout(Text.c_str(), static_cast<UINT32>(Text.size()), Format,
                                                   Max0(Rect.right - Rect.left), Max0(Rect.bottom - Rect.top), Layout.Put())))
    {
        return;
    }

    Layout->SetWordWrapping(Wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!Wrap && SUCCEEDED(m_Engine->Write()->CreateEllipsisTrimmingSign(Format, Ellipsis.Put())))
        Layout->SetTrimming(&Trimming, Ellipsis.Get());

    if (_wcsicmp(Align.c_str(), L"Center") == 0)
        Layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else if (_wcsicmp(Align.c_str(), L"Right") == 0)
        Layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (_wcsicmp(VAlign.c_str(), L"Top") == 0)
        Layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    else if (_wcsicmp(VAlign.c_str(), L"Bottom") == 0)
        Layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    else
        Layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    /* Highlight="text" bolds and underlines every match, ignoring case */
    std::wstring Highlight = Value(Element, L"Highlight", FALSE);
    if (!Highlight.empty())
    {
        std::wstring Lower = Text;
        std::wstring Needle = Highlight;
        SIZE_T Found = 0;

        for (WCHAR &Char : Lower)
            Char = towlower(Char);
        for (WCHAR &Char : Needle)
            Char = towlower(Char);

        while ((Found = Lower.find(Needle, Found)) != std::wstring::npos)
        {
            DWRITE_TEXT_RANGE Range = { static_cast<UINT32>(Found), static_cast<UINT32>(Needle.size()) };

            Layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, Range);
            Layout->SetUnderline(TRUE, Range);
            Found += Needle.size();
        }
    }

    if (FAILED(static_cast<ID2D1RenderTarget *>(Context)->CreateSolidColorBrush(&Color, NULL, Brush.Put())))
        return;

    static_cast<ID2D1RenderTarget *>(Context)->DrawTextLayout(UixD2D::Point(Rect.left, Rect.top), Layout.Get(), Brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

VOID
CUixScene::DrawImageElement(
    _In_ ID2D1DeviceContext *Context,
    _In_ PUIX_ELEMENT Element,
    _In_ const D2D1_RECT_F &Rect)
{
    FLOAT ImageScale;
    ID2D1Bitmap1 *Bitmap = m_Engine->GetImage(Value(Element, L"Source", FALSE), &ImageScale);
    std::wstring Stretch = Value(Element, L"Stretch", FALSE);
    D2D1_RECT_F Target = Rect;
    D2D1_SIZE_U Size;
    FLOAT Width;
    FLOAT Height;
    FLOAT Fit;

    if (!Bitmap)
        return;

    Size = Bitmap->GetPixelSize();
    Width = Size.width / ImageScale;
    Height = Size.height / ImageScale;

    if (_wcsicmp(Stretch.c_str(), L"Fill") != 0 && Width > 0.0f && Height > 0.0f)
    {
        if (_wcsicmp(Stretch.c_str(), L"None") == 0)
            Fit = 1.0f;
        else
            Fit = min((Rect.right - Rect.left) / Width, (Rect.bottom - Rect.top) / Height);

        Target.left = (Rect.left + Rect.right - Width * Fit) / 2.0f;
        Target.top = (Rect.top + Rect.bottom - Height * Fit) / 2.0f;
        Target.right = Target.left + Width * Fit;
        Target.bottom = Target.top + Height * Fit;
    }

    Context->DrawBitmap(Bitmap, &Target, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, NULL, NULL);
}

VOID
CUixScene::Draw(
    _In_ ID2D1DeviceContext *Context,
    _In_ PUIX_ELEMENT Element,
    _In_opt_ PUIX_ELEMENT Owner)
{
    D2D1_RECT_F Rect;
    D2D1_RECT_F Inner;
    std::wstring Background;
    std::wstring BorderBrush;
    FLOAT Radius;

    if (!Element->Visible)
        return;

    /* Child layers draw into their own surfaces */
    if (Owner && Element != Owner && Element->IsLayer)
        return;

    Rect = UixD2D::Rect(Element->Rect.Left, Element->Rect.Top, Element->Rect.Right, Element->Rect.Bottom);
    Radius = m_Engine->ResolveNumber(Value(Element, L"CornerRadius", FALSE), 0.0f);

    if (Element == m_Selected && Element->Get(L"SelectedBackground"))
        Background = Value(Element, L"SelectedBackground", FALSE);
    else
        Background = Value(Element, L"Background", FALSE);
    FillBackground(Context, Background, Rect, Radius);

    BorderBrush = Value(Element, L"BorderBrush", FALSE);
    if (!BorderBrush.empty())
    {
        const UIX_THICKNESS &Border = Element->Border;

        if (Border.Top > 0.0f)
            FillBackground(Context, BorderBrush, UixD2D::Rect(Rect.left, Rect.top, Rect.right, Rect.top + Border.Top), 0.0f);
        if (Border.Bottom > 0.0f)
            FillBackground(Context, BorderBrush, UixD2D::Rect(Rect.left, Rect.bottom - Border.Bottom, Rect.right, Rect.bottom), 0.0f);
        if (Border.Left > 0.0f)
            FillBackground(Context, BorderBrush, UixD2D::Rect(Rect.left, Rect.top, Rect.left + Border.Left, Rect.bottom), 0.0f);
        if (Border.Right > 0.0f)
            FillBackground(Context, BorderBrush, UixD2D::Rect(Rect.right - Border.Right, Rect.top, Rect.right, Rect.bottom), 0.0f);
    }

    Inner.left = Rect.left + Element->Border.Left + Element->Padding.Left;
    Inner.top = Rect.top + Element->Border.Top + Element->Padding.Top;
    Inner.right = max(Inner.left, Rect.right - Element->Border.Right - Element->Padding.Right);
    Inner.bottom = max(Inner.top, Rect.bottom - Element->Border.Bottom - Element->Padding.Bottom);

    if (Element->Kind == UixKindText)
        DrawTextElement(Context, Element, Inner);
    else if (Element->Kind == UixKindImage)
        DrawImageElement(Context, Element, Inner);

    if (!Element->Scroll)
    {
        for (PUIX_ELEMENT Child : Element->Children)
            Draw(Context, Child, Owner);
        return;
    }

    /* Scrolled children are clipped to the viewport and culled outside it */
    D2D1_RECT_F Clip = UixD2D::Rect(Rect.left + Element->Border.Left, Rect.top + Element->Border.Top,
                                   Rect.right - Element->Border.Right, Rect.bottom - Element->Border.Bottom);
    FLOAT Viewport = Inner.bottom - Inner.top;

    static_cast<ID2D1RenderTarget *>(Context)->PushAxisAlignedClip(&Clip, D2D1_ANTIALIAS_MODE_ALIASED);
    for (PUIX_ELEMENT Child : Element->Children)
    {
        if (Child->Rect.Bottom >= Clip.top && Child->Rect.Top <= Clip.bottom)
            Draw(Context, Child, Owner);
    }
    static_cast<ID2D1RenderTarget *>(Context)->PopAxisAlignedClip();

    if (Element->Extent > Viewport && Element->Get(L"ScrollThumbBrush"))
    {
        FLOAT BarWidth = m_Engine->ResolveNumber(Value(Element, L"ScrollBarWidth", FALSE), 3.0f);
        FLOAT Track = Clip.bottom - Clip.top;
        FLOAT Thumb = max(8.0f, Track * Viewport / Element->Extent);
        FLOAT Position = Clip.top + (Track - Thumb) * Element->ScrollOffset / (Element->Extent - Viewport);

        FillBackground(Context, Value(Element, L"ScrollBarBrush", FALSE),
                       UixD2D::Rect(Clip.right - BarWidth, Clip.top, Clip.right, Clip.bottom), 0.0f);
        FillBackground(Context, Value(Element, L"ScrollThumbBrush", FALSE),
                       UixD2D::Rect(Clip.right - BarWidth, Position, Clip.right, Position + Thumb), 0.0f);
    }
}
