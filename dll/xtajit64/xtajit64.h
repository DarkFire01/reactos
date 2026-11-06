/*
 * PROJECT:     ReactOS x64 emulator
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions of the x64 interpreter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winnt.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/ketypes.h>

/* x64 register numbers as encoded in instructions */
#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7

/* RFLAGS bits the interpreter maintains */
#define FLAG_CF 0x0001
#define FLAG_PF 0x0004
#define FLAG_AF 0x0010
#define FLAG_ZF 0x0040
#define FLAG_SF 0x0080
#define FLAG_DF 0x0400
#define FLAG_OF 0x0800

/* Why the interpreter stopped running x64 code */
typedef enum _XTA_EXIT
{
    XtaExitCallNative,
    XtaExitReturnNative
} XTA_EXIT;

/* Thread state, kept in the CHPE CPU area slots the emulator owns */
typedef struct _XTA_THREAD
{
    PCHPE_V2_CPU_AREA_INFO CpuArea;
    ULONG_PTR ResumeSp;
    ULONG_PTR NativeTarget;
    ULONG64 Gs;
    /* Where the native code that entered the current x64 run continues, and its stack then */
    ULONG_PTR ReturnPc;
    ULONG_PTR ReturnSp;
} XTA_THREAD, *PXTA_THREAD;

PCHPE_V2_CPU_AREA_INFO
NTAPI
XtapGetCpuArea(VOID);

XTA_EXIT
NTAPI
XtapInterpret(
    _Inout_ PXTA_THREAD Thread);

/* dispatch.s */
VOID
XtapRunNative(
    _Inout_ PXTA_THREAD Thread);

extern PUCHAR XtapX64Ret;
