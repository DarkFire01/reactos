/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX symbolic constants and prototypes (IEEE Std 1003.1-1990).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

//
// Standard file descriptors
//
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

//
// access() mode bits (s.2.9.1)
//
#define F_OK    00
#define X_OK    01
#define W_OK    02
#define R_OK    04

//
// lseek() whence values (s.2.9.2)
//
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

//
// Compile-time symbolic constants (s.2.9.3 / 2.9.4)
//
#define _POSIX_JOB_CONTROL
#define _POSIX_VERSION          199009L
#define _POSIX_SAVED_IDS

#define _POSIX_CHOWN_RESTRICTED 1
#define _POSIX_NO_TRUNC         1
#define _POSIX_VDISABLE         0

//
// sysconf() 'name' values (s.4.8.1)
//
#define _SC_ARG_MAX         1
#define _SC_CHILD_MAX       2
#define _SC_CLK_TCK         3
#define _SC_NGROUPS_MAX     4
#define _SC_OPEN_MAX        5
#define _SC_JOB_CONTROL     6
#define _SC_SAVED_IDS       7
#define _SC_STREAM_MAX      8
#define _SC_TZNAME_MAX      9
#define _SC_VERSION         10

//
// pathconf()/fpathconf() 'name' values (s.5.7.1)
//
#define _PC_LINK_MAX            1
#define _PC_MAX_CANON           2
#define _PC_MAX_INPUT           3
#define _PC_NAME_MAX            4
#define _PC_PATH_MAX            5
#define _PC_PIPE_BUF            6
#define _PC_CHOWN_RESTRICTED    7
#define _PC_NO_TRUNC            8
#define _PC_VDISABLE            9

#ifndef NULL
#define NULL    ((void *)0)
#endif

//
// Process
//
pid_t fork(void);
int execl(const char *Path, const char *Arg0, ...);
int execv(const char *Path, char *const Argv[]);
int execle(const char *Path, const char *Arg0, ...);
int execve(const char *Path, char *const Argv[], char *const Envp[]);
int execlp(const char *File, const char *Arg0, ...);
int execvp(const char *File, char *const Argv[]);
void _exit(int Status);

unsigned int alarm(unsigned int Seconds);
int pause(void);
unsigned int sleep(unsigned int Seconds);

//
// Identity
//
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t Uid);
int setgid(gid_t Gid);
int getgroups(int GidSetSize, gid_t GroupList[]);
char *getlogin(void);
pid_t getpgrp(void);
pid_t setsid(void);
int setpgid(pid_t Pid, pid_t ProcessGroup);

struct utsname;
int uname(struct utsname *Name);

time_t time(time_t *Time);
char *getenv(const char *Name);
char *ctermid(char *String);
char *ttyname(int FileDescriptor);
int isatty(int FileDescriptor);

long sysconf(int Name);

//
// File system
//
int chdir(const char *Path);
char *getcwd(char *Buffer, size_t Size);
int link(const char *OldPath, const char *NewPath);
int unlink(const char *Path);
int rmdir(const char *Path);
int rename(const char *OldPath, const char *NewPath);
int access(const char *Path, int Mode);
int chown(const char *Path, uid_t Owner, gid_t Group);

struct utimbuf;
int utime(const char *Path, const struct utimbuf *Times);

long pathconf(const char *Path, int Name);
long fpathconf(int FileDescriptor, int Name);

//
// File descriptors
//
int pipe(int FileDescriptors[2]);
int dup(int FileDescriptor);
int dup2(int OldFd, int NewFd);
int close(int FileDescriptor);
ssize_t read(int FileDescriptor, void *Buffer, size_t Count);
ssize_t write(int FileDescriptor, const void *Buffer, size_t Count);
off_t lseek(int FileDescriptor, off_t Offset, int Whence);

char *cuserid(char *String);
