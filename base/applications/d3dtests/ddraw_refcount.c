/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: COM reference counting and interface identity
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
    IDirectDraw7 *ddraw = NULL, *again = NULL;
    IDirectDraw *ddraw1 = NULL;
    IUnknown *unk1 = NULL, *unk2 = NULL;
    ULONG count;
    HRESULT hr;
    HWND hwnd;

    test_begin("ddraw_refcount");

    hwnd = test_create_window("ddraw_refcount", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    count = IDirectDraw7_AddRef(ddraw);
    ok_(count == 2, "AddRef reported %lu, expected 2", count);
    count = IDirectDraw7_Release(ddraw);
    ok_(count == 1, "Release reported %lu, expected 1", count);

    /* QueryInterface for the same interface must hand back the same pointer
       and take a reference. */
    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirectDraw7, (void **)&again);
    ok_(SUCCEEDED(hr), "QueryInterface(IDirectDraw7) returned 0x%08lx", hr);
    ok_(again == ddraw, "QI for the same interface returned %p, expected %p", again, ddraw);
    if (again)
        IDirectDraw7_Release(again);

    /* COM identity: IUnknown must be the same pointer whichever interface it
       is asked through. */
    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirectDraw, (void **)&ddraw1);
    ok_(SUCCEEDED(hr) && ddraw1 != NULL, "QueryInterface(IDirectDraw) returned 0x%08lx", hr);

    if (ddraw1)
    {
        IDirectDraw7_QueryInterface(ddraw, &IID_IUnknown, (void **)&unk1);
        IDirectDraw_QueryInterface(ddraw1, &IID_IUnknown, (void **)&unk2);
        ok_(unk1 != NULL && unk1 == unk2,
            "IUnknown is %p through v7 and %p through v1, expected them to agree", unk1, unk2);
        if (unk1) IUnknown_Release(unk1);
        if (unk2) IUnknown_Release(unk2);
        IDirectDraw_Release(ddraw1);
    }

    /* A surface keeps its creating ddraw object alive. */
    surface = make_rgb_surface(ddraw, 32, 32, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    ok_(surface != NULL, "created a surface");
    if (surface)
    {
        count = IDirectDrawSurface7_AddRef(surface);
        ok_(count == 2, "surface AddRef reported %lu, expected 2", count);
        count = IDirectDrawSurface7_Release(surface);
        ok_(count == 1, "surface Release reported %lu, expected 1", count);

        count = IDirectDrawSurface7_Release(surface);
        ok_(count == 0, "final surface Release reported %lu, expected 0", count);
        surface = NULL;
    }

    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
