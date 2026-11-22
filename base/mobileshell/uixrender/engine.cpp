/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Co-render engine: devices, composition target, resource resolution
 */

#include "uixp.h"
#include <stdlib.h>
#include <algorithm>

CUixEngine::CUixEngine()
    : m_Refs(1),
      m_Window(NULL),
      m_Scale(1.0f),
      m_Generation(1),
      m_Pending(FALSE)
{
}

CUixEngine::~CUixEngine()
{
    for (CUixScene *Scene : m_Scenes)
        Scene->Detach();
    m_Scenes.clear();
}

ULONG
STDMETHODCALLTYPE
CUixEngine::AddRef(VOID)
{
    return InterlockedIncrement(&m_Refs);
}

ULONG
STDMETHODCALLTYPE
CUixEngine::Release(VOID)
{
    ULONG Refs = InterlockedDecrement(&m_Refs);

    if (Refs == 0)
        delete this;
    return Refs;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::Startup(
    _In_opt_ HWND Window)
{
    static const D3D_FEATURE_LEVEL Levels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3
    };
    UixPtr<IDXGIDevice> Dxgi;
    HRESULT Status;

    Status = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION, m_Device.Put(), NULL, m_Immediate.Put());
    if (FAILED(Status))
    {
        Status = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION, m_Device.Put(), NULL, m_Immediate.Put());
        if (FAILED(Status))
            return Status;
    }

    Status = m_Device.As(&Dxgi);
    if (FAILED(Status))
        return Status;

    Status = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), NULL, m_Factory.PutVoid());
    if (FAILED(Status))
        return Status;

    Status = m_Factory->CreateDevice(Dxgi.Get(), m_Device2D.Put());
    if (FAILED(Status))
        return Status;

    Status = m_Device2D->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_Context.Put());
    if (FAILED(Status))
        return Status;

    /* Composed onto transparent surfaces, so ClearType is not an option */
    m_Context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    Status = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(m_Write.Put()));
    if (FAILED(Status))
        return Status;

    Status = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              __uuidof(IWICImagingFactory), m_Imaging.PutVoid());
    if (FAILED(Status))
        return Status;

    if (!Window)
        return S_OK;

    m_Window = Window;
    /* The newer entry point gives a device that also answers for IDCompositionDesktopDevice,
       which the shared visual of a native app window needs */
    Status = DCompositionCreateDevice2(Dxgi.Get(), __uuidof(IDCompositionDevice), m_Composition.PutVoid());
    if (FAILED(Status))
        return Status;

    Status = m_Composition->CreateTargetForHwnd(Window, TRUE, m_Target.Put());
    if (FAILED(Status))
        return Status;

    Status = m_Composition->CreateVisual(m_Root.Put());
    if (FAILED(Status))
        return Status;

    return m_Target->SetRoot(m_Root.Get());
}

VOID
STDMETHODCALLTYPE
CUixEngine::SetScale(
    _In_ FLOAT Scale)
{
    if (Scale > 0.0f && Scale != m_Scale)
    {
        m_Scale = Scale;
        m_Generation++;
        m_Pending = TRUE;
    }
}

