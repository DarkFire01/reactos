/*
 * PROJECT:     ReactCraft - a Minecraft-style voxel sandbox for the xboxogl ICD
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Entry point: window + GL context, the real-time frame loop, and
 *              input dispatch.  The heavy lifting lives in the modules:
 *                noise/blocks/texture/world/mesh/player/render  (engine + world)
 *                items/inventory/game/hud                       (gameplay)
 *
 * Controls: WASD move, arrows look, Space/Shift up-down or jump, F toggle fly,
 *           hold Q / left mouse to MINE (speed depends on the held tool),
 *           E / right mouse PLACE the selected block, 1-9 or mouse wheel pick a
 *           hotbar slot, Esc quit.
 */

#include "mc.h"
#include "world.h"
#include "mesh.h"
#include "player.h"
#include "render.h"
#include "blocks.h"
#include "items.h"
#include "inventory.h"
#include "game.h"
#include "survival.h"

static HWND  g_hwnd = NULL;
static HDC   g_hdc  = NULL;
static HGLRC g_rc   = NULL;
static int   g_running = 1;

static void SetupGL(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(g_hdc, &pfd);
    SetPixelFormat(g_hdc, pf, &pfd);
    g_rc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_rc);
}

static void UpdateTitle(void)
{
    char t[256];
    _snprintf(t, sizeof(t) - 1,
              "ReactCraft - %s | xyz %.0f %.0f %.0f | %s | holding: %s",
              RenderRendererName(), g_player.pos.x, g_player.pos.y, g_player.pos.z,
              g_player.fly ? "FLY" : "WALK", ItemName(InventorySelectedItem()));
    t[sizeof(t) - 1] = '\0';
    SetWindowTextA(g_hwnd, t);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_SIZE:
            RenderResize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { g_running = 0; PostQuitMessage(0); }
            else if (wp == 'F')  { g_player.fly = !g_player.fly; g_player.vel.y = 0; }
            else if (wp == 'E')  GameUse();
            else if (wp >= '1' && wp <= '9') InventorySelect((int)(wp - '1'));
            return 0;
        case WM_RBUTTONDOWN: GameUse(); return 0;
        case WM_MOUSEWHEEL:
            InventoryScroll(((short)HIWORD(wp) > 0) ? -1 : 1);
            return 0;
        case WM_CLOSE: g_running = 0; PostQuitMessage(0); return 0;
        case WM_DESTROY:
            if (g_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_rc); g_rc = NULL; }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSA wc;
    MSG msg;
    DWORD prevTick, titleTick;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(lpCmd);

    GenerateWorld();
    PlayerSpawn();
    GameInit();
    SurvivalInit();

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ReactCraftWnd";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("ReactCraftWnd", "ReactCraft",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                           NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    g_hdc = GetDC(g_hwnd);
    SetupGL();
    RenderResize(800, 600);
    RenderInit(g_hdc);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    prevTick = titleTick = GetTickCount();
    while (g_running)
    {
        double dt;
        DWORD now;

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;

        now = GetTickCount();
        dt = (double)(now - prevTick) / 1000.0;
        prevTick = now;
        if (dt > 0.1) dt = 0.1;

        PlayerUpdate(dt);
        GameUpdate(dt);
        SurvivalUpdate(dt);
        RenderFrame();
        if (now - titleTick > 250) { UpdateTitle(); titleTick = now; }
    }

    if (g_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_rc); }
    if (g_hdc) ReleaseDC(g_hwnd, g_hdc);
    return 0;
}
