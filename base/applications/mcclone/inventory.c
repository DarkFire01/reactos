#include "inventory.h"
#include "items.h"
#include "blocks.h"

Slot g_inv[INV_TOTAL];
int  g_invSel = 0;

void InventoryInit(void)
{
    int i;
    for (i = 0; i < INV_TOTAL; i++) { g_inv[i].item = BL_AIR; g_inv[i].count = 0; }
    /* a starter kit of wooden tools, so the player can mine right away */
    g_inv[0].item = IT_PICK_WOOD;   g_inv[0].count = 1;
    g_inv[1].item = IT_AXE_WOOD;    g_inv[1].count = 1;
    g_inv[2].item = IT_SHOVEL_WOOD; g_inv[2].count = 1;
    g_inv[3].item = IT_SWORD_WOOD;  g_inv[3].count = 1;
    g_inv[4].item = IT_APPLE;       g_inv[4].count = 5;
    g_invSel = 0;
}

int InventoryAdd(int item, int count)
{
    int max = ItemMaxStack(item), i;
    if (item == BL_AIR || count <= 0) return 0;

    /* top up matching stacks first (hotbar before backpack) */
    for (i = 0; i < INV_TOTAL && count > 0; i++)
        if (g_inv[i].item == item && g_inv[i].count < max)
        {
            int room = max - g_inv[i].count;
            int add = (count < room) ? count : room;
            g_inv[i].count += add;
            count -= add;
        }
    /* then fill empty slots */
    for (i = 0; i < INV_TOTAL && count > 0; i++)
        if (g_inv[i].item == BL_AIR || g_inv[i].count == 0)
        {
            int add = (count < max) ? count : max;
            g_inv[i].item = item;
            g_inv[i].count = add;
            count -= add;
        }
    return count;   /* leftover */
}

void InventorySelect(int i)
{
    if (i >= 0 && i < INV_HOTBAR) g_invSel = i;
}

void InventoryScroll(int dir)
{
    g_invSel = (g_invSel + dir) % INV_HOTBAR;
    if (g_invSel < 0) g_invSel += INV_HOTBAR;
}

int InventorySelectedItem(void)
{
    if (g_inv[g_invSel].count <= 0) return BL_AIR;
    return g_inv[g_invSel].item;
}

void InventoryConsumeSelected(void)
{
    if (g_inv[g_invSel].count > 0)
    {
        g_inv[g_invSel].count--;
        if (g_inv[g_invSel].count == 0) g_inv[g_invSel].item = BL_AIR;
    }
}
