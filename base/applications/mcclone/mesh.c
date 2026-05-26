#include "mc.h"
#include "mesh.h"
#include "world.h"
#include "blocks.h"
#include "texture.h"

static const int g_faceNormal[6][3] = {
    {  0,  1,  0 }, {  0, -1,  0 },
    {  0,  0, -1 }, {  0,  0,  1 },
    {  1,  0,  0 }, { -1,  0,  0 }
};

/* baked directional/AO shading per face */
static const float g_faceShade[6] = { 1.00f, 0.50f, 0.80f, 0.80f, 0.65f, 0.65f };

/* four CCW (viewed from outside) corners of each unit-cube face */
static const float g_faceVert[6][4][3] = {
    /* TOP    */ { {0,1,1},{1,1,1},{1,1,0},{0,1,0} },
    /* BOTTOM */ { {0,0,0},{1,0,0},{1,0,1},{0,0,1} },
    /* NORTH  */ { {1,0,0},{0,0,0},{0,1,0},{1,1,0} },
    /* SOUTH  */ { {0,0,1},{1,0,1},{1,1,1},{0,1,1} },
    /* EAST   */ { {1,0,1},{1,0,0},{1,1,0},{1,1,1} },
    /* WEST   */ { {0,0,0},{0,0,1},{0,1,1},{0,1,0} }
};

static GLuint g_listSolid[CHUNKS_X][CHUNKS_Z];
static GLuint g_listWater[CHUNKS_X][CHUNKS_Z];

static void EmitFace(int x, int y, int z, int face, int tile, float shade, float alpha)
{
    const float (*v)[3] = g_faceVert[face];
    const float *uv = g_uv[tile];
    glColor4f(shade, shade, shade, alpha);
    glTexCoord2f(uv[0], uv[3]); glVertex3f((GLfloat)(x + v[0][0]), (GLfloat)(y + v[0][1]), (GLfloat)(z + v[0][2]));
    glTexCoord2f(uv[2], uv[3]); glVertex3f((GLfloat)(x + v[1][0]), (GLfloat)(y + v[1][1]), (GLfloat)(z + v[1][2]));
    glTexCoord2f(uv[2], uv[1]); glVertex3f((GLfloat)(x + v[2][0]), (GLfloat)(y + v[2][1]), (GLfloat)(z + v[2][2]));
    glTexCoord2f(uv[0], uv[1]); glVertex3f((GLfloat)(x + v[3][0]), (GLfloat)(y + v[3][1]), (GLfloat)(z + v[3][2]));
}

/* A face is visible when the neighbour does not fully cover it and is a
 * different block (so adjacent same-type transparent blocks don't draw inner
 * faces). */
static int FaceVisible(unsigned char self, unsigned char neigh)
{
    return !IsOpaque(neigh) && neigh != self;
}

static void BuildChunk(int cx, int cz)
{
    int bx0 = cx * CHUNK, bz0 = cz * CHUNK;
    int x, y, z, f;

    glNewList(g_listSolid[cx][cz], GL_COMPILE);
    glBegin(GL_QUADS);
    for (x = bx0; x < bx0 + CHUNK; x++)
        for (z = bz0; z < bz0 + CHUNK; z++)
            for (y = 0; y < WORLD_H; y++)
            {
                unsigned char b = BlockAt(x, y, z);
                if (b == BL_AIR || b == BL_WATER) continue;
                for (f = 0; f < 6; f++)
                {
                    const int *n = g_faceNormal[f];
                    unsigned char nb = BlockAt(x + n[0], y + n[1], z + n[2]);
                    if (FaceVisible(b, nb))
                    {
                        float shade = Block(b)->emissive ? 1.0f : g_faceShade[f];
                        EmitFace(x, y, z, f, TileFor(b, f), shade, 1.0f);
                    }
                }
            }
    glEnd();
    glEndList();

    glNewList(g_listWater[cx][cz], GL_COMPILE);
    glBegin(GL_QUADS);
    for (x = bx0; x < bx0 + CHUNK; x++)
        for (z = bz0; z < bz0 + CHUNK; z++)
            for (y = 0; y < WORLD_H; y++)
            {
                if (BlockAt(x, y, z) != BL_WATER) continue;
                if (BlockAt(x, y + 1, z) == BL_AIR)
                    EmitFace(x, y, z, F_TOP, TileFor(BL_WATER, F_TOP), g_faceShade[F_TOP], 0.62f);
            }
    glEnd();
    glEndList();
}

void MeshInit(void)
{
    int cx, cz;
    for (cx = 0; cx < CHUNKS_X; cx++)
        for (cz = 0; cz < CHUNKS_Z; cz++)
        {
            g_listSolid[cx][cz] = glGenLists(1);
            g_listWater[cx][cz] = glGenLists(1);
        }
}

void BuildAllChunks(void)
{
    int cx, cz;
    for (cx = 0; cx < CHUNKS_X; cx++)
        for (cz = 0; cz < CHUNKS_Z; cz++)
            BuildChunk(cx, cz);
}

void RebuildAround(int x, int z)
{
    int cx0 = x / CHUNK, cz0 = z / CHUNK, dcx, dcz;
    for (dcx = -1; dcx <= 1; dcx++)
        for (dcz = -1; dcz <= 1; dcz++)
        {
            int cx = cx0 + dcx, cz = cz0 + dcz;
            if (cx >= 0 && cx < CHUNKS_X && cz >= 0 && cz < CHUNKS_Z)
                BuildChunk(cx, cz);
        }
}

void DrawChunks(void)
{
    int cx, cz;
    for (cx = 0; cx < CHUNKS_X; cx++)
        for (cz = 0; cz < CHUNKS_Z; cz++)
            glCallList(g_listSolid[cx][cz]);
}

void DrawWater(void)
{
    int cx, cz;
    for (cx = 0; cx < CHUNKS_X; cx++)
        for (cz = 0; cz < CHUNKS_Z; cz++)
            glCallList(g_listWater[cx][cz]);
}
