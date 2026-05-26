#include "mc.h"
#include "items.h"
#include "blocks.h"
#include "texture.h"

int ItemIsBlock(int item) { return item > BL_AIR && item < BL_COUNT; }
int ItemIsTool(int item)  { return item >= TOOL_BASE && item < IT_MAX; }
int ItemIsFood(int item)  { return item >= FOOD_BASE && item < IT_FOOD_MAX; }

int FoodValue(int item)
{
    switch (item)
    {
        case IT_APPLE:       return 4;
        case IT_BREAD:       return 5;
        case IT_PORK_RAW:    return 3;
        case IT_PORK_COOKED: return 8;
        default:             return 0;
    }
}

int ItemMaxStack(int item) { return ItemIsTool(item) ? 1 : 64; }

int ToolType(int item) { return ItemIsTool(item) ? (item - TOOL_BASE) / 4 : TOOL_NONE; }
int ToolTier(int item) { return ItemIsTool(item) ? (item - TOOL_BASE) % 4 : -1; }

void ToolTierColor(int tier, float *r, float *g, float *b)
{
    switch (tier)
    {
        case 0: *r = 0.55f; *g = 0.40f; *b = 0.25f; break; /* wood    */
        case 1: *r = 0.62f; *g = 0.62f; *b = 0.64f; break; /* stone   */
        case 2: *r = 0.86f; *g = 0.83f; *b = 0.78f; break; /* iron    */
        case 3: *r = 0.40f; *g = 0.90f; *b = 0.90f; break; /* diamond */
        default: *r = *g = *b = 1.0f; break;
    }
}

int ItemIcon(int item)
{
    if (ItemIsTool(item))
    {
        switch (ToolType(item))
        {
            case TOOL_PICK:   return TILE_PICK;
            case TOOL_AXE:    return TILE_AXE;
            case TOOL_SHOVEL: return TILE_SHOVEL;
            case TOOL_SWORD:  return TILE_SWORD;
        }
    }
    switch (item)
    {
        case IT_APPLE:                          return TILE_APPLE;
        case IT_BREAD:                          return TILE_BREAD;
        case IT_PORK_RAW: case IT_PORK_COOKED:  return TILE_MEAT;
    }
    if (ItemIsBlock(item)) return TileFor(item, F_SOUTH);
    return TILE_STONE;
}

const char *ItemName(int item)
{
    static const char *tier[4] = { "Wood", "Stone", "Iron", "Diamond" };
    static const char *type[4] = { "Pickaxe", "Axe", "Shovel", "Sword" };
    static char buf[40];
    if (ItemIsBlock(item)) return BlockName(item);
    if (ItemIsTool(item))
    {
        _snprintf(buf, sizeof(buf) - 1, "%s %s", tier[ToolTier(item)], type[ToolType(item)]);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    switch (item)
    {
        case IT_APPLE:       return "Apple";
        case IT_BREAD:       return "Bread";
        case IT_PORK_RAW:    return "Raw Porkchop";
        case IT_PORK_COOKED: return "Cooked Porkchop";
    }
    return "";
}

int PreferredTool(int block)
{
    switch (block)
    {
        case BL_STONE: case BL_COBBLE: case BL_SANDSTONE: case BL_BRICK:
        case BL_OBSIDIAN: case BL_MOSSY: case BL_ICE:
        case BL_COAL_ORE: case BL_IRON_ORE: case BL_GOLD_ORE:
        case BL_DIAMOND_ORE: case BL_REDSTONE_ORE: case BL_GLOWSTONE:
            return TOOL_PICK;
        case BL_LOG: case BL_PLANKS: case BL_PUMPKIN:
            return TOOL_AXE;
        case BL_DIRT: case BL_GRASS: case BL_SAND: case BL_GRAVEL: case BL_SNOWY:
            return TOOL_SHOVEL;
        default:
            return TOOL_NONE;
    }
}

double MiningTime(int block, int heldItem)
{
    double h = Block(block)->hardness;
    double base;
    int pref;
    if (h < 0.0) return 1.0e9;          /* bedrock / fluids: unbreakable */
    base = h * 1.5 + 0.05;
    pref = PreferredTool(block);
    if (ItemIsTool(heldItem) && pref != TOOL_NONE && ToolType(heldItem) == pref)
        base /= (double)(ToolTier(heldItem) + 2);  /* wood /2 .. diamond /5 */
    return (base < 0.08) ? 0.0 : base;
}

int BlockDrop(int block)
{
    switch (block)
    {
        case BL_STONE:               return BL_COBBLE;
        case BL_GRASS: case BL_SNOWY:return BL_DIRT;
        case BL_LEAVES:              return BL_AIR;     /* nothing */
        default:                     return block;      /* drops itself */
    }
}
