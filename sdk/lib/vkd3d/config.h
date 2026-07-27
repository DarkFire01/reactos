#define PACKAGE_NAME "vkd3d"
#define PACKAGE_STRING "vkd3d 1.14"
#define PACKAGE_VERSION "1.14"
/* ReactOS' <limits.h> already provides PATH_MAX with the Win32 value (259).
   Pull it in first so we defer to it instead of fighting over the macro;
   the fallback only applies on platforms that do not define it at all. */
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SONAME_LIBVULKAN "vulkan-1.dll"
