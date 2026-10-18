/*
 * PROJECT:     ReactOS OpenGL Cube Demo
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A lit spinning cube drawn with OpenGL 2.0 shaders and vertex buffers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include "resource.h"

#define CUBE_PI             3.14159265358979f
#define CUBE_VERTEX_COUNT   36

typedef struct _CUBE_VERTEX
{
    GLfloat Position[3];
    GLfloat Normal[3];
    GLfloat Color[3];
} CUBE_VERTEX, *PCUBE_VERTEX;

/* The OpenGL 2.0 entry points, which opengl32 only hands out through wglGetProcAddress */
static PFNGLCREATESHADERPROC pglCreateShader;
static PFNGLSHADERSOURCEPROC pglShaderSource;
static PFNGLCOMPILESHADERPROC pglCompileShader;
static PFNGLGETSHADERIVPROC pglGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC pglGetShaderInfoLog;
static PFNGLDELETESHADERPROC pglDeleteShader;
static PFNGLCREATEPROGRAMPROC pglCreateProgram;
static PFNGLATTACHSHADERPROC pglAttachShader;
static PFNGLBINDATTRIBLOCATIONPROC pglBindAttribLocation;
static PFNGLLINKPROGRAMPROC pglLinkProgram;
static PFNGLGETPROGRAMIVPROC pglGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC pglGetProgramInfoLog;
static PFNGLDELETEPROGRAMPROC pglDeleteProgram;
static PFNGLUSEPROGRAMPROC pglUseProgram;
static PFNGLGETUNIFORMLOCATIONPROC pglGetUniformLocation;
static PFNGLUNIFORMMATRIX4FVPROC pglUniformMatrix4fv;
static PFNGLUNIFORM3FPROC pglUniform3f;
static PFNGLGENBUFFERSPROC pglGenBuffers;
static PFNGLBINDBUFFERPROC pglBindBuffer;
static PFNGLBUFFERDATAPROC pglBufferData;
static PFNGLDELETEBUFFERSPROC pglDeleteBuffers;
static PFNGLVERTEXATTRIBPOINTERPROC pglVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC pglEnableVertexAttribArray;

#define CUBE_ATTRIB_POSITION    0
#define CUBE_ATTRIB_NORMAL      1
#define CUBE_ATTRIB_COLOR       2

static const char g_szVertexShader[] =
    "#version 110\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec3 aColor;\n"
    "uniform mat4 uModelView;\n"
    "uniform mat4 uProjection;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "varying vec3 vPosition;\n"
    "void main()\n"
    "{\n"
    "    vec4 Eye = uModelView * vec4(aPosition, 1.0);\n"
    "    vNormal = (uModelView * vec4(aNormal, 0.0)).xyz;\n"
    "    vColor = aColor;\n"
    "    vPosition = Eye.xyz;\n"
    "    gl_Position = uProjection * Eye;\n"
    "}\n";

static const char g_szFragmentShader[] =
    "#version 110\n"
    "uniform vec3 uLightDir;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "varying vec3 vPosition;\n"
    "void main()\n"
    "{\n"
    "    vec3 N = normalize(vNormal);\n"
    "    vec3 L = normalize(uLightDir);\n"
    "    vec3 V = normalize(-vPosition);\n"
    "    vec3 H = normalize(L + V);\n"
    "    float Diffuse = max(dot(N, L), 0.0);\n"
    "    float Specular = pow(max(dot(N, H), 0.0), 32.0);\n"
    "    vec3 Lit = vColor * (0.25 + 0.75 * Diffuse) + vec3(0.4) * Specular;\n"
    "    gl_FragColor = vec4(Lit, 1.0);\n"
    "}\n";

static HINSTANCE g_hInstance;
static HDC g_hdc;
static HGLRC g_hglrc;
static GLuint g_Program;
static GLuint g_VertexBuffer;
static GLint g_ModelViewLocation;
static GLint g_ProjectionLocation;
static GLint g_LightDirLocation;
static GLint g_cxClient = 1;
static GLint g_cyClient = 1;

