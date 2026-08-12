/*
 * PROJECT:     ReactOS DirectX regression test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Reproduce the D2VidTst.exe (Diablo II video test) access
 *              violation triggered when the display resolution / color
 *              palette changes and a DirectDraw operation follows.
 *
 * D2VidTst.exe runs three detection phases (matching its TestDDraw /
 * TestD3D / TestGLIDE switches): DDrawDetect, Direct3DDetect and
 * GlideDetect. This program replays the first two, recovered by analyzing
 * the binary's strings and import table. GlideDetect is not replayed: it
 * dynamically loads the 3dfx glide3x.dll and the original itself skips the
 * phase when that DLL is absent (always the case on ReactOS).
 *
 * Phase 1 - DDrawDetect ("sEnumerateDDrawDevices"):
 *   DirectDrawEnumerateA
 *     -> DirectDrawCreate
 *     -> QueryInterface(IID_IDirectDraw4)
 *     -> IDirectDraw4::SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)
 *     -> IDirectDraw4::EnumDisplayModes
 *     -> per mode: IDirectDraw4::SetDisplayMode        (res + bpp change!)
 *                  IDirectDraw4::CreateSurface         (primary, triple ->
 *                                                       double -> single)
 *                  IDirectDrawSurface4::GetAttachedSurface (backbuffer)
 *                  IDirectDraw4::CreatePalette / SetPalette / SetEntries
 *                  Blt (colorfill), Lock/Unlock, Flip
 *     -> IDirectDraw4::RestoreDisplayMode
 *
 * Phase 2 - Direct3DDetect ("sEnumerateD3DDevices"):
 *   per DirectDraw device:
 *     -> DirectDrawCreate -> QueryInterface(IID_IDirectDraw4)
 *     -> QueryInterface(IID_IDirect3D3)
 *     -> IDirectDraw4::GetCaps                          ("Failed to get DD4 Caps")
 *     -> IDirectDraw4::GetAvailableVidMem               (video/texture/local/AGP)
 *     -> IDirect3D3::EnumDevices
 *     -> IDirectDraw4::SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)
 *     -> per mode: IDirectDraw4::SetDisplayMode         (res + bpp change!)
 *                  IDirectDraw4::CreateSurface          (flip chain, 3DDEVICE)
 *                  IDirectDrawSurface4::GetAttachedSurface
 *                  IDirect3D3::CreateDevice(HAL)        ("Create 3D HAL Device")
 *                  IDirect3DDevice3::GetCaps / EnumTextureFormats
 *     -> IDirectDraw4::RestoreDisplayMode
 *
 * Command line (mirrors the original's switches): no arguments runs both
 * phases; "TestDDraw" runs only phase 1, "TestD3D" runs only phase 2.
 *
 * Every step is logged (flushed) to dxfailtest.log next to the EXE and to
 * the debugger via OutputDebugStringA, so when the access violation occurs
 * on ReactOS the last logged line identifies the faulting operation. An
 * unhandled exception filter additionally logs the exception code, the
 * faulting address and the access type before the process dies.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <initguid.h>
#include <ddraw.h>
#include <d3d.h>

#define ENUM_CLASS_NAME  "Enumeration Class"   /* same class name D2VidTst uses */
#define WINDOW_TITLE     "D2VideoTest"
#define MAX_DEVICES      8

typedef struct
{
    GUID Guid;
    BOOL HasGuid;       /* FALSE = primary device (NULL GUID) */
    char Description[128];
} DDRAW_DEVICE;

typedef struct
{
    DWORD Width;
    DWORD Height;
    DWORD Bpp;
} TEST_MODE;

/* The modes Diablo II cares about: 640x480 (game/cutscenes) and 800x600 (UI),
 * in 8-bit palettized and 16-bit. The 8bpp entries are the ones that change
 * both the resolution and the color palette depth at once. */
