/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX process-wait support (IEEE Std 1003.1-1990).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

//
// wait options
//
#define WNOHANG   0x00000001
#define WUNTRACED 0x00000002

//
// Wait status macros. Status layout: bits 0-7 = signal/stop indicator,
// bits 8-15 = exit status or signal number.
//
#define WIFEXITED(stat_val)    (((stat_val) & 0xff) == 0)
#define WEXITSTATUS(stat_val)  (((stat_val) & 0xff00) >> 8)
#define WIFSTOPPED(stat_val)   (((stat_val) & 0xff) == 0177)
#define WIFSIGNALED(stat_val)  (!WIFSTOPPED(stat_val) && !WIFEXITED(stat_val))
#define WTERMSIG(stat_val)     ((stat_val) & 0xff)
#define WSTOPSIG(stat_val)     (((stat_val) & 0xff00) >> 8)

pid_t wait(int *StatLoc);
pid_t waitpid(pid_t Pid, int *StatLoc, int Options);
