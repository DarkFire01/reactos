/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: texture surface creation and stage binding
 */


#include "d3dtest.h"
#include <ddraw.h>
#include <d3d.h>

static int format_count;
static DDPIXELFORMAT chosen;
static int have_format;

static HRESULT WINAPI format_cb(DDPIXELFORMAT *fmt, void *ctx)
{
    format_count++;
    /* Take the first plain RGB format with alpha we are offered. */
    if (!have_format && (fmt->dwFlags & DDPF_RGB) && fmt->dwRGBBitCount >= 16)
    {
        chosen = *fmt;
        have_format = 1;
    }
    return DDENUMRET_OK;
}

int main(void)
{
    IDirectDrawSurface7 *target = NULL, *texture = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_texture");

    hwnd = test_create_window("d3d7_texture", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    if (FAILED(hr))
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr))
    {
        skip_("no IDirect3D7 available (0x%08lx)", hr);
        goto cleanup;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    desc.dwWidth = 256;
    desc.dwHeight = 256;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &target, NULL);
    if (FAILED(hr))
    {
        skip_("cannot create a 3DDEVICE surface (0x%08lx)", hr);
        goto cleanup;
    }

    hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, target, &device);
    if (FAILED(hr))
        hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DRGBDevice, target, &device);
    if (FAILED(hr) || !device)
    {
        skip_("CreateDevice failed (0x%08lx)", hr);
        goto cleanup;
    }

    hr = IDirect3DDevice7_EnumTextureFormats(device, format_cb, NULL);
    ok_(SUCCEEDED(hr), "EnumTextureFormats returned 0x%08lx", hr);
    ok_(format_count > 0, "enumerated %d texture format(s)", format_count);

    if (!have_format)
    {
        skip_("no usable RGB texture format enumerated");
        goto cleanup;
    }
    info_("using a %lubpp RGB texture format", chosen.dwRGBBitCount);

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.dwWidth = 64;
    desc.dwHeight = 64;
    desc.ddpfPixelFormat = chosen;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &texture, NULL);
    ok_(SUCCEEDED(hr) && texture != NULL, "created a 64x64 texture (0x%08lx)", hr);

    if (texture)
    {
        IDirectDrawSurface7 *got = NULL;

        hr = IDirect3DDevice7_SetTexture(device, 0, texture);
        ok_(SUCCEEDED(hr), "SetTexture(stage 0) returned 0x%08lx", hr);

        hr = IDirect3DDevice7_GetTexture(device, 0, &got);
        ok_(SUCCEEDED(hr), "GetTexture(stage 0) returned 0x%08lx", hr);
        ok_(got == texture, "stage 0 holds %p, expected %p", got, texture);
        D3DTEST_RELEASE(got);

        hr = IDirect3DDevice7_SetTexture(device, 0, NULL);
        ok_(SUCCEEDED(hr), "clearing stage 0 returned 0x%08lx", hr);
    }

cleanup:
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
