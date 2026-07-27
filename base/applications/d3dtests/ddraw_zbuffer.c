/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: z-buffer format enumeration and attachment
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

#include <d3d.h>

static int zformat_count;
static DDPIXELFORMAT zformat;
static int have_zformat;

static HRESULT WINAPI z_cb(DDPIXELFORMAT *fmt, void *ctx)
{
    zformat_count++;
    if (!have_zformat && (fmt->dwFlags & DDPF_ZBUFFER))
    {
        zformat = *fmt;
        have_zformat = 1;
    }
    info_("  z format %d: %lu bit depth", zformat_count, fmt->dwZBufferBitDepth);
    return DDENUMRET_OK;
}

int main(void)
{
    IDirectDrawSurface7 *target = NULL, *zbuffer = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;
    HWND hwnd;

    test_begin("ddraw_zbuffer");

    hwnd = test_create_window("ddraw_zbuffer", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr))
    {
        skip_("no IDirect3D7 available (0x%08lx)", hr);
        goto cleanup;
    }

    hr = IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, z_cb, NULL);
    if (FAILED(hr))
        hr = IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DRGBDevice, z_cb, NULL);
    ok_(SUCCEEDED(hr), "EnumZBufferFormats returned 0x%08lx", hr);

    if (!have_zformat)
    {
        skip_("no z-buffer format enumerated");
        goto cleanup;
    }
    ok_(zformat_count > 0, "enumerated %d z-buffer format(s)", zformat_count);

    target = make_rgb_surface(ddraw, 128, 128, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
    if (!target)
    {
        skip_("cannot create a 3D render target");
        goto cleanup;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    desc.dwWidth = 128;
    desc.dwHeight = 128;
    desc.ddpfPixelFormat = zformat;

    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &zbuffer, NULL);
    if (FAILED(hr))
    {
        skip_("CreateSurface(ZBUFFER) returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a %lu-bit z-buffer", zformat.dwZBufferBitDepth);

    hr = IDirectDrawSurface7_AddAttachedSurface(target, zbuffer);
    ok_(SUCCEEDED(hr), "attaching the z-buffer to the target returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        DDSCAPS2 caps;
        IDirectDrawSurface7 *got = NULL;

        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_ZBUFFER;
        hr = IDirectDrawSurface7_GetAttachedSurface(target, &caps, &got);
        ok_(SUCCEEDED(hr) && got == zbuffer,
            "GetAttachedSurface(ZBUFFER) returned 0x%08lx and %p", hr, got);
        D3DTEST_RELEASE(got);

        IDirectDrawSurface7_DeleteAttachedSurface(target, 0, zbuffer);
    }

cleanup:
    D3DTEST_RELEASE(zbuffer);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
