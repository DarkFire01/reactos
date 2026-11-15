/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Theme loading: colors, brushes, fonts, images, metrics, strings
 */

#include "uixp.h"
#include <stdlib.h>

#define UIX_THEME_MAX_DEPTH 8

static
ULONG
HexDigit(
    _In_ WCHAR Char)
{
    if (Char >= L'0' && Char <= L'9')
        return Char - L'0';
    if (Char >= L'a' && Char <= L'f')
        return Char - L'a' + 10;
    if (Char >= L'A' && Char <= L'F')
        return Char - L'A' + 10;
    return 0x100;
}

BOOL
UixParseColor(
    _In_z_ PCWSTR Text,
    _Out_ D2D1_COLOR_F *Color)
{
    ULONG Value = 0;
    ULONG Digit;
    SIZE_T Length;
    SIZE_T Index;

    if (!Text || Text[0] != L'#')
        return FALSE;

    Length = wcslen(Text + 1);
    if (Length != 6 && Length != 8)
        return FALSE;

    for (Index = 1; Index <= Length; Index++)
    {
        Digit = HexDigit(Text[Index]);
        if (Digit > 0xF)
            return FALSE;
        Value = (Value << 4) | Digit;
    }

    if (Length == 6)
        Value |= 0xFF000000;

    Color->a = ((Value >> 24) & 0xFF) / 255.0f;
    Color->r = ((Value >> 16) & 0xFF) / 255.0f;
    Color->g = ((Value >> 8) & 0xFF) / 255.0f;
    Color->b = (Value & 0xFF) / 255.0f;
    return TRUE;
}

static
std::wstring
JoinPath(
    _In_ const std::wstring &Directory,
    _In_z_ PCWSTR Relative)
{
    WCHAR Full[MAX_PATH];
    std::wstring Joined;

    if (PathIsRelativeW(Relative))
    {
        Joined = Directory;
        if (!Joined.empty() && Joined.back() != L'\\')
            Joined += L'\\';
        Joined += Relative;
    }
    else
    {
        Joined = Relative;
    }

    if (GetFullPathNameW(Joined.c_str(), MAX_PATH, Full, NULL))
        return Full;
    return Joined;
}

static
std::wstring
DirectoryOf(
    _In_z_ PCWSTR File)
{
    std::wstring Directory = File;
    SIZE_T Slash = Directory.find_last_of(L"\\/");

    if (Slash == std::wstring::npos)
        return L".";
    return Directory.substr(0, Slash);
}

VOID
CUixTheme::Clear(VOID)
{
    m_Name.clear();
    m_Colors.clear();
    m_Brushes.clear();
    m_Fonts.clear();
    m_Images.clear();
    m_Metrics.clear();
    m_Strings.clear();
    m_ImageSets.clear();
}

HRESULT
CUixTheme::Load(
    _In_z_ PCWSTR ThemeFile)
{
    Clear();
    return LoadOne(ThemeFile, 0);
}

BOOL
CUixTheme::ParseColorValue(
    _In_z_ PCWSTR Value,
    _Out_ D2D1_COLOR_F *Color) const
{
    if (Value[0] == L'@')
        return FindColor(Value + 1, Color);
    return UixParseColor(Value, Color);
}

