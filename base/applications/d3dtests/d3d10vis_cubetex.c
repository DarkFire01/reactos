/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10 visual: a textured rotating cube
 */


#include "d3dvis.h"
#include <d3d10_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

struct d10_scene
{
    ID3D10Device *device;
    IDXGISwapChain *swapchain;
    ID3D10RenderTargetView *rtv;
    ID3D10DepthStencilView *dsv;
    ID3D10Texture2D *depth;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL d10_open(struct d10_scene *s, HWND hwnd)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE, D3D10_DRIVER_TYPE_WARP, D3D10_DRIVER_TYPE_REFERENCE,
    };
    DXGI_SWAP_CHAIN_DESC scd;
    D3D10_TEXTURE2D_DESC td;
    ID3D10Texture2D *bb = NULL;
    unsigned int i;
    HRESULT hr = E_FAIL;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    memset(&scd, 0, sizeof(scd));
    scd.BufferDesc.Width = VIS_WIDTH;
    scd.BufferDesc.Height = VIS_HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D10CreateDeviceAndSwapChain(NULL, types[i], NULL, 0, D3D10_SDK_VERSION,
                                           &scd, &s->swapchain, &s->device);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return FALSE;

    if (FAILED(IDXGISwapChain_GetBuffer(s->swapchain, 0, &IID_ID3D10Texture2D, (void **)&bb)))
        return FALSE;
    hr = ID3D10Device_CreateRenderTargetView(s->device, (ID3D10Resource *)bb, NULL, &s->rtv);
    ID3D10Texture2D_Release(bb);
    if (FAILED(hr))
        return FALSE;

    memset(&td, 0, sizeof(td));
    td.Width = VIS_WIDTH;
    td.Height = VIS_HEIGHT;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D10Device_CreateTexture2D(s->device, &td, NULL, &s->depth)))
        ID3D10Device_CreateDepthStencilView(s->device, (ID3D10Resource *)s->depth, NULL, &s->dsv);

    return TRUE;
}

static D3DTEST_UNUSED void d10_close(struct d10_scene *s)
{
    if (s->dsv) ID3D10DepthStencilView_Release(s->dsv);
    if (s->depth) ID3D10Texture2D_Release(s->depth);
    if (s->rtv) ID3D10RenderTargetView_Release(s->rtv);
    if (s->swapchain) IDXGISwapChain_Release(s->swapchain);
    if (s->device) ID3D10Device_Release(s->device);
    memset(s, 0, sizeof(*s));
}

static D3DTEST_UNUSED void d10_begin(struct d10_scene *s, const float *clear)
{
    D3D10_VIEWPORT vp;

    ID3D10Device_OMSetRenderTargets(s->device, 1, &s->rtv, s->dsv);
    ID3D10Device_ClearRenderTargetView(s->device, s->rtv, clear);
    if (s->dsv)
        ID3D10Device_ClearDepthStencilView(s->device, s->dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

    memset(&vp, 0, sizeof(vp));
    vp.Width = VIS_WIDTH;
    vp.Height = VIS_HEIGHT;
    vp.MaxDepth = 1.0f;
    ID3D10Device_RSSetViewports(s->device, 1, &vp);
}

static D3DTEST_UNUSED int d10_sample(struct d10_scene *s, DWORD *out, int max)
{
    ID3D10Texture2D *bb = NULL, *staging = NULL;
    D3D10_MAPPED_TEXTURE2D m;
    D3D10_TEXTURE2D_DESC td;
    int i, n = 0;

    if (FAILED(IDXGISwapChain_GetBuffer(s->swapchain, 0, &IID_ID3D10Texture2D, (void **)&bb)))
        return 0;
    ID3D10Texture2D_GetDesc(bb, &td);
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D10Device_CreateTexture2D(s->device, &td, NULL, &staging)))
        goto done;

    ID3D10Device_CopyResource(s->device, (ID3D10Resource *)staging, (ID3D10Resource *)bb);
    memset(&m, 0, sizeof(m));
    if (FAILED(ID3D10Texture2D_Map(staging, 0, D3D10_MAP_READ, 0, &m)))
        goto done;
    for (i = 0; i < max; i++)
    {
        int x = (i * 7) % (int)td.Width;
        int y = (i * 13) % (int)td.Height;
        out[n++] = *(DWORD *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
    }
    ID3D10Texture2D_Unmap(staging, 0);

done:
    if (staging) ID3D10Texture2D_Release(staging);
    if (bb) ID3D10Texture2D_Release(bb);
    return n;
}

