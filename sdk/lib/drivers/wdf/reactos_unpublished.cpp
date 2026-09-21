/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Framework routines the published sources declare but do not define
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Mostly the device companion half, which needs the UMDF reflector. With no
 * reflector a companion can never load, so every device runs without one,
 * which the framework already treats as an ordinary outcome.
 */

#include "fx.hpp"
#include "fxpnpcallbacks.hpp"
#include "fxcompanionlibrary.hpp"
#include "fxcompaniontarget.hpp"
#include "fxldr.h"

extern "C" {
#include "fxdynamics.h"
#include "fxlibrarycommon.h"
}

/* Companion library */

PVOID
FxCompanionLibrary::operator new(
    _In_ size_t Size)
{
    return MxMemory::MxAllocatePool2(POOL_FLAG_NON_PAGED, Size, FX_TAG);
}

VOID
FxCompanionLibrary::operator delete(
    _In_ PVOID Pointer)
{
    MxMemory::MxFreePool(Pointer);
}

NTSTATUS
FxCompanionLibrary::_CreateAndInitialize(
    _Out_ FxCompanionLibrary **CompanionLibrary)
{
    /* Nothing to attach to, the object only answers that no device needs a companion. */
    *CompanionLibrary = new FxCompanionLibrary();
    return (*CompanionLibrary != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

BOOLEAN
FxCompanionLibrary::IsCompanionRequiredForDevice(
    _In_ FxDevice *Device,
    _Out_ PCWSTR *CompanionName)
{
    UNREFERENCED_PARAMETER(Device);

    *CompanionName = NULL;
    return FALSE;
}

/* Verifier */

extern "C" {

VOID
FxVerifierQueryStateSeparationDetection(
    _In_ HANDLE Key,
    _Out_ FxStateSeparationDetectionOption *StateSeparationDetection)
{
    /* Off is the only setting the published sources know. */
    UNREFERENCED_PARAMETER(Key);

    *StateSeparationDetection = FxStateSeparationDetectionNone;
}

/**
 * @brief
 * Tells whether Driver Verifier has a rule class turned on.
 *
 * @param[in] RuleClassID
 * The rule class to test.
 *
 * @return
 * FALSE, the kernel verifier has no rule classes to turn on.
 */
BOOLEAN
VfIsRuleClassEnabled(
    _In_ ULONG RuleClassID)
{
    UNREFERENCED_PARAMETER(RuleClassID);

    return FALSE;
}

/**
 * @brief
 * Driver Verifier check for executable nonpaged pool.
 *
 * @param[in] PoolType
 * The pool type the client asked for.
 *
 * @param[in] CallingAddress
 * Where in the client the allocation came from.
 *
 * @param[in] PoolTag
 * The tag of the allocation.
 */
VOID
VfCheckNxPoolType(
    _In_ POOL_TYPE PoolType,
    _In_ PVOID CallingAddress,
    _In_ ULONG PoolTag)
{
    /* The kernel verifier has no NX pool rule to report against. */
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(CallingAddress);
    UNREFERENCED_PARAMETER(PoolTag);
}

} // extern "C"

/* Client binding */

/**
 * @brief
 * Checks that a client can run on this framework and, for a client built
 * against a newer one, shrinks its tables to what the framework has.
 *
 * @param[in,out] BindInfo
 * The client's bind info, either the original layout or WDF_BIND_INFO2.
 *
 * @param[in] FxMajorVersion
 * Major version of this framework.
 *
 * @param[in] FxMinorVersion
 * Minor version of this framework.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER when the client cannot bind.
 */
NTSTATUS
WdfBindClientHelper(
    _Inout_ PWDF_BIND_INFO BindInfo,
    _In_ WDF_MAJOR_VERSION FxMajorVersion,
    _In_ WDF_MINOR_VERSION FxMinorVersion)
{
    PWDF_BIND_INFO2 bindInfo2;
    WDF_MINOR_VERSION clientMinor;
    WDF_MINOR_VERSION minimumMinor;
    ULONG clientStructCount;

    if (BindInfo->Version.Major != FxMajorVersion)
    {
        __Print(("Client major version %u, framework major version %u\n",
                 BindInfo->Version.Major, FxMajorVersion));
        return STATUS_INVALID_PARAMETER;
    }

    clientMinor = BindInfo->Version.Minor;

    /* The original layout carries one version, and it has to be one this framework covers */
    if (BindInfo->Size == sizeof(WDF_BIND_INFO))
    {
        if (clientMinor <= FxMinorVersion)
            return STATUS_SUCCESS;

        __Print(("Client minor version %u is newer than framework minor version %u\n",
                 clientMinor, FxMinorVersion));
        return STATUS_INVALID_PARAMETER;
    }

    if (BindInfo->Size != sizeof(*bindInfo2))
    {
        __Print(("Unknown bind info size %u\n", BindInfo->Size));
        return STATUS_INVALID_PARAMETER;
    }

    bindInfo2 = CONTAINING_RECORD(BindInfo, WDF_BIND_INFO2, V1);
    minimumMinor = *bindInfo2->MinimumVersionRequired;

    if (minimumMinor > clientMinor)
    {
        __Print(("Client minimum minor version %u is above its target %u\n",
                 minimumMinor, clientMinor));
        return STATUS_INVALID_PARAMETER;
    }

    if (minimumMinor > FxMinorVersion)
    {
        __Print(("Client needs minor version %u, framework minor version is %u\n",
                 minimumMinor, FxMinorVersion));
        return STATUS_INVALID_PARAMETER;
    }

    *bindInfo2->ClientVersionHigherThanFramework = (clientMinor > FxMinorVersion);

    if (clientMinor == FxMinorVersion)
        return STATUS_SUCCESS;

    clientStructCount = *bindInfo2->StructCountPtr;

    if (clientMinor < FxMinorVersion)
    {
        /* An older client's tables must fit inside the framework's */
        if ((clientStructCount > WdfVersion.StructCount) ||
            (BindInfo->FuncCount > WdfVersion.FuncCount))
        {
            __Print(("Client tables (%u functions, %u structures) exceed the framework's (%u, %u)\n",
                     BindInfo->FuncCount, clientStructCount,
                     WdfVersion.FuncCount, WdfVersion.StructCount));
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    }

    /* A newer client knows more than the framework has, so it only gets what exists here */
    if ((clientStructCount < WdfVersion.StructCount) ||
        (BindInfo->FuncCount < WdfVersion.FuncCount))
    {
        __Print(("Client tables (%u functions, %u structures) are short of the framework's (%u, %u)\n",
                 BindInfo->FuncCount, clientStructCount,
                 WdfVersion.FuncCount, WdfVersion.StructCount));
        return STATUS_INVALID_PARAMETER;
    }

    BindInfo->FuncCount = WdfVersion.FuncCount;
    *bindInfo2->FuncCountPtr = WdfVersion.FuncCount;
    *bindInfo2->StructTable = (WDF_STRUCT_INFO)&WdfVersion.Structures;
    *bindInfo2->StructCountPtr = WdfVersion.StructCount;

    return STATUS_SUCCESS;
}

/* Companion target */

FxCompanionTarget::FxCompanionTarget(
    _In_ PFX_DRIVER_GLOBALS FxDriverGlobals,
    _In_ USHORT ObjectSize) :
    FxNonPagedObject(FX_TYPE_COMPANION_TARGET, ObjectSize, FxDriverGlobals)
{
}

_Must_inspect_result_
NTSTATUS
FxCompanionTarget::Init(
    _In_ FxDevice *Device)
{
    UNREFERENCED_PARAMETER(Device);

    return STATUS_NOT_SUPPORTED;
}

VOID
FxCompanionTarget::QueryPnPDeviceStateNotification(
    VOID)
{
}

_Must_inspect_result_
NTSTATUS
FxCompanionTarget::HandleQueryInterfaceForSecureDriver(
    _In_ FxIrp *Irp,
    _Out_ PBOOLEAN CompleteRequest)
{
    *CompleteRequest = FALSE;
    return Irp->GetStatus();
}

/* Only reached with a companion target, which is never created. */

NTSTATUS
FxPnpDeviceD0Entry::InvokeCompanionCallback(
    _In_ FxCompanionTarget *CompanionTarget)
{
    UNREFERENCED_PARAMETER(CompanionTarget);

    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDeviceD0Exit::InvokeCompanionCallback(
    _In_ FxCompanionTarget *CompanionTarget)
{
    UNREFERENCED_PARAMETER(CompanionTarget);

    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDevicePrepareHardware::InvokeCompanionCallback(
    _In_ FxCompanionTarget *CompanionTarget)
{
    UNREFERENCED_PARAMETER(CompanionTarget);

    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDeviceReleaseHardware::InvokeCompanionCallback(
    _In_ FxCompanionTarget *CompanionTarget)
{
    UNREFERENCED_PARAMETER(CompanionTarget);

    return STATUS_SUCCESS;
}

extern "C" {

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
WDFEXPORT(WdfCompanionTargetSendTaskSynchronously)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFCOMPANIONTARGET CompanionTarget,
    _In_ USHORT TaskQueueIdentifier,
    _In_ ULONG TaskOperationCode,
    _In_opt_ PWDF_MEMORY_DESCRIPTOR InputBuffer,
    _In_opt_ PWDF_MEMORY_DESCRIPTOR OutputBuffer,
    _In_opt_ PWDF_TASK_SEND_OPTIONS TaskOptions,
    _Out_ PULONG_PTR BytesReturned)
{
    FxCompanionTarget *target;

    UNREFERENCED_PARAMETER(TaskQueueIdentifier);
    UNREFERENCED_PARAMETER(TaskOperationCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(TaskOptions);

    /* Validates the handle, which can only be bogus since none are ever made. */
    FxObjectHandleGetPtr(GetFxDriverGlobals(DriverGlobals),
                         CompanionTarget,
                         FX_TYPE_COMPANION_TARGET,
                         (PVOID *)&target);

    *BytesReturned = 0;
    return STATUS_NOT_SUPPORTED;
}

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
PEPROCESS
NTAPI
WDFEXPORT(WdfCompanionTargetWdmGetCompanionProcess)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFCOMPANIONTARGET CompanionTarget)
{
    FxCompanionTarget *target;

    FxObjectHandleGetPtr(GetFxDriverGlobals(DriverGlobals),
                         CompanionTarget,
                         FX_TYPE_COMPANION_TARGET,
                         (PVOID *)&target);

    return NULL;
}

} // extern "C"