static const TEST_MODE g_TestModes[] =
{
    { 640, 480,  8 },
    { 640, 480, 16 },
    { 800, 600,  8 },
    { 800, 600, 16 },
};

/* Diablo II's Direct3D renderer runs in 16-bit modes only */
static const TEST_MODE g_D3DTestModes[] =
{
    { 640, 480, 16 },
    { 800, 600, 16 },
};

static HWND g_hWnd;
static FILE *g_LogFile;
static char g_LastStep[512] = "startup";
static DDRAW_DEVICE g_Devices[MAX_DEVICES];
static int g_DeviceCount;
static int g_ModesLogged;

static void Log(const char *Format, ...)
{
    char Buffer[512];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(Buffer, sizeof(Buffer) - 1, Format, Args);
    va_end(Args);
    Buffer[sizeof(Buffer) - 1] = '\0';

    lstrcpynA(g_LastStep, Buffer, sizeof(g_LastStep));

    OutputDebugStringA(Buffer);
    OutputDebugStringA("\n");

    if (g_LogFile)
    {
        fprintf(g_LogFile, "%s\n", Buffer);
        fflush(g_LogFile);   /* flush before the next DirectX call so the log survives the AV */
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *Info)
{
    EXCEPTION_RECORD *Rec = Info->ExceptionRecord;

    if (g_LogFile)
    {
        fprintf(g_LogFile,
                "!!!!! UNHANDLED EXCEPTION 0x%08lX at address %p\n"
                "!!!!! Last step before crash: %s\n",
                Rec->ExceptionCode, Rec->ExceptionAddress, g_LastStep);

        if (Rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            Rec->NumberParameters >= 2)
        {
            fprintf(g_LogFile,
                    "!!!!! Access violation %s address %p\n",
                    Rec->ExceptionInformation[0] ? "writing" : "reading",
                    (void *)Rec->ExceptionInformation[1]);
        }
        fflush(g_LogFile);
    }

    /* Try to give the desktop back so the machine stays usable */
    ChangeDisplaySettingsA(NULL, 0);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void PumpMessages(void)
{
    MSG Msg;
    while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&Msg);
        DispatchMessageA(&Msg);
    }
}

static LRESULT CALLBACK EnumWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static BOOL WINAPI DeviceEnumCallback(GUID *pGuid, LPSTR pszDesc, LPSTR pszName, LPVOID pContext)
{
    Log("Enumerating display device: %s (%s), GUID=%s",
        pszDesc ? pszDesc : "(null)",
        pszName ? pszName : "(null)",
        pGuid ? "specific" : "NULL/primary");

    if (g_DeviceCount < MAX_DEVICES)
    {
        DDRAW_DEVICE *Dev = &g_Devices[g_DeviceCount++];
        if (pGuid)
        {
            Dev->Guid = *pGuid;   /* the GUID pointer is only valid during the callback */
            Dev->HasGuid = TRUE;
        }
        else
        {
            Dev->HasGuid = FALSE;
        }
        lstrcpynA(Dev->Description, pszDesc ? pszDesc : "(unknown)", sizeof(Dev->Description));
    }
    return DDENUMRET_OK;
}

static HRESULT WINAPI ModeEnumCallback(LPDDSURFACEDESC2 pDesc, LPVOID pContext)
{
    g_ModesLogged++;
    Log("  EnumDisplayModes: %lux%lux%lu",
        pDesc->dwWidth, pDesc->dwHeight,
        pDesc->ddpfPixelFormat.dwRGBBitCount);
    return DDENUMRET_OK;
}

/* Create the primary surface the way D2VidTst does: triple buffered first,
 * then double, then single. Returns the primary and (optionally) backbuffer. */