static
VOID
ShowError(
    _In_opt_ HWND hwnd,
    _In_ UINT uId,
    ...)
{
    WCHAR szTitle[64];
    WCHAR szFormat[256];
    WCHAR szMessage[1024];
    va_list Args;

    LoadStringW(g_hInstance, IDS_APPTITLE, szTitle, ARRAYSIZE(szTitle));
    LoadStringW(g_hInstance, uId, szFormat, ARRAYSIZE(szFormat));

    va_start(Args, uId);
    _vsnwprintf(szMessage, ARRAYSIZE(szMessage) - 1, szFormat, Args);
    va_end(Args);
    szMessage[ARRAYSIZE(szMessage) - 1] = UNICODE_NULL;

    MessageBoxW(hwnd, szMessage, szTitle, MB_OK | MB_ICONERROR);
}

static
BOOL
LoadGlFunctions(VOID)
{
#define LOAD_GL_FUNCTION(Type, Name) \
    if (!(p##Name = (Type)wglGetProcAddress(#Name))) return FALSE

    LOAD_GL_FUNCTION(PFNGLCREATESHADERPROC, glCreateShader);
    LOAD_GL_FUNCTION(PFNGLSHADERSOURCEPROC, glShaderSource);
    LOAD_GL_FUNCTION(PFNGLCOMPILESHADERPROC, glCompileShader);
    LOAD_GL_FUNCTION(PFNGLGETSHADERIVPROC, glGetShaderiv);
    LOAD_GL_FUNCTION(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
    LOAD_GL_FUNCTION(PFNGLDELETESHADERPROC, glDeleteShader);
    LOAD_GL_FUNCTION(PFNGLCREATEPROGRAMPROC, glCreateProgram);
    LOAD_GL_FUNCTION(PFNGLATTACHSHADERPROC, glAttachShader);
    LOAD_GL_FUNCTION(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
    LOAD_GL_FUNCTION(PFNGLLINKPROGRAMPROC, glLinkProgram);
    LOAD_GL_FUNCTION(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
    LOAD_GL_FUNCTION(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
    LOAD_GL_FUNCTION(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
    LOAD_GL_FUNCTION(PFNGLUSEPROGRAMPROC, glUseProgram);
    LOAD_GL_FUNCTION(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    LOAD_GL_FUNCTION(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv);
    LOAD_GL_FUNCTION(PFNGLUNIFORM3FPROC, glUniform3f);
    LOAD_GL_FUNCTION(PFNGLGENBUFFERSPROC, glGenBuffers);
    LOAD_GL_FUNCTION(PFNGLBINDBUFFERPROC, glBindBuffer);
    LOAD_GL_FUNCTION(PFNGLBUFFERDATAPROC, glBufferData);
    LOAD_GL_FUNCTION(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
    LOAD_GL_FUNCTION(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
    LOAD_GL_FUNCTION(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);

#undef LOAD_GL_FUNCTION
    return TRUE;
}

/**
 * @brief Compile one shader stage.
 *
 * @param Type GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
 * @param pszSource The GLSL source.
 * @param pszLog Receives the compiler log on failure.
 * @param cchLog Size of pszLog.
 *
 * @return The shader, 0 on failure.
 */
static
GLuint
CompileShader(
    _In_ GLenum Type,
    _In_z_ const char *pszSource,
    _Out_writes_z_(cchLog) char *pszLog,
    _In_ GLsizei cchLog)
{
    GLuint Shader;
    GLint Compiled = GL_FALSE;

    Shader = pglCreateShader(Type);
    if (Shader == 0)
        return 0;

    pglShaderSource(Shader, 1, &pszSource, NULL);
    pglCompileShader(Shader);
    pglGetShaderiv(Shader, GL_COMPILE_STATUS, &Compiled);
    if (Compiled)
        return Shader;

    pglGetShaderInfoLog(Shader, cchLog, NULL, pszLog);
    pglDeleteShader(Shader);
    return 0;
}

static
BOOL
CreateCubeProgram(
    _Out_writes_z_(cchLog) char *pszLog,
    _In_ GLsizei cchLog)
{
    GLuint VertexShader, FragmentShader;
    GLint Linked = GL_FALSE;

    pszLog[0] = ANSI_NULL;

    VertexShader = CompileShader(GL_VERTEX_SHADER, g_szVertexShader, pszLog, cchLog);
    if (VertexShader == 0)
        return FALSE;

    FragmentShader = CompileShader(GL_FRAGMENT_SHADER, g_szFragmentShader, pszLog, cchLog);
    if (FragmentShader == 0)
    {
        pglDeleteShader(VertexShader);
        return FALSE;
    }

    g_Program = pglCreateProgram();
    pglAttachShader(g_Program, VertexShader);
    pglAttachShader(g_Program, FragmentShader);
    pglBindAttribLocation(g_Program, CUBE_ATTRIB_POSITION, "aPosition");
    pglBindAttribLocation(g_Program, CUBE_ATTRIB_NORMAL, "aNormal");
    pglBindAttribLocation(g_Program, CUBE_ATTRIB_COLOR, "aColor");
    pglLinkProgram(g_Program);

    /* The program keeps the stages alive for as long as it needs them */
    pglDeleteShader(VertexShader);
    pglDeleteShader(FragmentShader);

    pglGetProgramiv(g_Program, GL_LINK_STATUS, &Linked);
    if (!Linked)
    {
        pglGetProgramInfoLog(g_Program, cchLog, NULL, pszLog);
        pglDeleteProgram(g_Program);
        g_Program = 0;
        return FALSE;
    }

    g_ModelViewLocation = pglGetUniformLocation(g_Program, "uModelView");
    g_ProjectionLocation = pglGetUniformLocation(g_Program, "uProjection");
    g_LightDirLocation = pglGetUniformLocation(g_Program, "uLightDir");
    return TRUE;
}

/* Six faces, two triangles each, every face its own normal and color */
static
VOID
CreateCubeBuffer(VOID)
{
    static const GLfloat FaceNormals[6][3] =
    {
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f },
        { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }
    };
    static const GLfloat FaceColors[6][3] =
    {
        { 0.90f, 0.20f, 0.20f }, { 0.20f, 0.75f, 0.30f }, { 0.20f, 0.40f, 0.90f },
        { 0.95f, 0.80f, 0.20f }, { 0.75f, 0.30f, 0.85f }, { 0.20f, 0.80f, 0.85f }
    };
    /* Two in-plane axes per face, ordered so the triangles wind counter clockwise from outside */
    static const GLfloat FaceAxes[6][2][3] =
    {
        { { 1, 0, 0 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0, -1 }, { 0, 1, 0 } },
        { { 0, 0, 1 }, { 0, 1, 0 } }, { { 1, 0, 0 }, { 0, 0, -1 } }, { { 1, 0, 0 }, { 0, 0, 1 } }
    };
    static const GLfloat Corners[6][2] =
    {
        { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }
    };
    CUBE_VERTEX Vertices[CUBE_VERTEX_COUNT];
    ULONG Face, Corner, Axis;
    PCUBE_VERTEX pVertex = Vertices;

    for (Face = 0; Face < 6; Face++)
    {
        for (Corner = 0; Corner < 6; Corner++, pVertex++)
        {
            for (Axis = 0; Axis < 3; Axis++)
            {
                pVertex->Position[Axis] = FaceNormals[Face][Axis] +
                                          Corners[Corner][0] * FaceAxes[Face][0][Axis] +
                                          Corners[Corner][1] * FaceAxes[Face][1][Axis];
                pVertex->Normal[Axis] = FaceNormals[Face][Axis];
                pVertex->Color[Axis] = FaceColors[Face][Axis];
            }
        }
    }

    pglGenBuffers(1, &g_VertexBuffer);
    pglBindBuffer(GL_ARRAY_BUFFER, g_VertexBuffer);
    pglBufferData(GL_ARRAY_BUFFER, sizeof(Vertices), Vertices, GL_STATIC_DRAW);

    pglVertexAttribPointer(CUBE_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(CUBE_VERTEX),
                           (const GLvoid *)(ULONG_PTR)FIELD_OFFSET(CUBE_VERTEX, Position));
    pglVertexAttribPointer(CUBE_ATTRIB_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(CUBE_VERTEX),
                           (const GLvoid *)(ULONG_PTR)FIELD_OFFSET(CUBE_VERTEX, Normal));
    pglVertexAttribPointer(CUBE_ATTRIB_COLOR, 3, GL_FLOAT, GL_FALSE, sizeof(CUBE_VERTEX),
                           (const GLvoid *)(ULONG_PTR)FIELD_OFFSET(CUBE_VERTEX, Color));
    pglEnableVertexAttribArray(CUBE_ATTRIB_POSITION);
    pglEnableVertexAttribArray(CUBE_ATTRIB_NORMAL);
    pglEnableVertexAttribArray(CUBE_ATTRIB_COLOR);
}

/* Column major, as glUniformMatrix4fv takes it untransposed */
static
VOID
MatrixMultiply(
    _Out_writes_(16) GLfloat *pResult,
    _In_reads_(16) const GLfloat *pLeft,
    _In_reads_(16) const GLfloat *pRight)
{
    GLfloat Product[16];
    ULONG Row, Column, i;

    for (Column = 0; Column < 4; Column++)
    {
        for (Row = 0; Row < 4; Row++)
        {
            Product[Column * 4 + Row] = 0.0f;
            for (i = 0; i < 4; i++)
                Product[Column * 4 + Row] += pLeft[i * 4 + Row] * pRight[Column * 4 + i];
        }
    }

    memcpy(pResult, Product, sizeof(Product));
}

static
VOID
MatrixRotation(
    _Out_writes_(16) GLfloat *pMatrix,
    _In_ GLfloat Angle,
    _In_ GLfloat x,
    _In_ GLfloat y,
    _In_ GLfloat z)
{
    GLfloat c = cosf(Angle);
    GLfloat s = sinf(Angle);
    GLfloat t = 1.0f - c;
    GLfloat Length = sqrtf(x * x + y * y + z * z);

    x /= Length;
    y /= Length;
    z /= Length;

    pMatrix[0] = t * x * x + c;
    pMatrix[1] = t * x * y + s * z;
    pMatrix[2] = t * x * z - s * y;
    pMatrix[3] = 0.0f;
    pMatrix[4] = t * x * y - s * z;
    pMatrix[5] = t * y * y + c;
    pMatrix[6] = t * y * z + s * x;
    pMatrix[7] = 0.0f;
    pMatrix[8] = t * x * z + s * y;
    pMatrix[9] = t * y * z - s * x;
    pMatrix[10] = t * z * z + c;
    pMatrix[11] = 0.0f;
    pMatrix[12] = 0.0f;
    pMatrix[13] = 0.0f;
    pMatrix[14] = 0.0f;
    pMatrix[15] = 1.0f;
}

static
VOID
MatrixPerspective(
    _Out_writes_(16) GLfloat *pMatrix,
    _In_ GLfloat FieldOfView,
    _In_ GLfloat Aspect,
    _In_ GLfloat Near,
    _In_ GLfloat Far)
{
    GLfloat f = 1.0f / tanf(FieldOfView / 2.0f);

    memset(pMatrix, 0, 16 * sizeof(*pMatrix));
    pMatrix[0] = f / Aspect;
    pMatrix[5] = f;
    pMatrix[10] = (Far + Near) / (Near - Far);
    pMatrix[11] = -1.0f;
    pMatrix[14] = (2.0f * Far * Near) / (Near - Far);
}

static
VOID
RenderFrame(VOID)
{
    GLfloat Projection[16], ModelView[16], Spin[16], Tilt[16];
    GLfloat Seconds = (GLfloat)GetTickCount() / 1000.0f;

    glViewport(0, 0, g_cxClient, g_cyClient);
    glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    MatrixPerspective(Projection, CUBE_PI / 4.0f, (GLfloat)g_cxClient / (GLfloat)g_cyClient, 0.5f, 50.0f);

    MatrixRotation(Spin, Seconds * 0.9f, 0.0f, 1.0f, 0.0f);
    MatrixRotation(Tilt, Seconds * 0.6f, 1.0f, 0.0f, 0.3f);
    MatrixMultiply(ModelView, Tilt, Spin);

    /* Push the cube back from the eye */
    ModelView[14] -= 6.0f;

    pglUseProgram(g_Program);
    pglUniformMatrix4fv(g_ProjectionLocation, 1, GL_FALSE, Projection);
    pglUniformMatrix4fv(g_ModelViewLocation, 1, GL_FALSE, ModelView);
    pglUniform3f(g_LightDirLocation, 0.5f, 0.8f, 1.0f);

    glDrawArrays(GL_TRIANGLES, 0, CUBE_VERTEX_COUNT);

    SwapBuffers(g_hdc);
}

static
BOOL
InitializeOpenGl(
    _In_ HWND hwnd)
{
    PIXELFORMATDESCRIPTOR pfd;
    const char *pszVersion;
    char szLog[1024];
    int iPixelFormat;

    g_hdc = GetDC(hwnd);

    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    iPixelFormat = ChoosePixelFormat(g_hdc, &pfd);
    if ((iPixelFormat == 0) || !SetPixelFormat(g_hdc, iPixelFormat, &pfd))
    {
        ShowError(hwnd, IDS_NOOPENGL);
        return FALSE;
    }

    g_hglrc = wglCreateContext(g_hdc);
    if ((g_hglrc == NULL) || !wglMakeCurrent(g_hdc, g_hglrc))
    {
        ShowError(hwnd, IDS_NOOPENGL);
        return FALSE;
    }

    /* The version string starts with major.minor */
    pszVersion = (const char *)glGetString(GL_VERSION);
    if ((pszVersion == NULL) || (pszVersion[0] < '2') || !LoadGlFunctions())
    {
        ShowError(hwnd, IDS_NOOPENGL20, pszVersion ? pszVersion : "?");
        return FALSE;
    }

    if (!CreateCubeProgram(szLog, sizeof(szLog)))
    {
        ShowError(hwnd, IDS_NOSHADERS, szLog);
        return FALSE;
    }

    CreateCubeBuffer();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    return TRUE;
}

static
VOID
ShutdownOpenGl(
    _In_ HWND hwnd)
{
    if (g_hglrc != NULL)
    {
        if (g_VertexBuffer != 0)
            pglDeleteBuffers(1, &g_VertexBuffer);
        if (g_Program != 0)
            pglDeleteProgram(g_Program);

        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hglrc);
        g_hglrc = NULL;
    }

    if (g_hdc != NULL)
    {
        ReleaseDC(hwnd, g_hdc);
        g_hdc = NULL;
    }
}

static
LRESULT
CALLBACK
CubeWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
            return InitializeOpenGl(hwnd) ? 0 : -1;

        case WM_SIZE:
            g_cxClient = max(LOWORD(lParam), 1);
            g_cyClient = max(HIWORD(lParam), 1);
            return 0;

        case WM_ERASEBKGND:
            /* Every frame covers the whole client area */
            return 1;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
                DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            ShutdownOpenGl(hwnd);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    static const WCHAR szClassName[] = L"ReactOSOpenGLCube";
    WCHAR szTitle[64];
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    g_hInstance = hInstance;
    LoadStringW(hInstance, IDS_APPTITLE, szTitle, ARRAYSIZE(szTitle));

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CubeWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = szClassName;
    if (!RegisterClassExW(&wc))
        return 1;

    hwnd = CreateWindowExW(0,
                           szClassName,
                           szTitle,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,
                           640,
                           480,
                           NULL,
                           NULL,
                           hInstance,
                           NULL);
    if (hwnd == NULL)
        return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* Draw whenever there is nothing else to do */
    for (;;)
    {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                break;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }

        if (IsIconic(hwnd))
        {
            WaitMessage();
            continue;
        }

        RenderFrame();
    }

    return (int)msg.wParam;
}
