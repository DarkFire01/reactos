/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: vertex and pixel shader creation from bytecode
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device_ex(IDirect3D9 *d3d, HWND hwnd, BOOL want_depth)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    HRESULT hr;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    if (want_depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
    }

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

static D3DTEST_UNUSED IDirect3DDevice9 *create_device(IDirect3D9 *d3d, HWND hwnd)
{
    return create_device_ex(d3d, hwnd, FALSE);
}

/* vs_1_1: pass position straight through and emit a constant colour.
   Hand-assembled so the test does not depend on d3dcompiler. */
static const DWORD vs_code[] =
{
    0xfffe0101,                                     /* vs_1_1                   */
    0x0000001f, 0x80000000, 0x900f0000,             /* dcl_position v0          */
    0x00000001, 0xc00f0000, 0x90e40000,             /* mov oPos, v0             */
    0x00000001, 0xd00f0000, 0xa0e40000,             /* mov oD0, c0              */
    0x0000ffff,                                     /* end                      */
};

/* ps_1_1: output the interpolated diffuse colour. */
static const DWORD ps_code[] =
{
    0xffff0101,                                     /* ps_1_1                   */
    0x00000001, 0x800f0000, 0x90e40000,             /* mov r0, v0               */
    0x0000ffff,                                     /* end                      */
};

int main(void)
{
    IDirect3DVertexShader9 *vs = NULL, *got_vs = NULL;
    IDirect3DPixelShader9 *ps = NULL, *got_ps = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    float constants[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    float readback[4] = { 0 };
    D3DCAPS9 caps;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_shader");

    hwnd = test_create_window("d3d9_shader", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);

    if (caps.VertexShaderVersion < D3DVS_VERSION(1, 1))
    {
        skip_("device advertises no vs_1_1 support");
    }
    else
    {
        hr = IDirect3DDevice9_CreateVertexShader(device, vs_code, &vs);
        ok_(SUCCEEDED(hr) && vs != NULL, "CreateVertexShader(vs_1_1) returned 0x%08lx", hr);

        if (vs)
        {
            hr = IDirect3DDevice9_SetVertexShader(device, vs);
            ok_(SUCCEEDED(hr), "SetVertexShader returned 0x%08lx", hr);

            hr = IDirect3DDevice9_GetVertexShader(device, &got_vs);
            ok_(SUCCEEDED(hr), "GetVertexShader returned 0x%08lx", hr);
            ok_(got_vs == vs, "device holds %p, expected %p", got_vs, vs);
            D3DTEST_RELEASE(got_vs);

            hr = IDirect3DDevice9_SetVertexShaderConstantF(device, 0, constants, 1);
            ok_(SUCCEEDED(hr), "SetVertexShaderConstantF returned 0x%08lx", hr);

            hr = IDirect3DDevice9_GetVertexShaderConstantF(device, 0, readback, 1);
            ok_(SUCCEEDED(hr), "GetVertexShaderConstantF returned 0x%08lx", hr);
            ok_(readback[0] == 1.0f && readback[1] == 0.5f &&
                readback[2] == 0.25f && readback[3] == 1.0f,
                "constant reads back as %.2f,%.2f,%.2f,%.2f",
                readback[0], readback[1], readback[2], readback[3]);

            IDirect3DDevice9_SetVertexShader(device, NULL);
        }
    }

    if (caps.PixelShaderVersion < D3DPS_VERSION(1, 1))
    {
        skip_("device advertises no ps_1_1 support");
    }
    else
    {
        hr = IDirect3DDevice9_CreatePixelShader(device, ps_code, &ps);
        ok_(SUCCEEDED(hr) && ps != NULL, "CreatePixelShader(ps_1_1) returned 0x%08lx", hr);

        if (ps)
        {
            hr = IDirect3DDevice9_SetPixelShader(device, ps);
            ok_(SUCCEEDED(hr), "SetPixelShader returned 0x%08lx", hr);

            hr = IDirect3DDevice9_GetPixelShader(device, &got_ps);
            ok_(SUCCEEDED(hr), "GetPixelShader returned 0x%08lx", hr);
            ok_(got_ps == ps, "device holds %p, expected %p", got_ps, ps);
            D3DTEST_RELEASE(got_ps);

            IDirect3DDevice9_SetPixelShader(device, NULL);
        }
    }

    /* Garbage must be rejected rather than accepted and crashed on later. */
    {
        static const DWORD junk[] = { 0xdeadbeef, 0x0000ffff };
        IDirect3DVertexShader9 *bad = NULL;

        hr = IDirect3DDevice9_CreateVertexShader(device, junk, &bad);
        ok_(FAILED(hr), "CreateVertexShader on junk returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(bad);
    }

cleanup:
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
