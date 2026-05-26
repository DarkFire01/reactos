#include "mc.h"
#include "game.h"
#include "world.h"
#include "mesh.h"
#include "blocks.h"
#include "player.h"
#include "items.h"
#include "inventory.h"
#include "survival.h"

/* current crosshair target (solid block) and the empty cell in front of it */
static int    g_aimValid = 0;
static int    g_aimX, g_aimY, g_aimZ;
static int    g_placeX, g_placeY, g_placeZ;

/* mining progress on the aimed block */
static int    g_mineX = -9999, g_mineY, g_mineZ;
static double g_mineProgress = 0.0;
static double g_mineNeed = 0.0;

void GameInit(void)
{
    InventoryInit();
    g_aimValid = 0;
    g_mineProgress = 0.0;
    g_mineX = -9999;
}

int GameAim(int *x, int *y, int *z)
{
    if (!g_aimValid) return 0;
    *x = g_aimX; *y = g_aimY; *z = g_aimZ;
    return 1;
}

double GameMineFraction(void)
{
    if (!g_aimValid || g_mineNeed <= 0.0) return 0.0;
    if (g_mineX != g_aimX || g_mineY != g_aimY || g_mineZ != g_aimZ) return 0.0;
    return (g_mineProgress >= g_mineNeed) ? 1.0 : (g_mineProgress / g_mineNeed);
}

static void BreakAimed(void)
{
    int b = BlockAt(g_aimX, g_aimY, g_aimZ);
    int drop;
    if (b == BL_BEDROCK || b == BL_AIR) return;
    drop = BlockDrop(b);
    if (drop != BL_AIR) InventoryAdd(drop, 1);
    SetBlock(g_aimX, g_aimY, g_aimZ, BL_AIR);
    RebuildAround(g_aimX, g_aimZ);
    g_mineProgress = 0.0;
    g_mineX = -9999;
}

void GameUpdate(double dt)
{
    int hx, hy, hz, px, py, pz;
    int breaking = (GetAsyncKeyState('Q') & 0x8000) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);

    g_aimValid = PlayerRayPick(&hx, &hy, &hz, &px, &py, &pz);
    if (g_aimValid)
    {
        g_aimX = hx; g_aimY = hy; g_aimZ = hz;
        g_placeX = px; g_placeY = py; g_placeZ = pz;
    }

    if (!breaking || !g_aimValid)
    {
        g_mineProgress = 0.0;
        g_mineX = -9999;
        return;
    }

    /* reset progress if the aimed block changed */
    if (g_mineX != g_aimX || g_mineY != g_aimY || g_mineZ != g_aimZ)
    {
        g_mineX = g_aimX; g_mineY = g_aimY; g_mineZ = g_aimZ;
        g_mineProgress = 0.0;
        g_mineNeed = MiningTime(BlockAt(g_aimX, g_aimY, g_aimZ), InventorySelectedItem());
    }

    if (g_mineNeed >= 1.0e8) return;       /* unbreakable */
    g_mineProgress += dt;
    if (g_mineProgress >= g_mineNeed) BreakAimed();
}

void GamePlace(void)
{
    int item = InventorySelectedItem();
    if (!ItemIsBlock(item)) return;        /* tools/food don't place */
    if (!g_aimValid) return;
    if (BlockAt(g_placeX, g_placeY, g_placeZ) != BL_AIR) return;
    SetBlock(g_placeX, g_placeY, g_placeZ, (unsigned char)item);
    if (PlayerBlocked()) { SetBlock(g_placeX, g_placeY, g_placeZ, BL_AIR); return; }
    InventoryConsumeSelected();
    RebuildAround(g_placeX, g_placeZ);
}

/* Right-click / E: eat the held food, otherwise place the held block. */
void GameUse(void)
{
    int item = InventorySelectedItem();
    if (ItemIsFood(item))
    {
        if (SurvivalEat(item)) InventoryConsumeSelected();
        return;
    }
    GamePlace();
}
