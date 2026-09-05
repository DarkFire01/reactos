/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Declarations shared between the kdnet translation units
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _KDNET_PRIVATE_H_
#define _KDNET_PRIVATE_H_

#define NOEXTAPI
#include <ntifs.h>
#include <windbgkd.h>
#include <arc/arc.h>
#include <kddll.h>
/*
 * Prevent kdnetextensibility.h from defining convenience macros like
 * `KdInitializeController` that break member access on local tables.
 */
#define _KDNET_INTERNAL_ 1
#include <kdnetextensibility.h>

/* Early boot printf (COM1) set by KdDebuggerInitialize0. */
extern ULONG (*FrLdrDbgPrint)(const char *Format, ...);

/** Global extensibility exports table (filled by extensibility module via KdInitializeLibrary). */
extern PKDNET_EXTENSIBILITY_EXPORTS KdNetExtensibilityExports;

/** Hardware context (adapter) handed to the extension's controller routines.
 *  Set by KdDebuggerInitialize0 after KdSetupPciDeviceForDebugging. */
extern PVOID KdNetHardwareContext;

/** Shared data block describing the debug NIC, passed to KdInitializeController. */
extern KDNET_SHARED_DATA KdNetSharedData;

/** TRUE once KdInitializeController has succeeded at least once. */
extern BOOLEAN KdNetInitialized;

/*
 * Architecture-specific timing primitives (implemented per-arch under i386/,
 * amd64/, ...). MUST be used instead of KeStallExecutionProcessor for any
 * wall-clock timing in the kdnet init path: the HAL StallScaleFactor is not yet
 * calibrated when kdnet runs (KdInitSystem is called from KiSystemStartup,
 * before the HAL clock is set up), so KeStallExecutionProcessor under-delays by
 * ~1000x.
 */

/** Monotonic hardware cycle/tick counter (TSC on x86). */
ULONG64 KdNetReadTimeStampCounter(VOID);

/** Hardware ticks per microsecond, calibrated once against an independent timer
 *  (the 8254 PIT on x86) and cached. Never returns zero. */
ULONG64 KdNetGetTicksPerMicrosecond(VOID);

#endif /* _KDNET_PRIVATE_H_ */