FLOAT
STDMETHODCALLTYPE
CUixEngine::GetScale(VOID)
{
    return m_Scale;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::LoadTheme(
    _In_z_ PCWSTR ThemeFile)
{
    CUixTheme Theme;
    HRESULT Status;

    /* Keep the current theme if the new one is broken */
    Status = Theme.Load(ThemeFile);
    if (FAILED(Status))
        return Status;

    m_Theme = Theme;
    m_Images.clear();
    m_Formats.clear();
    m_Generation++;
    m_Pending = TRUE;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::CreateScene(
    _In_z_ PCWSTR MarkupFile,
    _Outptr_ IUixScene **Scene)
{
    CUixScene *Made;
    HRESULT Status;

    *Scene = NULL;
    Made = new CUixScene(this);
    if (!Made)
        return E_OUTOFMEMORY;

    Status = Made->Load(MarkupFile);
    if (FAILED(Status))
    {
        Made->Release();
        return Status;
    }

    *Scene = Made;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::GetRootVisual(
    _Outptr_ IDCompositionVisual **Visual)
{
    *Visual = m_Root.Get();
    if (!*Visual)
        return E_NOT_VALID_STATE;
    (*Visual)->AddRef();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::GetCompositionDevice(
    _Outptr_ IDCompositionDevice **Device)
{
    *Device = m_Composition.Get();
    if (!*Device)
        return E_NOT_VALID_STATE;
    (*Device)->AddRef();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::GetDirect3DDevice(
    _Outptr_ ID3D11Device **Device)
{
    *Device = m_Device.Get();
    if (!*Device)
        return E_NOT_VALID_STATE;
    (*Device)->AddRef();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::GetDirect2DContext(
    _Outptr_ ID2D1DeviceContext **Context)
{
    *Context = m_Context.Get();
    if (!*Context)
        return E_NOT_VALID_STATE;
    (*Context)->AddRef();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::CopyToSurface(
    _In_ IDCompositionSurface *Surface,
    _In_ ID3D11Texture2D *Source,
    _In_ UINT Width,
    _In_ UINT Height)
{
    UixPtr<IDXGISurface> Target;
    UixPtr<ID3D11Texture2D> Texture;
    D3D11_BOX Box;
    POINT Offset;
    HRESULT Status;

    /*
     * The surface is filled by a copy rather than by Direct2D drawing into it, which
     * keeps the same path working on compositors whose BeginDraw only hands out DXGI.
     */
    Status = Surface->BeginDraw(NULL, __uuidof(IDXGISurface), Target.PutVoid(), &Offset);
    if (FAILED(Status))
        return Status;

    Status = Target.As(&Texture);
    if (SUCCEEDED(Status))
    {
        Box.left = 0;
        Box.top = 0;
        Box.front = 0;
        Box.right = Width;
        Box.bottom = Height;
        Box.back = 1;
        m_Immediate->CopySubresourceRegion(Texture.Get(), 0, Offset.x, Offset.y, 0, Source, 0, &Box);
    }

    Surface->EndDraw();
    return Status;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::PreRender(VOID)
{
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::BeginRender(VOID)
{
    return m_Composition ? S_OK : E_NOT_VALID_STATE;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::Render(VOID)
{
    HRESULT Status = S_OK;
    HRESULT SceneStatus;

    for (CUixScene *Scene : m_Scenes)
    {
        SceneStatus = Scene->RenderLayers();
        if (FAILED(SceneStatus))
            Status = SceneStatus;
    }
    return Status;
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::EndRender(VOID)
{
    m_Pending = FALSE;
    return m_Composition->Commit();
}

HRESULT
STDMETHODCALLTYPE
CUixEngine::PostPresent(VOID)
{
    return S_OK;
}

BOOL
STDMETHODCALLTYPE
CUixEngine::ShouldRender(VOID)
{
    if (m_Pending)
        return TRUE;

    for (CUixScene *Scene : m_Scenes)
    {
        if (Scene->HasDirtyLayers())
            return TRUE;
    }
    return FALSE;
}

VOID
CUixEngine::AttachScene(
    _In_ CUixScene *Scene)
{
    m_Scenes.push_back(Scene);
    m_Pending = TRUE;
}

VOID
CUixEngine::DetachScene(
    _In_ CUixScene *Scene)
{
    m_Scenes.erase(std::remove(m_Scenes.begin(), m_Scenes.end(), Scene), m_Scenes.end());
    m_Pending = TRUE;
}

FLOAT
CUixEngine::ResolveNumber(
    _In_ const std::wstring &Value,
    _In_ FLOAT Default) const
{
    FLOAT Number;

    if (Value.empty())
        return Default;

    if (Value[0] == L'@')
        return m_Theme.FindMetric(Value.c_str() + 1, &Number) ? Number : Default;

    if (Value[0] == L'-' || Value[0] == L'.' || iswdigit(Value[0]))
        return static_cast<FLOAT>(_wtof(Value.c_str()));
    return Default;
}

BOOL
CUixEngine::ResolveColor(
    _In_ const std::wstring &Value,
    _Out_ D2D1_COLOR_F *Color) const
{
    if (Value.empty())
        return FALSE;
    if (Value[0] == L'@')
        return m_Theme.FindColor(Value.c_str() + 1, Color);
    return UixParseColor(Value.c_str(), Color);
}

BOOL
CUixEngine::ResolveBrush(
    _In_ const std::wstring &Value,
    _Out_ UIX_BRUSH_DEF *Brush) const
{
    const UIX_BRUSH_DEF *Found;

    if (Value.empty())
        return FALSE;

    if (Value[0] == L'@')
    {
        Found = m_Theme.FindBrush(Value.c_str() + 1);
        if (Found)
        {
            *Brush = *Found;
            return TRUE;
        }
    }

    Brush->Kind = UixBrushSolid;
    Brush->Stops.clear();
    Brush->Image.clear();
    Brush->Horizontal = FALSE;
    Brush->Stretch = UixStretchFill;
    return ResolveColor(Value, &Brush->Color);
}

std::wstring
CUixEngine::ResolveString(
    _In_ const std::wstring &Value) const
{
    PCWSTR Found;

    if (Value.size() > 1 && Value[0] == L'@')
    {
        Found = m_Theme.FindString(Value.c_str() + 1);
        if (Found)
            return Found;
    }
    return Value;
}

/* Family="A, B" takes the first installed family, so a skin can name a fallback */
std::wstring
CUixEngine::PickFamily(
    _In_ const std::wstring &Families)
{
    UixPtr<IDWriteFontCollection> Fonts;
    std::wstring Name;
    SIZE_T Start = 0;
    SIZE_T End;
    UINT32 Index;
    BOOL Exists;

    if (Families.find(L',') == std::wstring::npos)
        return Families;
    if (FAILED(m_Write->GetSystemFontCollection(Fonts.Put(), FALSE)))
        return Families.substr(0, Families.find(L','));

    while (Start < Families.size())
    {
        End = Families.find(L',', Start);
        if (End == std::wstring::npos)
            End = Families.size();

        Name = Families.substr(Start, End - Start);
        Name.erase(0, Name.find_first_not_of(L' '));
        Name.erase(Name.find_last_not_of(L' ') + 1);
        Exists = FALSE;
        if (!Name.empty() && SUCCEEDED(Fonts->FindFamilyName(Name.c_str(), &Index, &Exists)) && Exists)
            return Name;
        Start = End + 1;
    }
    return Name;
}

IDWriteTextFormat *
CUixEngine::GetTextFormat(
    _In_ const std::wstring &Value)
{
    UixPtr<IDWriteTextFormat> Format;
    UIX_FONT_DEF Font;
    const UIX_FONT_DEF *Found = NULL;
    std::wstring Key = Value.empty() ? L"@Default" : Value;

    auto Entry = m_Formats.find(Key);
    if (Entry != m_Formats.end())
        return Entry->second.Get();

    if (Key[0] == L'@')
        Found = m_Theme.FindFont(Key.c_str() + 1);
    if (!Found)
        Found = m_Theme.FindFont(L"Default");

    if (Found)
    {
        Font = *Found;
    }
    else
    {
        Font.Family = L"Tahoma";
        Font.Size = 11.0f;
        Font.Weight = DWRITE_FONT_WEIGHT_NORMAL;
        Font.Style = DWRITE_FONT_STYLE_NORMAL;
    }

    Font.Family = PickFamily(Font.Family);
    if (FAILED(m_Write->CreateTextFormat(Font.Family.c_str(), NULL, Font.Weight, Font.Style,
                                         DWRITE_FONT_STRETCH_NORMAL, Font.Size, L"en-us", Format.Put())))
    {
        return NULL;
    }

    m_Formats[Key] = Format;
    return Format.Get();
}

HRESULT
CUixEngine::LoadBitmap(
    _In_z_ PCWSTR Path,
    _Outptr_ ID2D1Bitmap1 **Bitmap)
{
    UixPtr<IWICBitmapDecoder> Decoder;
    UixPtr<IWICBitmapFrameDecode> Frame;
    UixPtr<IWICFormatConverter> Converter;
    HRESULT Status;

    *Bitmap = NULL;
    Status = m_Imaging->CreateDecoderFromFilename(Path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, Decoder.Put());
    if (FAILED(Status))
        return Status;

    Status = Decoder->GetFrame(0, Frame.Put());
    if (FAILED(Status))
        return Status;

    Status = m_Imaging->CreateFormatConverter(Converter.Put());
    if (FAILED(Status))
        return Status;

    Status = Converter->Initialize(Frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(Status))
        return Status;

    return m_Context->CreateBitmapFromWicBitmap(Converter.Get(), NULL, Bitmap);
}

ID2D1Bitmap1 *
CUixEngine::GetImage(
    _In_ const std::wstring &Value,
    _Out_ FLOAT *ImageScale)
{
    UixPtr<ID2D1Bitmap1> Bitmap;
    UIX_IMAGE_DEF Image;
    std::wstring Key;

    *ImageScale = 1.0f;

    /* "file:<path>" shows a picture from disk, used for previews */
    if (Value.compare(0, 5, L"file:") == 0 && Value.size() > 5)
    {
        Image.Path = Value.substr(5);
        Image.Scale = 1.0f;
    }
    else if (Value.size() < 2 || Value[0] != L'@' || !m_Theme.FindImage(Value.c_str() + 1, &Image))
    {
        return NULL;
    }

    *ImageScale = Image.Scale;
    Key = Image.Path;

    auto Entry = m_Images.find(Key);
    if (Entry != m_Images.end())
        return Entry->second.Get();

    /* A failed load is cached as NULL so it is not retried every frame */
    LoadBitmap(Image.Path.c_str(), Bitmap.Put());
    m_Images[Key] = Bitmap;
    return Bitmap.Get();
}

EXTERN_C
UIX_API
HRESULT
WINAPI
UixCreateEngine(
    _Outptr_ IUixEngine **Engine)
{
    *Engine = new CUixEngine();
    return *Engine ? S_OK : E_OUTOFMEMORY;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);
    return TRUE;
}
