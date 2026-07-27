/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: surface Lock/Unlock and direct pixel access
 */


#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDrawSurface7 *surface = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;
    HWND hwnd;
    DWORD *row;
    int readback_ok = 1;
    int x;

    test_begin("ddraw_lock");

    hwnd = test_create_window("ddraw_lock", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    /* Pin the layout to 32bpp X8R8G8B8 so the pixel arithmetic below is exact. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = 32;
    desc.dwHeight = 32;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;

    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &surface, NULL);
    ok_(SUCCEEDED(hr) && surface != NULL, "created 32bpp X8R8G8B8 surface (0x%08lx)", hr);
    if (!surface)
        goto cleanup;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL);
    ok_(SUCCEEDED(hr), "Lock returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        ok_(desc.lpSurface != NULL, "Lock produced a surface pointer");
        ok_(desc.lPitch >= 32 * 4, "pitch %ld covers one 32bpp row", desc.lPitch);

        row = (DWORD *)desc.lpSurface;
        for (x = 0; x < 32; x++)
            row[x] = 0x00c0ffee + x;

        hr = IDirectDrawSurface7_Unlock(surface, NULL);
        ok_(SUCCEEDED(hr), "Unlock returned 0x%08lx", hr);

        /* Lock again: the writes must have landed in the surface itself, not
           in a scratch buffer that Unlock threw away. */
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        hr = IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL);
        ok_(SUCCEEDED(hr), "second Lock returned 0x%08lx", hr);
        if (SUCCEEDED(hr))
        {
            row = (DWORD *)desc.lpSurface;
            for (x = 0; x < 32; x++)
            {
                if (row[x] != (DWORD)(0x00c0ffee + x))
                {
                    readback_ok = 0;
                    break;
                }
            }
            ok_(readback_ok, "pixels written through the lock read back unchanged");
            IDirectDrawSurface7_Unlock(surface, NULL);
        }
    }

cleanup:
    D3DTEST_RELEASE(surface);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
