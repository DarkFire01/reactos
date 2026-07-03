/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pseudo-terminal helpers, ioctl and select
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"
#include <subsys/posix/psxext.h>

/* Same value as TIOCGPTN in the SDK termios.h */
#define PSX_TIOCGPTN            0x80045430

#define PSX_SELECT_INFINITE     0xFFFFFFFF
#define PSX_SELECT_POLL_MS      10

typedef struct _PSX_TIMEVAL
{
    long tv_sec;
    long tv_usec;
} PSX_TIMEVAL, *PPSX_TIMEVAL;

/* There is no pty access control, so grantpt and unlockpt always succeed */
int
__cdecl
grantpt(
    _In_ int FileDescriptor)
{
    UNREFERENCED_PARAMETER(FileDescriptor);
    return 0;
}

int
__cdecl
unlockpt(
    _In_ int FileDescriptor)
{
    UNREFERENCED_PARAMETER(FileDescriptor);
    return 0;
}

/**
 * @brief Sends a device control request. Only pty descriptors support it,
 * others fail with ENOTTY.
 */
int
__cdecl
ioctl(
    _In_ int FileDescriptor,
    _In_ unsigned long Request,
    _Inout_opt_ void *Arg)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PSX_API_IOCTL, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)Request;
    ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Arg;
    return (int)PsxCallServer(&Message);
}

/**
 * @brief Returns "/dev/pts/N" for a pty master, in a static buffer.
 */
char *
__cdecl
ptsname(
    _In_ int FileDescriptor)
{
    static const char Prefix[] = "/dev/pts/";
    static char Buffer[24];
    unsigned int Index = 0;
    char Digits[12];
    int DigitCount = 0;
    int Length = 0;

    if (ioctl(FileDescriptor, PSX_TIOCGPTN, &Index) != 0)
        return NULL;

    while (Prefix[Length] != '\0')
    {
        Buffer[Length] = Prefix[Length];
        Length++;
    }

    do
    {
        Digits[DigitCount++] = (char)('0' + (Index % 10));
        Index /= 10;
    }
    while (Index != 0 && DigitCount < 11);

    while (DigitCount > 0)
        Buffer[Length++] = Digits[--DigitCount];

    Buffer[Length] = '\0';
    return Buffer;
}

/**
 * @brief Waits for descriptors to become ready.
 *
 * The server only polls once without blocking, so the wait loop runs here to
 * keep the server free for other clients. Write descriptors are always
 * reported ready and exception descriptors are ignored. The fd_set arguments
 * are bit arrays of ULONG words.
 */
int
__cdecl
select(
    _In_ int FdCount,
    _Inout_opt_ void *ReadFds,
    _Inout_opt_ void *WriteFds,
    _Inout_opt_ void *ExceptFds,
    _In_opt_ void *Timeout)
{
    PSX_API_MESSAGE Message;
    PULONG ReadSet = (PULONG)ReadFds;
    PULONG WriteSet = (PULONG)WriteFds;
    ULONG Fds[PSX_SELECT_MAXFDS];
    ULONG Count = 0;
    ULONG TimeoutMs;
    ULONG Waited = 0;
    BOOLEAN HasWrite = FALSE;
    LARGE_INTEGER Interval;
    LONG Result;
    ULONG i;
    int Ready = 0;

    UNREFERENCED_PARAMETER(ExceptFds);

    if (FdCount < 0)
        FdCount = 0;
    if (FdCount > PSX_SELECT_MAXFDS * 32)
        FdCount = PSX_SELECT_MAXFDS * 32;

    /* Collect the requested read descriptors */
    for (i = 0; i < (ULONG)FdCount && Count < PSX_SELECT_MAXFDS; i++)
    {
        if (ReadSet && (ReadSet[i >> 5] & (1UL << (i & 31))))
            Fds[Count++] = i;
    }

    if (Timeout == NULL)
    {
        TimeoutMs = PSX_SELECT_INFINITE;
    }
    else
    {
        PPSX_TIMEVAL TimeValue = (PPSX_TIMEVAL)Timeout;

        TimeoutMs = (ULONG)(TimeValue->tv_sec * 1000 + TimeValue->tv_usec / 1000);
    }

    if (WriteSet)
    {
        for (i = 0; i < (ULONG)FdCount; i++)
        {
            if (WriteSet[i >> 5] & (1UL << (i & 31)))
            {
                HasWrite = TRUE;
                break;
            }
        }
    }

    for (;;)
    {
        PsxInitMessage(&Message, PSX_API_SELECT, PSX_BODY_DATALEN(3 * sizeof(ULONG)));
        ((PULONG)Message.Data.Raw)[0] = Count;
        ((PULONG)Message.Data.Raw)[1] = 0;
        ((PULONG)Message.Data.Raw)[2] = (ULONG)(ULONG_PTR)Fds;

        /* Returns the bitmask of ready entries in Fds */
        Result = PsxCallServer(&Message);
        if (Result < 0)
            return -1;

        if (Result != 0 || HasWrite)
            break;
        if (TimeoutMs == 0)
            break;
        if (TimeoutMs != PSX_SELECT_INFINITE && Waited >= TimeoutMs)
            break;

        Interval.QuadPart = -(LONGLONG)PSX_SELECT_POLL_MS * 10000;
        NtDelayExecution(FALSE, &Interval);
        Waited += PSX_SELECT_POLL_MS;
    }

    if (ReadSet)
    {
        for (i = 0; i < (ULONG)((FdCount + 31) >> 5); i++)
            ReadSet[i] = 0;

        for (i = 0; i < Count; i++)
        {
            if ((ULONG)Result & (1UL << i))
            {
                ReadSet[Fds[i] >> 5] |= (1UL << (Fds[i] & 31));
                Ready++;
            }
        }
    }

    if (WriteSet)
    {
        for (i = 0; i < (ULONG)FdCount; i++)
        {
            if (WriteSet[i >> 5] & (1UL << (i & 31)))
                Ready++;
        }
    }

    return Ready;
}

/**
 * @brief Makes the caller a process group leader, same as setpgid(0, 0).
 */
int
__cdecl
setpgrp(void)
{
    return setpgid(0, 0);
}
