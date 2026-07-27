/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: device capability and shader model reporting
 */


#include "d3dtest.h"
#include <d3d9.h>

int main(void)
{
    IDirect3D9 *d3d = NULL;
    D3DCAPS9 caps;
    HRESULT hr;

    test_begin("d3d9_caps");

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate9 returned an object");
    if (!d3d)
        return test_end();

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr))
    {
        info_("no HAL caps (0x%08lx), querying the reference device", hr);
        hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, &caps);
    }
    ok_(SUCCEEDED(hr), "GetDeviceCaps returned 0x%08lx", hr);
    if (FAILED(hr))
    {
        D3DTEST_RELEASE(d3d);
        return test_end();
    }

    info_("vertex shader %u.%u, pixel shader %u.%u",
          (unsigned)((caps.VertexShaderVersion >> 8) & 0xff),
          (unsigned)(caps.VertexShaderVersion & 0xff),
          (unsigned)((caps.PixelShaderVersion >> 8) & 0xff),
          (unsigned)(caps.PixelShaderVersion & 0xff));
    info_("max texture %ux%u, %u simultaneous RTs, %u stages, %u streams",
          caps.MaxTextureWidth, caps.MaxTextureHeight,
          caps.NumSimultaneousRTs, caps.MaxTextureBlendStages, caps.MaxStreams);

    ok_(caps.MaxTextureWidth > 0 && caps.MaxTextureHeight > 0,
        "device reports a usable maximum texture size");
    ok_(caps.NumSimultaneousRTs >= 1, "device reports %u simultaneous render target(s)",
        caps.NumSimultaneousRTs);
    ok_(caps.MaxStreams >= 1, "device reports %u vertex stream(s)", caps.MaxStreams);

    /* Shader model reporting should at least be self-consistent. */
    if (caps.VertexShaderVersion >= D3DVS_VERSION(1, 1))
        info_("vertex shaders 1.1 or better are advertised");
    else
        skip_("no vertex shader support advertised");

    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1))
        info_("pixel shaders 1.1 or better are advertised");
    else
        skip_("no pixel shader support advertised");

    D3DTEST_RELEASE(d3d);
    return test_end();
}
