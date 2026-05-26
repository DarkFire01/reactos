/* The voxel grid and its procedural generator. */
#ifndef WORLD_H_INCLUDED
#define WORLD_H_INCLUDED

unsigned char BlockAt(int x, int y, int z);
void          SetBlock(int x, int y, int z, unsigned char b);
int           InBounds(int x, int y, int z);
int           SurfaceHeight(int x, int z);   /* terrain column height (no caves) */
void          GenerateWorld(void);

#endif /* WORLD_H_INCLUDED */
