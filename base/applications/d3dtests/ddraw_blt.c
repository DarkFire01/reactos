/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: colour fill and surface-to-surface blitting
 */


#include "d3dtest.h"
#include <ddraw.h>

static D3DTEST_UNUSED IDirectDrawSurface7 *make_surface(IDirectDraw7 *ddraw, DWORD w, DWORD h)
{
    IDirectDrawSurface7 *surface = NULL;
    DDSURFACEDESC2 desc;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
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

static DWORD D3DTEST_UNUSED read_pixel(IDirectDrawSurface7 *surface, int x, int y)
{
    DDSURFACEDESC2 desc;
    DWORD value = 0;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        value = *(DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch + x * 4);
        IDirectDrawSurface7_Unlock(surface, NULL);
    }
    return value;
}

int main(void)
{
    IDirectDrawSurface7 *src = NULL, *dst = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDBLTFX fx;
    HRESULT hr;
    HWND hwnd;
    RECT rect;
    DWORD pixel;

    test_begin("ddraw_blt");

    hwnd = test_create_window("ddraw_blt", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    src = make_surface(ddraw, 64, 64);
    dst = make_surface(ddraw, 64, 64);
    ok_(src != NULL && dst != NULL, "created source and destination surfaces");
    if (!src || !dst)
        goto cleanup;

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x00ff0000;
    hr = IDirectDrawSurface7_Blt(src, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok_(SUCCEEDED(hr), "colour fill of source returned 0x%08lx", hr);
    pixel = read_pixel(src, 10, 10);
    ok_(pixel == 0x00ff0000, "source pixel is 0x%08lx after fill, expected 0x00ff0000", pixel);

    fx.dwFillColor = 0x000000ff;
    hr = IDirectDrawSurface7_Blt(dst, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok_(SUCCEEDED(hr), "colour fill of destination returned 0x%08lx", hr);

    /* Copy the left half of the red source over the blue destination. */
    rect.left = 0; rect.top = 0; rect.right = 32; rect.bottom = 64;
    hr = IDirectDrawSurface7_Blt(dst, &rect, src, &rect, DDBLT_WAIT, NULL);
    ok_(SUCCEEDED(hr), "surface-to-surface Blt returned 0x%08lx", hr);

    pixel = read_pixel(dst, 10, 10);
    ok_(pixel == 0x00ff0000, "blitted region is 0x%08lx, expected the source red", pixel);
    pixel = read_pixel(dst, 50, 10);
    ok_(pixel == 0x000000ff, "untouched region is 0x%08lx, expected the fill blue", pixel);

    hr = IDirectDrawSurface7_BltFast(dst, 32, 0, src, &rect, DDBLTFAST_WAIT);
    ok_(SUCCEEDED(hr), "BltFast returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(src);
    D3DTEST_RELEASE(dst);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
