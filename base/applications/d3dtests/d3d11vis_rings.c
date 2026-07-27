/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11 visual: a ring texture on the octahedron
 */


#include "d3dvis.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

struct d11_scene
{
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swapchain;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D *depth;
    D3D_FEATURE_LEVEL level;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL d11_open(struct d11_scene *s, HWND hwnd)
{
    static const D3D_DRIVER_TYPE types[] =
    {
        D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE,
    };
    DXGI_SWAP_CHAIN_DESC scd;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *bb = NULL;
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
        hr = D3D11CreateDeviceAndSwapChain(NULL, types[i], NULL, 0, NULL, 0,
                D3D11_SDK_VERSION, &scd, &s->swapchain, &s->device, &s->level, &s->context);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return FALSE;

    if (FAILED(IDXGISwapChain_GetBuffer(s->swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return FALSE;
    hr = ID3D11Device_CreateRenderTargetView(s->device, (ID3D11Resource *)bb, NULL, &s->rtv);
    ID3D11Texture2D_Release(bb);
    if (FAILED(hr))
        return FALSE;

    memset(&td, 0, sizeof(td));
    td.Width = VIS_WIDTH;
    td.Height = VIS_HEIGHT;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s->device, &td, NULL, &s->depth)))
        ID3D11Device_CreateDepthStencilView(s->device, (ID3D11Resource *)s->depth, NULL, &s->dsv);

    return TRUE;
}

static D3DTEST_UNUSED void d11_close(struct d11_scene *s)
{
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->depth) ID3D11Texture2D_Release(s->depth);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->swapchain) IDXGISwapChain_Release(s->swapchain);
    if (s->context) ID3D11DeviceContext_Release(s->context);
    if (s->device) ID3D11Device_Release(s->device);
    memset(s, 0, sizeof(*s));
}

static D3DTEST_UNUSED void d11_begin(struct d11_scene *s, const float *clear)
{
    D3D11_VIEWPORT vp;

    ID3D11DeviceContext_OMSetRenderTargets(s->context, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_ClearRenderTargetView(s->context, s->rtv, clear);
    if (s->dsv)
        ID3D11DeviceContext_ClearDepthStencilView(s->context, s->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    memset(&vp, 0, sizeof(vp));
    vp.Width = (float)VIS_WIDTH;
    vp.Height = (float)VIS_HEIGHT;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s->context, 1, &vp);
}

static D3DTEST_UNUSED int d11_sample(struct d11_scene *s, DWORD *out, int max)
{
    ID3D11Texture2D *bb = NULL, *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_TEXTURE2D_DESC td;
    int i, n = 0;

    if (FAILED(IDXGISwapChain_GetBuffer(s->swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return 0;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s->device, &td, NULL, &staging)))
        goto done;

    ID3D11DeviceContext_CopyResource(s->context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)bb);
    memset(&m, 0, sizeof(m));
    if (FAILED(ID3D11DeviceContext_Map(s->context, (ID3D11Resource *)staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        goto done;
    for (i = 0; i < max; i++)
    {
        int x = (i * 7) % (int)td.Width;
        int y = (i * 13) % (int)td.Height;
        out[n++] = *(DWORD *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
    }
    ID3D11DeviceContext_Unmap(s->context, (ID3D11Resource *)staging, 0);

done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (bb) ID3D11Texture2D_Release(bb);
    return n;
}

/* Compile a shader, reporting the compiler's own message on failure. */
static D3DTEST_UNUSED ID3D10Blob *d11_compile(const char *source, const char *profile)
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

/* Constants shared by the shaders below: a world-view-projection matrix plus a
   light direction. Kept to 16-byte multiples as constant buffers require. */
struct d11_constants
{
    float wvp[16];
    float world[16];
    float light[4];
};

static D3DTEST_UNUSED ID3D11Buffer *d11_make_cb(ID3D11Device *device, UINT size)
{
    ID3D11Buffer *cb = NULL;
    D3D11_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = (size + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(device, &bd, NULL, &cb)))
        return NULL;
    return cb;
}

static D3DTEST_UNUSED void d11_update_cb(ID3D11DeviceContext *context, ID3D11Buffer *cb,
                                         const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE m;

    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, data, size);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)cb, 0);
    }
}

/* HLSL matrices are column-major by default; transpose on the way in. */
static D3DTEST_UNUSED void d11_store_matrix(float *dst, const struct vis_mat *src)
{
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            dst[i * 4 + j] = src->m[j][i];
}

static D3DTEST_UNUSED ID3D11Buffer *d11_make_vb(ID3D11Device *device,
                                                const void *data, UINT size)
{
    ID3D11Buffer *vb = NULL;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = data;
    if (FAILED(ID3D11Device_CreateBuffer(device, &bd, &sd, &vb)))
        return NULL;
    return vb;
}

static D3DTEST_UNUSED ID3D11Buffer *d11_make_ib(ID3D11Device *device,
                                                const void *data, UINT size)
{
    ID3D11Buffer *ib = NULL;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = data;
    if (FAILED(ID3D11Device_CreateBuffer(device, &bd, &sd, &ib)))
        return NULL;
    return ib;
}

