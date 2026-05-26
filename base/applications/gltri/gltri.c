/*
 * PROJECT:     ReactOS Xbox NV2A OpenGL test app
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Exercise the xboxogl ICD: create a WGL context, print the GL
 *              capability strings, and render a depth-tested rotating cube on
 *              the NV2A 3D (Kelvin) engine.
 *
 * This is a deliberately tiny WGL client.  If the xboxogl ICD is registered
 * and loadable, opengl32 routes the wgl/gl calls here.  A spinning, solid-
 * coloured cube exercises the hardware depth buffer (the back faces are hidden
 * by the front ones), and the GL_VENDOR / GL_RENDERER / GL_VERSION strings are
 * drawn via GDI so the result is visible on screen (no console/serial needed).
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static HGLRC g_rc = NULL;
static char  g_caps[1024];
static char  g_status[128] = "init";
static float g_angle = 0.0f;

#define ANIM_TIMER_ID  1

static void
GatherCaps(void)
{
    const char *vendor   = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version  = (const char *)glGetString(GL_VERSION);
    const char *exts     = (const char *)glGetString(GL_EXTENSIONS);

    _snprintf(g_caps, sizeof(g_caps) - 1,
              "GL_VENDOR  : %s\n"
              "GL_RENDERER: %s\n"
              "GL_VERSION : %s\n"
              "GL_EXTENSIONS: %s",
              vendor   ? vendor   : "(null)",
              renderer ? renderer : "(null)",
              version  ? version  : "(null)",
              (exts && *exts) ? exts : "(none)");
    g_caps[sizeof(g_caps) - 1] = '\0';
}

static BOOL
SetupGL(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0)
    {
        strcpy(g_status, "ChoosePixelFormat FAILED");
        return FALSE;
    }
    if (!SetPixelFormat(hdc, pf, &pfd))
    {
        strcpy(g_status, "SetPixelFormat FAILED");
        return FALSE;
    }

    g_rc = wglCreateContext(hdc);
    if (!g_rc)
    {
        strcpy(g_status, "wglCreateContext FAILED");
        return FALSE;
    }
    if (!wglMakeCurrent(hdc, g_rc))
    {
        strcpy(g_status, "wglMakeCurrent FAILED");
        return FALSE;
    }

    _snprintf(g_status, sizeof(g_status) - 1, "GL cube OK (pf=%d)", pf);
    GatherCaps();
    return TRUE;
}

/* One solid-coloured quad face. */
static void
Face(float r, float g, float b,
     const float a[3], const float bb[3], const float c[3], const float d[3])
{
    glColor3f(r, g, b);
    glVertex3f(a[0],  a[1],  a[2]);
    glVertex3f(bb[0], bb[1], bb[2]);
    glVertex3f(c[0],  c[1],  c[2]);
    glVertex3f(d[0],  d[1],  d[2]);
}

static void
RenderGL(int w, int h)
{
    /* 8 cube corners (unit cube centred on the origin). */
    static const float v[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
    };
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    float fw = 0.5f * aspect;   /* frustum half-width at the near plane */

    glViewport(0, 0, w, h);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.10f, 0.10f, 0.30f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-fw, fw, -0.5, 0.5, 1.0, 20.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(g_angle,        1.0f, 0.0f, 0.0f);
    glRotatef(g_angle * 0.7f, 0.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
        /* +Z front (red), -Z back (green) */
        Face(1.0f, 0.2f, 0.2f, v[4], v[5], v[6], v[7]);
        Face(0.2f, 1.0f, 0.2f, v[1], v[0], v[3], v[2]);
        /* +Y top (blue), -Y bottom (yellow) */
        Face(0.2f, 0.4f, 1.0f, v[3], v[7], v[6], v[2]);
        Face(1.0f, 1.0f, 0.2f, v[0], v[1], v[5], v[4]);
        /* +X right (magenta), -X left (cyan) */
        Face(1.0f, 0.2f, 1.0f, v[1], v[2], v[6], v[5]);
        Face(0.2f, 1.0f, 1.0f, v[0], v[4], v[7], v[3]);
    glEnd();

    glFinish();
}

static void
DrawOverlay(HDC hdc)
{
    RECT rc;
    char buf[1200];

    _snprintf(buf, sizeof(buf) - 1, "%s\n\n%s", g_status, g_caps);
    buf[sizeof(buf) - 1] = '\0';

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 0));
    rc.left = 8; rc.top = 8; rc.right = 632; rc.bottom = 200;
    DrawTextA(hdc, buf, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
}

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HDC hdc = GetDC(hwnd);
            SetupGL(hdc);
            /* keep the DC; the ICD bound its context to it */
            SetTimer(hwnd, ANIM_TIMER_ID, 33, NULL);   /* ~30 fps spin */
            return 0;
        }
        case WM_TIMER:
            if (wp == ANIM_TIMER_ID)
            {
                g_angle += 2.0f;
                if (g_angle >= 360.0f) g_angle -= 360.0f;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);
            if (g_rc)
                RenderGL(cr.right - cr.left, cr.bottom - cr.top);
            DrawOverlay(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, ANIM_TIMER_ID);
            if (g_rc)
            {
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(g_rc);
                g_rc = NULL;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(lpCmd);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GlTriWnd";
    RegisterClassA(&wc);

    hwnd = CreateWindowA("GlTriWnd", "xboxogl - GL 1.1 depth-tested cube",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                         NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
