#include "mc.h"
#include "world.h"
#include "blocks.h"
#include "noise.h"

static unsigned char g_world[WORLD_X * WORLD_Z * WORLD_H];

int InBounds(int x, int y, int z)
{
    return x >= 0 && x < WORLD_X && y >= 0 && y < WORLD_H && z >= 0 && z < WORLD_Z;
}

unsigned char BlockAt(int x, int y, int z)
{
    if (y < 0)        return BL_BEDROCK;   /* never draw the underside of the map */
    if (y >= WORLD_H) return BL_AIR;
    if (x < 0 || x >= WORLD_X || z < 0 || z >= WORLD_Z) return BL_AIR; /* open edges */
    return g_world[(x * WORLD_Z + z) * WORLD_H + y];
}

void SetBlock(int x, int y, int z, unsigned char b)
{
    if (!InBounds(x, y, z)) return;
    g_world[(x * WORLD_Z + z) * WORLD_H + y] = b;
}

/* Terrain height of a column before caves are carved.  Mountainous regions get
 * a much larger amplitude so the world has both flat plains and tall peaks. */
int SurfaceHeight(int x, int z)
{
    double e  = Fbm2(x * 0.045, z * 0.045, 4);              /* 0..1 elevation   */
    double mf = ValueNoise2(x * 0.006 + 50, z * 0.006 + 50); /* mountain factor */
    double amp = 7.0 + mf * mf * 38.0;                       /* 7..45            */
    int h = (int)(10.0 + e * amp);
    if (h < 2) h = 2;
    if (h > WORLD_H - 6) h = WORLD_H - 6;
    return h;
}

/* Biome classification from low-frequency temperature/humidity fields. */
enum { BIOME_PLAINS, BIOME_FOREST, BIOME_DESERT, BIOME_TUNDRA, BIOME_MOUNTAIN };

static int BiomeAt(int x, int z, int h)
{
    double temp  = ValueNoise2(x * 0.012 + 200, z * 0.012 + 200);
    double humid = ValueNoise2(x * 0.012 - 200, z * 0.012 - 200);
    if (h > SEA_LEVEL + 26)              return BIOME_MOUNTAIN;
    if (temp < 0.32)                     return BIOME_TUNDRA;
    if (temp > 0.66 && humid < 0.42)     return BIOME_DESERT;
    if (humid > 0.60)                    return BIOME_FOREST;
    return BIOME_PLAINS;
}

static void Column(int x, int z)
{
    int h = SurfaceHeight(x, z);
    int biome = BiomeAt(x, z, h);
    int beach = (h >= SEA_LEVEL - 1 && h <= SEA_LEVEL + 1);
    int y;

    for (y = 0; y <= h; y++)
    {
        unsigned char b;
        if (y == 0)                         b = BL_BEDROCK;
        else if (y <= 2 && (Hash3(x, y, z) & 1)) b = BL_BEDROCK;
        else if (y == h)
        {
            if (biome == BIOME_DESERT)      b = BL_SAND;
            else if (beach)                 b = BL_SAND;
            else if (biome == BIOME_TUNDRA) b = BL_SNOWY;
            else if (biome == BIOME_MOUNTAIN && h > SEA_LEVEL + 34) b = BL_STONE;
            else                            b = BL_GRASS;
        }
        else if (y >= h - 3)
        {
            if (biome == BIOME_DESERT)      b = BL_SANDSTONE;
            else                            b = BL_DIRT;
        }
        else                                b = BL_STONE;
        SetBlock(x, y, z, b);
    }

    /* fill open water up to sea level (freeze the surface in the tundra) */
    for (y = h + 1; y <= SEA_LEVEL; y++)
        SetBlock(x, y, z, BL_WATER);
    if (h < SEA_LEVEL && biome == BIOME_TUNDRA)
        SetBlock(x, SEA_LEVEL, z, BL_ICE);
}

