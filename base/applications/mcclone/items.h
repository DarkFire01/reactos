/* Item registry.  Block items reuse the block id (1..BL_COUNT-1); tool items
 * live above TOOL_BASE.  Also holds the mining-speed / drop rules. */
#ifndef ITEMS_H
#define ITEMS_H

#include "blocks.h"

#define TOOL_BASE 64
enum {
    IT_PICK_WOOD = TOOL_BASE, IT_PICK_STONE, IT_PICK_IRON, IT_PICK_DIAMOND,
    IT_AXE_WOOD, IT_AXE_STONE, IT_AXE_IRON, IT_AXE_DIAMOND,
    IT_SHOVEL_WOOD, IT_SHOVEL_STONE, IT_SHOVEL_IRON, IT_SHOVEL_DIAMOND,
    IT_SWORD_WOOD, IT_SWORD_STONE, IT_SWORD_IRON, IT_SWORD_DIAMOND,
    IT_MAX
};

enum { TOOL_PICK = 0, TOOL_AXE, TOOL_SHOVEL, TOOL_SWORD, TOOL_NONE };

#define FOOD_BASE 96
enum { IT_APPLE = FOOD_BASE, IT_BREAD, IT_PORK_RAW, IT_PORK_COOKED, IT_FOOD_MAX };

int         ItemIsBlock(int item);
int         ItemIsTool(int item);
int         ItemIsFood(int item);
int         FoodValue(int item);                 /* hunger points restored */
int         ItemMaxStack(int item);
int         ItemIcon(int item);                  /* atlas tile for the HUD */
const char *ItemName(int item);
int         ToolType(int item);                  /* TOOL_* (TOOL_NONE if not a tool) */
int         ToolTier(int item);                  /* 0..3, or -1 */
void        ToolTierColor(int tier, float *r, float *g, float *b);

int         PreferredTool(int block);            /* TOOL_* best for this block */
double      MiningTime(int block, int heldItem); /* seconds to break (huge if unbreakable) */
int         BlockDrop(int block);                /* item dropped, BL_AIR for none */

#endif /* ITEMS_H */
