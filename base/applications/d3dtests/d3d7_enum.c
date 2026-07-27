/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: device enumeration through IDirect3D7
 */


#include "d3dtest.h"
#include <ddraw.h>
#include <d3d.h>

static int device_count;

static HRESULT WINAPI device_cb(char *desc, char *name, D3DDEVICEDESC7 *caps, void *ctx)
{
    device_count++;
    info_("device %d: '%s' (%s), vertex caps 0x%08lx",
          device_count, name ? name : "?", desc ? desc : "?", caps->dwDevCaps);
    return DDENUMRET_OK;
}

int main(void)
{
    IDirect3D7 *d3d = NULL;
    IDirectDraw7 *ddraw = NULL;
    HRESULT hr;

    test_begin("d3d7_enum");

    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        return test_end();

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr) || !d3d)
    {
        skip_("no IDirect3D7 available (0x%08lx) -- no 3D on this adapter", hr);
        D3DTEST_RELEASE(ddraw);
        return test_end();
    }
    ok_(SUCCEEDED(hr), "QueryInterface(IID_IDirect3D7) returned 0x%08lx", hr);

    hr = IDirect3D7_EnumDevices(d3d, device_cb, NULL);
    ok_(SUCCEEDED(hr), "EnumDevices returned 0x%08lx", hr);
    ok_(device_count > 0, "enumerated %d 3D device(s)", device_count);

    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
    return test_end();
}
