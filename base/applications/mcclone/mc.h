/*
 * PROJECT:     ReactCraft - a Minecraft-style voxel sandbox for the xboxogl ICD
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Shared types and world constants used across every module.
 *
 * The whole game is fixed-function OpenGL 1.1 (immediate mode + display lists);
 * no shaders, VBOs, GLU, alpha test or stencil (the NV2A ICD lacks those).
 */
#ifndef MC_H
#define MC_H

#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((d) * (M_PI / 180.0))

/* World dimensions. The world is a fixed brick of voxels (no streaming) sized
 * to what the emulated NV2A can mesh into display lists at a sane frame rate. */
#define CHUNK     16                  /* blocks per chunk edge (X and Z)      */
#define CHUNKS_X  4                    /* 64x64 footprint - bump for a bigger  */
#define CHUNKS_Z  4                    /* world once NV2A frame rate is known  */
#define WORLD_X   (CHUNK * CHUNKS_X)
#define WORLD_Z   (CHUNK * CHUNKS_Z)
#define WORLD_H   64
#define SEA_LEVEL 16

/* Cube face indices, shared by the block table and the mesher. */
enum { F_TOP, F_BOTTOM, F_NORTH, F_SOUTH, F_EAST, F_WEST };

typedef struct { double x, y, z; } Vec3;

#endif /* MC_H */
