/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: stretched and mirrored blits
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

static DWORD read_px(IDirectDrawSurface7 *s, int x, int y)
{
    DDSURFACEDESC2 desc;
    DWORD v = 0;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(s, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        v = *(DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch + x * 4) & 0x00ffffff;
        IDirectDrawSurface7_Unlock(s, NULL);
    }
    return v;
}

static void fill_lr(IDirectDrawSurface7 *s, int w, int h, DWORD left, DWORD right)
{
    DDSURFACEDESC2 desc;
    DWORD *row;
    int x, y;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(s, NULL, &desc, DDLOCK_WAIT, NULL)))
        return;
    for (y = 0; y < h; y++)
    {
        row = (DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);
        for (x = 0; x < w; x++)
            row[x] = (x < w / 2) ? left : right;
    }
    IDirectDrawSurface7_Unlock(s, NULL);
}

int main(void)
{
    IDirectDrawSurface7 *src = NULL, *dst = NULL;
    IDirectDraw7 *ddraw = NULL;
    DWORD caps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    RECT srect, drect;
    DDBLTFX fx;
    HRESULT hr;
    HWND hwnd;
    DWORD px;

    test_begin("ddraw_stretch");

    hwnd = test_create_window("ddraw_stretch", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    src = make_rgb_surface(ddraw, 32, 32, caps);
    dst = make_rgb_surface(ddraw, 64, 64, caps);
    ok_(src != NULL && dst != NULL, "created a 32x32 source and 64x64 destination");
    if (!src || !dst)
        goto cleanup;

    /* Left half red, right half green. */
    fill_lr(src, 32, 32, 0x00ff0000, 0x0000ff00);

    /* Stretch the whole 32x32 source over the whole 64x64 destination. */
    srect.left = 0; srect.top = 0; srect.right = 32; srect.bottom = 32;
    drect.left = 0; drect.top = 0; drect.right = 64; drect.bottom = 64;
    hr = IDirectDrawSurface7_Blt(dst, &drect, src, &srect, DDBLT_WAIT, NULL);
    ok_(SUCCEEDED(hr), "2x stretch Blt returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        px = read_px(dst, 8, 8);
        ok_(px == 0x00ff0000, "stretched left half is 0x%06lx, expected red", px);
        px = read_px(dst, 56, 8);
        ok_(px == 0x0000ff00, "stretched right half is 0x%06lx, expected green", px);
    }

    /* Mirror left-to-right: the halves should swap. */
    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    hr = IDirectDrawSurface7_Blt(dst, &drect, src, &srect, DDBLT_WAIT | DDBLT_DDFX, &fx);
    if (FAILED(hr))
    {
        skip_("no mirroring blit support (0x%08lx)", hr);
    }
    else
    {
        ok_(SUCCEEDED(hr), "mirrored Blt returned 0x%08lx", hr);
        px = read_px(dst, 8, 8);
        ok_(px == 0x0000ff00, "mirrored left half is 0x%06lx, expected green", px);
        px = read_px(dst, 56, 8);
        ok_(px == 0x00ff0000, "mirrored right half is 0x%06lx, expected red", px);
    }

cleanup:
    D3DTEST_RELEASE(src);
    D3DTEST_RELEASE(dst);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
