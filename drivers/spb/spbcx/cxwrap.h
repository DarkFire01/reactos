/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client-side wrappers for the KMDF class-extension device-init API
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * The WDK ships these inline in its own wdfcx.h. ReactOS's
 * sdk/lib/drivers/wdf/wdfcx.h carries only the typedefs, so the four a class
 * extension actually calls are written out here in exactly the shape every other
 * KMDF wrapper has: an indirect call through WdfFunctions at the index
 * <wdffuncenum.h> assigns. wdf01000 implements all four
 * (shared/core/fxcxdeviceinitapi.cpp), so these reach real code.
 */

#pragma once

FORCEINLINE PWDFCXDEVICE_INIT
WdfCxDeviceInitAllocate(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_WDFCXDEVICEINITALLOCATE)
                WdfFunctions[WdfCxDeviceInitAllocateTableIndex])(
        WdfDriverGlobals, DeviceInit);
}

FORCEINLINE NTSTATUS
WdfCxDeviceInitAssignWdmIrpPreprocessCallback(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDFCXDEVICE_WDM_IRP_PREPROCESS EvtCxDeviceWdmIrpPreprocess,
    _In_ UCHAR MajorFunction,
    _In_opt_ PUCHAR MinorFunctions,
    _In_ ULONG NumMinorFunctions)
{
    return ((PFN_WDFCXDEVICEINITASSIGNWDMIRPPREPROCESSCALLBACK)
                WdfFunctions[WdfCxDeviceInitAssignWdmIrpPreprocessCallbackTableIndex])(
        WdfDriverGlobals, CxDeviceInit, EvtCxDeviceWdmIrpPreprocess,
        MajorFunction, MinorFunctions, NumMinorFunctions);
}

FORCEINLINE VOID
WdfCxDeviceInitSetIoInCallerContextCallback(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext)
{
    ((PFN_WDFCXDEVICEINITSETIOINCALLERCONTEXTCALLBACK)
         WdfFunctions[WdfCxDeviceInitSetIoInCallerContextCallbackTableIndex])(
        WdfDriverGlobals, CxDeviceInit, EvtIoInCallerContext);
}

FORCEINLINE VOID
WdfCxDeviceInitSetFileObjectConfig(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_FILEOBJECT_CONFIG CxFileObjectConfig,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES FileObjectAttributes)
{
    ((PFN_WDFCXDEVICEINITSETFILEOBJECTCONFIG)
         WdfFunctions[WdfCxDeviceInitSetFileObjectConfigTableIndex])(
        WdfDriverGlobals, CxDeviceInit, CxFileObjectConfig, FileObjectAttributes);
}

/*
 * Forwarding a CX-preprocessed IRP is not WdfDeviceWdmDispatchPreprocessedIrp:
 * a class extension's preprocess callback is handed a DispatchContext that says
 * where in the preprocess chain it sits, and passing that back is what lets the
 * framework continue to the next handler rather than restart.
 */
typedef NTSTATUS
(NTAPI *PFN_WDFDEVICEWDMDISPATCHIRP_)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ PVOID DispatchContext);

FORCEINLINE NTSTATUS
ScxWdfDeviceWdmDispatchIrp(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ PVOID DispatchContext)
{
    return ((PFN_WDFDEVICEWDMDISPATCHIRP_)
                WdfFunctions[WdfDeviceWdmDispatchIrpTableIndex])(
        WdfDriverGlobals, Device, Irp, DispatchContext);
}