/* Sprinkle ores into stone, rarer and deeper for the valuable ones. */
static void Ores(int x, int z)
{
    int h = SurfaceHeight(x, z), y;
    for (y = 3; y < h - 1; y++)
    {
        unsigned int r;
        if (BlockAt(x, y, z) != BL_STONE) continue;
        r = Hash3(x, y, z) % 1000;
        if      (y < 50 && r < 14)               SetBlock(x, y, z, BL_COAL_ORE);
        else if (y < 40 && r >= 14 && r < 22)    SetBlock(x, y, z, BL_IRON_ORE);
        else if (y < 22 && r >= 22 && r < 25)    SetBlock(x, y, z, BL_GOLD_ORE);
        else if (y < 18 && r >= 25 && r < 29)    SetBlock(x, y, z, BL_REDSTONE_ORE);
        else if (y < 14 && r >= 29 && r < 31)    SetBlock(x, y, z, BL_DIAMOND_ORE);
    }
}

/* Carve winding caves along a thin iso-surface of 3D noise; flood the deepest
 * carved cells with lava. */
static void Caves(int x, int z)
{
    int h = SurfaceHeight(x, z), y;
    for (y = 2; y < h - 1; y++)
    {
        double n = ValueNoise3(x * 0.085, y * 0.13, z * 0.085) * 0.7
                 + ValueNoise3(x * 0.17 + 30, y * 0.26, z * 0.17 + 30) * 0.3;
        if (fabs(n - 0.5) < 0.052)
        {
            unsigned char cur = BlockAt(x, y, z);
            if (cur == BL_BEDROCK) continue;
            SetBlock(x, y, z, (y < 8) ? BL_LAVA : BL_AIR);
        }
    }
}

static void PlantTree(int x, int gy, int z)
{
    int th = 4 + (int)(Hash2(x * 7 + 3, z * 13 + 5) % 3);
    int top = gy + th, dx, dy, dz, yy;
    for (yy = gy; yy < top; yy++) SetBlock(x, yy, z, BL_LOG);
    for (dy = -2; dy <= 1; dy++)
        for (dx = -2; dx <= 2; dx++)
            for (dz = -2; dz <= 2; dz++)
            {
                int r2 = dx * dx + dz * dz + dy * dy;
                if (r2 > 6) continue;
                if (dx == 0 && dz == 0 && dy < 0) continue;
                if (BlockAt(x + dx, top + dy, z + dz) == BL_AIR)
                    SetBlock(x + dx, top + dy, z + dz, BL_LEAVES);
            }
}

static void PlantCactus(int x, int gy, int z)
{
    int ch = 1 + (int)(Hash2(x * 11 + 1, z * 17 + 2) % 3), yy;
    for (yy = gy; yy < gy + ch; yy++) SetBlock(x, yy, z, BL_CACTUS);
}

static void Vegetate(int x, int z)
{
    int h = SurfaceHeight(x, z);
    int biome = BiomeAt(x, z, h);
    unsigned char top = BlockAt(x, h, z);
    if (h <= SEA_LEVEL) return;

    if (top == BL_SAND && biome == BIOME_DESERT)
    {
        if ((Hash2(x + 5, z + 9) % 100) < 3) PlantCactus(x, h + 1, z);
        return;
    }
    if (top == BL_GRASS || top == BL_SNOWY)
    {
        int chance = (biome == BIOME_FOREST) ? 9 : 2;
        if ((Hash2(x, z) % 100) < chance) PlantTree(x, h + 1, z);
    }
}

void GenerateWorld(void)
{
    int x, z;
    memset(g_world, BL_AIR, sizeof(g_world));
    for (x = 0; x < WORLD_X; x++)
        for (z = 0; z < WORLD_Z; z++)
            Column(x, z);
    for (x = 0; x < WORLD_X; x++)
        for (z = 0; z < WORLD_Z; z++)
            Ores(x, z);
    for (x = 0; x < WORLD_X; x++)
        for (z = 0; z < WORLD_Z; z++)
            Caves(x, z);
    for (x = 2; x < WORLD_X - 2; x++)
        for (z = 2; z < WORLD_Z - 2; z++)
            Vegetate(x, z);
}
