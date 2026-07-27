/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: exclusive fullscreen mode and a flipping chain
 */


#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDrawSurface7 *primary = NULL, *back = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    DDSCAPS2 caps;
    HRESULT hr;
    HWND hwnd;
    int i;

    test_begin("ddraw_flip");

    hwnd = test_create_window("ddraw_flip", 640, 480);
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, hwnd,
            DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    ok_(SUCCEEDED(hr), "SetCooperativeLevel(EXCLUSIVE|FULLSCREEN) returned 0x%08lx", hr);
    if (FAILED(hr))
    {
        skip_("cannot take exclusive mode, skipping the flip chain");
        goto cleanup;
    }

    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    if (FAILED(hr))
    {
        skip_("SetDisplayMode(640x480x32) returned 0x%08lx", hr);
        goto restore;
    }
    ok_(SUCCEEDED(hr), "SetDisplayMode(640x480x32) returned 0x%08lx", hr);

    /* Primary with one back buffer, i.e. a two-deep flip chain. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    desc.dwBackBufferCount = 1;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &primary, NULL);
    ok_(SUCCEEDED(hr) && primary != NULL, "created flipping primary (0x%08lx)", hr);
    if (!primary)
        goto restore;

    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = IDirectDrawSurface7_GetAttachedSurface(primary, &caps, &back);
    ok_(SUCCEEDED(hr) && back != NULL, "GetAttachedSurface(BACKBUFFER) returned 0x%08lx", hr);

    if (back)
    {
        DDBLTFX fx;

        memset(&fx, 0, sizeof(fx));
        fx.dwSize = sizeof(fx);

        for (i = 0; i < 3; i++)
        {
            fx.dwFillColor = (i == 0) ? 0x00ff0000 : (i == 1) ? 0x0000ff00 : 0x000000ff;
            hr = IDirectDrawSurface7_Blt(back, NULL, NULL, NULL,
                                         DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            ok_(SUCCEEDED(hr), "frame %d: colour fill of back buffer (0x%08lx)", i, hr);

            hr = IDirectDrawSurface7_Flip(primary, NULL, DDFLIP_WAIT);
            ok_(SUCCEEDED(hr), "frame %d: Flip returned 0x%08lx", i, hr);
            test_pump();
        }
    }

restore:
    IDirectDraw7_RestoreDisplayMode(ddraw);
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
cleanup:
    D3DTEST_RELEASE(back);
    D3DTEST_RELEASE(primary);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
