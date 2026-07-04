/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL client side of the pseudo-terminal layer: the SVR4 pty
 *              helpers (grantpt/unlockpt/ptsname) plus ioctl() and select(),
 *              which ride the PSX_API_IOCTL / PSX_API_SELECT extension opcodes.
 *              A terminal emulator (dtterm) opens "/dev/ptmx", ptsname()s the
 *              slave, forks a shell on "/dev/pts/N", and select()s the master.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"
#include <subsys/posix/psxext.h>

#define PSX_TIOCGPTN   0x80045430   // matches sdk/include/psx/termios.h

extern int __cdecl setpgid(int pid, int pgid);

//
// grantpt/unlockpt -- SVR4 slave setup. No ACL model on the subsystem, so both
// simply succeed (the pty pair is already usable after open("/dev/ptmx")).
//
int __cdecl grantpt(int FileDescriptor)  { (void)FileDescriptor; return 0; }
int __cdecl unlockpt(int FileDescriptor) { (void)FileDescriptor; return 0; }

//
// ioctl(fd, request, arg) -- PSX_API_IOCTL (0x41). Carries the fd, the request
// code, and the arg pointer; psxss serves the pty control set (winsize, pts
// index, termios). Non-pty fds get ENOTTY.
//
int __cdecl
ioctl(int FileDescriptor, unsigned long Request, void *Arg)
{
    PSX_API_MESSAGE Message;
    LONG Result;

    PsxInitMessage(&Message, PSX_API_IOCTL, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Request;
    ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Arg;
    Result = PsxCallServer(&Message);
    return (int)Result;             // 0 = ok, -1 = error (errno set by PsxCallServer)
}

//
// ptsname(fd) -- name the slave of a master fd. TIOCGPTN gives the index N;
// we format "/dev/pts/N" into a static buffer (the standard ptsname contract).
//
char * __cdecl
ptsname(int FileDescriptor)
{
    static char Buffer[24];
    unsigned int Index = 0;
    char Digits[12];
    int d = 0, i = 0;

    if (ioctl(FileDescriptor, PSX_TIOCGPTN, &Index) != 0)
        return (char *)0;

    // "/dev/pts/" prefix
    {
        const char *Prefix = "/dev/pts/";
        while (Prefix[i]) { Buffer[i] = Prefix[i]; i++; }
    }
    // decimal Index (reversed, then appended)
    do { Digits[d++] = (char)('0' + (Index % 10)); Index /= 10; } while (Index && d < 11);
    while (d > 0) Buffer[i++] = Digits[--d];
    Buffer[i] = '\0';
    return Buffer;
}

//
// select(nfds, readfds, writefds, exceptfds, timeout) -- PSX_API_SELECT (0x42).
// Read readiness is a real wait in psxss; write readiness is treated as always
// ready (pty/pipe buffers accept data). exceptfds is ignored. The fd_sets are the
// standard "unsigned long bit array" layout (bit i => fd i), matching X11's
// Xpoll.h and newlib -- we address them as ULONG words to stay header-agnostic.
//
struct psx_timeval { long tv_sec; long tv_usec; };

int __cdecl
select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout)
{
    PSX_API_MESSAGE Message;
    PULONG r = (PULONG)readfds;
    PULONG w = (PULONG)writefds;
    ULONG Fds[PSX_SELECT_MAXFDS];
    ULONG Count = 0, TimeoutMs, i;
    LONG Result;
    int Ready = 0;

    (void)exceptfds;
    if (nfds < 0) nfds = 0;
    if (nfds > PSX_SELECT_MAXFDS * 32) nfds = PSX_SELECT_MAXFDS * 32;

    // Gather the read fds into a compact array.
    for (i = 0; (i < (ULONG)nfds) && (Count < PSX_SELECT_MAXFDS); i++)
    {
        if (r && (r[i >> 5] & (1UL << (i & 31))))
            Fds[Count++] = i;
    }

    if (timeout == (void *)0)
        TimeoutMs = 0xFFFFFFFF;                 // block indefinitely
    else
    {
        struct psx_timeval *tv = (struct psx_timeval *)timeout;
        TimeoutMs = (ULONG)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    }

    // Wait CLIENT-side. The server does a single non-blocking poll (psxss is a
    // single-threaded LPC server -- blocking there stalls the whole subsystem), so we
    // re-issue it until a read fd is ready, a write fd is present (writes are always
    // ready), or our timeout elapses. This keeps psxss free to service other clients
    // while dtterm's Xt loop select()s in the background.
    {
        ULONG Waited = 0, HasWrite = 0;
        if (w)
            for (i = 0; i < (ULONG)nfds; i++)
                if (w[i >> 5] & (1UL << (i & 31))) { HasWrite = 1; break; }

        for (;;)
        {
            PsxInitMessage(&Message, PSX_API_SELECT, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
            ((PULONG)Message.Data.Raw)[0] = Count;
            ((PULONG)Message.Data.Raw)[1] = 0;      // wait is client-side now (unused)
            ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Fds;
            Result = PsxCallServer(&Message);       // ready bitmask (immediate), -1 = error
            if (Result < 0)
                return -1;
            if ((Result != 0) || HasWrite)
                break;                              // a read fd is ready, or writes are
            if (TimeoutMs == 0)
                break;                              // non-blocking select
            if ((TimeoutMs != 0xFFFFFFFF) && (Waited >= TimeoutMs))
                break;                              // timeout elapsed
            {
                LARGE_INTEGER Interval;
                Interval.QuadPart = -(LONGLONG)10 * 10000;   // 10ms, relative (100ns units)
                NtDelayExecution(FALSE, &Interval);
            }
            Waited += 10;
        }
    }

    // Rebuild readfds from the ready bitmask.
    if (r)
    {
        for (i = 0; i < (ULONG)((nfds + 31) >> 5); i++) r[i] = 0;
        for (i = 0; i < Count; i++)
        {
            if ((ULONG)Result & (1UL << i))
            {
                r[Fds[i] >> 5] |= (1UL << (Fds[i] & 31));
                Ready++;
            }
        }
    }

    // Writability: report every requested write fd as ready (buffers accept data).
    if (w)
    {
        for (i = 0; i < (ULONG)nfds; i++)
            if (w[i >> 5] & (1UL << (i & 31))) Ready++;
    }

    return Ready;
}

//
// setpgrp() -- BSD spelling of "become a process-group leader"; maps to setpgid(0,0).
//
int __cdecl
setpgrp(void)
{
    return setpgid(0, 0);
}
