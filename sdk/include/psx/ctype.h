/* POSIX <ctype.h> -- ASCII classification (impl in libpsxcrt ctype.c). MIT. */
#pragma once
int isupper(int), islower(int), isdigit(int), isalpha(int), isalnum(int);
int isxdigit(int), isspace(int), isprint(int), isgraph(int), iscntrl(int), ispunct(int);
int toupper(int), tolower(int), isascii(int), toascii(int);
