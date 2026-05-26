/*
 * PROJECT:     ReactOS Xbox NV2A OpenGL test app
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Sanity test for GL primitive topologies on the NV2A: points,
 *              lines, line strip, line loop, and triangles — all rasterised by
 *              the GPU (no software path).
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static HGLRC g_rc = NULL;
static char  g_status[128] = "init";

static BOOL SetupGL(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) { strcpy(g_status, "pixelformat FAILED"); return FALSE; }
    g_rc = wglCreateContext(hdc);
    if (!g_rc || !wglMakeCurrent(hdc, g_rc)) { strcpy(g_status, "context FAILED"); return FALSE; }
    _snprintf(g_status, sizeof(g_status)-1, "primitives OK - %s", (const char*)glGetString(GL_RENDERER));
    g_status[sizeof(g_status)-1] = '\0';
    return TRUE;
}

static void RenderGL(int w, int h)
{
    int i;
    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, w, h, 0.0, -1.0, 1.0);   /* top-left origin */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* GL_POINTS — a row of fat points (glPointSize) in cyan */
    glPointSize(7.0f);
    glBegin(GL_POINTS);
        glColor3f(0.2f, 1.0f, 1.0f);
        for (i = 0; i < 20; i++)
            glVertex2f(40.0f + i * 24.0f, 40.0f);
    glEnd();

    /* glRect — a filled orange rectangle (top-right) */
    glColor3f(1.0f, 0.6f, 0.1f);
    glRectf(540.0f, 70.0f, 610.0f, 200.0f);

    /* GL_LINES — independent yellow segments forming a fan */
    glBegin(GL_LINES);
        glColor3f(1.0f, 1.0f, 0.2f);
        for (i = 0; i < 8; i++)
        {
            glVertex2f(60.0f, 90.0f);
            glVertex2f(60.0f + i * 40.0f, 200.0f);
        }
    glEnd();

    /* GL_LINE_STRIP — a green zigzag */
    glBegin(GL_LINE_STRIP);
        glColor3f(0.2f, 1.0f, 0.2f);
        for (i = 0; i < 12; i++)
            glVertex2f(40.0f + i * 30.0f, 250.0f + ((i & 1) ? 40.0f : 0.0f));
    glEnd();

    /* GL_LINE_LOOP — a magenta square outline (closes automatically) */
    glBegin(GL_LINE_LOOP);
        glColor3f(1.0f, 0.2f, 1.0f);
        glVertex2f(60.0f,  360.0f);
        glVertex2f(260.0f, 360.0f);
        glVertex2f(260.0f, 520.0f);
        glVertex2f(60.0f,  520.0f);
    glEnd();

    /* GL_TRIANGLES — a filled gradient triangle (confirms tris still work) */
    glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(360.0f, 360.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(560.0f, 360.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(460.0f, 540.0f);
    glEnd();

    glFinish();
}

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE: { HDC hdc = GetDC(hwnd); SetupGL(hdc); return 0; }
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT cr;
            GetClientRect(hwnd, &cr);
            if (g_rc) RenderGL(cr.right - cr.left, cr.bottom - cr.top);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255,255,0));
            TextOutA(hdc, 8, 8, g_status, (int)strlen(g_status));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (g_rc) { wglMakeCurrent(NULL,NULL); wglDeleteContext(g_rc); g_rc=NULL; }
            PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSA wc; HWND hwnd; MSG msg;
    UNREFERENCED_PARAMETER(hPrev); UNREFERENCED_PARAMETER(lpCmd);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GlPrimsWnd";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("GlPrimsWnd", "xboxogl - points / lines / triangles",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         640, 600, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow); UpdateWindow(hwnd);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}
