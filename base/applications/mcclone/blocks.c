#include "mc.h"
#include "blocks.h"
#include "texture.h"

/* name, top, side, bottom, opaque, solid, emissive, hardness */
static const BlockDef g_def[BL_COUNT] = {
    /* AIR     */ { "Air",       0, 0, 0, 0, 0, 0, 0.0f },
    /* GRASS   */ { "Grass",     TILE_GRASS_TOP, TILE_GRASS_SIDE, TILE_DIRT, 1, 1, 0, 0.6f },
    /* DIRT    */ { "Dirt",      TILE_DIRT, TILE_DIRT, TILE_DIRT, 1, 1, 0, 0.5f },
    /* STONE   */ { "Stone",     TILE_STONE, TILE_STONE, TILE_STONE, 1, 1, 0, 1.5f },
    /* COBBLE  */ { "Cobble",    TILE_COBBLE, TILE_COBBLE, TILE_COBBLE, 1, 1, 0, 2.0f },
    /* BEDROCK */ { "Bedrock",   TILE_BEDROCK, TILE_BEDROCK, TILE_BEDROCK, 1, 1, 0, -1.0f },
    /* SAND    */ { "Sand",      TILE_SAND, TILE_SAND, TILE_SAND, 1, 1, 0, 0.5f },
    /* SANDSTN */ { "Sandstone", TILE_SANDSTONE_TOP, TILE_SANDSTONE_SIDE, TILE_SANDSTONE_TOP, 1, 1, 0, 0.8f },
    /* GRAVEL  */ { "Gravel",    TILE_GRAVEL, TILE_GRAVEL, TILE_GRAVEL, 1, 1, 0, 0.6f },
    /* WATER   */ { "Water",     TILE_WATER, TILE_WATER, TILE_WATER, 0, 0, 0, -1.0f },
    /* LAVA    */ { "Lava",      TILE_LAVA, TILE_LAVA, TILE_LAVA, 1, 0, 1, -1.0f },
    /* LOG     */ { "Wood",      TILE_LOG_TOP, TILE_LOG_SIDE, TILE_LOG_TOP, 1, 1, 0, 2.0f },
    /* PLANKS  */ { "Planks",    TILE_PLANKS, TILE_PLANKS, TILE_PLANKS, 1, 1, 0, 2.0f },
    /* LEAVES  */ { "Leaves",    TILE_LEAVES, TILE_LEAVES, TILE_LEAVES, 1, 1, 0, 0.2f },
    /* COAL    */ { "Coal Ore",  TILE_COAL_ORE, TILE_COAL_ORE, TILE_COAL_ORE, 1, 1, 0, 3.0f },
    /* IRON    */ { "Iron Ore",  TILE_IRON_ORE, TILE_IRON_ORE, TILE_IRON_ORE, 1, 1, 0, 3.0f },
    /* GOLD    */ { "Gold Ore",  TILE_GOLD_ORE, TILE_GOLD_ORE, TILE_GOLD_ORE, 1, 1, 0, 3.0f },
    /* DIAMOND */ { "Diamond Ore", TILE_DIAMOND_ORE, TILE_DIAMOND_ORE, TILE_DIAMOND_ORE, 1, 1, 0, 3.0f },
    /* REDSTN  */ { "Redstone Ore", TILE_REDSTONE_ORE, TILE_REDSTONE_ORE, TILE_REDSTONE_ORE, 1, 1, 0, 3.0f },
    /* SNOWY   */ { "Snow",      TILE_SNOW, TILE_SNOW_SIDE, TILE_DIRT, 1, 1, 0, 0.6f },
    /* ICE     */ { "Ice",       TILE_ICE, TILE_ICE, TILE_ICE, 1, 1, 0, 0.5f },
    /* CACTUS  */ { "Cactus",    TILE_CACTUS_TOP, TILE_CACTUS_SIDE, TILE_CACTUS_TOP, 1, 1, 0, 0.4f },
    /* BRICK   */ { "Brick",     TILE_BRICK, TILE_BRICK, TILE_BRICK, 1, 1, 0, 2.0f },
    /* GLOWSTN */ { "Glowstone", TILE_GLOWSTONE, TILE_GLOWSTONE, TILE_GLOWSTONE, 1, 1, 1, 0.3f },
    /* OBSIDIAN*/ { "Obsidian",  TILE_OBSIDIAN, TILE_OBSIDIAN, TILE_OBSIDIAN, 1, 1, 0, 10.0f },
    /* MOSSY   */ { "Mossy Cobble", TILE_MOSSY, TILE_MOSSY, TILE_MOSSY, 1, 1, 0, 2.0f },
    /* PUMPKIN */ { "Pumpkin",   TILE_PUMPKIN_TOP, TILE_PUMPKIN_SIDE, TILE_PUMPKIN_TOP, 1, 1, 0, 1.0f },
};

const BlockDef *Block(int id)
{
    if (id < 0 || id >= BL_COUNT) id = BL_AIR;
    return &g_def[id];
}

int IsSolid(int id)  { return Block(id)->solid; }
int IsOpaque(int id) { return Block(id)->opaque; }

int TileFor(int id, int face)
{
    const BlockDef *d = Block(id);
    /* the pumpkin shows its carved face on +Z so it reads as a "front" */
    if (id == BL_PUMPKIN && face == F_SOUTH) return TILE_PUMPKIN_FACE;
    if (face == F_TOP)    return d->tileTop;
    if (face == F_BOTTOM) return d->tileBottom;
    return d->tileSide;
}

const char *BlockName(int id) { return Block(id)->name; }
