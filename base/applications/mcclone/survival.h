/* Survival state: health, hunger, the day/night clock, and the damage rules. */
#ifndef SURVIVAL_H
#define SURVIVAL_H

#define MAX_HEALTH 20
#define MAX_HUNGER 20

extern int g_health;   /* 0..20 (2 per heart)     */
extern int g_hunger;   /* 0..20 (2 per drumstick) */

void   SurvivalInit(void);
void   SurvivalUpdate(double dt);
int    SurvivalDead(void);
double SurvivalDayLight(void);                 /* 0.12 (midnight) .. 1.0 (noon) */
void   SurvivalSkyColor(float *r, float *g, float *b);
void   SurvivalDamage(int n);
int    SurvivalEat(int item);                  /* 1 if the food was eaten */

#endif /* SURVIVAL_H */
