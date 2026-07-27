/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: attached surfaces and mipmap chains
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

static int enum_count;

static HRESULT WINAPI attach_cb(IDirectDrawSurface7 *surface, DDSURFACEDESC2 *desc, void *ctx)
{
    enum_count++;
    info_("  attached level %d: %lux%lu", enum_count, desc->dwWidth, desc->dwHeight);
    IDirectDrawSurface7_Release(surface);
    return DDENUMRET_OK;
}

int main(void)
{
    IDirectDrawSurface7 *mipmap = NULL, *level = NULL;
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    DDSCAPS2 caps;
    HRESULT hr;
    HWND hwnd;

    test_begin("ddraw_attach");

    hwnd = test_create_window("ddraw_attach", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    /* A complex mipmap chain: 32x32 down to 1x1 is six levels. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
                        | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = 32;
    desc.dwHeight = 32;
    desc.dwMipMapCount = 6;

    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &mipmap, NULL);
    if (FAILED(hr))
    {
        skip_("no mipmap chain support (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a 32x32 six-level mipmap chain");

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirectDrawSurface7_GetSurfaceDesc(mipmap, &desc);
    ok_(SUCCEEDED(hr), "GetSurfaceDesc returned 0x%08lx", hr);
    ok_(desc.dwMipMapCount == 6, "chain reports %lu level(s), expected 6", desc.dwMipMapCount);

    /* Walk down one level and confirm it halved. */
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
    hr = IDirectDrawSurface7_GetAttachedSurface(mipmap, &caps, &level);
    ok_(SUCCEEDED(hr) && level != NULL, "GetAttachedSurface(next level) returned 0x%08lx", hr);

    if (level)
    {
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        IDirectDrawSurface7_GetSurfaceDesc(level, &desc);
        ok_(desc.dwWidth == 16 && desc.dwHeight == 16,
            "level 1 is %lux%lu, expected 16x16", desc.dwWidth, desc.dwHeight);
        D3DTEST_RELEASE(level);
    }

    hr = IDirectDrawSurface7_EnumAttachedSurfaces(mipmap, NULL, attach_cb);
    ok_(SUCCEEDED(hr), "EnumAttachedSurfaces returned 0x%08lx", hr);
    ok_(enum_count >= 1, "enumerated %d attached surface(s)", enum_count);

cleanup:
    D3DTEST_RELEASE(mipmap);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
