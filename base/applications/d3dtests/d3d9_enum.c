/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: adapter, mode and format enumeration
 */


#include "d3dtest.h"
#include <d3d9.h>

int main(void)
{
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9 *d3d = NULL;
    D3DDISPLAYMODE mode;
    UINT count, modes;
    HRESULT hr;
    UINT i;

    test_begin("d3d9_enum");

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate9 returned an object");
    if (!d3d)
        return test_end();

    count = IDirect3D9_GetAdapterCount(d3d);
    ok_(count >= 1, "GetAdapterCount reports %u adapter(s)", count);

    memset(&id, 0, sizeof(id));
    hr = IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id);
    ok_(SUCCEEDED(hr), "GetAdapterIdentifier returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        info_("adapter 0: '%s'", id.Description);
        info_("driver '%s', vendor 0x%04lx device 0x%04lx",
              id.Driver, (unsigned long)id.VendorId, (unsigned long)id.DeviceId);
    }

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    ok_(SUCCEEDED(hr), "GetAdapterDisplayMode returned 0x%08lx", hr);
    ok_(mode.Width > 0 && mode.Height > 0,
        "current mode is %ux%u fmt %u", mode.Width, mode.Height, mode.Format);

    /* Unlike d3d8, d3d9 enumerates modes per format. */
    modes = IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    info_("X8R8G8B8 has %u mode(s)", modes);

    for (i = 0; i < modes && i < 5; i++)
    {
        memset(&mode, 0, sizeof(mode));
        hr = IDirect3D9_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &mode);
        ok_(SUCCEEDED(hr), "EnumAdapterModes(%u) returned 0x%08lx", i, hr);
        if (SUCCEEDED(hr))
            info_("  mode %u: %ux%u @%uHz", i, mode.Width, mode.Height, mode.RefreshRate);
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    info_("CheckDeviceFormat(A8R8G8B8 texture) returned 0x%08lx", hr);

    hr = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1);
    info_("CheckDeviceFormat(DXT1 texture) returned 0x%08lx", hr);

    D3DTEST_RELEASE(d3d);
    return test_end();
}