static D3DTEST_UNUSED ID3D10Blob *d10_compile(const char *source, const char *profile)
{
    ID3D10Blob *code = NULL, *errors = NULL;
    HRESULT hr;

    hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, "main", profile, 0, 0,
                    &code, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            info_("%s: %s", profile, (const char *)ID3D10Blob_GetBufferPointer(errors));
            ID3D10Blob_Release(errors);
        }
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

struct d10_constants { float wvp[16]; float world[16]; float light[4]; };

static D3DTEST_UNUSED void d10_store_matrix(float *dst, const struct vis_mat *src)
{
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            dst[i * 4 + j] = src->m[j][i];
}

struct d10_vertex { float pos[3]; float normal[3]; float uv[2]; float colour[4]; };

static D3DTEST_UNUSED void d10_fill(struct d10_vertex *dst,
                                    const struct vis_vertex *src, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        dst[i].pos[0] = src[i].x; dst[i].pos[1] = src[i].y; dst[i].pos[2] = src[i].z;
        dst[i].normal[0] = src[i].nx; dst[i].normal[1] = src[i].ny; dst[i].normal[2] = src[i].nz;
        dst[i].uv[0] = src[i].u; dst[i].uv[1] = src[i].v;
        dst[i].colour[0] = ((src[i].colour >> 16) & 0xff) / 255.0f;
        dst[i].colour[1] = ((src[i].colour >> 8) & 0xff) / 255.0f;
        dst[i].colour[2] = (src[i].colour & 0xff) / 255.0f;
        dst[i].colour[3] = ((src[i].colour >> 24) & 0xff) / 255.0f;
    }
}

static const D3D10_INPUT_ELEMENT_DESC d10_layout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D10_INPUT_PER_VERTEX_DATA, 0 },
};

static D3DTEST_UNUSED ID3D10Buffer *d10_make_buffer(ID3D10Device *device, const void *data,
                                                    UINT size, UINT bind)
{
    ID3D10Buffer *buffer = NULL;
    D3D10_SUBRESOURCE_DATA sd;
    D3D10_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = size;
    bd.Usage = D3D10_USAGE_DEFAULT;
    bd.BindFlags = bind;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = data;
    if (FAILED(ID3D10Device_CreateBuffer(device, &bd, data ? &sd : NULL, &buffer)))
        return NULL;
    return buffer;
}

static D3DTEST_UNUSED ID3D10ShaderResourceView *d10_make_texture(ID3D10Device *device,
        const DWORD *pixels, int size)
{
    ID3D10ShaderResourceView *srv = NULL;
    ID3D10Texture2D *texture = NULL;
    D3D10_SUBRESOURCE_DATA sd;
    D3D10_TEXTURE2D_DESC td;

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = size;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pixels;
    sd.SysMemPitch = size * 4;

    /* A 10.0 feature level only has to support B8G8R8A8 for the swap chain,
       not as a shader resource; that arrived with 10.1. Fall back to RGBA and
       swap the two channels over by hand so the picture still comes out. */
    if (FAILED(ID3D10Device_CreateTexture2D(device, &td, &sd, &texture)))
    {
        DWORD *rgba = malloc(size * size * sizeof(*rgba));
        int i;

        if (!rgba)
            return NULL;
        for (i = 0; i < size * size; i++)
        {
            DWORD c = pixels[i];
            rgba[i] = (c & 0xff00ff00) | ((c >> 16) & 0xff) | ((c & 0xff) << 16);
        }
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.pSysMem = rgba;
        if (FAILED(ID3D10Device_CreateTexture2D(device, &td, &sd, &texture)))
            texture = NULL;
        free(rgba);
        if (!texture)
            return NULL;
    }
    ID3D10Device_CreateShaderResourceView(device, (ID3D10Resource *)texture, NULL, &srv);
    ID3D10Texture2D_Release(texture);
    return srv;
}

