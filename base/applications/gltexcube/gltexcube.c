/*
 * PROJECT:     ReactOS Xbox NV2A OpenGL test app
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Exercise the xboxogl ICD with 3D + texturing combined: a
 *              depth-tested rotating cube with a procedural checkerboard texture
 *              mapped onto every face.  Validates that hardware texturing works
 *              under perspective + the depth buffer (not just 2D ortho sprites).
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static HGLRC  g_rc = NULL;
static GLuint g_tex = 0;
static float  g_angle = 0.0f;
static char   g_status[128] = "init";

#define ANIM_TIMER_ID 1
#define TEX_SIZE 64

/* Build a 64x64 RGBA checkerboard (orange/blue) with a red border, so both the
 * texture mapping and the per-face orientation are obvious on screen. */
static void MakeTexture(void)
{
    static unsigned char px[TEX_SIZE * TEX_SIZE * 4];
    int x, y;
    for (y = 0; y < TEX_SIZE; y++)
        for (x = 0; x < TEX_SIZE; x++)
        {
            unsigned char *p = &px[(y * TEX_SIZE + x) * 4];
            int c = ((x >> 4) + (y >> 4)) & 1;   /* 16px squares -> coarse 4x4 grid */
            if (c) { p[0] = 240; p[1] = 200; p[2] = 40; }   /* orange */
            else   { p[0] = 40;  p[1] = 90;  p[2] = 210; }  /* blue   */
            if (x < 2 || y < 2 || x >= TEX_SIZE - 2 || y >= TEX_SIZE - 2)
            { p[0] = 230; p[1] = 30; p[2] = 30; }            /* red border */
            p[3] = 255;
        }

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static BOOL SetupGL(HDC hdc)
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

    pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0) { strcpy(g_status, "ChoosePixelFormat FAILED"); return FALSE; }
    if (!SetPixelFormat(hdc, pf, &pfd)) { strcpy(g_status, "SetPixelFormat FAILED"); return FALSE; }

    g_rc = wglCreateContext(hdc);
    if (!g_rc) { strcpy(g_status, "wglCreateContext FAILED"); return FALSE; }
    if (!wglMakeCurrent(hdc, g_rc)) { strcpy(g_status, "wglMakeCurrent FAILED"); return FALSE; }

    MakeTexture();
    _snprintf(g_status, sizeof(g_status) - 1, "GL textured cube OK - %s",
              (const char *)glGetString(GL_RENDERER));
    g_status[sizeof(g_status) - 1] = '\0';
    return TRUE;
}

/* One textured quad face: 4 corners + their UVs (full texture per face). */
static void Face(const float v0[3], const float v1[3],
                 const float v2[3], const float v3[3])
{
    glTexCoord2f(0.0f, 0.0f); glVertex3f(v0[0], v0[1], v0[2]);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(v1[0], v1[1], v1[2]);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(v2[0], v2[1], v2[2]);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(v3[0], v3[1], v3[2]);
}

static void RenderGL(int w, int h)
{
    static const float v[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
    };
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    float fw = 0.5f * aspect;

    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);   /* opaque cube: drop the 3 back faces so they can't bleed through */
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.10f, 0.10f, 0.15f, 1.0f);
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

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);   /* white tint -> texture shown as-is */
    glBindTexture(GL_TEXTURE_2D, g_tex);

    glBegin(GL_QUADS);
        Face(v[4], v[5], v[6], v[7]);  /* +Z */
        Face(v[1], v[0], v[3], v[2]);  /* -Z */
        Face(v[3], v[7], v[6], v[2]);  /* +Y */
        Face(v[0], v[1], v[5], v[4]);  /* -Y */
        Face(v[1], v[2], v[6], v[5]);  /* +X */
        Face(v[0], v[4], v[7], v[3]);  /* -X */
    glEnd();

    glFinish();
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
            SetTimer(hwnd, ANIM_TIMER_ID, 33, NULL);
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
            if (g_rc) RenderGL(cr.right - cr.left, cr.bottom - cr.top);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 0));
            TextOutA(hdc, 8, 8, g_status, (int)strlen(g_status));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, ANIM_TIMER_ID);
            if (g_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_rc); g_rc = NULL; }
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
    wc.lpszClassName = "GlTexCubeWnd";
    RegisterClassA(&wc);

    hwnd = CreateWindowA("GlTexCubeWnd", "xboxogl - textured 3D cube",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 512, 512,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
