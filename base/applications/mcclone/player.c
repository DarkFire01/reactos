#include "mc.h"
#include "player.h"
#include "world.h"
#include "blocks.h"

Player g_player;

#define EYE_HEIGHT  1.62
#define PLAYER_HALF 0.30
#define PLAYER_TOP  1.80
#define GRAVITY     22.0
#define JUMP_VEL    8.2
#define WALK_SPEED  4.6
#define FLY_SPEED   11.0

void PlayerSpawn(void)
{
    int sx = WORLD_X / 2, sz = WORLD_Z / 2;
    int h = SurfaceHeight(sx, sz);
    if (h < SEA_LEVEL) h = SEA_LEVEL;
    g_player.pos.x = sx + 0.5;
    g_player.pos.z = sz + 0.5;
    g_player.pos.y = h + 2.0;
    g_player.vel.x = g_player.vel.y = g_player.vel.z = 0.0;
    g_player.yaw = 45.0;
    g_player.pitch = -20.0;
    g_player.fly = 1;
    g_player.onGround = 0;
}

void PlayerLookDir(double *fx, double *fy, double *fz)
{
    double cy = cos(DEG2RAD(g_player.yaw)),  sy = sin(DEG2RAD(g_player.yaw));
    double cp = cos(DEG2RAD(g_player.pitch)), sp = sin(DEG2RAD(g_player.pitch));
    *fx = cp * sy;
    *fy = sp;
    *fz = -cp * cy;
}

static int HitsAt(Vec3 p)
{
    int x0 = (int)floor(p.x - PLAYER_HALF), x1 = (int)floor(p.x + PLAYER_HALF);
    int z0 = (int)floor(p.z - PLAYER_HALF), z1 = (int)floor(p.z + PLAYER_HALF);
    int y0 = (int)floor(p.y),               y1 = (int)floor(p.y + PLAYER_TOP);
    int x, y, z;
    for (x = x0; x <= x1; x++)
        for (z = z0; z <= z1; z++)
            for (y = y0; y <= y1; y++)
                if (IsSolid(BlockAt(x, y, z))) return 1;
    return 0;
}

int PlayerBlocked(void) { return HitsAt(g_player.pos); }

/* Move one axis at a time so we slide along walls. */
static void MoveAxis(double dx, double dy, double dz)
{
    Vec3 np = g_player.pos;
    np.x += dx; np.y += dy; np.z += dz;
    if (!HitsAt(np)) { g_player.pos = np; return; }
    if (dy < 0) g_player.onGround = 1;
    if (dy != 0.0) g_player.vel.y = 0.0;
}

void PlayerUpdate(double dt)
{
    double speed = g_player.fly ? FLY_SPEED : WALK_SPEED;
    double cy = cos(DEG2RAD(g_player.yaw)), sy = sin(DEG2RAD(g_player.yaw));
    double fwdx = sy, fwdz = -cy, rgtx = cy, rgtz = sy;
    double mvx = 0, mvz = 0, l;

    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) g_player.yaw   -= 90.0 * dt;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_player.yaw   += 90.0 * dt;
    if (GetAsyncKeyState(VK_UP)    & 0x8000) g_player.pitch += 75.0 * dt;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) g_player.pitch -= 75.0 * dt;
    if (g_player.pitch >  89.0) g_player.pitch =  89.0;
    if (g_player.pitch < -89.0) g_player.pitch = -89.0;

    if (GetAsyncKeyState('W') & 0x8000) { mvx += fwdx; mvz += fwdz; }
    if (GetAsyncKeyState('S') & 0x8000) { mvx -= fwdx; mvz -= fwdz; }
    if (GetAsyncKeyState('D') & 0x8000) { mvx += rgtx; mvz += rgtz; }
    if (GetAsyncKeyState('A') & 0x8000) { mvx -= rgtx; mvz -= rgtz; }
    l = sqrt(mvx * mvx + mvz * mvz);
    if (l > 1e-6) { mvx = mvx / l * speed * dt; mvz = mvz / l * speed * dt; }

    MoveAxis(mvx, 0, 0);
    MoveAxis(0, 0, mvz);

    if (g_player.fly)
    {
        double vy = 0;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) vy += speed * dt;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) vy -= speed * dt;
        g_player.vel.y = 0;
        MoveAxis(0, vy, 0);
    }
    else
    {
        g_player.vel.y -= GRAVITY * dt;
        if (g_player.vel.y < -55.0) g_player.vel.y = -55.0;
        g_player.onGround = 0;
        MoveAxis(0, g_player.vel.y * dt, 0);
        if (g_player.onGround && (GetAsyncKeyState(VK_SPACE) & 0x8000))
            g_player.vel.y = JUMP_VEL;
    }
}

int PlayerRayPick(int *hx, int *hy, int *hz, int *px, int *py, int *pz)
{
    double ex = g_player.pos.x, ey = g_player.pos.y + EYE_HEIGHT, ez = g_player.pos.z;
    double fx, fy, fz, t;
    int lx = (int)floor(ex), ly = (int)floor(ey), lz = (int)floor(ez);
    PlayerLookDir(&fx, &fy, &fz);
    *px = lx; *py = ly; *pz = lz;
    for (t = 0.0; t < 6.0; t += 0.02)
    {
        int cx = (int)floor(ex + fx * t);
        int cy = (int)floor(ey + fy * t);
        int cz = (int)floor(ez + fz * t);
        if (cx == lx && cy == ly && cz == lz) continue;
        if (IsSolid(BlockAt(cx, cy, cz))) { *hx = cx; *hy = cy; *hz = cz; return 1; }
        *px = cx; *py = cy; *pz = cz;
        lx = cx; ly = cy; lz = cz;
    }
    return 0;
}
