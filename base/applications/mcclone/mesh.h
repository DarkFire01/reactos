/* Turns the voxel grid into per-chunk display lists and draws them. */
#ifndef MESH_H
#define MESH_H

void MeshInit(void);            /* allocate the display lists (needs a GL ctx) */
void BuildAllChunks(void);
void RebuildAround(int x, int z); /* rebuild the chunk at (x,z) and neighbours */
void DrawChunks(void);          /* opaque solids pass                          */
void DrawWater(void);           /* translucent water pass                      */

#endif /* MESH_H */
