/* Procedural texture atlas: an 8x8 grid of 16px tiles uploaded as one GL
 * texture.  g_uv[t] holds the half-texel-inset UV rect for tile slot t. */
#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/gl.h>

#define ATLAS_TILES 8
#define TILE_PX     16
#define ATLAS_PX    (ATLAS_TILES * TILE_PX)   /* 128 */

/* Atlas tile slots. */
enum {
    TILE_GRASS_TOP = 0, TILE_GRASS_SIDE, TILE_DIRT, TILE_STONE, TILE_COBBLE, TILE_BEDROCK,
    TILE_SAND, TILE_SANDSTONE_TOP, TILE_SANDSTONE_SIDE, TILE_GRAVEL,
    TILE_WATER, TILE_LAVA, TILE_LOG_TOP, TILE_LOG_SIDE, TILE_PLANKS, TILE_LEAVES,
    TILE_COAL_ORE, TILE_IRON_ORE, TILE_GOLD_ORE, TILE_DIAMOND_ORE, TILE_REDSTONE_ORE,
    TILE_SNOW, TILE_SNOW_SIDE, TILE_ICE, TILE_CACTUS_TOP, TILE_CACTUS_SIDE,
    TILE_BRICK, TILE_GLOWSTONE, TILE_OBSIDIAN, TILE_MOSSY,
    TILE_PUMPKIN_TOP, TILE_PUMPKIN_SIDE, TILE_PUMPKIN_FACE,
    TILE_PICK, TILE_AXE, TILE_SHOVEL, TILE_SWORD,   /* tool icons (transparent bg) */
    TILE_APPLE, TILE_BREAD, TILE_MEAT,              /* food icons (transparent bg) */
    TILE_COUNT
};

extern GLuint g_atlasTex;
extern float  g_uv[ATLAS_TILES * ATLAS_TILES][4];

void BuildAtlas(void);

#endif /* TEXTURE_H */
