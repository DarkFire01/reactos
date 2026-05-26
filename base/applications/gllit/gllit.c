/*
 * PROJECT:     ReactOS Xbox NV2A OpenGL test app
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Sanity test for multi-light lighting + fog on the xboxogl ICD.
 *              A white rotating cube lit by TWO coloured directional lights
 *              (red from the right, green from the left) inside linear fog —
 *              so each face takes a blend of both light colours and the far
 *              side fades toward the fog colour.
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static HGLRC g_rc = NULL;
static float g_angle = 0.0f;
static char  g_status[128] = "init";
#define TIMER_ID 1

static void InitGLState(void)
{
    GLfloat l0pos[4] = { 1.0f, 0.3f, 0.6f, 0.0f };   /* directional, from right  */
    GLfloat l0dif[4] = { 1.0f, 0.2f, 0.2f, 1.0f };   /* red   */
    GLfloat l1pos[4] = { -1.0f, 0.3f, 0.6f, 0.0f };  /* directional, from left   */
    GLfloat l1dif[4] = { 0.2f, 1.0f, 0.3f, 1.0f };   /* green */
    GLfloat white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    GLfloat famb[4]  = { 0.15f, 0.15f, 0.15f, 1.0f };
    GLfloat fogcol[4]= { 0.0f, 0.0f, 0.12f, 1.0f };

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);

    glEnable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, famb);
    glLightfv(GL_LIGHT0, GL_POSITION, l0pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  l0dif);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT1, GL_POSITION, l1pos);
    glLightfv(GL_LIGHT1, GL_DIFFUSE,  l1dif);
    glEnable(GL_LIGHT1);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, white);

    /* Linear fog over the cube's depth range so the back fades to the bg colour. */
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogfv(GL_FOG_COLOR, fogcol);
    glFogf(GL_FOG_START, 3.0f);
    glFogf(GL_FOG_END, 8.0f);
}

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
    InitGLState();
    _snprintf(g_status, sizeof(g_status)-1, "lighting+fog OK - %s", (const char*)glGetString(GL_RENDERER));
    g_status[sizeof(g_status)-1] = '\0';
    return TRUE;
}

static void Face(float nx, float ny, float nz,
                 const float a[3], const float b[3], const float c[3], const float d[3])
{
    glNormal3f(nx, ny, nz);
    glVertex3f(a[0],a[1],a[2]); glVertex3f(b[0],b[1],b[2]);
    glVertex3f(c[0],c[1],c[2]); glVertex3f(d[0],d[1],d[2]);
}

static void RenderGL(int w, int h)
{
    static const float v[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    float aspect = (h > 0) ? (float)w/(float)h : 1.0f, fw = 0.5f*aspect;

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.12f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-fw, fw, -0.5, 0.5, 1.0, 20.0);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(g_angle, 1.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
        Face( 0, 0, 1, v[4],v[5],v[6],v[7]);
        Face( 0, 0,-1, v[1],v[0],v[3],v[2]);
        Face( 0, 1, 0, v[3],v[7],v[6],v[2]);
        Face( 0,-1, 0, v[0],v[1],v[5],v[4]);
        Face( 1, 0, 0, v[1],v[2],v[6],v[5]);
        Face(-1, 0, 0, v[0],v[4],v[7],v[3]);
    glEnd();
    glFinish();
}

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE: { HDC hdc = GetDC(hwnd); SetupGL(hdc); SetTimer(hwnd, TIMER_ID, 33, NULL); return 0; }
        case WM_TIMER:
            if (wp == TIMER_ID) { g_angle += 1.5f; if (g_angle >= 360.0f) g_angle -= 360.0f; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT cr;
            GetClientRect(hwnd, &cr);
            if (g_rc) RenderGL(cr.right-cr.left, cr.bottom-cr.top);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255,255,0));
            TextOutA(hdc, 8, 8, g_status, (int)strlen(g_status));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
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
    wc.lpszClassName = "GlLitWnd";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("GlLitWnd", "xboxogl - multi-light + fog",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         512, 512, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow); UpdateWindow(hwnd);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}
