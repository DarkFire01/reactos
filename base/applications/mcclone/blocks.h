/* Block type registry: ids, per-face atlas tiles, and physical properties. */
#ifndef BLOCKS_H
#define BLOCKS_H

enum {
    BL_AIR = 0,
    BL_GRASS, BL_DIRT, BL_STONE, BL_COBBLE, BL_BEDROCK,
    BL_SAND, BL_SANDSTONE, BL_GRAVEL,
    BL_WATER, BL_LAVA,
    BL_LOG, BL_PLANKS, BL_LEAVES,
    BL_COAL_ORE, BL_IRON_ORE, BL_GOLD_ORE, BL_DIAMOND_ORE, BL_REDSTONE_ORE,
    BL_SNOWY, BL_ICE,
    BL_CACTUS, BL_BRICK, BL_GLOWSTONE, BL_OBSIDIAN, BL_MOSSY, BL_PUMPKIN,
    BL_COUNT
};

typedef struct {
    const char *name;
    int   tileTop, tileSide, tileBottom;
    unsigned char opaque;   /* fully hides the neighbouring face            */
    unsigned char solid;    /* blocks player/mob movement                   */
    unsigned char emissive; /* light source (glowstone, lava)               */
    float hardness;         /* relative break time (0 = instant)            */
} BlockDef;

const BlockDef *Block(int id);
int   IsSolid(int id);
int   IsOpaque(int id);
int   TileFor(int id, int face);
const char *BlockName(int id);

#endif /* BLOCKS_H */
