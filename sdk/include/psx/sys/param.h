/* BSD <sys/param.h> -- the limits/macros the reskit userland expects. MIT. */
#pragma once
#include <sys/types.h>
#include <limits.h>
#ifndef MAXPATHLEN
#define MAXPATHLEN  260
#endif
#ifndef MAXBSIZE
#define MAXBSIZE    65536
#endif
#define MAXSYMLINKS 8
#define DEV_BSIZE   512
#define NBBY        8
#define howmany(x, y)  (((x) + ((y) - 1)) / (y))
#define roundup(x, y)  ((((x) + ((y) - 1)) / (y)) * (y))
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
