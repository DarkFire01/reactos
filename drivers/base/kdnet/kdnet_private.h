

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

/*
 * PoSetHiberRange is exported by ntoskrnl but may not be declared by headers in
 * some configurations; declare it for kdnet usage.
 */
NTSYSAPI
VOID
NTAPI
PoSetHiberRange(
    _In_opt_ PVOID MemoryMap,
    _In_ ULONG Flags,
    _In_ PVOID Address,
    _In_ ULONG_PTR Length,
    _In_ ULONG Tag);

/** Global extensibility exports table (filled by extensibility module via KdInitializeLibrary). */
extern PKDNET_EXTENSIBILITY_EXPORTS KdNetExtensibilityExports;

/** Hardware context (adapter) handed to the extension's controller routines.
 *  Set by KdDebuggerInitialize0 after KdSetupPciDeviceForDebugging. */
extern PVOID KdNetHardwareContext;

/** Shared data block describing the debug NIC, passed to KdInitializeController. */
extern KDNET_SHARED_DATA KdNetSharedData;

/** TRUE once KdInitializeController has succeeded at least once. */
extern BOOLEAN KdNetInitialized;

#endif /* _KDNET_PRIVATE_H_ */