static HRESULT CreatePrimary(IDirectDraw4 *pDD4,
                             IDirectDrawSurface4 **ppPrimary,
                             IDirectDrawSurface4 **ppBack,
                             BOOL For3D)
{
    DDSURFACEDESC2 Desc;
    DDSCAPS2 Caps;
    HRESULT hr;
    DWORD BackBuffers;

    *ppPrimary = NULL;
    *ppBack = NULL;

    for (BackBuffers = 2; BackBuffers >= 1; BackBuffers--)
    {
        ZeroMemory(&Desc, sizeof(Desc));
        Desc.dwSize = sizeof(Desc);
        Desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        Desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        if (For3D)
            Desc.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE;
        Desc.dwBackBufferCount = BackBuffers;

        Log("CreateSurface (primary, %lu backbuffers)...", BackBuffers);
        hr = IDirectDraw4_CreateSurface(pDD4, &Desc, ppPrimary, NULL);
        if (SUCCEEDED(hr))
        {
            ZeroMemory(&Caps, sizeof(Caps));
            Caps.dwCaps = DDSCAPS_BACKBUFFER;
            Log("GetAttachedSurface (backbuffer)...");
            hr = IDirectDrawSurface4_GetAttachedSurface(*ppPrimary, &Caps, ppBack);
            if (FAILED(hr))
            {
                Log("Failed to get backbuffer surface (hr=0x%08lX)", hr);
                IDirectDrawSurface4_Release(*ppPrimary);
                *ppPrimary = NULL;
                return hr;
            }
            return DD_OK;
        }
        Log("Failed to open window for %s buffering (hr=0x%08lX). Trying %s buffered...",
            BackBuffers == 2 ? "triple" : "double",
            hr,
            BackBuffers == 2 ? "double" : "single");
    }

    /* A 3D device needs a backbuffer render target - no single buffer fallback */
    if (For3D)
        return hr;

    /* Single buffered fallback */
    ZeroMemory(&Desc, sizeof(Desc));
    Desc.dwSize = sizeof(Desc);
    Desc.dwFlags = DDSD_CAPS;
    Desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    Log("CreateSurface (primary single buffered)...");
    hr = IDirectDraw4_CreateSurface(pDD4, &Desc, ppPrimary, NULL);
    if (FAILED(hr))
    {
        Log("CreateSurface (primary single buffered) failed! (hr=0x%08lX)", hr);
        *ppPrimary = NULL;
    }
    return hr;
}

/* The core of the repro: change resolution + bit depth, then immediately
 * perform DirectDraw operations on it. */
