#include "texture.h"
#include "noise.h"
#include <string.h>

GLuint g_atlasTex = 0;
float  g_uv[ATLAS_TILES * ATLAS_TILES][4];

static unsigned char g_atlas[ATLAS_PX * ATLAS_PX * 4];

static int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void PxA(int tile, int tx, int ty, int r, int g, int b, int a)
{
    int col = tile % ATLAS_TILES;
    int row = tile / ATLAS_TILES;
    int x = col * TILE_PX + tx;
    int y = row * TILE_PX + ty;
    unsigned char *p = &g_atlas[(y * ATLAS_PX + x) * 4];
    p[0] = (unsigned char)Clamp255(r);
    p[1] = (unsigned char)Clamp255(g);
    p[2] = (unsigned char)Clamp255(b);
    p[3] = (unsigned char)Clamp255(a);
}

static void Px(int tile, int tx, int ty, int r, int g, int b) { PxA(tile, tx, ty, r, g, b, 255); }

/* Flat-ish tile with per-texel value jitter. */
static void Fill(int tile, int r, int g, int b, int jitter)
{
    int tx, ty;
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int n = (int)(Hash2(tx + tile * 131, ty + tile * 977) % 512) - 256;
            int j = (n * jitter) / 256;
            Px(tile, tx, ty, r + j, g + j, b + j);
        }
}

/* Stone base sprinkled with coloured ore specks. */
static void Ore(int tile, int sr, int sg, int sb)
{
    int i;
    Fill(tile, 128, 128, 132, 36);
    for (i = 0; i < 22; i++)
    {
        int px = (int)(Hash2(tile * 17 + i, 3) % TILE_PX);
        int py = (int)(Hash2(7, tile * 23 + i) % TILE_PX);
        Px(tile, px, py, sr, sg, sb);
        if (px + 1 < TILE_PX) Px(tile, px + 1, py, sr, sg, sb);
        if (py + 1 < TILE_PX) Px(tile, px, py + 1, sr, sg, sb);
    }
}

/* Tool icons are grayscale on a transparent background; the HUD tints them by
 * tier colour, so the same glyph serves wood/stone/iron/diamond. */
static void ClearTile(int tile)
{
    int x, y;
    for (y = 0; y < TILE_PX; y++)
        for (x = 0; x < TILE_PX; x++)
            PxA(tile, x, y, 0, 0, 0, 0);
}

static void ToolHandle(int tile)
{
    int k;
    for (k = 0; k < 8; k++)
    {
        PxA(tile, 4 + k, 12 - k, 130, 95, 60, 255);
        PxA(tile, 5 + k, 12 - k, 130, 95, 60, 255);
    }
}

static void BuildTools(void)
{
    int x, y;

    /* pickaxe: handle + a curved bar across the top */
    ClearTile(TILE_PICK);
    ToolHandle(TILE_PICK);
    for (x = 2; x <= 13; x++) Px(TILE_PICK, x, 3, 205, 205, 210);
    Px(TILE_PICK, 2, 4, 205, 205, 210);  Px(TILE_PICK, 13, 4, 205, 205, 210);
    Px(TILE_PICK, 3, 2, 205, 205, 210);  Px(TILE_PICK, 12, 2, 205, 205, 210);

    /* axe: handle + blocky head top-right */
    ClearTile(TILE_AXE);
    ToolHandle(TILE_AXE);
    for (y = 2; y <= 7; y++)
        for (x = 9; x <= 13; x++) Px(TILE_AXE, x, y, 205, 205, 210);

    /* shovel: handle + small square head */
    ClearTile(TILE_SHOVEL);
    ToolHandle(TILE_SHOVEL);
    for (y = 2; y <= 6; y++)
        for (x = 8; x <= 12; x++) Px(TILE_SHOVEL, x, y, 205, 205, 210);

    /* sword: vertical blade, crossguard, handle */
    ClearTile(TILE_SWORD);
    for (y = 2; y <= 11; y++) { Px(TILE_SWORD, 7, y, 215, 215, 220); Px(TILE_SWORD, 8, y, 215, 215, 220); }
    for (x = 5; x <= 10; x++) Px(TILE_SWORD, x, 11, 150, 150, 155);
    for (y = 12; y <= 14; y++) { Px(TILE_SWORD, 7, y, 130, 95, 60); Px(TILE_SWORD, 8, y, 130, 95, 60); }
}

