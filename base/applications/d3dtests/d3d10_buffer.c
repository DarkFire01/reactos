/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: buffer creation and description round-trip
 */


#include "d3dtest.h"
#include <d3d10_1.h>

static HRESULT D3DTEST_UNUSED create_d3d10_device(ID3D10Device **device)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE,
        D3D10_DRIVER_TYPE_WARP,
        D3D10_DRIVER_TYPE_REFERENCE,
    };
    HRESULT hr = E_FAIL;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D10CreateDevice(NULL, types[i], NULL, 0, D3D10_SDK_VERSION, device);
        if (SUCCEEDED(hr))
            return hr;
    }
    return hr;
}

int main(void)
{
    ID3D10Device *device = NULL;
    ID3D10Buffer *buffer = NULL;
    D3D10_BUFFER_DESC desc, got;
    D3D10_SUBRESOURCE_DATA data;
    float vertices[12] = { 0 };
    HRESULT hr;

    test_begin("d3d10_buffer");

    hr = create_d3d10_device(&device);
    if (FAILED(hr))
    {
        skip_("no Direct3D 10 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_VERTEX_BUFFER;

    memset(&data, 0, sizeof(data));
    data.pSysMem = vertices;

    hr = ID3D10Device_CreateBuffer(device, &desc, &data, &buffer);
    ok_(SUCCEEDED(hr) && buffer != NULL, "CreateBuffer(VERTEX_BUFFER) returned 0x%08lx", hr);

    if (buffer)
    {
        memset(&got, 0, sizeof(got));
        ID3D10Buffer_GetDesc(buffer, &got);
        ok_(got.ByteWidth == sizeof(vertices), "buffer is %u bytes, expected %u",
            got.ByteWidth, (unsigned)sizeof(vertices));
        ok_(got.BindFlags == D3D10_BIND_VERTEX_BUFFER,
            "bind flags are 0x%08lx, expected VERTEX_BUFFER", (unsigned long)got.BindFlags);
        D3DTEST_RELEASE(buffer);
    }

    /* A constant buffer must be a multiple of 16 bytes. */
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = 64;
    desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    hr = ID3D10Device_CreateBuffer(device, &desc, NULL, &buffer);
    ok_(SUCCEEDED(hr) && buffer != NULL, "CreateBuffer(CONSTANT_BUFFER, 64) returned 0x%08lx", hr);
    D3DTEST_RELEASE(buffer);

    /* A dynamic buffer with no CPU write access is contradictory. */
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = 64;
    desc.Usage = D3D10_USAGE_DYNAMIC;
    desc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = 0;
    hr = ID3D10Device_CreateBuffer(device, &desc, NULL, &buffer);
    ok_(FAILED(hr), "DYNAMIC buffer without CPU write returned 0x%08lx, expected failure", hr);
    D3DTEST_RELEASE(buffer);

    D3DTEST_RELEASE(device);
    return test_end();
}
