/* Deterministic integer-hash value noise used by worldgen and texturing. */
#ifndef NOISE_H
#define NOISE_H

unsigned int Hash2(int x, int y);
unsigned int Hash3(int x, int y, int z);
double HashUnit2(int x, int z);          /* lattice hash -> [0,1)            */
double ValueNoise2(double x, double z);  /* smooth 2D value noise -> [0,1]   */
double ValueNoise3(double x, double y, double z);
double Fbm2(double x, double z, int octaves);  /* normalized fractal sum     */

#endif /* NOISE_H */
