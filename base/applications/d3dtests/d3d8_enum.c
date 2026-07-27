/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: adapter, mode and format enumeration
 */


#include "d3dtest.h"
#include <d3d8.h>

int main(void)
{
    IDirect3D8 *d3d = NULL;
    D3DADAPTER_IDENTIFIER8 id;
    D3DDISPLAYMODE mode;
    UINT count, modes;
    HRESULT hr;
    UINT i;

    test_begin("d3d8_enum");

    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate8 returned an object");
    if (!d3d)
        return test_end();

    count = IDirect3D8_GetAdapterCount(d3d);
    ok_(count >= 1, "GetAdapterCount reports %u adapter(s)", count);

    memset(&id, 0, sizeof(id));
    hr = IDirect3D8_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id);
    ok_(SUCCEEDED(hr), "GetAdapterIdentifier returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
        info_("adapter 0: '%s' driver '%s'", id.Description, id.Driver);

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    ok_(SUCCEEDED(hr), "GetAdapterDisplayMode returned 0x%08lx", hr);
    ok_(mode.Width > 0 && mode.Height > 0,
        "current mode is %ux%u format %u", mode.Width, mode.Height, mode.Format);

    modes = IDirect3D8_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT);
    info_("GetAdapterModeCount reports %u mode(s)", modes);

    for (i = 0; i < modes && i < 5; i++)
    {
        memset(&mode, 0, sizeof(mode));
        hr = IDirect3D8_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT, i, &mode);
        ok_(SUCCEEDED(hr), "EnumAdapterModes(%u) returned 0x%08lx", i, hr);
        if (SUCCEEDED(hr))
            info_("  mode %u: %ux%u @%uHz fmt %u", i, mode.Width, mode.Height,
                  mode.RefreshRate, mode.Format);
    }

    hr = IDirect3D8_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                    D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
    info_("CheckDeviceType(HAL, X8R8G8B8, windowed) returned 0x%08lx", hr);

    D3DTEST_RELEASE(d3d);
    return test_end();
}