static void TestMode(IDirectDraw4 *pDD4, const TEST_MODE *pMode)
{
    IDirectDrawSurface4 *pPrimary = NULL;
    IDirectDrawSurface4 *pBack = NULL;
    IDirectDrawPalette *pPalette = NULL;
    IDirectDrawSurface4 *pTarget;
    PALETTEENTRY Entries[256];
    DDSURFACEDESC2 Desc;
    DDBLTFX BltFx;
    HRESULT hr;
    int i;

    Log("=== Testing mode %lux%lux%lu ===", pMode->Width, pMode->Height, pMode->Bpp);

    /* --- 1. The mode switch: resolution AND color depth change at once --- */
    Log("IDirectDraw4_SetDisplayMode(%lu, %lu, %lu)...", pMode->Width, pMode->Height, pMode->Bpp);
    hr = IDirectDraw4_SetDisplayMode(pDD4, pMode->Width, pMode->Height, pMode->Bpp, 0, 0);
    if (FAILED(hr))
    {
        Log("SetDisplayMode (mode enumeration) failed! (hr=0x%08lX)", hr);
        return;
    }
    PumpMessages();

    /* --- 2. First DirectDraw operation after the mode switch --- */
    hr = CreatePrimary(pDD4, &pPrimary, &pBack, FALSE);
    if (FAILED(hr))
        return;

    pTarget = pBack ? pBack : pPrimary;

    /* --- 3. Palette work (the "color pallet change") on 8bpp modes --- */
    if (pMode->Bpp == 8)
    {
        for (i = 0; i < 256; i++)
        {
            Entries[i].peRed   = (BYTE)i;
            Entries[i].peGreen = (BYTE)(255 - i);
            Entries[i].peBlue  = (BYTE)(i ^ 0xAA);
            Entries[i].peFlags = 0;
        }

        Log("CreatePalette (8BIT | ALLOW256)...");
        hr = IDirectDraw4_CreatePalette(pDD4, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
                                        Entries, &pPalette, NULL);
        if (SUCCEEDED(hr))
        {
            Log("SetPalette on primary surface...");
            hr = IDirectDrawSurface4_SetPalette(pPrimary, pPalette);
            if (FAILED(hr))
                Log("SetPalette failed (hr=0x%08lX)", hr);

            /* Change the palette while the surface is live */
            for (i = 0; i < 256; i++)
                Entries[i].peRed = (BYTE)(255 - i);
            Log("IDirectDrawPalette_SetEntries (live palette change)...");
            hr = IDirectDrawPalette_SetEntries(pPalette, 0, 0, 256, Entries);
            if (FAILED(hr))
                Log("SetEntries failed (hr=0x%08lX)", hr);
        }
        else
        {
            Log("CreatePalette failed (hr=0x%08lX)", hr);
            pPalette = NULL;
        }
    }

    /* --- 4. Blt right after mode/palette change --- */
    ZeroMemory(&BltFx, sizeof(BltFx));
    BltFx.dwSize = sizeof(BltFx);
    BltFx.dwFillColor = 0;
    Log("Blt (DDBLT_COLORFILL) on %s...", pBack ? "backbuffer" : "primary");
    hr = IDirectDrawSurface4_Blt(pTarget, NULL, NULL, NULL,
                                 DDBLT_COLORFILL | DDBLT_WAIT, &BltFx);
    if (FAILED(hr))
        Log("Blt failed (hr=0x%08lX)", hr);

    /* --- 5. Lock/Unlock: direct framebuffer access after mode change --- */
    ZeroMemory(&Desc, sizeof(Desc));
    Desc.dwSize = sizeof(Desc);
    Log("Lock on %s...", pBack ? "backbuffer" : "primary");
    hr = IDirectDrawSurface4_Lock(pTarget, NULL, &Desc, DDLOCK_WAIT, NULL);
    if (SUCCEEDED(hr))
    {
        BYTE *Row = (BYTE *)Desc.lpSurface;
        DWORD y;
        Log("Lock OK: lpSurface=%p pitch=%ld, writing pixels...", Desc.lpSurface, Desc.lPitch);
        for (y = 0; y < pMode->Height; y++)
        {
            memset(Row, (int)(y & 0xFF), pMode->Width * (pMode->Bpp / 8));
            Row += Desc.lPitch;
        }
        Log("Unlock...");
        IDirectDrawSurface4_Unlock(pTarget, NULL);
    }
    else
    {
        Log("Lock failed (hr=0x%08lX)", hr);
    }

    /* --- 6. Flip the chain a few times --- */
    if (pBack)
    {
        for (i = 0; i < 3; i++)
        {
            Log("Flip #%d...", i + 1);
            hr = IDirectDrawSurface4_Flip(pPrimary, NULL, DDFLIP_WAIT);
            if (FAILED(hr))
            {
                Log("Flip failed (hr=0x%08lX)", hr);
                break;
            }
        }
    }

    PumpMessages();

    Log("Releasing surfaces for mode %lux%lux%lu...", pMode->Width, pMode->Height, pMode->Bpp);
    if (pPalette)
        IDirectDrawPalette_Release(pPalette);
    if (pBack)
        IDirectDrawSurface4_Release(pBack);
    if (pPrimary)
        IDirectDrawSurface4_Release(pPrimary);
    Log("Mode %lux%lux%lu done.", pMode->Width, pMode->Height, pMode->Bpp);
}

