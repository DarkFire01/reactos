/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: GDI shapes and text drawn onto a DirectDraw surface
 */


#include "d3dvis.h"
#include <ddraw.h>

/* A windowed DirectDraw presenter: primary + clipper, plus a system-memory
   back surface the test draws into with plain pixel writes. */
struct dd_screen
{
    IDirectDraw7 *ddraw;
    IDirectDrawSurface7 *primary;
    IDirectDrawSurface7 *back;
    IDirectDrawClipper *clipper;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL dd_open(struct dd_screen *s, HWND hwnd)
{
    DDSURFACEDESC2 desc;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (FAILED(DirectDrawCreateEx(NULL, (void **)&s->ddraw, &IID_IDirectDraw7, NULL)))
        return FALSE;
    if (FAILED(IDirectDraw7_SetCooperativeLevel(s->ddraw, hwnd, DDSCL_NORMAL)))
        return FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->primary, NULL)))
        return FALSE;

    if (SUCCEEDED(IDirectDraw7_CreateClipper(s->ddraw, 0, &s->clipper, NULL)))
    {
        IDirectDrawClipper_SetHWnd(s->clipper, 0, hwnd);
        IDirectDrawSurface7_SetClipper(s->primary, s->clipper);
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = VIS_WIDTH;
    desc.dwHeight = VIS_HEIGHT;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->back, NULL)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void dd_close(struct dd_screen *s)
{
    if (s->back) IDirectDrawSurface7_Release(s->back);
    if (s->clipper) IDirectDrawClipper_Release(s->clipper);
    if (s->primary) IDirectDrawSurface7_Release(s->primary);
    if (s->ddraw) IDirectDraw7_Release(s->ddraw);
    memset(s, 0, sizeof(*s));
}

/* Hand the caller a pointer to the back surface pixels. */
static D3DTEST_UNUSED BOOL dd_lock(struct dd_screen *s, DWORD **pixels, int *pitch_dwords)
{
    DDSURFACEDESC2 desc;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(s->back, NULL, &desc, DDLOCK_WAIT, NULL)))
        return FALSE;
    *pixels = (DWORD *)desc.lpSurface;
    *pitch_dwords = desc.lPitch / 4;
    return TRUE;
}

static D3DTEST_UNUSED void dd_unlock(struct dd_screen *s)
{
    IDirectDrawSurface7_Unlock(s->back, NULL);
}

/* Blit the back surface to wherever the window currently sits. */
static D3DTEST_UNUSED void dd_present(struct dd_screen *s)
{
    RECT dst;
    POINT tl = { 0, 0 };

    GetClientRect(s->hwnd, &dst);
    ClientToScreen(s->hwnd, &tl);
    OffsetRect(&dst, tl.x, tl.y);
    IDirectDrawSurface7_Blt(s->primary, &dst, s->back, NULL, DDBLT_WAIT, NULL);
}

/* Copy a strip out of the back surface for the end-of-run verification. */
static D3DTEST_UNUSED int dd_sample(struct dd_screen *s, DWORD *out, int max)
{
    DWORD *pixels;
    int pitch, i, n = 0;

    if (!dd_lock(s, &pixels, &pitch))
        return 0;
    for (i = 0; i < max && n < max; i++)
    {
        int x = (i * 7) % VIS_WIDTH;
        int y = (i * 13) % VIS_HEIGHT;
        out[n++] = pixels[y * pitch + x];
    }
    dd_unlock(s);
    return n;
}

int main(int argc, char **argv)
{
    DWORD sample[256];
    struct dd_screen s;
    DDBLTFX fx;
    HDC dc = NULL;
    int frame = 0;
    HWND hwnd;
    HRESULT hr;
    int n;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_gdi");

    hwnd = vis_create_window("DirectDraw: GDI on a surface");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        goto done;
    }

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);

    while (vis_frame(frame++))
    {
        fx.dwFillColor = 0xff102030;
        IDirectDrawSurface7_Blt(s.back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);

        hr = IDirectDrawSurface7_GetDC(s.back, &dc);
        if (FAILED(hr))
        {
            skip_("GetDC on the back surface returned 0x%08lx", hr);
            break;
        }

        {
            float t = frame * 0.1f;
            HBRUSH brush = CreateSolidBrush(RGB(0x40, 0xa0, 0xff));
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(0xff, 0xd0, 0x40));
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, pen);
            int cx = VIS_WIDTH / 2, cy = VIS_HEIGHT / 2;
            int r = 90 + (int)(40 * sin(t));
            int i;

            Ellipse(dc, cx - r, cy - r, cx + r, cy + r);

            for (i = 0; i < 8; i++)
            {
                double a = t + i * 0.785;
                MoveToEx(dc, cx, cy, NULL);
                LineTo(dc, cx + (int)(170 * cos(a)), cy + (int)(130 * sin(a)));
            }

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0xff, 0xff, 0xff));
            TextOutA(dc, 12, 10, "GDI on a DirectDraw surface", 27);

            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            DeleteObject(brush);
        }

        IDirectDrawSurface7_ReleaseDC(s.back, dc);
        dc = NULL;
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d frames of GDI drawing", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, 0xff102030);

    vis_wait_if_held();
done:
    if (dc) IDirectDrawSurface7_ReleaseDC(s.back, dc);
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
