/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: device capability reporting
 */


#include "d3dtest.h"
#include <d3d8.h>

int main(void)
{
    IDirect3D8 *d3d = NULL;
    D3DCAPS8 caps;
    HRESULT hr;

    test_begin("d3d8_caps");

    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate8 returned an object");
    if (!d3d)
        return test_end();

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr))
    {
        info_("no HAL caps (0x%08lx), querying the reference device", hr);
        hr = IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, &caps);
    }
    ok_(SUCCEEDED(hr), "GetDeviceCaps returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        info_("vertex shader version %u.%u, pixel shader version %u.%u",
              (unsigned)((caps.VertexShaderVersion >> 8) & 0xff),
              (unsigned)(caps.VertexShaderVersion & 0xff),
              (unsigned)((caps.PixelShaderVersion >> 8) & 0xff),
              (unsigned)(caps.PixelShaderVersion & 0xff));
        info_("max texture %ux%u, %u simultaneous render targets, %u texture blend stages",
              caps.MaxTextureWidth, caps.MaxTextureHeight,
              caps.MaxSimultaneousTextures, caps.MaxTextureBlendStages);

        ok_(caps.MaxTextureWidth > 0 && caps.MaxTextureHeight > 0,
            "device reports a usable maximum texture size");
        ok_(caps.MaxTextureBlendStages > 0, "device reports at least one blend stage");
        ok_(caps.DeviceType == D3DDEVTYPE_HAL || caps.DeviceType == D3DDEVTYPE_REF,
            "caps carry a sensible device type (%u)", caps.DeviceType);
    }

    D3DTEST_RELEASE(d3d);
    return test_end();
}
