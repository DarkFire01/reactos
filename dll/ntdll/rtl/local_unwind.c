/*
 * Minimal unwind helper used by kernel32 on ARM64 MSVC builds.
 */
#if defined(_M_ARM64)

void __cdecl _local_unwind(void)
{
}

#endif
