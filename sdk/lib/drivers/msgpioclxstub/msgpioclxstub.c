/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GpioClx client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* wdfdriverentry does not walk .kmdfclassbind, so clients bind through here */

#include <ntddk.h>
#include <windef.h>
#include <wdf.h>
#include <fxldr.h>
#include "msgpioclxstub.h"

#define NDEBUG
#include <debug.h>

/* What msgpioclx registers with WdfRegisterClassLibrary */
#define GPIOCLX_STUB_CLASS_NAME    L"GPIOClx"
#define GPIOCLX_STUB_MAJOR_VERSION 1
#define GPIOCLX_STUB_MINOR_VERSION 0

/* Owned by wdfdriverentry */
extern WDF_BIND_INFO BindInfo;
extern PWDF_DRIVER_GLOBALS WdfDriverGlobals;

PGPIO_CLX_EXPORTED_INTERFACES GpioClxFunctions[GPIO_CLX_TOTAL_EXPORTS];

static WDF_CLASS_BIND_INFO GpioClxStubClassBindInfo;
static BOOLEAN GpioClxStubBound;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
GpioClxStubBindClass(VOID)
{
    PWDF_CLASS_BIND_INFO Info = &GpioClxStubClassBindInfo;
    NTSTATUS Status;

    PAGED_CODE();

    if (GpioClxStubBound)
        return STATUS_SUCCESS;

    /* GpioClx keys clients by WDFDRIVER and hands back no globals */
    RtlZeroMemory(Info, sizeof(*Info));
    Info->Size = sizeof(*Info);
    Info->ClassName = (PWCHAR)GPIOCLX_STUB_CLASS_NAME;
    Info->Version.Major = GPIOCLX_STUB_MAJOR_VERSION;
    Info->Version.Minor = GPIOCLX_STUB_MINOR_VERSION;
    Info->FunctionTable = (VOID (NTAPI **)(VOID))GpioClxFunctions;
    Info->FunctionTableCount = RTL_NUMBER_OF(GpioClxFunctions);

    Status = WdfVersionBindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, Info);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx class bind failed 0x%lx\n", Status);
        return Status;
    }

    GpioClxStubBound = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
GpioClxStubUnbindClass(VOID)
{
    PAGED_CODE();

    if (!GpioClxStubBound)
        return;

    WdfVersionUnbindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, &GpioClxStubClassBindInfo);

    GpioClxStubBound = FALSE;
    RtlZeroMemory(GpioClxFunctions, sizeof(GpioClxFunctions));
}
