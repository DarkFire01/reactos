/*
 * UCRT / crtmath expect __acrt_initialize_fma3 for .CRT$XIC; ARM64 has no FMA3 lib path yet.
 */
#ifdef _M_ARM64

int __cdecl __acrt_initialize_fma3(void)
{
    return 0;
}

#endif /* _M_ARM64 */
