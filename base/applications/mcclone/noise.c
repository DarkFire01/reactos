#include "noise.h"
#include <math.h>

/* All arithmetic is done unsigned so signed overflow is never invoked. */
unsigned int Hash2(int x, int y)
{
    unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

unsigned int Hash3(int x, int y, int z)
{
    unsigned int h = (unsigned int)x * 374761393u
                   + (unsigned int)y * 668265263u
                   + (unsigned int)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    h = (h ^ (h >> 9)) * 2246822519u;
    return h ^ (h >> 15);
}

static double Smooth(double t) { return t * t * (3.0 - 2.0 * t); }
static double Lerp(double a, double b, double t) { return a + (b - a) * t; }

double HashUnit2(int x, int z) { return (double)(Hash2(x, z) & 0xffff) / 65535.0; }
static double HashUnit3(int x, int y, int z) { return (double)(Hash3(x, y, z) & 0xffff) / 65535.0; }

double ValueNoise2(double x, double z)
{
    int xi = (int)floor(x), zi = (int)floor(z);
    double xf = x - xi, zf = z - zi;
    double v00 = HashUnit2(xi,     zi);
    double v10 = HashUnit2(xi + 1, zi);
    double v01 = HashUnit2(xi,     zi + 1);
    double v11 = HashUnit2(xi + 1, zi + 1);
    double sx = Smooth(xf), sz = Smooth(zf);
    return Lerp(Lerp(v00, v10, sx), Lerp(v01, v11, sx), sz);
}

double ValueNoise3(double x, double y, double z)
{
    int xi = (int)floor(x), yi = (int)floor(y), zi = (int)floor(z);
    double xf = x - xi, yf = y - yi, zf = z - zi;
    double sx = Smooth(xf), sy = Smooth(yf), sz = Smooth(zf);
    double c000 = HashUnit3(xi,     yi,     zi),     c100 = HashUnit3(xi + 1, yi,     zi);
    double c010 = HashUnit3(xi,     yi + 1, zi),     c110 = HashUnit3(xi + 1, yi + 1, zi);
    double c001 = HashUnit3(xi,     yi,     zi + 1), c101 = HashUnit3(xi + 1, yi,     zi + 1);
    double c011 = HashUnit3(xi,     yi + 1, zi + 1), c111 = HashUnit3(xi + 1, yi + 1, zi + 1);
    double x00 = Lerp(c000, c100, sx), x10 = Lerp(c010, c110, sx);
    double x01 = Lerp(c001, c101, sx), x11 = Lerp(c011, c111, sx);
    double y0 = Lerp(x00, x10, sy), y1 = Lerp(x01, x11, sy);
    return Lerp(y0, y1, sz);
}

double Fbm2(double x, double z, int octaves)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
    int i;
    for (i = 0; i < octaves; i++)
    {
        sum  += ValueNoise2(x * freq, z * freq) * amp;
        norm += amp;
        amp  *= 0.5;
        freq *= 2.0;
    }
    return (norm > 0.0) ? sum / norm : 0.0;
}
