/* Gameplay glue: aiming, held-to-mine with tool speeds, placing, and drops. */
#ifndef GAME_H
#define GAME_H

void   GameInit(void);
void   GameUpdate(double dt);          /* raycast + progress the held mining */
void   GamePlace(void);                /* place the selected block */
void   GameUse(void);                  /* eat held food, else place block */
int    GameAim(int *x, int *y, int *z);/* the solid block under the crosshair */
double GameMineFraction(void);         /* 0..1 progress on the current target */

#endif /* GAME_H */
