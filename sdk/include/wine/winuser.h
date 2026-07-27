
#pragma once

#include <psdk/winuser.h>

/* Wine's <winuser.h> declares these publicly, while ReactOS keeps them in
   <reactos/undocuser.h>. Provide them here so Wine sources -- which are
   compiled with sdk/include/wine ahead of the PSDK -- build unmodified. */

#ifndef DCX_USESTYLE
#define DCX_USESTYLE     0x00010000
#endif

#ifndef DCX_NORECOMPUTE
#define DCX_NORECOMPUTE  0x00100000
#endif
