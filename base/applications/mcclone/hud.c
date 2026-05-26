#include "mc.h"
#include "hud.h"
#include "texture.h"
#include "items.h"
#include "inventory.h"
#include "game.h"
#include "survival.h"

static void Rect(float x0, float y0, float x1, float y1)
{
    glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
}

static void TexQuad(float x0, float y0, float x1, float y1, int tile)
{
    const float *uv = g_uv[tile];
    glTexCoord2f(uv[0], uv[3]); glVertex2f(x0, y0);
    glTexCoord2f(uv[2], uv[3]); glVertex2f(x1, y0);
    glTexCoord2f(uv[2], uv[1]); glVertex2f(x1, y1);
    glTexCoord2f(uv[0], uv[1]); glVertex2f(x0, y1);
}

/* segment bitmasks [a b c d e f g] for digits 0..9 */
static const unsigned char g_seg[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void Digit(float x, float y, float w, float h, float t, int d)
{
    unsigned char m = g_seg[d % 10];
    float mid = y + h * 0.5f;
    glBegin(GL_QUADS);
    if (m & 0x01) Rect(x + t, y + h - t, x + w - t, y + h);          /* a */
    if (m & 0x02) Rect(x + w - t, mid, x + w, y + h - t);            /* b */
    if (m & 0x04) Rect(x + w - t, y + t, x + w, mid);               /* c */
    if (m & 0x08) Rect(x + t, y, x + w - t, y + t);                 /* d */
    if (m & 0x10) Rect(x, y + t, x + t, mid);                       /* e */
    if (m & 0x20) Rect(x, mid, x + t, y + h - t);                   /* f */
    if (m & 0x40) Rect(x + t, mid - t * 0.5f, x + w - t, mid + t * 0.5f); /* g */
    glEnd();
}

/* one status icon (heart/drumstick): fill = 2 full, 1 half, 0 empty */
static void Pip(float x, float y, float s, int fill, float r, float g, float b)
{
    glColor4f(0.12f, 0.12f, 0.12f, 0.7f);
    glBegin(GL_QUADS); Rect(x, y, x + s, y + s); glEnd();
    if (fill >= 2)
    {
        glColor4f(r, g, b, 1.0f);
        glBegin(GL_QUADS); Rect(x, y, x + s, y + s); glEnd();
    }
    else if (fill == 1)
    {
        glColor4f(r, g, b, 1.0f);
        glBegin(GL_QUADS); Rect(x, y, x + s * 0.5f, y + s); glEnd();
    }
}

static void Number(float x, float y, float h, int n)
{
    float w = h * 0.6f, t = h * 0.16f;
    int digits[6], nd = 0, i;
    if (n <= 0) return;
    while (n > 0 && nd < 6) { digits[nd++] = n % 10; n /= 10; }
    /* draw right-to-left from x (x is the right edge) */
    for (i = 0; i < nd; i++)
        Digit(x - (i + 1) * (w + t), y, w, h, t, digits[i]);
}

void HudDraw(int w, int h)
{
    const float cell = 46.0f, gap = 4.0f;
    float total = INV_HOTBAR * cell + (INV_HOTBAR - 1) * gap;
    float x0 = (w - total) * 0.5f, y0 = 10.0f;
    int cx = w / 2, cy = h / 2, i;
    double frac;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, w, 0, h, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* night darkening: a translucent blue sheet over the world (under the HUD) */
    {
        double dl = SurvivalDayLight();
        float a = (float)((1.0 - dl) * 0.65);
        if (a > 0.01f)
        {
            glColor4f(0.02f, 0.03f, 0.12f, a);
            glBegin(GL_QUADS); Rect(0, 0, (float)w, (float)h); glEnd();
        }
    }
    /* death: red wash */
    if (SurvivalDead())
    {
        glColor4f(0.5f, 0.0f, 0.0f, 0.45f);
        glBegin(GL_QUADS); Rect(0, 0, (float)w, (float)h); glEnd();
    }

    /* crosshair */
    glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    glBegin(GL_LINES);
    glVertex2i(cx - 10, cy); glVertex2i(cx + 10, cy);
    glVertex2i(cx, cy - 10); glVertex2i(cx, cy + 10);
    glEnd();

    /* mining progress bar under the crosshair */
    frac = GameMineFraction();
    if (frac > 0.0)
    {
        glColor4f(0.0f, 0.0f, 0.0f, 0.6f);
        glBegin(GL_QUADS); Rect((float)(cx - 16), (float)(cy - 24), (float)(cx + 16), (float)(cy - 18)); glEnd();
        glColor4f(0.9f, 0.9f, 0.9f, 0.9f);
        glBegin(GL_QUADS); Rect((float)(cx - 16), (float)(cy - 24), (float)(cx - 16 + (float)(32.0 * frac)), (float)(cy - 18)); glEnd();
    }

    /* hotbar cell backgrounds */
    for (i = 0; i < INV_HOTBAR; i++)
    {
        float x = x0 + i * (cell + gap);
        glColor4f(0.0f, 0.0f, 0.0f, (i == g_invSel) ? 0.65f : 0.40f);
        glBegin(GL_QUADS); Rect(x, y0, x + cell, y0 + cell); glEnd();
    }

    /* item icons */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_atlasTex);
    for (i = 0; i < INV_HOTBAR; i++)
    {
        float x = x0 + i * (cell + gap);
        int item = g_inv[i].item;
        if (g_inv[i].count <= 0 || item == 0) continue;
        if (ItemIsTool(item))
        {
            float r, g, b;
            ToolTierColor(ToolTier(item), &r, &g, &b);
            glColor4f(r, g, b, 1.0f);
        }
        else glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        TexQuad(x + 6, y0 + 6, x + cell - 6, y0 + cell - 6, ItemIcon(item));
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);

    /* stack counts */
    glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
    for (i = 0; i < INV_HOTBAR; i++)
    {
        float x = x0 + i * (cell + gap);
        if (g_inv[i].count > 1)
            Number(x + cell - 5, y0 + 5, 12.0f, g_inv[i].count);
    }

    /* selection highlight */
    {
        float x = x0 + g_invSel * (cell + gap);
        glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(x, y0); glVertex2f(x + cell, y0);
        glVertex2f(x + cell, y0 + cell); glVertex2f(x, y0 + cell);
        glEnd();
    }

    /* health (hearts, left) and hunger (drumsticks, right) above the hotbar */
    {
        float py = y0 + cell + 8.0f, s = 13.0f, step = 16.0f;
        for (i = 0; i < 10; i++)
        {
            int hp = g_health - i * 2;
            Pip(x0 + i * step, py, s, hp >= 2 ? 2 : (hp == 1 ? 1 : 0), 0.85f, 0.10f, 0.12f);
        }
        for (i = 0; i < 10; i++)
        {
            int hg = g_hunger - i * 2;
            Pip(x0 + total - s - i * step, py, s, hg >= 2 ? 2 : (hg == 1 ? 1 : 0), 0.70f, 0.45f, 0.15f);
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_FOG);
    glEnable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}