static void TestDevice(const DDRAW_DEVICE *pDev)
{
    IDirectDraw *pDD = NULL;
    IDirectDraw4 *pDD4 = NULL;
    HRESULT hr;
    UINT i;

    Log("----- Testing device: %s -----", pDev->Description);

    Log("DirectDrawCreate...");
    hr = DirectDrawCreate(pDev->HasGuid ? (GUID *)&pDev->Guid : NULL, &pDD, NULL);
    if (FAILED(hr))
    {
        Log("Enumeration DirectDrawCreate failed (hr=0x%08lX)", hr);
        return;
    }

    Log("QueryInterface(IID_IDirectDraw4)...");
    hr = IDirectDraw_QueryInterface(pDD, &IID_IDirectDraw4, (void **)&pDD4);
    if (FAILED(hr))
    {
        Log("Enumeration failed to get DirectDraw4 interface (hr=0x%08lX)", hr);
        IDirectDraw_Release(pDD);
        return;
    }

    Log("SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)...");
    hr = IDirectDraw4_SetCooperativeLevel(pDD4, g_hWnd,
                                          DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN |
                                          DDSCL_ALLOWREBOOT);
    if (FAILED(hr))
    {
        Log("SetCooperativeLevel (mode enumeration) failed (hr=0x%08lX)", hr);
        goto Cleanup;
    }

    g_ModesLogged = 0;
    Log("IDirectDraw_EnumDisplayModes...");
    hr = IDirectDraw4_EnumDisplayModes(pDD4, 0, NULL, NULL, ModeEnumCallback);
    if (FAILED(hr))
        Log("EnumDisplayModes failed (hr=0x%08lX)", hr);
    else
        Log("EnumDisplayModes reported %d modes", g_ModesLogged);

    for (i = 0; i < ARRAYSIZE(g_TestModes); i++)
    {
        TestMode(pDD4, &g_TestModes[i]);
    }

    Log("RestoreDisplayMode...");
    hr = IDirectDraw4_RestoreDisplayMode(pDD4);
    if (FAILED(hr))
        Log("RestoreDisplayMode failed (hr=0x%08lX)", hr);

    Log("SetCooperativeLevel(NORMAL)...");
    IDirectDraw4_SetCooperativeLevel(pDD4, g_hWnd, DDSCL_NORMAL);

Cleanup:
    Log("Releasing DirectDraw interfaces...");
    IDirectDraw4_Release(pDD4);
    IDirectDraw_Release(pDD);
    Log("----- Device done: %s -----", pDev->Description);
}

/* ---------------------------------------------------------------------- */
/* Phase 2: Direct3DDetect                                                */
/* ---------------------------------------------------------------------- */