static D3DTEST_UNUSED ID3D11ShaderResourceView *d11_make_texture(ID3D11Device *device,
        const DWORD *pixels, int size)
{
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Texture2D *texture = NULL;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_TEXTURE2D_DESC td;

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = size;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pixels;
    sd.SysMemPitch = size * 4;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &td, &sd, &texture)))
        return NULL;
    ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)texture, NULL, &srv);
    ID3D11Texture2D_Release(texture);
    return srv;
}

/* The vertex layout every d3d11 scene here uses. */
struct d11_vertex { float pos[3]; float normal[3]; float uv[2]; float colour[4]; };

static D3DTEST_UNUSED void d11_fill(struct d11_vertex *dst,
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

static const D3D11_INPUT_ELEMENT_DESC d11_layout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

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
    ID3D11InputLayout *layout = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Buffer *vb = NULL, *ib = NULL, *cb = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *sampler = NULL;
    ID3D10Blob *vsb = NULL, *psb = NULL;
    struct d11_vertex verts[VIS_MODEL_VERTICES];
    struct d11_constants constants;
    static DWORD pixels[64 * 64];
    DWORD sample[256];
    struct d11_scene s;
    struct vis_mat world, view, proj, wv, wvp, ry, rx;
    float clear[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    UINT stride = sizeof(struct d11_vertex), offset = 0;
    int frame = 0, n;
    HWND hwnd;

    (void)pixels; (void)srv; (void)sampler;
    vis_parse_args(argc, argv);
    test_begin("d3d11vis_rings");

    hwnd = vis_create_window("Direct3D 11: ring-textured model");
    if (!d11_open(&s, hwnd)) { skip_("no Direct3D 11 device"); goto done; }
    ok_(1, "created a Direct3D 11 device and swap chain at feature level 0x%04x",
        (unsigned)s.level);

    if (!(vsb = d11_compile(hlsl_vs, "vs_4_0")) || !(psb = d11_compile(hlsl_ps_texture, "ps_4_0")))
    {
        skip_("could not compile the scene shaders");
        goto done;
    }
    ok_(1, "compiled the vertex and pixel shaders");

    ID3D11Device_CreateVertexShader(s.device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(s.device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    ID3D11Device_CreateInputLayout(s.device, d11_layout, ARRAYSIZE(d11_layout),
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &layout);

    d11_fill(verts, vis_model, VIS_MODEL_VERTICES);
    vb = d11_make_vb(s.device, verts, sizeof(verts));
    ib = d11_make_ib(s.device, vis_model_indices, sizeof(vis_model_indices));
    cb = d11_make_cb(s.device, sizeof(constants));
    if (!vs || !ps || !layout || !vb || !ib || !cb)
    {
        skip_("could not build the scene resources");
        goto done;
    }

    vis_tex_rings(pixels, 64, 64);
    srv = d11_make_texture(s.device, pixels, 64);
    ok_(srv != NULL, "created and uploaded the 64x64 texture");
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = (FLOAT)D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s.device, &sd, &sampler);
    }
    if (!srv || !sampler) { skip_("could not build the texture resources"); goto done; }

    vis_perspective(&proj, 1.05f, (float)VIS_WIDTH / VIS_HEIGHT, 1.0f, 100.0f);
    vis_lookat(&view, 0.0f, 1.2f, -5.0f, 0.0f, 0.0f, 0.0f);

    while (vis_frame(frame++))
    {
        float t = frame * 0.045f;

        vis_rotate_y(&ry, t);
        vis_rotate_x(&rx, t * 0.55f);
        vis_mul(&world, &rx, &ry);
        vis_mul(&wv, &world, &view);
        vis_mul(&wvp, &wv, &proj);

        memset(&constants, 0, sizeof(constants));
        d11_store_matrix(constants.wvp, &wvp);
        d11_store_matrix(constants.world, &world);
        constants.light[0] = -0.4f;
        constants.light[1] = 0.7f;
        constants.light[2] = -0.6f;
        d11_update_cb(s.context, cb, &constants, sizeof(constants));

        d11_begin(&s, clear);
        ID3D11DeviceContext_IASetInputLayout(s.context, layout);
        ID3D11DeviceContext_IASetVertexBuffers(s.context, 0, 1, &vb, &stride, &offset);
        ID3D11DeviceContext_IASetIndexBuffer(s.context, ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(s.context,
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s.context, vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s.context, 0, 1, &cb);
        ID3D11DeviceContext_PSSetShader(s.context, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s.context, 0, 1, &cb);
        ID3D11DeviceContext_PSSetShaderResources(s.context, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(s.context, 0, 1, &sampler);

        ID3D11DeviceContext_DrawIndexed(s.context, VIS_MODEL_INDICES, 0, 0);
        IDXGISwapChain_Present(s.swapchain, 0, 0);
    }
    ok_(frame > 1, "rendered %d frames", frame - 1);

    n = d11_sample(&s, sample, ARRAYSIZE(sample));
    if (n) vis_check_rendered(sample, n, 0xff1f1a1a);
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
    d11_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
