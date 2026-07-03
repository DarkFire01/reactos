/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header for PSXDLL.DLL (the POSIX client library). Mirrors
 *              the per-process client state of the NT 4.0 psxdll.dll.
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
#include <ndk/exfuncs.h>       // NtQuerySystemTime
#include <ndk/psfuncs.h>       // NtTerminateProcess
#include <ndk/kefuncs.h>       // NtDelayExecution

#include <subsys/posix/psxmsg.h>

//
// A few POSIX entry points call each other across translation units (raise ->
// getpid, chdir -> access). Declare them here since psxdll pulls in no CRT.
//
int __cdecl getpid(void);
int __cdecl getpgrp(void);
int __cdecl getuid(void);
int __cdecl getgid(void);
int __cdecl isatty(int FileDescriptor);
int __cdecl sigprocmask(int How, const unsigned long *Set, unsigned long *OldSet);
char * __cdecl getenv(const char *Name);
int __cdecl access(const char *Path, int ModeMask);
DECLSPEC_NORETURN void __cdecl _exit(int Status);

//
// Path buffer sizes (ANSI NT device paths, e.g. "\DosDevices\X:\...").
//
#define PSX_PATH_MAX    260
#define PSX_ROOT_MAX    64

//
// Per-process client state (the NT 4.0 psxdll globals: g_ApiPort,
// g_ClientToServer, g_SharedHeap, g_pErrno).
//
extern HANDLE PsxApiPort;          // \PSXSS\ApiPort connection handle
extern LONG   PsxClientToServer;   // serverViewBase - clientViewBase (pointer fixup)
extern PVOID  PsxSharedHeap;       // RtlCreateHeap laid over the shared section
extern PLONG  PsxErrnoLocation;    // caller's errno cell (set by PdxInitializeData)
extern char ***PsxEnvironLocation; // caller's environ cell
extern ULONG  PsxSessionId;        // session id assigned at connect
extern CHAR   PsxStartupCwd[PSX_PATH_MAX];   // current dir as an NT device path
extern CHAR   PsxStartupRoot[PSX_ROOT_MAX];  // POSIX "/" as an NT device prefix
extern ULONG  PsxStartupCwdLen;
extern ULONG  PsxStartupRootLen;

//
// A client pointer P appears in the server's mapping of the shared section at
// (P + PsxClientToServer); that server-relative address is what travels in a
// message (paths, struct stat, ...). Buffers the server reaches by
// NtReadVirtualMemory (read/write data, getgroups list) use the RAW pointer.
//
#define PsxServerPtr(ClientPtr) \
    ((ULONG)((ULONG_PTR)(ClientPtr) + (LONG_PTR)PsxClientToServer))

//
// DataLength (LPC body size) for a request whose inline payload is PayloadSize.
// The fixed reply fields (DataChannel..RetryTag) sit between the PORT_MESSAGE
// header and the Data union.
//
#define PSX_BODY_DATALEN(PayloadSize) \
    ((USHORT)((FIELD_OFFSET(PSX_API_MESSAGE, Data) - sizeof(PORT_MESSAGE)) + (PayloadSize)))

//
// init.c
//
NTSTATUS PsxInitialize(VOID);
VOID PsxSetErrno(IN LONG ErrnoValue);

FORCEINLINE
VOID
PsxInitMessage(
    OUT PPSX_API_MESSAGE Message,
    IN ULONG ApiNumber,
    IN USHORT DataLength)
{
    RtlZeroMemory(Message, sizeof(*Message));
    Message->Header.u1.s1.TotalLength = (USHORT)(sizeof(PORT_MESSAGE) + DataLength);
    Message->Header.u1.s1.DataLength = DataLength;
    Message->ApiNumber = ApiNumber;
}

//
// lpc.c -- send a request to \PSXSS\ApiPort and translate the reply to a POSIX
// return value (-1 + errno on failure), retrying on EINTR.
//
LONG PsxCallServer(IN OUT PPSX_API_MESSAGE Message);

//
// path.c -- POSIX<->NT path translation + shared-section marshalling. The client
// owns POSIX->NT translation: "/etc/x" -> "<root>\etc\x", "x" -> "<cwd>\x", with
// '/'->'\'. Marshalled UNICODE_STRINGs carry server-relative Buffers.
//
BOOLEAN PsxMarshalPath(IN PCSTR PosixPath, OUT PUNICODE_STRING NtPath);
VOID    PsxFreeMarshalledPath(IN PUNICODE_STRING NtPath);
PVOID   PsxAllocShared(IN ULONG Size);
VOID    PsxFreeShared(IN PVOID Block);
ULONG   PsxBuildNtPath(IN PCSTR PosixPath, OUT PCHAR NtOut, IN ULONG NtMax);

//
// tty.c -- controlling-terminal second phase (bytes/termios flow client<->the
// session leader over \PSXSS\PSXSES\P<sid>, when psxss flags HasData).
//
int PsxTtyReadWrite(int IsWrite, int FileDescriptor, void *Buffer, unsigned int Count);
int PsxTtyTermios(int IsSet, void *Termios);
VOID PsxTtyForkReset(VOID);
