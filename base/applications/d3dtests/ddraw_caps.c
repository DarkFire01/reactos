/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: hardware and emulation capability reporting
 */

#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDraw7 *ddraw = NULL;
    DDCAPS hal, hel;
    DWORD total, free;
    DDSCAPS2 caps;
    HRESULT hr;

    test_begin("ddraw_caps");

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        return test_end();

    memset(&hal, 0, sizeof(hal));
    memset(&hel, 0, sizeof(hel));
    hal.dwSize = sizeof(hal);
    hel.dwSize = sizeof(hel);

    hr = IDirectDraw7_GetCaps(ddraw, &hal, &hel);
    ok_(SUCCEEDED(hr), "GetCaps returned 0x%08lx", hr);

    info_("HAL caps  0x%08lx, blt caps 0x%08lx", hal.dwCaps, hal.dwCaps2);
    info_("HEL caps  0x%08lx, blt caps 0x%08lx", hel.dwCaps, hel.dwCaps2);
    ok_(hal.dwCaps != 0 || hel.dwCaps != 0, "at least one of HAL/HEL reports capabilities");

    if (hal.dwCaps & DDCAPS_3D)
        info_("HAL reports 3D acceleration");
    else
        skip_("HAL reports no 3D acceleration");

    /* Video memory reporting is optional but should not fail outright. */
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_VIDEOMEMORY;
    hr = IDirectDraw7_GetAvailableVidMem(ddraw, &caps, &total, &free);
    if (SUCCEEDED(hr))
        info_("video memory: %lu total, %lu free", total, free);
    else
        skip_("GetAvailableVidMem returned 0x%08lx", hr);

    D3DTEST_RELEASE(ddraw);
    return test_end();
}
