/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: cooperative level transitions
 */


#include "d3dtest.h"
#include <ddraw.h>

static D3DTEST_UNUSED IDirectDrawSurface7 *make_rgb_surface(IDirectDraw7 *ddraw,
                                                            DWORD w, DWORD h, DWORD caps)
{
    IDirectDrawSurface7 *surface = NULL;
    DDSURFACEDESC2 desc;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = caps;
    desc.dwWidth = w;
    desc.dwHeight = h;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;

    if (FAILED(IDirectDraw7_CreateSurface(ddraw, &desc, &surface, NULL)))
        return NULL;
    return surface;
}

int main(void)
{
    IDirectDrawSurface7 *primary = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;
    HWND hwnd;

    test_begin("ddraw_cooplevel");

    hwnd = test_create_window("ddraw_cooplevel", 320, 240);
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
    ok_(SUCCEEDED(hr), "SetCooperativeLevel(NORMAL) returned 0x%08lx", hr);

    /* NORMAL may be set repeatedly. */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
    ok_(SUCCEEDED(hr), "setting NORMAL twice returned 0x%08lx", hr);

    /* EXCLUSIVE without FULLSCREEN is not a legal combination. */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_EXCLUSIVE);
    ok_(FAILED(hr), "EXCLUSIVE without FULLSCREEN returned 0x%08lx, expected failure", hr);

    /* A windowed primary must be creatable at NORMAL level. */
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &primary, NULL);
    ok_(SUCCEEDED(hr), "creating a windowed primary at NORMAL returned 0x%08lx", hr);

    /* A back buffer count on a windowed primary is invalid. */
    if (primary)
    {
        IDirectDrawSurface7 *bad = NULL;

        D3DTEST_RELEASE(primary);
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        desc.dwBackBufferCount = 1;
        hr = IDirectDraw7_CreateSurface(ddraw, &desc, &bad, NULL);
        ok_(FAILED(hr),
            "a flipping primary at NORMAL level returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(bad);
    }

    /* Documented as requiring exclusive mode, but Windows returns S_OK here.
       Restore unconditionally afterwards so this can never leave the desktop
       in a different mode. */
    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    info_("SetDisplayMode at NORMAL level returned 0x%08lx (Windows: S_OK)", hr);
    IDirectDraw7_RestoreDisplayMode(ddraw);

    D3DTEST_RELEASE(primary);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
