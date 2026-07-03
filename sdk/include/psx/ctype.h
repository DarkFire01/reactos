/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ASCII character classification (<ctype.h>)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

int isupper(int), islower(int), isdigit(int), isalpha(int), isalnum(int);
int isxdigit(int), isspace(int), isprint(int), isgraph(int), iscntrl(int), ispunct(int);
int toupper(int), tolower(int), isascii(int), toascii(int);
