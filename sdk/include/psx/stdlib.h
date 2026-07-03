/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Standard C general utilities (<stdlib.h>) for the POSIX userland.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>      /* size_t */

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1
#define RAND_MAX        0x7fff
#define MB_CUR_MAX      1

void   *malloc(size_t Size);
void   *calloc(size_t Count, size_t Size);
void   *realloc(void *Ptr, size_t Size);
void    free(void *Ptr);

void    exit(int Status);
void    abort(void);
int     atexit(void (*Function)(void));

int     atoi(const char *String);
long    atol(const char *String);
double  atof(const char *String);
long    strtol(const char *String, char **End, int Base);
unsigned long strtoul(const char *String, char **End, int Base);
double  strtod(const char *String, char **End);

char   *getenv(const char *Name);
int     putenv(char *String);
int     system(const char *Command);

int     abs(int Value);
long    labs(long Value);
int     rand(void);
void    srand(unsigned int Seed);

void    qsort(void *Base, size_t Count, size_t Size,
              int (*Compare)(const void *, const void *));
void   *bsearch(const void *Key, const void *Base, size_t Count, size_t Size,
                int (*Compare)(const void *, const void *));

/* MSVC-ism the reskit uses; normally from <stdlib.h>. */
#define __max(a,b) (((a) > (b)) ? (a) : (b))
#define __min(a,b) (((a) < (b)) ? (a) : (b))
