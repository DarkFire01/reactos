/* First-person player: position, look, movement+collision, and a voxel ray
 * pick used for breaking and placing blocks. */
#ifndef PLAYER_H
#define PLAYER_H

#include "mc.h"

typedef struct {
    Vec3   pos;        /* feet (AABB base centre) */
    Vec3   vel;
    double yaw, pitch; /* degrees; yaw 0 looks toward -Z, pitch + looks up */
    int    fly;
    int    onGround;
} Player;

extern Player g_player;

void PlayerSpawn(void);
void PlayerLookDir(double *fx, double *fy, double *fz);
void PlayerUpdate(double dt);   /* reads the keyboard, moves + collides */
int  PlayerBlocked(void);       /* does the AABB currently overlap a solid? */
int  PlayerRayPick(int *hx, int *hy, int *hz, int *px, int *py, int *pz);

#endif /* PLAYER_H */
