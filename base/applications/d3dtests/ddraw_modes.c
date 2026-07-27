/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: display mode enumeration and GetDisplayMode
 */

#include "d3dtest.h"
#include <ddraw.h>

static int mode_count;
static int found_32bpp;

static HRESULT WINAPI mode_cb(DDSURFACEDESC2 *desc, void *ctx)
{
    mode_count++;
    if (desc->ddpfPixelFormat.dwRGBBitCount == 32)
        found_32bpp = 1;
    if (mode_count <= 8)
        info_("mode %2d: %lux%lu %lubpp", mode_count,
              desc->dwWidth, desc->dwHeight, desc->ddpfPixelFormat.dwRGBBitCount);
    return DDENUMRET_OK;
}

int main(void)
{
    IDirectDraw7 *ddraw = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;

    test_begin("ddraw_modes");

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        return test_end();

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok_(SUCCEEDED(hr), "SetCooperativeLevel(NORMAL) returned 0x%08lx", hr);

    hr = IDirectDraw7_EnumDisplayModes(ddraw, 0, NULL, NULL, mode_cb);
    ok_(SUCCEEDED(hr), "EnumDisplayModes returned 0x%08lx", hr);
    ok_(mode_count > 0, "enumerated %d display mode(s)", mode_count);
    if (!found_32bpp)
        skip_("no 32bpp mode enumerated");

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirectDraw7_GetDisplayMode(ddraw, &desc);
    ok_(SUCCEEDED(hr), "GetDisplayMode returned 0x%08lx", hr);
    ok_(desc.dwWidth > 0 && desc.dwHeight > 0,
        "current mode is %lux%lu %lubpp", desc.dwWidth, desc.dwHeight,
        desc.ddpfPixelFormat.dwRGBBitCount);

    D3DTEST_RELEASE(ddraw);
    return test_end();
}
