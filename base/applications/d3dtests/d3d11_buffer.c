/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: buffer creation, mapping and updates
 */


#include "d3dtest.h"
#include <d3d11.h>

/* Try the hardware driver first and fall back to WARP/reference, so the test
   still exercises the API surface on a machine with no 3D driver. */
static HRESULT D3DTEST_UNUSED create_d3d11_device(ID3D11Device **device, ID3D11DeviceContext **context,
                                   D3D_FEATURE_LEVEL *level)
{
    static const D3D_DRIVER_TYPE types[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    HRESULT hr = E_FAIL;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D11CreateDevice(NULL, types[i], NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                               device, level, context);
        if (SUCCEEDED(hr))
            return hr;
    }
    return hr;
}

int main(void)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    ID3D11Buffer *buffer = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_SUBRESOURCE_DATA data;
    D3D11_BUFFER_DESC desc, got;
    D3D_FEATURE_LEVEL level;
    float vertices[12] = { 0 };
    HRESULT hr;

    test_begin("d3d11_buffer");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    memset(&data, 0, sizeof(data));
    data.pSysMem = vertices;

    hr = ID3D11Device_CreateBuffer(device, &desc, &data, &buffer);
    ok_(SUCCEEDED(hr) && buffer != NULL, "CreateBuffer(VERTEX_BUFFER) returned 0x%08lx", hr);

    if (buffer)
    {
        memset(&got, 0, sizeof(got));
        ID3D11Buffer_GetDesc(buffer, &got);
        ok_(got.ByteWidth == sizeof(vertices), "buffer is %u bytes, expected %u",
            got.ByteWidth, (unsigned)sizeof(vertices));
        ok_(got.BindFlags == D3D11_BIND_VERTEX_BUFFER, "bind flags are 0x%08lx",
            (unsigned long)got.BindFlags);
        D3DTEST_RELEASE(buffer);
    }

    /* A dynamic buffer is the one kind the CPU may Map for writing. */
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = 256;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer);
    ok_(SUCCEEDED(hr) && buffer != NULL, "CreateBuffer(DYNAMIC) returned 0x%08lx", hr);

    if (buffer && context)
    {
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)buffer, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        ok_(SUCCEEDED(hr), "Map(WRITE_DISCARD) returned 0x%08lx", hr);
        if (SUCCEEDED(hr))
        {
            ok_(mapped.pData != NULL, "Map produced a pointer");
            memset(mapped.pData, 0x5a, 256);
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)buffer, 0);
            ok_(1, "Unmap completed");
        }
        D3DTEST_RELEASE(buffer);
    }

    /* Mapping a DEFAULT buffer must be refused. */
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = 256;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer)) && context)
    {
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)buffer, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        ok_(FAILED(hr), "Map on a DEFAULT buffer returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(buffer);
    }

    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
