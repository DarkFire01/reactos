/*
 * PROJECT:     ReactOS Qualcomm TrustZone driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/tzif.h>

#define TZ_POOL_TAG                 'zTcQ'

/* Information service, used to probe what the monitor speaks */
#define TZ_SVC_INFO                 0x06
#define TZ_INFO_IS_CALL_AVAILABLE   0x01
#define TZ_INFO_PROBED_COMMAND      0x02
#define TZ_INFO_FEATURE_VERSION     0x03

/* Feature whose version tells whether 64-bit calls are accepted */
#define TZ_FEATURE_SMC64            10
#define TZ_SMC64_MIN_VERSION        0x01000000

/* Monitor return that hands the call back with a value in x1 */
#define TZ_SMC_RETURN_WITH_VALUE    (-2)

/* Trusted OS asking the normal world to service a listener */
#define TZ_RESULT_INCOMPLETE        1
#define TZ_RESULT_LISTENER_REQUEST  0xEE02
#define TZ_OS_LISTENER_RESPONSE     (QCOM_TZ_OWNER_TRUSTED_OS | 0x000103)

/* Register file of the 64-bit convention: three arguments, then a spill address */
#define TZ_REGISTER_ARGS            4
#define TZ_SPILL_ARGS               (QCOM_TZ_MAX_ARGS - (TZ_REGISTER_ARGS - 1))

/* Command buffer of the legacy convention */
#define TZ_LEGACY_COMMAND_SIZE      84
#define TZ_LEGACY_BUFFER_OFFSET     16
#define TZ_LEGACY_RESPONSE_OFFSET   28

#define TZ_SPILL_SIZE_SMCCC         PAGE_SIZE
#define TZ_SPILL_SIZE_LEGACY        0x10000

typedef enum _TZ_CONVENTION
{
    TzConventionLegacy,
    TzConventionSmc32,
    TzConventionSmc64
} TZ_CONVENTION;

typedef struct _TZ_DEVICE
{
    WDFDEVICE WdfDevice;
    WDFWAITLOCK Lock;
    TZ_CONVENTION Convention;
    BOOLEAN Ready;

    /* Shared bounce page, reused by every call while the lock is held */
    PVOID Spill;
    PHYSICAL_ADDRESS SpillPhysical;
    SIZE_T SpillSize;
} TZ_DEVICE, *PTZ_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TZ_DEVICE, TzGetDevice)

/* smc.s */
LONG
TzSmcCommandBuffer(
    _In_ ULONG_PTR CommandAddress);

LONG
TzSmcFast(
    _In_ ULONG FunctionId,
    _In_ ULONG ParamId,
    _Inout_updates_(TZ_REGISTER_ARGS) PULONG64 Registers,
    _Out_ PULONG64 Session);

VOID
TzCleanInvalidateDcache(
    _In_ PVOID Buffer,
    _In_ SIZE_T Length);

VOID
TzInvalidateDcache(
    _In_ PVOID Buffer,
    _In_ SIZE_T Length);

/* smc.c */
NTSTATUS
TzInitializeMonitor(
    _Inout_ PTZ_DEVICE Device);

VOID
TzReleaseMonitor(
    _Inout_ PTZ_DEVICE Device);

NTSTATUS
TzSipCall(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG SmcId,
    _In_ ULONG ParamId,
    _Inout_updates_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(4) PULONG64 Result);

NTSTATUS
TzOsCall(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG SmcId,
    _In_ ULONG ParamId,
    _In_reads_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(3) PULONG64 Result);
