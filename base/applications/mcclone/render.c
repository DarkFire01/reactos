#include "mc.h"
#include "render.h"
#include "mesh.h"
#include "player.h"
#include "texture.h"
#include "game.h"
#include "hud.h"
#include "survival.h"

static HDC  g_hdc = NULL;
static int  g_w = 800, g_h = 600;
static char g_renderer[96] = "?";

void RenderResize(int w, int h)
{
    g_w = w; g_h = (h > 0) ? h : 1;
}

const char *RenderRendererName(void) { return g_renderer; }

void RenderInit(HDC hdc)
{
    GLfloat sky[4] = { 0.55f, 0.72f, 0.95f, 1.0f };
    const char *r;

    g_hdc = hdc;
    r = (const char *)glGetString(GL_RENDERER);
    _snprintf(g_renderer, sizeof(g_renderer) - 1, "%s", r ? r : "?");
    g_renderer[sizeof(g_renderer) - 1] = '\0';

    BuildAtlas();
    MeshInit();
    BuildAllChunks();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogfv(GL_FOG_COLOR, sky);
    glFogf(GL_FOG_START, 30.0f);
    glFogf(GL_FOG_END,   58.0f);

    glClearColor(sky[0], sky[1], sky[2], 1.0f);
}

/* gluLookAt by hand: s = right = normalize(forward x up); u = up = s x forward. */
static void ApplyCamera(void)
{
    double fx, fy, fz, sx, sz, sl, ux, uy, uz;
    double ex = g_player.pos.x, ey = g_player.pos.y + 1.62, ez = g_player.pos.z;
    GLfloat m[16];

    PlayerLookDir(&fx, &fy, &fz);
    sx = -fz; sz = fx;
    sl = sqrt(sx * sx + sz * sz); if (sl < 1e-6) sl = 1e-6;
    sx /= sl; sz /= sl;
    ux = -sz * fy;
    uy =  sz * fx - sx * fz;
    uz =  sx * fy;

    m[0] = (GLfloat)sx;    m[4] = 0.0f;           m[8]  = (GLfloat)sz;    m[12] = 0.0f;
    m[1] = (GLfloat)ux;    m[5] = (GLfloat)uy;    m[9]  = (GLfloat)uz;    m[13] = 0.0f;
    m[2] = (GLfloat)(-fx); m[6] = (GLfloat)(-fy); m[10] = (GLfloat)(-fz); m[14] = 0.0f;
    m[3] = 0.0f;           m[7] = 0.0f;           m[11] = 0.0f;           m[15] = 1.0f;

    glLoadMatrixf(m);
    glTranslatef((GLfloat)(-ex), (GLfloat)(-ey), (GLfloat)(-ez));
}

static void SetProjection(void)
{
    double aspect = (double)g_w / (double)g_h;
    double fov = 70.0, znear = 0.1, zfar = 120.0;
    double top = znear * tan(DEG2RAD(fov * 0.5));
    double right = top * aspect;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, znear, zfar);
    glMatrixMode(GL_MODELVIEW);
}

/* Black wireframe cube around the block under the crosshair (MC's selection
 * box).  Slightly inflated to avoid z-fighting with the block's own faces. */
static void DrawSelectionBox(void)
{
    int bx, by, bz;
    float e = 0.003f, x0, y0, z0, x1, y1, z1;
    if (!GameAim(&bx, &by, &bz)) return;
    x0 = bx - e; y0 = by - e; z0 = bz - e;
    x1 = bx + 1 + e; y1 = by + 1 + e; z1 = bz + 1 + e;

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_LINES);
    /* bottom */
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0);  glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1);
    glVertex3f(x1,y0,z1); glVertex3f(x0,y0,z1);  glVertex3f(x0,y0,z1); glVertex3f(x0,y0,z0);
    /* top */
    glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0);  glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1);
    glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);  glVertex3f(x0,y1,z1); glVertex3f(x0,y1,z0);
    /* verticals */
    glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0);  glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0);
    glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1);  glVertex3f(x0,y0,z1); glVertex3f(x0,y1,z1);
    glEnd();
    glEnable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
}

void RenderFrame(void)
{
    GLfloat sky[4];
    SurvivalSkyColor(&sky[0], &sky[1], &sky[2]);
    sky[3] = 1.0f;
    glClearColor(sky[0], sky[1], sky[2], 1.0f);
    glFogfv(GL_FOG_COLOR, sky);

    glViewport(0, 0, g_w, g_h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    SetProjection();
    glLoadIdentity();
    ApplyCamera();

    glBindTexture(GL_TEXTURE_2D, g_atlasTex);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    DrawChunks();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    DrawWater();
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    DrawSelectionBox();
    HudDraw(g_w, g_h);
    SwapBuffers(g_hdc);
}
