/* POSIX <sys/time.h> -- timeval/timezone (in sys/types.h) + gettimeofday/utimes. MIT. */
#pragma once
#include <sys/types.h>
int gettimeofday(struct timeval *tv, struct timezone *tz);
int utimes(const char *path, const struct timeval *times);
