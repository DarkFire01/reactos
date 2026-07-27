/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: source colour keying during a blit
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

static void fill_half(IDirectDrawSurface7 *surface, DWORD left, DWORD right)
{
    DDSURFACEDESC2 desc;
    DWORD *row;
    int x, y;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL)))
        return;

    for (y = 0; y < 32; y++)
    {
        row = (DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);
        for (x = 0; x < 32; x++)
            row[x] = (x < 16) ? left : right;
    }
    IDirectDrawSurface7_Unlock(surface, NULL);
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
    DDCOLORKEY key;
    DDBLTFX fx;
    HRESULT hr;
    HWND hwnd;
    DWORD pixel;

    test_begin("ddraw_colorkey");

    hwnd = test_create_window("ddraw_colorkey", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    src = make_surface(ddraw, 32, 32);
    dst = make_surface(ddraw, 32, 32);
    ok_(src != NULL && dst != NULL, "created source and destination surfaces");
    if (!src || !dst)
        goto cleanup;

    /* Left half magenta (the key), right half green. */
    fill_half(src, 0x00ff00ff, 0x0000ff00);

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x00000080;
    IDirectDrawSurface7_Blt(dst, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);

    key.dwColorSpaceLowValue = 0x00ff00ff;
    key.dwColorSpaceHighValue = 0x00ff00ff;
    hr = IDirectDrawSurface7_SetColorKey(src, DDCKEY_SRCBLT, &key);
    ok_(SUCCEEDED(hr), "SetColorKey(SRCBLT) returned 0x%08lx", hr);
    if (FAILED(hr))
    {
        skip_("no source colour key support, skipping the keyed blit");
        goto cleanup;
    }

    hr = IDirectDrawSurface7_Blt(dst, NULL, src, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
    ok_(SUCCEEDED(hr), "keyed Blt returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        pixel = read_pixel(dst, 4, 4);
        ok_(pixel == 0x00000080,
            "keyed pixel is 0x%08lx, expected the destination to show through", pixel);
        pixel = read_pixel(dst, 24, 4);
        ok_(pixel == 0x0000ff00, "unkeyed pixel is 0x%08lx, expected the source green", pixel);
    }

cleanup:
    D3DTEST_RELEASE(src);
    D3DTEST_RELEASE(dst);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
