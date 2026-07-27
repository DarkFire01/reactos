/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: clipper creation, window association and clip lists
 */


#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDrawClipper *clipper = NULL;
    IDirectDraw7 *ddraw = NULL;
    HWND hwnd, got = NULL;
    char buffer[512];
    RGNDATA *region = (RGNDATA *)buffer;
    DWORD size = sizeof(buffer);
    HRESULT hr;

    test_begin("ddraw_clipper");

    hwnd = test_create_window("ddraw_clipper", 320, 240);
    ok_(hwnd != NULL, "created test window");
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    hr = IDirectDraw7_CreateClipper(ddraw, 0, &clipper, NULL);
    ok_(SUCCEEDED(hr) && clipper != NULL, "CreateClipper returned 0x%08lx", hr);
    if (!clipper)
        goto cleanup;

    /* A fresh clipper has no window yet. */
    hr = IDirectDrawClipper_GetHWnd(clipper, &got);
    ok_(SUCCEEDED(hr), "GetHWnd on a fresh clipper returned 0x%08lx", hr);
    ok_(got == NULL, "fresh clipper reports window %p, expected NULL", got);

    hr = IDirectDrawClipper_SetHWnd(clipper, 0, hwnd);
    ok_(SUCCEEDED(hr), "SetHWnd returned 0x%08lx", hr);

    got = NULL;
    hr = IDirectDrawClipper_GetHWnd(clipper, &got);
    ok_(SUCCEEDED(hr), "GetHWnd after SetHWnd returned 0x%08lx", hr);
    ok_(got == hwnd, "clipper reports window %p, expected %p", got, hwnd);

    /* With a window attached the clip list comes from the window region. */
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, region, &size);
    if (SUCCEEDED(hr))
        info_("clip list has %lu rect(s)", region->rdh.nCount);
    else
        skip_("GetClipList returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(clipper);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
