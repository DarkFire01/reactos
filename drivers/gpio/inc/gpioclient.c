/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GpioClx client stub - the missing "gpioclxstub.lib"
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GpioClx client calls the API through an indirect table: every
 * GPIO_CLX_Xxx() inline in <gpioclx.h> does
 *
 *     ((PFN_GPIOCLXXXX) GpioClxFunctions[GpioClxXxxTableIndex])(GpioClxDriverGlobals, ...)
 *
 * and both of those symbols live here, empty, until the KMDF class loader binds
 * GpioClx to this client. Binding is driven by the WDF_CLASS_BIND_INFO below,
 * placed in ".kmdfclassbind$b" so it falls between the
 * __KMDF_CLASS_BIND_START/END markers wdfdriverentry defines; FxStubBindClasses
 * walks that section at DriverEntry and calls WdfVersionBindClass for each
 * entry, which reaches gpioclx's GpiopClassBindClient.
 *
 * The class name is "GPIOClx" with that exact casing: it is the wide string
 * in the reference client, whose bind descriptor the decomp calls
 * GPIOClx_BIND_INFO, and it is how the live Windows registry spells the key
 * under Control\Wdf\Kmdf. Version 1.1 is what that key carries.
 *
 * Same shape as drivers/spb/inc/spbclient.c, which does this for SpbCx.
 */

#include <ntddk.h>
#include <wdf.h>
#include <wdfcx.h>
#include <gpioclx.h>

/* Filled in by the bind */
GPIOCLXFUNC GpioClxFunctions[GpioClxFunctionTableNumEntries];
PGPIO_DRIVER_GLOBALS GpioClxDriverGlobals;

#if defined(__GNUC__)
#define GPIO_CLASS_BIND_SECTION __attribute__((section(".kmdfclassbind$b"), used))
#else
#pragma section(".kmdfclassbind$b", read, write)
#define GPIO_CLASS_BIND_SECTION __declspec(allocate(".kmdfclassbind$b"))
#endif

GPIO_CLASS_BIND_SECTION
WDF_CLASS_BIND_INFO GPIOClx_BIND_INFO =
{
    sizeof(WDF_CLASS_BIND_INFO),
    L"GPIOClx",
    { 1, 1, 0 },
    (VOID (NTAPI **)(VOID))GpioClxFunctions,
    GpioClxFunctionTableNumEntries,
    (PVOID)&GpioClxDriverGlobals,
    NULL,
    NULL,
    NULL
};
