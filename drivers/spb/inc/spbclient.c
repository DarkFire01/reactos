/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx client stub - the missing "spbcxstub.lib"
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * An SpbCx client calls the API through an indirect table: every SpbXxx() inline
 * in <spbcx.h> does
 *
 *     ((PFN_SPBXXX) SpbFunctions[SpbXxxTableIndex])(SpbDriverGlobals, ...)
 *
 * and both of those symbols live here, empty, until the KMDF class loader binds
 * SpbCx to this client. Binding is driven by the WDF_CLASS_BIND_INFO below,
 * placed in ".kmdfclassbind$b" so it falls between the
 * __KMDF_CLASS_BIND_START/END markers wdfdriverentry defines; FxStubBindClasses
 * walks that section at DriverEntry and calls WdfVersionBindClass for each
 * entry, which reaches spbcx's ScxClassBindClient.
 *
 * wdfldr resolves ClassName through
 * HKLM\System\CurrentControlSet\Control\Wdf\Kmdf\<ClassName>\Versions\<Ma>\<Mi>,
 * whose "Service" value names the class extension's driver service. The name is
 * "SPBCx", and that exact casing is the wide string in the reference client,
 * whose bind descriptor is named SPBCx_BIND_INFO.
 *
 * Same shape as sdk/lib/drivers/ucxstub/ucxstub.c, which does this for UCX.
 */

#include <ntddk.h>
#include <wdf.h>
#include <wdfcx.h>
#include <spb.h>
#include <spbcx.h>

/*
 * Filled in by the bind. SpbCx serves 1.0 and 1.1 with the same thirteen
 * functions, so asking for 1.0 would bind identically; 1.1 is asked for because
 * that is what a current client is built against.
 */
SPBFUNC SpbFunctions[SpbFunctionTableNumEntries];
PSPB_DRIVER_GLOBALS SpbDriverGlobals;

#if defined(__GNUC__)
#define SPB_CLASS_BIND_SECTION __attribute__((section(".kmdfclassbind$b"), used))
#else
#pragma section(".kmdfclassbind$b", read, write)
#define SPB_CLASS_BIND_SECTION __declspec(allocate(".kmdfclassbind$b"))
#endif

SPB_CLASS_BIND_SECTION
WDF_CLASS_BIND_INFO SPBCx_BIND_INFO =
{
    sizeof(WDF_CLASS_BIND_INFO),
    L"SPBCx",
    { 1, 1, 0 },
    (VOID (NTAPI **)(VOID))SpbFunctions,
    SpbFunctionTableNumEntries,
    (PVOID)&SpbDriverGlobals,
    NULL,
    NULL,
    NULL
};
