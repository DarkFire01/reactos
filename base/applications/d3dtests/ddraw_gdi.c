/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: GDI interop through GetDC/ReleaseDC on a surface
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
    IDirectDrawSurface7 *surface = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;
    HWND hwnd;
    HDC dc = NULL;
    DWORD pixel;
    COLORREF got;

    test_begin("ddraw_gdi");

    hwnd = test_create_window("ddraw_gdi", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    surface = make_rgb_surface(ddraw, 64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    ok_(surface != NULL, "created a 64x64 offscreen surface");
    if (!surface)
        goto cleanup;

    hr = IDirectDrawSurface7_GetDC(surface, &dc);
    ok_(SUCCEEDED(hr) && dc != NULL, "GetDC on the surface returned 0x%08lx", hr);
    if (FAILED(hr))
        goto cleanup;

    /* Paint through GDI, then read the result back through a ddraw lock: this
       is the path ReactOS historically gets wrong, so verify the pixels. */
    {
        HBRUSH brush = CreateSolidBrush(RGB(0x12, 0x34, 0x56));
        RECT r = { 0, 0, 64, 64 };

        FillRect(dc, &r, brush);
        DeleteObject(brush);

        SetPixel(dc, 5, 5, RGB(0xff, 0x00, 0x00));
        got = GetPixel(dc, 5, 5);
        ok_(got == RGB(0xff, 0x00, 0x00),
            "GetPixel through the surface DC returned 0x%06lx", (unsigned long)got);
    }

    hr = IDirectDrawSurface7_ReleaseDC(surface, dc);
    ok_(SUCCEEDED(hr), "ReleaseDC returned 0x%08lx", hr);
    dc = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL);
    ok_(SUCCEEDED(hr), "Lock after the GDI paint returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        pixel = *(DWORD *)((BYTE *)desc.lpSurface + 20 * desc.lPitch + 20 * 4) & 0x00ffffff;
        ok_(pixel == 0x00123456,
            "GDI fill shows as 0x%06lx through a ddraw lock, expected 0x00123456", pixel);

        pixel = *(DWORD *)((BYTE *)desc.lpSurface + 5 * desc.lPitch + 5 * 4) & 0x00ffffff;
        ok_(pixel == 0x00ff0000,
            "GDI SetPixel shows as 0x%06lx, expected 0x00ff0000", pixel);

        IDirectDrawSurface7_Unlock(surface, NULL);
    }

    /* A second GetDC while one is outstanding must be refused. */
    hr = IDirectDrawSurface7_GetDC(surface, &dc);
    if (SUCCEEDED(hr))
    {
        HDC second = NULL;
        HRESULT hr2 = IDirectDrawSurface7_GetDC(surface, &second);
        ok_(FAILED(hr2), "a second GetDC returned 0x%08lx, expected failure", hr2);
        if (SUCCEEDED(hr2))
            IDirectDrawSurface7_ReleaseDC(surface, second);
        IDirectDrawSurface7_ReleaseDC(surface, dc);
        dc = NULL;
    }

cleanup:
    if (dc) IDirectDrawSurface7_ReleaseDC(surface, dc);
    D3DTEST_RELEASE(surface);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
