/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: colour-filled sprites bouncing off the edges
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
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

/* Read back a run along the horizontal centre line instead of the usual
   scatter. The scatter pattern steps 7 pixels across and 13 down per sample,
   which walks straight past sprites this small nearly every time; sprite 0 is
   pinned to the vertical centre of the frame and only moves sideways, so a
   dense sweep of that one line always crosses it. Two pixels per sample is
   what fits the whole width into the caller's 256-entry buffer. */
static D3DTEST_UNUSED int dd_sample(struct dd_screen *s, DWORD *out, int max)
{
    DWORD *pixels;
    int pitch, i, n = 0;

    if (!dd_lock(s, &pixels, &pitch))
        return 0;
    for (i = 0; i < max; i++)
    {
        int x = (i * 2) % VIS_WIDTH;
        out[n++] = pixels[(VIS_HEIGHT / 2) * pitch + x];
    }
    dd_unlock(s);
    return n;
}

#define BOUNCE_BACKGROUND 0x00101830

struct sprite
{
    int x, y, w, h, vx, vy;
    DWORD colour;
};

int main(int argc, char **argv)
{
    /* Sprite 0 never leaves the vertical centre of the frame: the readback at
       the end sweeps that one line, and something has to be guaranteed to sit
       on it. The rest bounce in both axes. */
    static struct sprite sprites[5] =
    {
        {  20, VIS_HEIGHT / 2 - 32, 64, 64,  7,  0, 0x00ffd040 },
        {  60,  40, 48, 48,  5,  4, 0x00e04060 },
        { 300,  80, 40, 72, -6,  3, 0x0040c0ff },
        { 180, 250, 56, 40,  4, -5, 0x0060ff80 },
        { 420, 300, 36, 36, -3, -6, 0x00c080ff },
    };
    DWORD sample[256];
    struct dd_screen s;
    DDBLTFX fx;
    int frame = 0, i, n;
    int filled = 1;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_bounce");

    hwnd = vis_create_window("DirectDraw: bouncing sprites");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        dd_close(&s);
        test_destroy_window(hwnd);
        return test_end();
    }
    ok_(1, "opened a %dx%d windowed DirectDraw screen", VIS_WIDTH, VIS_HEIGHT);

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);

    while (vis_frame(frame++))
    {
        fx.dwFillColor = BOUNCE_BACKGROUND;
        hr = IDirectDrawSurface7_Blt(s.back, NULL, NULL, NULL,
                                     DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (frame == 1 && FAILED(hr))
        {
            skip_("Blt(DDBLT_COLORFILL) returned 0x%08lx", hr);
            filled = 0;
            break;
        }

        for (i = 0; i < (int)ARRAYSIZE(sprites); i++)
        {
            RECT dst;

            sprites[i].x += sprites[i].vx;
            sprites[i].y += sprites[i].vy;

            if (sprites[i].x < 0)
            {
                sprites[i].x = 0;
                sprites[i].vx = -sprites[i].vx;
            }
            else if (sprites[i].x + sprites[i].w > VIS_WIDTH)
            {
                sprites[i].x = VIS_WIDTH - sprites[i].w;
                sprites[i].vx = -sprites[i].vx;
            }

            if (sprites[i].y < 0)
            {
                sprites[i].y = 0;
                sprites[i].vy = -sprites[i].vy;
            }
            else if (sprites[i].y + sprites[i].h > VIS_HEIGHT)
            {
                sprites[i].y = VIS_HEIGHT - sprites[i].h;
                sprites[i].vy = -sprites[i].vy;
            }

            dst.left = sprites[i].x;
            dst.top = sprites[i].y;
            dst.right = sprites[i].x + sprites[i].w;
            dst.bottom = sprites[i].y + sprites[i].h;

            fx.dwFillColor = sprites[i].colour;
            hr = IDirectDrawSurface7_Blt(s.back, &dst, NULL, NULL,
                                         DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            if (frame == 1 && i == 0)
                ok_(SUCCEEDED(hr), "colour-filling a sprite returned 0x%08lx", hr);
        }
        dd_present(&s);
    }
    if (!filled)
    {
        skip_("this driver cannot colour fill, so there is nothing to verify");
    }
    else
    {
        ok_(frame > 1, "rendered %d frames of bouncing sprites", frame - 1);

        n = dd_sample(&s, sample, ARRAYSIZE(sample));
        if (n)
            vis_check_rendered(sample, n, BOUNCE_BACKGROUND);
    }

    vis_wait_if_held();
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
