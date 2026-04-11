/*
 * MSVC ARM64: use CRT _logb (math/arm64/_logb.s); /X build has no C99 logb().
 */
#ifdef _M_ARM64

double __cdecl _logb(double);

float __cdecl _logbf(float x)
{
    return (float)_logb((double)x);
}

#endif /* _M_ARM64 */
