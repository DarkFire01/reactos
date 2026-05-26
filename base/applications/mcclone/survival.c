#include "mc.h"
#include "survival.h"
#include "player.h"
#include "world.h"
#include "blocks.h"
#include "items.h"

#define DAY_SECONDS 360.0     /* a full day/night cycle */

int g_health = MAX_HEALTH;
int g_hunger = MAX_HUNGER;

static double g_time;         /* time of day, 0..1 (0.25 == noon) */
static int    g_dead;
static double g_deathTimer;

/* timers */
static double g_hungerTimer, g_regenTimer, g_starveTimer, g_lavaTimer, g_drownTimer;
static double g_air;          /* breath, seconds */

/* fall tracking */
static int    g_airborne;
static double g_fallPeakY;

void SurvivalInit(void)
{
    g_health = MAX_HEALTH;
    g_hunger = MAX_HUNGER;
    g_time = 0.25;            /* start at noon */
    g_dead = 0;
    g_deathTimer = 0;
    g_hungerTimer = g_regenTimer = g_starveTimer = g_lavaTimer = g_drownTimer = 0;
    g_air = 10.0;
    g_airborne = 0;
    g_fallPeakY = 0;
}

static double SunHeight(void) { return sin(g_time * 2.0 * M_PI); }

double SurvivalDayLight(void)
{
    double b = 0.5 + 0.7 * SunHeight();
    if (b < 0.12) b = 0.12;
    if (b > 1.0)  b = 1.0;
    return b;
}

static double Clampf(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void SurvivalSkyColor(float *r, float *g, float *b)
{
    double sun = SunHeight();
    double mix = Clampf((SurvivalDayLight() - 0.12) / (1.0 - 0.12), 0.0, 1.0);
    double dr = 0.55, dg = 0.72, db = 0.95;     /* day   */
    double nr = 0.03, ng = 0.04, nb = 0.10;     /* night */
    double sr = nr + (dr - nr) * mix;
    double sg = ng + (dg - ng) * mix;
    double sb = nb + (db - nb) * mix;
    double horizon = Clampf(1.0 - fabs(sun) / 0.30, 0.0, 1.0) * 0.5;  /* sunrise/set glow */
    *r = (float)(sr + (0.95 - sr) * horizon);
    *g = (float)(sg + (0.55 - sg) * horizon);
    *b = (float)(sb + (0.25 - sb) * horizon);
}

void SurvivalDamage(int n)
{
    if (g_dead) return;
    g_health -= n;
    if (g_health < 0) g_health = 0;
}

int SurvivalEat(int item)
{
    if (!ItemIsFood(item) || g_hunger >= MAX_HUNGER) return 0;
    g_hunger += FoodValue(item);
    if (g_hunger > MAX_HUNGER) g_hunger = MAX_HUNGER;
    return 1;
}

int SurvivalDead(void) { return g_dead; }

static void Environment(double dt)
{
    double ex = g_player.pos.x, ez = g_player.pos.z;
    double eyeY = g_player.pos.y + 1.5, feetY = g_player.pos.y + 0.1;
    int headB = BlockAt((int)floor(ex), (int)floor(eyeY), (int)floor(ez));
    int feetB = BlockAt((int)floor(ex), (int)floor(feetY), (int)floor(ez));

    /* lava burns */
    if (headB == BL_LAVA || feetB == BL_LAVA)
    {
        g_lavaTimer += dt;
        while (g_lavaTimer >= 0.5) { SurvivalDamage(1); g_lavaTimer -= 0.5; }
    }
    else g_lavaTimer = 0;

    /* drowning */
    if (headB == BL_WATER)
    {
        g_air -= dt;
        if (g_air <= 0.0)
        {
            g_drownTimer += dt;
            while (g_drownTimer >= 1.0) { SurvivalDamage(1); g_drownTimer -= 1.0; }
        }
    }
    else { g_air = 10.0; g_drownTimer = 0; }
}

static void Falling(double dt)
{
    (void)dt;
    if (g_player.fly) { g_airborne = 0; return; }
    if (g_player.onGround)
    {
        if (g_airborne)
        {
            double dist = g_fallPeakY - g_player.pos.y;
            if (dist > 3.0) SurvivalDamage((int)(dist - 3.0));
            g_airborne = 0;
        }
    }
    else
    {
        if (!g_airborne) { g_airborne = 1; g_fallPeakY = g_player.pos.y; }
        else if (g_player.pos.y > g_fallPeakY) g_fallPeakY = g_player.pos.y;
    }
}

static void Metabolism(double dt)
{
    g_hungerTimer += dt;
    while (g_hungerTimer >= 14.0) { if (g_hunger > 0) g_hunger--; g_hungerTimer -= 14.0; }

    if (g_hunger >= 18 && g_health < MAX_HEALTH)
    {
        g_regenTimer += dt;
        while (g_regenTimer >= 2.0) { if (g_health < MAX_HEALTH) g_health++; g_regenTimer -= 2.0; }
    }
    else g_regenTimer = 0;

    if (g_hunger == 0)
    {
        g_starveTimer += dt;
        while (g_starveTimer >= 2.0) { SurvivalDamage(1); g_starveTimer -= 2.0; }
    }
    else g_starveTimer = 0;
}

void SurvivalUpdate(double dt)
{
    g_time += dt / DAY_SECONDS;
    if (g_time >= 1.0) g_time -= 1.0;

    if (g_dead)
    {
        g_deathTimer += dt;
        if (g_deathTimer >= 2.0)
        {
            PlayerSpawn();
            g_health = MAX_HEALTH;
            g_hunger = MAX_HUNGER;
            g_dead = 0;
            g_deathTimer = 0;
            g_air = 10.0;
        }
        return;
    }

    Falling(dt);
    Environment(dt);
    Metabolism(dt);

    if (g_health <= 0) { g_dead = 1; g_deathTimer = 0; }
}
