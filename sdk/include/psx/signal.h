/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX signals (IEEE Std 1003.1-1990, s.3.3)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

typedef unsigned long sigset_t;

#ifndef _SIG_ATOMIC_T_DEFINED
typedef int sig_atomic_t;
#define _SIG_ATOMIC_T_DEFINED
#endif

//
// Special signal-handler values (note SIG_DFL == (void*)-1 on NT POSIX).
//
#define SIG_DFL  ((void (*)(int)) -1)   // default action  (0xFFFFFFFF)
#define SIG_ERR  ((void (*)(int))  0)   // error return
#define SIG_IGN  ((void (*)(int))  1)   // ignore signal

//
// Signal numbers (NT 4.0 POSIX numbering)
//
#define SIGABRT  1
#define SIGALRM  2
#define SIGFPE   3
#define SIGHUP   4
#define SIGILL   5
#define SIGINT   6
#define SIGKILL  7
#define SIGPIPE  8
#define SIGQUIT  9
#define SIGSEGV  10
#define SIGTERM  11
#define SIGUSR1  12
#define SIGUSR2  13
#define SIGCHLD  14
#define SIGCONT  15
#define SIGSTOP  16
#define SIGTSTP  17
#define SIGTTIN  18
#define SIGTTOU  19

struct sigaction
{
    void     (*sa_handler)(int);    // handler, SIG_DFL, or SIG_IGN
    sigset_t sa_mask;               // signals blocked during the handler
    int      sa_flags;              // SA_* flags
};

#define SA_NOCLDSTOP 0x00000001

//
// "how" values for sigprocmask()
//
#define SIG_BLOCK    1
#define SIG_UNBLOCK  2
#define SIG_SETMASK  3

int kill(pid_t Pid, int Signal);
int sigemptyset(sigset_t *Set);
int sigfillset(sigset_t *Set);
int sigaddset(sigset_t *Set, int Signal);
int sigdelset(sigset_t *Set, int Signal);
int sigismember(const sigset_t *Set, int Signal);
int sigaction(int Signal, const struct sigaction *Action, struct sigaction *OldAction);
int sigprocmask(int How, const sigset_t *Set, sigset_t *OldSet);
int sigpending(sigset_t *Set);
int sigsuspend(const sigset_t *Mask);

void (*signal(int Signal, void (*Handler)(int)))(int);
int raise(int Signal);