/* Food icons, also grayscale-free (real colours) on transparent backgrounds. */
static void BuildFood(void)
{
    int x, y, dx, dy;

    /* apple: red disk with a green stem */
    ClearTile(TILE_APPLE);
    for (y = 4; y <= 13; y++)
        for (x = 3; x <= 12; x++)
        {
            dx = x - 7; dy = y - 9;
            if (dx * dx + dy * dy <= 16) PxA(TILE_APPLE, x, y, 210, 40, 40, 255);
        }
    Px(TILE_APPLE, 8, 3, 90, 150, 50); Px(TILE_APPLE, 8, 2, 90, 150, 50);

    /* bread: rounded brown loaf */
    ClearTile(TILE_BREAD);
    for (y = 5; y <= 11; y++)
        for (x = 2; x <= 13; x++)
        {
            int edge = (x == 2 || x == 13) && (y == 5 || y == 11);
            if (!edge) PxA(TILE_BREAD, x, y, 180, 130, 70, 255);
        }
    for (x = 4; x <= 11; x += 3) { Px(TILE_BREAD, x, 7, 140, 95, 45); Px(TILE_BREAD, x, 9, 140, 95, 45); }

    /* meat: pink chop with a bone nub */
    ClearTile(TILE_MEAT);
    for (y = 4; y <= 12; y++)
        for (x = 4; x <= 12; x++)
        {
            dx = x - 8; dy = y - 8;
            if (dx * dx + dy * dy <= 18) PxA(TILE_MEAT, x, y, 205, 120, 120, 255);
        }
    Px(TILE_MEAT, 3, 8, 235, 230, 215); Px(TILE_MEAT, 2, 7, 235, 230, 215); Px(TILE_MEAT, 2, 9, 235, 230, 215);
}

