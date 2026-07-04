/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX terminal interface (IEEE Std 1003.1-1990, s.7.1.2.1).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

typedef unsigned long cc_t;
typedef unsigned long speed_t;
typedef unsigned long tcflag_t;

#define NCCS    11

struct termios
{
    tcflag_t c_iflag;       // input modes
    tcflag_t c_oflag;       // output modes
    tcflag_t c_cflag;       // control modes
    tcflag_t c_lflag;       // local modes
    speed_t  c_ispeed;      // input speed
    speed_t  c_ospeed;      // output speed
    cc_t     c_cc[NCCS];    // control characters
};

//
// c_iflag - input modes
//
#define BRKINT  0x00000001  // signal interrupt on break
#define ICRNL   0x00000002  // map CR to NL on input
#define IGNBRK  0x00000004  // ignore break condition
#define IGNCR   0x00000008  // ignore CR
#define IGNPAR  0x00000010  // ignore characters with parity errors
#define INLCR   0x00000020  // map NL to CR on input
#define INPCK   0x00000040  // enable input parity check
#define ISTRIP  0x00000080  // strip character
#define IXOFF   0x00000100  // enable start/stop input control
#define IXON    0x00000200  // enable start/stop output control
#define PARMRK  0x00000400  // mark parity errors

//
// c_oflag - output modes
//
#define OPOST   0x00000001  // perform output processing
#define ONLCR   0x00000002  // map NL to CR-NL on output
#define ONLRET  0x00000004  // NL performs CR function
#define OCRNL   0x00000008  // map CR to NL on output
#define ONOCR   0x00000010  // no CR output at column 0
#define OFILL   0x00000020  // use fill characters for delay
// Output delay masks (terminal emulators set/clear these on the pty)
#define NLDLY   0x00000100  // newline delay mask
#define     NL0 0x00000000
#define     NL1 0x00000100
#define CRDLY   0x00000600  // carriage-return delay mask
#define     CR0 0x00000000
#define TABDLY  0x00001800  // horizontal-tab delay mask
#define     TAB0 0x00000000
#define     TAB3 0x00001800  // expand tabs to spaces
#define BSDLY   0x00002000  // backspace delay mask
#define VTDLY   0x00004000  // vertical-tab delay mask
#define FFDLY   0x00008000  // form-feed delay mask

//
// c_cflag - control modes
//
#define CLOCAL  0x00000001  // ignore modem status lines
#define CREAD   0x00000002  // enable receiver
#define CSIZE   0x000000F0  // number of bits per byte
#define     CS5 0x00000010  //     5 bits
#define     CS6 0x00000020  //     6 bits
#define     CS7 0x00000040  //     7 bits
#define     CS8 0x00000080  //     8 bits
#define CSTOPB  0x00000100  // send two stop bits, else one
#define HUPCL   0x00000200  // hang up on last close
#define PARENB  0x00000400  // parity enable
#define PARODD  0x00000800  // odd parity, else even
#define CBAUD   0x0000F000  // baud-rate mask (our baud is index-based; pty ignores it)

//
// c_lflag - local modes
//
#define ECHO    0x00000001  // enable echo
#define ECHOE   0x00000002  // echo ERASE as an error-correcting backspace
#define ECHOK   0x00000004  // echo KILL
#define ECHONL  0x00000008  // echo '\n'
#define ICANON  0x00000010  // canonical input (erase and kill processing)
#define IEXTEN  0x00000020  // enable extended functions
#define ISIG    0x00000040  // enable signals
#define NOFLSH  0x00000080  // disable flush after intr, quit, or suspend
#define TOSTOP  0x00000100  // send SIGTTOU for background output
#define ECHOCTL 0x00000200  // echo control chars as ^X
#define ECHOPRT 0x00000400  // echo erased chars backward over
#define ECHOKE  0x00000800  // BS-SP-BS erase whole line on KILL
#define FLUSHO  0x00001000  // output being flushed
#define PENDIN  0x00002000  // retype pending input at next read

//
// c_cc - subscript names
//
#define VEOF    0           // EOF character
#define VEOL    1           // EOL character
#define VERASE  2           // ERASE character
#define VINTR   3           // INTR character
#define VKILL   4           // KILL character
#define VMIN    5           // MIN value (non-canonical)
#define VQUIT   6           // QUIT character
#define VSUSP   7           // SUSP character
#define VTIME   8           // TIME value (non-canonical)
#define VSTART  9           // START character
#define VSTOP   10          // STOP character

//
// speed_t values
//
#define B0      0
#define B50     1
#define B75     2
#define B110    3
#define B134    4
#define B150    5
#define B200    6
#define B300    7
#define B600    8
#define B1200   9
#define B1800   10
#define B2400   11
#define B4800   12
#define B9600   13
#define B19200  14
#define B38400  15

//
// Optional actions for tcsetattr()
//
#define TCSANOW     1
#define TCSADRAIN   2
#define TCSAFLUSH   3

//
// Queue selectors for tcflush()
//
#define TCIFLUSH    0
#define TCOFLUSH    1
#define TCIOFLUSH   2

//
// Actions for tcflow()
//
#define TCOOFF      0
#define TCOON       1
#define TCIOFF      2
#define TCION       3

int tcgetattr(int FileDescriptor, struct termios *Termios);
int tcsetattr(int FileDescriptor, int Actions, const struct termios *Termios);
int tcsendbreak(int FileDescriptor, int Duration);
int tcdrain(int FileDescriptor);
int tcflush(int FileDescriptor, int QueueSelector);
int tcflow(int FileDescriptor, int Action);

pid_t tcgetpgrp(int FileDescriptor);
int   tcsetpgrp(int FileDescriptor, pid_t ProcessGroup);

speed_t cfgetospeed(const struct termios *Termios);
int     cfsetospeed(struct termios *Termios, speed_t Speed);
speed_t cfgetispeed(const struct termios *Termios);
int     cfsetispeed(struct termios *Termios, speed_t Speed);

//
// Window size + terminal ioctls -- needed by terminal emulators (dtterm) and
// the pseudo-terminal layer. TIOCGWINSZ/TIOCSWINSZ carry the row/col geometry
// to the pty slave; TCSBRK sends an RS-232 break.
//
#ifndef _WINSIZE_DEFINED
#define _WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;      // rows, in characters
    unsigned short ws_col;      // columns, in characters
    unsigned short ws_xpixel;   // horizontal size, in pixels
    unsigned short ws_ypixel;   // vertical size, in pixels
};
#endif

#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TCSBRK      0x5409
#define TIOCSCTTY   0x540E      // make this the controlling terminal
#define TIOCGPTN    0x80045430  // get pty slave index (pts number)
#define TIOCSPTLCK  0x40045431  // (un)lock pty slave -- accepted, no-op
