/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* wdfdriverentry does not walk .kmdfclassbind, so clients bind through here */

#include <ntddk.h>
#include <windef.h>
#include <wdf.h>
#include <fxldr.h>
#include "spbcxstub.h"

#define NDEBUG
#include <debug.h>

/* What spbcx registers with WdfRegisterClassLibrary; 1.1 is the newest it accepts */
#define SPBCX_STUB_CLASS_NAME    L"SPBCx"
#define SPBCX_STUB_MAJOR_VERSION 1
#define SPBCX_STUB_MINOR_VERSION 1

/* Owned by wdfdriverentry */
extern WDF_BIND_INFO BindInfo;
extern PWDF_DRIVER_GLOBALS WdfDriverGlobals;

SPBFUNC SpbFunctions[SpbFunctionTableNumEntries];
PSPB_DRIVER_GLOBALS SpbDriverGlobals;

static WDF_CLASS_BIND_INFO SpbCxStubClassBindInfo;
static BOOLEAN SpbCxStubBound;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
SpbCxStubBindClass(VOID)
{
    PWDF_CLASS_BIND_INFO Info = &SpbCxStubClassBindInfo;
    NTSTATUS Status;

    PAGED_CODE();

    if (SpbCxStubBound)
        return STATUS_SUCCESS;

    RtlZeroMemory(Info, sizeof(*Info));
    Info->Size = sizeof(*Info);
    Info->ClassName = (PWCHAR)SPBCX_STUB_CLASS_NAME;
    Info->Version.Major = SPBCX_STUB_MAJOR_VERSION;
    Info->Version.Minor = SPBCX_STUB_MINOR_VERSION;
    Info->FunctionTable = (VOID (NTAPI **)(VOID))SpbFunctions;
    Info->FunctionTableCount = RTL_NUMBER_OF(SpbFunctions);
    Info->ClassBindInfo = &SpbDriverGlobals;

    Status = WdfVersionBindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, Info);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx class bind failed 0x%lx\n", Status);
        return Status;
    }

    SpbCxStubBound = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
SpbCxStubUnbindClass(VOID)
{
    PAGED_CODE();

    if (!SpbCxStubBound)
        return;

    WdfVersionUnbindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, &SpbCxStubClassBindInfo);

    SpbCxStubBound = FALSE;
    SpbDriverGlobals = NULL;
    RtlZeroMemory(SpbFunctions, sizeof(SpbFunctions));
}
