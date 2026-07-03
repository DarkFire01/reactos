/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL terminal syscalls. The cf* speed accessors are pure
 *              client-side struct-termios field access (c_ispeed/c_ospeed at
 *              ULONG indices 4/5 of the 68-byte NT 4.0 termios). tcgetattr/
 *              tcsetattr validate the tty at the server; the full termios state
 *              exchange with posix.exe (session phase-2) is still TODO, so
 *              tcgetattr reports a sane cooked-mode default.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

// ULONG indices into struct termios (c_iflag,c_oflag,c_cflag,c_lflag, then
// c_ispeed,c_ospeed); c_cc[11] begins at byte 24.
#define TIOS_IFLAG   0
#define TIOS_OFLAG   1
#define TIOS_CFLAG   2
#define TIOS_LFLAG   3
#define TIOS_ISPEED  4
#define TIOS_OSPEED  5
#define TIOS_CC_OFF  24
#define NCCS_LOCAL   11

//
// cf{get,set}{i,o}speed -- client-side accessors on struct termios.
//
unsigned long __cdecl cfgetispeed(const void *Termios)
{ return ((const unsigned long *)Termios)[TIOS_ISPEED]; }

unsigned long __cdecl cfgetospeed(const void *Termios)
{ return ((const unsigned long *)Termios)[TIOS_OSPEED]; }

int __cdecl cfsetispeed(void *Termios, unsigned long Speed)
{ ((unsigned long *)Termios)[TIOS_ISPEED] = Speed; return 0; }

int __cdecl cfsetospeed(void *Termios, unsigned long Speed)
{ ((unsigned long *)Termios)[TIOS_OSPEED] = Speed; return 0; }

//
// tcgetattr(fd, termios) -- ApiNumber 0x2F. Validates the fd is a tty; fills a
// cooked-mode default (the real per-tty state exchange is server-side TODO).
//
int __cdecl
tcgetattr(int FileDescriptor, void *Termios)
{
    PSX_API_MESSAGE Message;
    unsigned long *T = (unsigned long *)Termios;
    unsigned char *Cc;
    int i;

    PsxInitMessage(&Message, PsxApiTcgetattr, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return -1;      // ENOTTY / EBADF

    // Controlling terminal: fetch the real termios from the session leader.
    if (((PULONG)Message.Data.Raw)[5] != 0)     // HasData @ body +0x44
        return PsxTtyTermios(0 /* get */, Termios);

    // Non-leader fallback: sane cooked-mode defaults (1:1 with MSTOOLS termios.h).
    RtlZeroMemory(Termios, 68);
    T[TIOS_IFLAG]  = 0x0100 | 0x0400;   // ICRNL | IXON
    T[TIOS_OFLAG]  = 0x0001 | 0x0004;   // OPOST | ONLCR
    T[TIOS_CFLAG]  = 0x0030 | 0x0080;   // CS8 | CREAD
    T[TIOS_LFLAG]  = 0x0001 | 0x0002 | 0x0008 | 0x0010; // ISIG|ICANON|ECHO|ECHOE
    T[TIOS_ISPEED] = 13;                // B9600
    T[TIOS_OSPEED] = 13;
    Cc = (unsigned char *)Termios + TIOS_CC_OFF;
    Cc[0] = 3;    // VINTR  = ^C
    Cc[1] = 28;   // VQUIT  = ^backslash
    Cc[2] = 127;  // VERASE = DEL
    Cc[3] = 21;   // VKILL  = ^U
    Cc[4] = 4;    // VEOF   = ^D
    for (i = 5; i < NCCS_LOCAL; i++)
        Cc[i] = 0;
    return 0;
}

//
// tcsetattr(fd, action, termios) -- ApiNumber 0x30. Validates the tty; the
// state push to posix.exe is server-side TODO (accepted as a no-op for now).
//
int __cdecl
tcsetattr(int FileDescriptor, int Action, const void *Termios)
{
    PSX_API_MESSAGE Message;
    UNREFERENCED_PARAMETER(Action);
    UNREFERENCED_PARAMETER(Termios);

    PsxInitMessage(&Message, PsxApiTcsetattr, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return -1;

    // Controlling terminal: push the termios to the session leader (raw/cooked).
    if (((PULONG)Message.Data.Raw)[5] != 0)     // HasData @ body +0x44
        return PsxTtyTermios(1 /* set */, (void *)Termios);
    return 0;
}

//
// The remaining line-control ops have no serial hardware behind them on the
// subsystem; they succeed as no-ops. tcgetpgrp reports the caller's group.
//
int __cdecl tcdrain(int Fd)                  { UNREFERENCED_PARAMETER(Fd); return 0; }
int __cdecl tcflow(int Fd, int Action)       { UNREFERENCED_PARAMETER(Fd); UNREFERENCED_PARAMETER(Action); return 0; }
int __cdecl tcflush(int Fd, int Queue)       { UNREFERENCED_PARAMETER(Fd); UNREFERENCED_PARAMETER(Queue); return 0; }
int __cdecl tcsendbreak(int Fd, int Dur)     { UNREFERENCED_PARAMETER(Fd); UNREFERENCED_PARAMETER(Dur); return 0; }
int __cdecl tcgetpgrp(int Fd)                { UNREFERENCED_PARAMETER(Fd); return getpgrp(); }
int __cdecl tcsetpgrp(int Fd, int Pgrp)      { UNREFERENCED_PARAMETER(Fd); UNREFERENCED_PARAMETER(Pgrp); return 0; }
