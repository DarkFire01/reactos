/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Terminal control functions (termios and line control)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/* ULONG indices into the 68-byte struct termios; c_cc[11] begins at byte 24 */
#define TIOS_IFLAG      0
#define TIOS_OFLAG      1
#define TIOS_CFLAG      2
#define TIOS_LFLAG      3
#define TIOS_ISPEED     4
#define TIOS_OSPEED     5
#define TIOS_CC_OFFSET  24
#define TIOS_NCCS       11
#define TIOS_SIZE       68

#define PSX_TCGETS      0x5401
#define PSX_TCSETS      0x5402

/* HasData flag in the tcgetattr/tcsetattr reply */
#define PSX_TIOS_HASDATA_SLOT 5

unsigned long
__cdecl
cfgetispeed(
    _In_ const void *Termios)
{
    return ((const unsigned long *)Termios)[TIOS_ISPEED];
}

unsigned long
__cdecl
cfgetospeed(
    _In_ const void *Termios)
{
    return ((const unsigned long *)Termios)[TIOS_OSPEED];
}

int
__cdecl
cfsetispeed(
    _Inout_ void *Termios,
    _In_ unsigned long Speed)
{
    ((unsigned long *)Termios)[TIOS_ISPEED] = Speed;
    return 0;
}

int
__cdecl
cfsetospeed(
    _Inout_ void *Termios,
    _In_ unsigned long Speed)
{
    ((unsigned long *)Termios)[TIOS_OSPEED] = Speed;
    return 0;
}

/**
 * @brief Gets terminal attributes. A pty slave answers through ioctl, the
 * controlling terminal through the session leader, and anything else gets
 * cooked mode defaults.
 */
int
__cdecl
tcgetattr(
    _In_ int FileDescriptor,
    _Out_writes_bytes_(TIOS_SIZE) void *Termios)
{
    PSX_API_MESSAGE Message;
    unsigned long *Flags = (unsigned long *)Termios;
    unsigned char *ControlChars;
    int i;

    /* Non-pty descriptors fail with ENOTTY and fall through */
    if (ioctl(FileDescriptor, PSX_TCGETS, Termios) == 0)
        return 0;

    PsxInitMessage(&Message, PsxApiTcgetattr, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return -1;

    if (((PULONG)Message.Data.Raw)[PSX_TIOS_HASDATA_SLOT] != 0)
        return PsxTtyTermios(0, Termios);

    RtlZeroMemory(Termios, TIOS_SIZE);
    Flags[TIOS_IFLAG] = 0x0100 | 0x0400;                    /* ICRNL | IXON */
    Flags[TIOS_OFLAG] = 0x0001 | 0x0004;                    /* OPOST | ONLCR */
    Flags[TIOS_CFLAG] = 0x0030 | 0x0080;                    /* CS8 | CREAD */
    Flags[TIOS_LFLAG] = 0x0001 | 0x0002 | 0x0008 | 0x0010;  /* ISIG | ICANON | ECHO | ECHOE */
    Flags[TIOS_ISPEED] = 13;                                /* B9600 */
    Flags[TIOS_OSPEED] = 13;

    ControlChars = (unsigned char *)Termios + TIOS_CC_OFFSET;
    ControlChars[0] = 3;    /* VINTR = ^C */
    ControlChars[1] = 28;   /* VQUIT = ^backslash */
    ControlChars[2] = 127;  /* VERASE = DEL */
    ControlChars[3] = 21;   /* VKILL = ^U */
    ControlChars[4] = 4;    /* VEOF = ^D */
    for (i = 5; i < TIOS_NCCS; i++)
        ControlChars[i] = 0;

    return 0;
}

/**
 * @brief Sets terminal attributes on a pty slave or the controlling terminal.
 * Other terminals accept the call as a no-op.
 */
int
__cdecl
tcsetattr(
    _In_ int FileDescriptor,
    _In_ int Action,
    _In_reads_bytes_(TIOS_SIZE) const void *Termios)
{
    PSX_API_MESSAGE Message;

    UNREFERENCED_PARAMETER(Action);

    if (ioctl(FileDescriptor, PSX_TCSETS, (void *)Termios) == 0)
        return 0;

    PsxInitMessage(&Message, PsxApiTcsetattr, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    if (PsxCallServer(&Message) < 0)
        return -1;

    if (((PULONG)Message.Data.Raw)[PSX_TIOS_HASDATA_SLOT] != 0)
        return PsxTtyTermios(1, (void *)Termios);

    return 0;
}

/* There is no serial hardware behind the terminals, so line control is a no-op */
int
__cdecl
tcdrain(
    _In_ int Fd)
{
    UNREFERENCED_PARAMETER(Fd);
    return 0;
}

int
__cdecl
tcflow(
    _In_ int Fd,
    _In_ int Action)
{
    UNREFERENCED_PARAMETER(Fd);
    UNREFERENCED_PARAMETER(Action);
    return 0;
}

int
__cdecl
tcflush(
    _In_ int Fd,
    _In_ int Queue)
{
    UNREFERENCED_PARAMETER(Fd);
    UNREFERENCED_PARAMETER(Queue);
    return 0;
}

int
__cdecl
tcsendbreak(
    _In_ int Fd,
    _In_ int Duration)
{
    UNREFERENCED_PARAMETER(Fd);
    UNREFERENCED_PARAMETER(Duration);
    return 0;
}

int
__cdecl
tcgetpgrp(
    _In_ int Fd)
{
    UNREFERENCED_PARAMETER(Fd);
    return getpgrp();
}

int
__cdecl
tcsetpgrp(
    _In_ int Fd,
    _In_ int Pgrp)
{
    UNREFERENCED_PARAMETER(Fd);
    UNREFERENCED_PARAMETER(Pgrp);
    return 0;
}
