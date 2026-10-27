#define PACKAGE_NAME "vkd3d"
#define PACKAGE_STRING "vkd3d 2.1"
#define PACKAGE_VERSION "2.1"
#ifdef __REACTOS__
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#endif
#define SONAME_LIBVULKAN "vulkan-1.dll"
#ifdef __REACTOS__
/* ReactOS msvcrt has _strtod_l() but no _strtof_l() */
#define HAVE__STRTOD_L 1
#else
#define HAVE__STRTOF_L 1
#endif