void BuildAtlas(void)
{
    int tx, ty, i;

    Fill(TILE_GRASS_TOP,      70, 140, 55, 40);
    Fill(TILE_DIRT,           120, 85, 55, 35);
    Fill(TILE_STONE,          128, 128, 132, 38);
    Fill(TILE_COBBLE,         110, 110, 114, 70);
    Fill(TILE_BEDROCK,        50, 50, 54, 60);
    Fill(TILE_SAND,           214, 200, 140, 26);
    Fill(TILE_SANDSTONE_TOP,  220, 208, 152, 18);
    Fill(TILE_GRAVEL,         130, 124, 122, 60);
    Fill(TILE_WATER,          50, 95, 200, 22);
    Fill(TILE_LAVA,           220, 95, 25, 60);
    Fill(TILE_LOG_TOP,        150, 110, 60, 28);
    Fill(TILE_PLANKS,         178, 140, 92, 0);  /* pattern added below */
    Fill(TILE_LEAVES,         50, 115, 45, 55);
    Fill(TILE_SNOW,           238, 244, 250, 14);
    Fill(TILE_ICE,            150, 190, 230, 22);
    Fill(TILE_CACTUS_TOP,     90, 140, 70, 24);
    Fill(TILE_GLOWSTONE,      230, 200, 110, 60);
    Fill(TILE_OBSIDIAN,       40, 30, 55, 30);
    Fill(TILE_MOSSY,          90, 110, 80, 70);
    Fill(TILE_PUMPKIN_TOP,    200, 140, 40, 30);

    Ore(TILE_COAL_ORE,     35, 35, 35);
    Ore(TILE_IRON_ORE,     200, 160, 120);
    Ore(TILE_GOLD_ORE,     240, 210, 70);
    Ore(TILE_DIAMOND_ORE,  90, 220, 220);
    Ore(TILE_REDSTONE_ORE, 220, 40, 40);

    /* grass side: grassy lip over dirt */
    Fill(TILE_GRASS_SIDE, 120, 85, 55, 35);
    for (ty = 0; ty < 5; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int n = (int)(Hash2(tx + 9, ty + 17) % 256) - 128;
            Px(TILE_GRASS_SIDE, tx, ty, 70, 140 + n / 6, 55);
        }

    /* snow side: snowy cap over dirt */
    Fill(TILE_SNOW_SIDE, 120, 85, 55, 35);
    for (ty = 0; ty < 5; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
            Px(TILE_SNOW_SIDE, tx, ty, 238, 244, 250);

    /* sandstone side: layered bands */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int band = (ty % 6 < 1) ? -25 : 0;
            int n = (int)(Hash2(tx * 3, ty) % 32) - 16;
            Px(TILE_SANDSTONE_SIDE, tx, ty, 220 + band + n / 4, 208 + band, 152 + band);
        }

    /* log side: vertical bark */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int dark = ((tx / 2) & 1) ? -18 : 12;
            int n = (int)(Hash2(tx, ty * 7) % 64) - 32;
            Px(TILE_LOG_SIDE, tx, ty, 120 + dark + n / 8, 80 + dark + n / 10, 45 + dark);
        }

    /* planks: horizontal boards with seams */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int seam = (ty % 4 == 0) ? -40 : 0;
            int n = (int)(Hash2(tx * 3, ty) % 48) - 24;
            Px(TILE_PLANKS, tx, ty, 178 + seam + n / 6, 140 + seam + n / 8, 92 + seam);
        }

    /* brick: offset courses with mortar */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int course = ty / 4;
            int offset = (course & 1) ? 4 : 0;
            int mortar = (ty % 4 == 0) || (((tx + offset) % 8) == 0);
            if (mortar) Px(TILE_BRICK, tx, ty, 200, 195, 185);
            else        Px(TILE_BRICK, tx, ty, 165, 60, 50);
        }

    /* cactus side: green with ribs */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int rib = (tx == 1 || tx == TILE_PX - 2) ? -30 : 0;
            int n = (int)(Hash2(tx, ty * 5) % 40) - 20;
            Px(TILE_CACTUS_SIDE, tx, ty, 60 + rib, 130 + rib + n / 6, 55 + rib);
        }

    /* pumpkin side + carved face */
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int rib = (tx % 4 == 0) ? -25 : 0;
            Px(TILE_PUMPKIN_SIDE, tx, ty, 210 + rib, 130 + rib, 35);
        }
    for (ty = 0; ty < TILE_PX; ty++)
        for (tx = 0; tx < TILE_PX; tx++)
        {
            int rib = (tx % 4 == 0) ? -25 : 0;
            int eye = ((ty >= 4 && ty <= 6) && ((tx >= 3 && tx <= 5) || (tx >= 10 && tx <= 12)));
            int mouth = (ty >= 9 && ty <= 12) && (tx >= 3 && tx <= 12) && (((tx + ty) & 1) == 0);
            if (eye || mouth) Px(TILE_PUMPKIN_FACE, tx, ty, 255, 220, 90);
            else              Px(TILE_PUMPKIN_FACE, tx, ty, 210 + rib, 130 + rib, 35);
        }

    BuildTools();
    BuildFood();

    /* UV rects with a half-texel inset (GL_NEAREST never bleeds across tiles) */
    for (i = 0; i < ATLAS_TILES * ATLAS_TILES; i++)
    {
        int col = i % ATLAS_TILES;
        int row = i / ATLAS_TILES;
        float inset = 0.5f / (float)ATLAS_PX;
        g_uv[i][0] = (float)col / ATLAS_TILES + inset;
        g_uv[i][1] = (float)row / ATLAS_TILES + inset;
        g_uv[i][2] = (float)(col + 1) / ATLAS_TILES - inset;
        g_uv[i][3] = (float)(row + 1) / ATLAS_TILES - inset;
    }

    glGenTextures(1, &g_atlasTex);
    glBindTexture(GL_TEXTURE_2D, g_atlasTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_PX, ATLAS_PX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_atlas);
}