static const char hlsl_vs[] =
    "cbuffer constants : register(b0)\n"
    "{\n"
    "    float4x4 wvp;\n"
    "    float4x4 world;\n"
    "    float4   light;\n"
    "};\n"
    "struct vs_in  { float3 pos : POSITION; float3 nrm : NORMAL;\n"
    "                float2 uv : TEXCOORD0; float4 col : COLOR; };\n"
    "struct vs_out { float4 pos : SV_POSITION; float3 nrm : NORMAL;\n"
    "                float2 uv : TEXCOORD0; float4 col : COLOR; };\n"
    "vs_out main(vs_in v)\n"
    "{\n"
    "    vs_out o;\n"
    "    o.pos = mul(float4(v.pos, 1.0f), wvp);\n"
    "    o.nrm = mul(float4(v.nrm, 0.0f), world).xyz;\n"
    "    o.uv  = v.uv;\n"
    "    o.col = v.col;\n"
    "    return o;\n"
    "}\n";

static const char hlsl_ps_colour[] =
    "struct ps_in { float4 pos : SV_POSITION; float3 nrm : NORMAL;\n"
    "               float2 uv : TEXCOORD0; float4 col : COLOR; };\n"
    "float4 main(ps_in i) : SV_TARGET { return i.col; }\n";

