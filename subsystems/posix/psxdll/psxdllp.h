/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header for the POSIX client library
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <stdarg.h>

#define WIN32_NO_STATUS
#include <ndk/umtypes.h>
#include <ndk/lpcfuncs.h>
#include <ndk/lpctypes.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/exfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/kefuncs.h>

#include <subsys/posix/psxmsg.h>

/* POSIX entry points used across translation units (psxdll links no CRT) */
int
__cdecl
getpid(VOID);

int
__cdecl
getpgrp(VOID);

int
__cdecl
getuid(VOID);

int
__cdecl
getgid(VOID);

int
__cdecl
isatty(
    _In_ int FileDescriptor);

int
__cdecl
open(
    _In_z_ const char *Path,
    _In_ int OpenFlag,
    ...);

int
__cdecl
close(
    _In_ int FileDescriptor);

int
__cdecl
ioctl(
    _In_ int FileDescriptor,
    _In_ unsigned long Request,
    _Inout_opt_ void *Arg);

int
__cdecl
setpgid(
    _In_ int Pid,
    _In_ int Pgid);

int
__cdecl
sigprocmask(
    _In_ int How,
    _In_opt_ const unsigned long *Set,
    _Out_opt_ unsigned long *OldSet);

char *
__cdecl
getenv(
    _In_opt_z_ const char *Name);

int
__cdecl
access(
    _In_z_ const char *Path,
    _In_ int ModeMask);

DECLSPEC_NORETURN
void
__cdecl
_exit(
    _In_ int Status);

/* Path buffer sizes for ANSI NT device paths */
#define PSX_PATH_MAX    260
#define PSX_ROOT_MAX    64

/* Per-process client state */
extern HANDLE PsxApiPort;
extern LONG PsxClientToServer;
extern PVOID PsxSharedHeap;
extern PLONG PsxErrnoLocation;
extern char ***PsxEnvironLocation;
extern ULONG PsxSessionId;
extern CHAR PsxStartupCwd[PSX_PATH_MAX];
extern CHAR PsxStartupRoot[PSX_ROOT_MAX];
extern ULONG PsxStartupCwdLen;
extern ULONG PsxStartupRootLen;

/*
 * Converts a client pointer into the server's view of the shared section.
 * Buffers the server reaches with NtReadVirtualMemory use the raw pointer instead.
 */
#define PsxServerPtr(ClientPtr) \
    ((ULONG)((ULONG_PTR)(ClientPtr) + (LONG_PTR)PsxClientToServer))

/* LPC DataLength for a request whose inline payload is PayloadSize bytes */
#define PSX_BODY_DATALEN(PayloadSize) \
    ((USHORT)((FIELD_OFFSET(PSX_API_MESSAGE, Data) - sizeof(PORT_MESSAGE)) + (PayloadSize)))

/* init.c */
NTSTATUS
PsxInitialize(VOID);

VOID
PsxSetErrno(
    _In_ LONG ErrnoValue);

FORCEINLINE
VOID
PsxInitMessage(
    _Out_ PPSX_API_MESSAGE Message,
    _In_ ULONG ApiNumber,
    _In_ USHORT DataLength)
{
    RtlZeroMemory(Message, sizeof(*Message));
    Message->Header.u1.s1.TotalLength = (USHORT)(sizeof(PORT_MESSAGE) + DataLength);
    Message->Header.u1.s1.DataLength = DataLength;
    Message->ApiNumber = ApiNumber;
}

/* lpc.c */
LONG
PsxCallServer(
    _Inout_ PPSX_API_MESSAGE Message);

/* path.c */
ULONG
PsxStringLengthA(
    _In_z_ PCSTR String);

BOOLEAN
PsxMarshalPath(
    _In_opt_z_ PCSTR PosixPath,
    _Out_ PUNICODE_STRING NtPath);

VOID
PsxFreeMarshalledPath(
    _Inout_ PUNICODE_STRING NtPath);

PVOID
PsxAllocShared(
    _In_ ULONG Size);

VOID
PsxFreeShared(
    _In_opt_ PVOID Block);

ULONG
PsxBuildNtPath(
    _In_opt_z_ PCSTR PosixPath,
    _Out_writes_z_(NtMax) PCHAR NtOut,
    _In_ ULONG NtMax);

/* tty.c */
int
PsxTtyReadWrite(
    _In_ int IsWrite,
    _In_ int FileDescriptor,
    _Inout_updates_bytes_(Count) void *Buffer,
    _In_ unsigned int Count);

int
PsxTtyTermios(
    _In_ int IsSet,
    _Inout_ void *Termios);

VOID
PsxTtyForkReset(VOID);
