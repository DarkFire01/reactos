/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     URS client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* wdfdriverentry does not walk .kmdfclassbind, so clients bind through here */

#include <ntddk.h>
#include <windef.h>
#include <wdf.h>
#include <fxldr.h>
#include <ursdevice.h>
#include "ursstub.h"

#define NDEBUG
#include <debug.h>

/* Owned by wdfdriverentry */
extern WDF_BIND_INFO BindInfo;
extern PWDF_DRIVER_GLOBALS WdfDriverGlobals;

PFN_URSFUNC UrsFunctions[UrsFunctionTableNumEntries];
PURS_DRIVER_GLOBALS UrsDriverGlobals;

static WDF_CLASS_BIND_INFO UrsStubClassBindInfo;
static BOOLEAN UrsStubBound;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
UrsStubBindClass(VOID)
{
    PWDF_CLASS_BIND_INFO Info = &UrsStubClassBindInfo;
    NTSTATUS Status;

    PAGED_CODE();

    if (UrsStubBound)
        return STATUS_SUCCESS;

    RtlZeroMemory(Info, sizeof(*Info));
    Info->Size = sizeof(*Info);
    Info->ClassName = (PWCHAR)L"UrsCx";
    Info->Version.Major = URS_CLASS_MAJOR_VERSION;
    Info->Version.Minor = URS_CLASS_MINOR_VERSION;
    Info->FunctionTable = UrsFunctions;
    Info->FunctionTableCount = UrsFunctionTableNumEntries;
    Info->ClassBindInfo = &UrsDriverGlobals;

    Status = WdfVersionBindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, Info);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("URS class bind failed 0x%lx\n", Status);
        return Status;
    }

    UrsStubBound = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
UrsStubUnbindClass(VOID)
{
    PAGED_CODE();

    if (!UrsStubBound)
        return;

    WdfVersionUnbindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, &UrsStubClassBindInfo);

    UrsStubBound = FALSE;
    UrsDriverGlobals = NULL;
    RtlZeroMemory(UrsFunctions, sizeof(UrsFunctions));
}