static HRESULT CALLBACK D3DDeviceEnumCallback(GUID *pGuid, char *pszDesc, char *pszName,
                                              D3DDEVICEDESC *pHwDesc, D3DDEVICEDESC *pHelDesc,
                                              void *pContext)
{
    Log("  D3D device: %s (%s), HW rasterization: %s",
        pszDesc ? pszDesc : "(null)",
        pszName ? pszName : "(null)",
        (pHwDesc && pHwDesc->dwFlags) ? "yes" : "no");
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK TextureFormatCallback(DDPIXELFORMAT *pFormat, void *pContext)
{
    (*(int *)pContext)++;
    return D3DENUMRET_OK;
}

static void LogVidMem(IDirectDraw4 *pDD4, const char *pszWhat, DWORD Caps)
{
    DDSCAPS2 SurfCaps;
    DWORD Total = 0, Free = 0;
    HRESULT hr;

    ZeroMemory(&SurfCaps, sizeof(SurfCaps));
    SurfCaps.dwCaps = Caps;
    hr = IDirectDraw4_GetAvailableVidMem(pDD4, &SurfCaps, &Total, &Free);
    if (FAILED(hr))
        Log("Failed to get available %s memory (hr=0x%08lX)", pszWhat, hr);
    else
        Log("%s Memory Total= %2.3f MB, Free= %2.3f MB",
            pszWhat, Total / (1024.0 * 1024.0), Free / (1024.0 * 1024.0));
}

/* Mode switch, then create a 3D HAL device on the fresh flip chain -
 * the Direct3D flavor of the same crashy pattern. */
static void TestD3DMode(IDirectDraw4 *pDD4, IDirect3D3 *pD3D3, const TEST_MODE *pMode)
{
    IDirectDrawSurface4 *pPrimary = NULL;
    IDirectDrawSurface4 *pBack = NULL;
    IDirect3DDevice3 *pDevice = NULL;
    D3DDEVICEDESC HwDesc, HelDesc;
    int FormatCount = 0;
    HRESULT hr;

    Log("=== Testing D3D mode %lux%lux%lu ===", pMode->Width, pMode->Height, pMode->Bpp);

    Log("IDirectDraw4_SetDisplayMode(%lu, %lu, %lu)...", pMode->Width, pMode->Height, pMode->Bpp);
    hr = IDirectDraw4_SetDisplayMode(pDD4, pMode->Width, pMode->Height, pMode->Bpp, 0, 0);
    if (FAILED(hr))
    {
        Log("SetDisplayMode (mode enumeration) failed! (hr=0x%08lX)", hr);
        return;
    }
    PumpMessages();

    hr = CreatePrimary(pDD4, &pPrimary, &pBack, TRUE);
    if (FAILED(hr))
    {
        Log("CreateSurface (primary, mode enumeration) failed! (hr=0x%08lX)", hr);
        return;
    }

    Log("Create 3D HAL Device (mode enumeration)...");
    hr = IDirect3D3_CreateDevice(pD3D3, &IID_IDirect3DHALDevice, pBack, &pDevice, NULL);
    if (FAILED(hr))
    {
        Log("Create 3D HAL Device (mode enumeration) failed! (hr=0x%08lX)", hr);
        goto Cleanup;
    }

    ZeroMemory(&HwDesc, sizeof(HwDesc));
    ZeroMemory(&HelDesc, sizeof(HelDesc));
    HwDesc.dwSize = sizeof(HwDesc);
    HelDesc.dwSize = sizeof(HelDesc);
    Log("IDirect3DDevice3_GetCaps...");
    hr = IDirect3DDevice3_GetCaps(pDevice, &HwDesc, &HelDesc);
    if (FAILED(hr))
        Log("Failed to get D3D device caps (hr=0x%08lX)", hr);

    Log("IDirect3DDevice3_EnumTextureFormats...");
    hr = IDirect3DDevice3_EnumTextureFormats(pDevice, TextureFormatCallback, &FormatCount);
    if (FAILED(hr))
        Log("EnumTextureFormats() failed (hr=0x%08lX)", hr);
    else
        Log("EnumTextureFormats reported %d formats", FormatCount);

Cleanup:
    Log("Releasing D3D device and surfaces for mode %lux%lux%lu...",
        pMode->Width, pMode->Height, pMode->Bpp);
    if (pDevice)
        IDirect3DDevice3_Release(pDevice);
    if (pBack)
        IDirectDrawSurface4_Release(pBack);
    if (pPrimary)
        IDirectDrawSurface4_Release(pPrimary);
    Log("D3D mode %lux%lux%lu done.", pMode->Width, pMode->Height, pMode->Bpp);
}

static void TestDeviceD3D(const DDRAW_DEVICE *pDev)
{
    IDirectDraw *pDD = NULL;
    IDirectDraw4 *pDD4 = NULL;
    IDirect3D3 *pD3D3 = NULL;
    DDCAPS HalCaps, HelCaps;
    HRESULT hr;
    UINT i;

    Log("----- Direct3D Enumeration: %s -----", pDev->Description);

    Log("DirectDrawCreate...");
    hr = DirectDrawCreate(pDev->HasGuid ? (GUID *)&pDev->Guid : NULL, &pDD, NULL);
    if (FAILED(hr))
    {
        Log("Enumeration DirectDrawCreate failed (hr=0x%08lX)", hr);
        return;
    }

    Log("QueryInterface(IID_IDirectDraw4)...");
    hr = IDirectDraw_QueryInterface(pDD, &IID_IDirectDraw4, (void **)&pDD4);
    if (FAILED(hr))
    {
        Log("Enumeration failed to get DirectDraw4 interface (hr=0x%08lX)", hr);
        IDirectDraw_Release(pDD);
        return;
    }

    Log("QueryInterface(IID_IDirect3D3)...");
    hr = IDirectDraw4_QueryInterface(pDD4, &IID_IDirect3D3, (void **)&pD3D3);
    if (FAILED(hr))
    {
        Log("Failed to get D3D interface (hr=0x%08lX)", hr);
        goto Cleanup;
    }

    ZeroMemory(&HalCaps, sizeof(HalCaps));
    ZeroMemory(&HelCaps, sizeof(HelCaps));
    HalCaps.dwSize = sizeof(HalCaps);
    HelCaps.dwSize = sizeof(HelCaps);
    Log("IDirectDraw4_GetCaps...");
    hr = IDirectDraw4_GetCaps(pDD4, &HalCaps, &HelCaps);
    if (FAILED(hr))
        Log("Failed to get DD4 Caps (hr=0x%08lX)", hr);

    LogVidMem(pDD4, "Video", DDSCAPS_VIDEOMEMORY);
    LogVidMem(pDD4, "Texture", DDSCAPS_TEXTURE);
    LogVidMem(pDD4, "Local texture", DDSCAPS_TEXTURE | DDSCAPS_LOCALVIDMEM);
    LogVidMem(pDD4, "AGP texture", DDSCAPS_TEXTURE | DDSCAPS_NONLOCALVIDMEM);

    Log("Detecting boards which support Direct3D...");
    hr = IDirect3D3_EnumDevices(pD3D3, D3DDeviceEnumCallback, NULL);
    if (FAILED(hr))
        Log("IDirect3D3_EnumDevices failed (hr=0x%08lX)", hr);

    Log("SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)...");
    hr = IDirectDraw4_SetCooperativeLevel(pDD4, g_hWnd,
                                          DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN |
                                          DDSCL_ALLOWREBOOT);
    if (FAILED(hr))
    {
        Log("SetCooperativeLevel (mode enumeration) failed (hr=0x%08lX)", hr);
        goto Cleanup;
    }

    for (i = 0; i < ARRAYSIZE(g_D3DTestModes); i++)
    {
        TestD3DMode(pDD4, pD3D3, &g_D3DTestModes[i]);
    }

    Log("RestoreDisplayMode...");
    hr = IDirectDraw4_RestoreDisplayMode(pDD4);
    if (FAILED(hr))
        Log("RestoreDisplayMode failed (hr=0x%08lX)", hr);

    Log("SetCooperativeLevel(NORMAL)...");
    IDirectDraw4_SetCooperativeLevel(pDD4, g_hWnd, DDSCL_NORMAL);

Cleanup:
    Log("Releasing Direct3D/DirectDraw interfaces...");
    if (pD3D3)
        IDirect3D3_Release(pD3D3);
    IDirectDraw4_Release(pDD4);
    IDirectDraw_Release(pDD);
    Log("----- Direct3D done: %s -----", pDev->Description);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR pszCmdLine, int nCmdShow)
{
    char LogPath[MAX_PATH];
    OSVERSIONINFOA OsVer;
    WNDCLASSA WndClass;
    BOOL RunDDraw, RunD3D;
    HRESULT hr;
    char *p;
    int i;

    SetUnhandledExceptionFilter(CrashFilter);

    /* Mirror the original's TestDDraw / TestD3D switches; no args = both.
     * (The original also has TestGLIDE, which needs 3dfx glide3x.dll and
     * is skipped when the DLL is missing - not replayed here.) */
    RunDDraw = (pszCmdLine == NULL || *pszCmdLine == '\0' || strstr(pszCmdLine, "TestDDraw") != NULL);
    RunD3D   = (pszCmdLine == NULL || *pszCmdLine == '\0' || strstr(pszCmdLine, "TestD3D") != NULL);

    /* Log file next to the EXE (survives the fullscreen crash) */
    GetModuleFileNameA(NULL, LogPath, sizeof(LogPath));
    p = strrchr(LogPath, '\\');
    if (p)
        lstrcpyA(p + 1, "dxfailtest.log");
    else
        lstrcpyA(LogPath, "dxfailtest.log");
    g_LogFile = fopen(LogPath, "w");

    Log("DirectXFailTest: D2VidTst.exe DirectDraw mode-switch repro");

    ZeroMemory(&OsVer, sizeof(OsVer));
    OsVer.dwOSVersionInfoSize = sizeof(OsVer);
    if (GetVersionExA(&OsVer))
    {
        Log("OS version %lu.%lu build %lu (%s)",
            OsVer.dwMajorVersion, OsVer.dwMinorVersion,
            OsVer.dwBuildNumber, OsVer.szCSDVersion);
    }

    Log("Registering window class...");
    ZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.lpfnWndProc = EnumWndProc;
    WndClass.hInstance = hInstance;
    WndClass.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    WndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    WndClass.lpszClassName = ENUM_CLASS_NAME;
    if (!RegisterClassA(&WndClass))
    {
        Log("RegisterClass failed = %lu", GetLastError());
        return 1;
    }

    Log("Creating window for DirectDraw enumeration...");
    g_hWnd = CreateWindowExA(0, ENUM_CLASS_NAME, WINDOW_TITLE, WS_POPUP,
                             0, 0,
                             GetSystemMetrics(SM_CXSCREEN),
                             GetSystemMetrics(SM_CYSCREEN),
                             NULL, NULL, hInstance, NULL);
    if (!g_hWnd)
    {
        Log("***** Failed to create window for DirectDraw enumeration= %lu", GetLastError());
        return 1;
    }
    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hWnd);
    SetForegroundWindow(g_hWnd);
    PumpMessages();

    Log("Detecting boards which support DirectDraw...");
    hr = DirectDrawEnumerateA(DeviceEnumCallback, NULL);
    if (FAILED(hr))
        Log("DirectDrawEnumerate failed (hr=0x%08lX)", hr);

    if (g_DeviceCount == 0)
    {
        /* No callback fired: still try the primary device like DirectDrawCreate(NULL) would */
        Log("No devices enumerated, falling back to primary device");
        g_Devices[0].HasGuid = FALSE;
        lstrcpyA(g_Devices[0].Description, "Primary Display Driver");
        g_DeviceCount = 1;
    }

    if (RunDDraw)
    {
        Log("DDrawDetect...");
        for (i = 0; i < g_DeviceCount; i++)
        {
            TestDevice(&g_Devices[i]);
        }
    }

    if (RunD3D)
    {
        Log("Direct3DDetect...");
        for (i = 0; i < g_DeviceCount; i++)
        {
            TestDeviceD3D(&g_Devices[i]);
        }
    }

    Log("Destroyed DDraw window.");
    DestroyWindow(g_hWnd);
    UnregisterClassA(ENUM_CLASS_NAME, hInstance);

    /* Make sure the desktop mode is restored even if a test path leaked */
    ChangeDisplaySettingsA(NULL, 0);

    Log("Video Test application completed without error.");
    if (g_LogFile)
        fclose(g_LogFile);

    MessageBoxA(NULL, "Video Test application completed without error.",
                WINDOW_TITLE, MB_OK | MB_ICONINFORMATION);
    return 0;
}