HRESULT
CUixTheme::LoadOne(
    _In_z_ PCWSTR ThemeFile,
    _In_ ULONG Depth)
{
    UIX_XML_NODE Root;
    std::wstring Directory;
    PCWSTR Base;
    PCWSTR Name;
    HRESULT Status;

    if (Depth > UIX_THEME_MAX_DEPTH)
        return E_FAIL;

    Status = UixXmlLoad(ThemeFile, &Root);
    if (FAILED(Status))
        return Status;

    Directory = DirectoryOf(ThemeFile);

    /* The base theme loads first so this one overrides it */
    Base = Root.Attr(L"Base");
    if (Base)
    {
        Status = LoadOne(JoinPath(Directory, Base).c_str(), Depth + 1);
        if (FAILED(Status))
            return Status;
    }

    if (Root.Attr(L"Name"))
        m_Name = Root.Attr(L"Name");

    for (const UIX_XML_NODE &Node : Root.Children)
    {
        Name = Node.Attr(L"Name");

        if (Node.Name == L"ImageSet")
        {
            UIX_IMAGE_SET Set;

            Set.Prefix = Node.Attr(L"Prefix", L"");
            Set.Directory = JoinPath(Directory, Node.Attr(L"Directory", L"."));
            Set.Scale = static_cast<FLOAT>(_wtof(Node.Attr(L"Scale", L"1")));
            if (Set.Scale <= 0.0f)
                Set.Scale = 1.0f;

            /* Later sets win, so search order is newest first */
            m_ImageSets.insert(m_ImageSets.begin(), Set);
            continue;
        }

        if (!Name)
            continue;

        if (Node.Name == L"Color")
        {
            D2D1_COLOR_F Color;

            if (ParseColorValue(Node.Attr(L"Value", L""), &Color))
                m_Colors[Name] = Color;
        }
        else if (Node.Name == L"Brush")
        {
            UIX_BRUSH_DEF Brush;
            PCWSTR Type = Node.Attr(L"Type", L"Solid");
            PCWSTR Stretch = Node.Attr(L"Stretch", L"Fill");

            Brush.Kind = UixBrushSolid;
            Brush.Color = UixD2D::Color(0.0f, 0.0f, 0.0f, 0.0f);
            Brush.Horizontal = (_wcsicmp(Node.Attr(L"Direction", L"Vertical"), L"Horizontal") == 0);
            Brush.Stretch = UixStretchFill;

            if (_wcsicmp(Stretch, L"Uniform") == 0)
                Brush.Stretch = UixStretchUniform;
            else if (_wcsicmp(Stretch, L"UniformToFill") == 0)
                Brush.Stretch = UixStretchUniformToFill;

            if (_wcsicmp(Type, L"Gradient") == 0)
            {
                Brush.Kind = UixBrushGradient;
                for (const UIX_XML_NODE &Stop : Node.Children)
                {
                    D2D1_GRADIENT_STOP Entry;

                    if (Stop.Name != L"Stop")
                        continue;
                    Entry.position = static_cast<FLOAT>(_wtof(Stop.Attr(L"Offset", L"0")));
                    if (ParseColorValue(Stop.Attr(L"Color", L""), &Entry.color))
                        Brush.Stops.push_back(Entry);
                }
                if (Brush.Stops.empty())
                    continue;
            }
            else if (_wcsicmp(Type, L"Image") == 0)
            {
                Brush.Kind = UixBrushImage;
                Brush.Image = Node.Attr(L"Image", L"");
            }
            else if (!ParseColorValue(Node.Attr(L"Color", L""), &Brush.Color))
            {
                continue;
            }

            m_Brushes[Name] = Brush;
        }
        else if (Node.Name == L"Font")
        {
            UIX_FONT_DEF Font;
            PCWSTR Weight = Node.Attr(L"Weight", L"Normal");

            Font.Family = Node.Attr(L"Family", L"Tahoma");
            Font.Size = static_cast<FLOAT>(_wtof(Node.Attr(L"Size", L"11")));
            Font.Style = (_wcsicmp(Node.Attr(L"Style", L"Normal"), L"Italic") == 0) ?
                         DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

            if (_wcsicmp(Weight, L"Bold") == 0)
                Font.Weight = DWRITE_FONT_WEIGHT_BOLD;
            else if (_wcsicmp(Weight, L"SemiBold") == 0)
                Font.Weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
            else if (_wcsicmp(Weight, L"Light") == 0)
                Font.Weight = DWRITE_FONT_WEIGHT_LIGHT;
            else if (iswdigit(Weight[0]))
                Font.Weight = static_cast<DWRITE_FONT_WEIGHT>(_wtoi(Weight));
            else
                Font.Weight = DWRITE_FONT_WEIGHT_NORMAL;

            m_Fonts[Name] = Font;
        }
        else if (Node.Name == L"Image")
        {
            UIX_IMAGE_DEF Image;

            Image.Path = JoinPath(Directory, Node.Attr(L"Source", L""));
            Image.Scale = static_cast<FLOAT>(_wtof(Node.Attr(L"Scale", L"1")));
            if (Image.Scale <= 0.0f)
                Image.Scale = 1.0f;
            m_Images[Name] = Image;
        }
        else if (Node.Name == L"Metric")
        {
            m_Metrics[Name] = static_cast<FLOAT>(_wtof(Node.Attr(L"Value", L"0")));
        }
        else if (Node.Name == L"String")
        {
            m_Strings[Name] = Node.Attr(L"Value", L"");
        }
    }

    return S_OK;
}

BOOL
CUixTheme::FindColor(
    _In_z_ PCWSTR Name,
    _Out_ D2D1_COLOR_F *Color) const
{
    auto Entry = m_Colors.find(Name);

    if (Entry != m_Colors.end())
    {
        *Color = Entry->second;
        return TRUE;
    }

    /* A solid brush is usable wherever a color is */
    auto Brush = m_Brushes.find(Name);
    if (Brush != m_Brushes.end() && Brush->second.Kind == UixBrushSolid)
    {
        *Color = Brush->second.Color;
        return TRUE;
    }
    return FALSE;
}

const UIX_BRUSH_DEF *
CUixTheme::FindBrush(
    _In_z_ PCWSTR Name) const
{
    auto Entry = m_Brushes.find(Name);

    return (Entry != m_Brushes.end()) ? &Entry->second : NULL;
}

const UIX_FONT_DEF *
CUixTheme::FindFont(
    _In_z_ PCWSTR Name) const
{
    auto Entry = m_Fonts.find(Name);

    return (Entry != m_Fonts.end()) ? &Entry->second : NULL;
}

BOOL
CUixTheme::FindImage(
    _In_z_ PCWSTR Name,
    _Out_ UIX_IMAGE_DEF *Image) const
{
    auto Entry = m_Images.find(Name);

    if (Entry != m_Images.end())
    {
        *Image = Entry->second;
        return TRUE;
    }

    for (const UIX_IMAGE_SET &Set : m_ImageSets)
    {
        SIZE_T Length = Set.Prefix.size();

        if (wcsncmp(Name, Set.Prefix.c_str(), Length) == 0 && Name[Length])
        {
            Image->Path = Set.Directory + L"\\" + (Name + Length) + L".png";
            Image->Scale = Set.Scale;
            if (GetFileAttributesW(Image->Path.c_str()) != INVALID_FILE_ATTRIBUTES)
                return TRUE;
        }
    }
    return FALSE;
}

BOOL
CUixTheme::FindMetric(
    _In_z_ PCWSTR Name,
    _Out_ FLOAT *Value) const
{
    auto Entry = m_Metrics.find(Name);

    if (Entry == m_Metrics.end())
        return FALSE;

    *Value = Entry->second;
    return TRUE;
}

PCWSTR
CUixTheme::FindString(
    _In_z_ PCWSTR Name) const
{
    auto Entry = m_Strings.find(Name);

    return (Entry != m_Strings.end()) ? Entry->second.c_str() : NULL;
}
