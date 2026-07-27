/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: primary and offscreen surface creation
 */


#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDrawSurface7 *primary = NULL, *offscreen = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    HWND hwnd;
    HRESULT hr;

    test_begin("ddraw_surface");

    hwnd = test_create_window("ddraw_surface", 320, 240);
    ok_(hwnd != NULL, "created test window");

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
    ok_(SUCCEEDED(hr), "SetCooperativeLevel(NORMAL) returned 0x%08lx", hr);

    /* Primary surface, windowed. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &primary, NULL);
    ok_(SUCCEEDED(hr) && primary != NULL, "created primary surface (0x%08lx)", hr);

    if (primary)
    {
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &desc);
        ok_(SUCCEEDED(hr), "GetSurfaceDesc on primary returned 0x%08lx", hr);
        info_("primary is %lux%lu %lubpp, pitch %ld", desc.dwWidth, desc.dwHeight,
              desc.ddpfPixelFormat.dwRGBBitCount, desc.lPitch);
        ok_(desc.dwWidth > 0 && desc.dwHeight > 0, "primary has non-zero dimensions");
    }

    /* A plain system-memory offscreen surface must work with no 3D at all. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = 64;
    desc.dwHeight = 64;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &offscreen, NULL);
    ok_(SUCCEEDED(hr) && offscreen != NULL, "created 64x64 offscreen surface (0x%08lx)", hr);

    if (offscreen)
    {
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        hr = IDirectDrawSurface7_GetSurfaceDesc(offscreen, &desc);
        ok_(SUCCEEDED(hr), "GetSurfaceDesc on offscreen returned 0x%08lx", hr);
        ok_(desc.dwWidth == 64 && desc.dwHeight == 64,
            "offscreen reports %lux%lu, expected 64x64", desc.dwWidth, desc.dwHeight);
    }

    D3DTEST_RELEASE(offscreen);
    D3DTEST_RELEASE(primary);
    D3DTEST_RELEASE(ddraw);

done:
    test_destroy_window(hwnd);
    return test_end();
}