static const char hlsl_ps_texture[] =
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct ps_in { float4 pos : SV_POSITION; float3 nrm : NORMAL;\n"
    "               float2 uv : TEXCOORD0; float4 col : COLOR; };\n"
    "float4 main(ps_in i) : SV_TARGET { return tex.Sample(smp, i.uv); }\n";

static const char hlsl_ps_lit[] =
    "cbuffer constants : register(b0)\n"
    "{\n"
    "    float4x4 wvp;\n"
    "    float4x4 world;\n"
    "    float4   light;\n"
    "};\n"
    "struct ps_in { float4 pos : SV_POSITION; float3 nrm : NORMAL;\n"
    "               float2 uv : TEXCOORD0; float4 col : COLOR; };\n"
    "float4 main(ps_in i) : SV_TARGET\n"
    "{\n"
    "    float3 n = normalize(i.nrm);\n"
    "    float  d = saturate(dot(n, normalize(light.xyz)));\n"
    "    return float4(i.col.rgb * (0.25f + 0.75f * d), 1.0f);\n"
    "}\n";

int main(int argc, char **argv)
{
    ID3D10InputLayout *layout = NULL;
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10Buffer *vb = NULL, *ib = NULL, *cb = NULL;
    ID3D10ShaderResourceView *srv = NULL;
    ID3D10SamplerState *sampler = NULL;
    ID3D10Blob *vsb = NULL, *psb = NULL;
    struct d10_vertex verts[VIS_CUBE_VERTICES];
    struct d10_constants constants;
    static DWORD pixels[64 * 64];
    DWORD sample[256];
    struct d10_scene s;
    struct vis_mat world, view, proj, wv, wvp, ry, rx;
    float clear[4] = { 0.09f, 0.11f, 0.16f, 1.0f };
    UINT stride = sizeof(struct d10_vertex), offset = 0;
    D3D10_BUFFER_DESC bd;
    int frame = 0, n;
    HWND hwnd;

    (void)pixels; (void)srv; (void)sampler;
    vis_parse_args(argc, argv);
    test_begin("d3d10vis_cubetex");

    hwnd = vis_create_window("Direct3D 10: textured cube");
    if (!d10_open(&s, hwnd)) { skip_("no Direct3D 10 device"); goto done; }
    ok_(1, "created a Direct3D 10 device and swap chain");

    if (!(vsb = d10_compile(hlsl_vs, "vs_4_0")) || !(psb = d10_compile(hlsl_ps_texture, "ps_4_0")))
    {
        skip_("could not compile the scene shaders");
        goto done;
    }
    ok_(1, "compiled the vertex and pixel shaders");

    ID3D10Device_CreateVertexShader(s.device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &vs);
    ID3D10Device_CreatePixelShader(s.device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), &ps);
    ID3D10Device_CreateInputLayout(s.device, d10_layout, ARRAYSIZE(d10_layout),
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &layout);

    d10_fill(verts, vis_cube, VIS_CUBE_VERTICES);
    vb = d10_make_buffer(s.device, verts, sizeof(verts), D3D10_BIND_VERTEX_BUFFER);
    ib = d10_make_buffer(s.device, vis_cube_indices, sizeof(vis_cube_indices), D3D10_BIND_INDEX_BUFFER);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = (sizeof(constants) + 15) & ~15u;
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ID3D10Device_CreateBuffer(s.device, &bd, NULL, &cb);

    if (!vs || !ps || !layout || !vb || !ib || !cb)
    {
        skip_("could not build the scene resources");
        goto done;
    }

    vis_tex_checker(pixels, 64, 64, 8, 0xff40e0a0, 0xff203050);
    srv = d10_make_texture(s.device, pixels, 64);
    ok_(srv != NULL, "created and uploaded the 64x64 texture");
    {
        D3D10_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = (FLOAT)D3D10_FLOAT32_MAX;
        ID3D10Device_CreateSamplerState(s.device, &sd, &sampler);
    }
    if (!srv || !sampler) { skip_("could not build the texture resources"); goto done; }

    vis_perspective(&proj, 1.05f, (float)VIS_WIDTH / VIS_HEIGHT, 1.0f, 100.0f);
    vis_lookat(&view, 0.0f, 1.2f, -6.0f, 0.0f, 0.0f, 0.0f);

    while (vis_frame(frame++))
    {
        float t = frame * 0.045f;
        void *mapped = NULL;

        vis_rotate_y(&ry, t);
        vis_rotate_x(&rx, t * 0.55f);
        vis_mul(&world, &rx, &ry);
        vis_mul(&wv, &world, &view);
        vis_mul(&wvp, &wv, &proj);

        memset(&constants, 0, sizeof(constants));
        d10_store_matrix(constants.wvp, &wvp);
        d10_store_matrix(constants.world, &world);
        constants.light[0] = -0.4f;
        constants.light[1] = 0.7f;
        constants.light[2] = -0.6f;
        if (SUCCEEDED(ID3D10Buffer_Map(cb, D3D10_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped, &constants, sizeof(constants));
            ID3D10Buffer_Unmap(cb);
        }

        d10_begin(&s, clear);
        ID3D10Device_IASetInputLayout(s.device, layout);
        ID3D10Device_IASetVertexBuffers(s.device, 0, 1, &vb, &stride, &offset);
        ID3D10Device_IASetIndexBuffer(s.device, ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D10Device_IASetPrimitiveTopology(s.device, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D10Device_VSSetShader(s.device, vs);
        ID3D10Device_VSSetConstantBuffers(s.device, 0, 1, &cb);
        ID3D10Device_PSSetShader(s.device, ps);
        ID3D10Device_PSSetConstantBuffers(s.device, 0, 1, &cb);
        ID3D10Device_PSSetShaderResources(s.device, 0, 1, &srv);
        ID3D10Device_PSSetSamplers(s.device, 0, 1, &sampler);

        ID3D10Device_DrawIndexed(s.device, VIS_CUBE_INDICES, 0, 0);
        IDXGISwapChain_Present(s.swapchain, 0, 0);
    }
    ok_(frame > 1, "rendered %d frames", frame - 1);

    n = d10_sample(&s, sample, ARRAYSIZE(sample));
    if (n) vis_check_rendered(sample, n, 0xff291c17);
    else skip_("could not read the back buffer");

    vis_wait_if_held();
done:
    D3DTEST_RELEASE(sampler);
    D3DTEST_RELEASE(srv);
    D3DTEST_RELEASE(cb);
    D3DTEST_RELEASE(ib);
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(layout);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb);
    d10_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
