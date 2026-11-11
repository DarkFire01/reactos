/*
 * PROJECT:     ReactOS Qualcomm TrustZone driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Secure monitor calling conventions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "tz.h"

#define QCDBG_COMPONENT "qcomtz"
#include <qcom/qcomdbg.h>

/* Bounce memory must be reachable by a monitor that only takes 32-bit addresses */
static
NTSTATUS
TzAcquireBuffer(
    _In_ PTZ_DEVICE Device,
    _In_ SIZE_T Length,
    _Out_ PVOID *Buffer,
    _Out_ PPHYSICAL_ADDRESS Physical)
{
    PHYSICAL_ADDRESS Lowest, Highest, Boundary;
    PVOID Memory;

    Length = ROUND_TO_PAGES(Length);

    if (Device->Spill && Length <= Device->SpillSize)
    {
        *Buffer = Device->Spill;
        *Physical = Device->SpillPhysical;
        return STATUS_SUCCESS;
    }

    Lowest.QuadPart = 0;
    Highest.QuadPart = 0xFFFFFFFF;
    Boundary.QuadPart = 0;
    Memory = MmAllocateContiguousNodeMemory(Length,
                                            Lowest,
                                            Highest,
                                            Boundary,
                                            PAGE_READWRITE,
                                            MM_ANY_NODE_OK);
    if (!Memory)
    {
        *Buffer = NULL;
        Physical->QuadPart = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Buffer = Memory;
    *Physical = MmGetPhysicalAddress(Memory);
    return STATUS_SUCCESS;
}

static
VOID
TzReleaseBuffer(
    _In_ PTZ_DEVICE Device,
    _In_ PVOID Buffer)
{
    if (Buffer && Buffer != Device->Spill)
        MmFreeContiguousMemory(Buffer);
}

/* Arguments past the third move to memory, and its address takes the last register */
static
NTSTATUS
TzSmcccCall(
    _In_ PTZ_DEVICE Device,
    _In_ BOOLEAN Wide,
    _In_ ULONG SmcId,
    _In_ ULONG ParamId,
    _In_reads_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(4) PULONG64 Result,
    _Out_opt_ PULONG64 Session)
{
    ULONG64 Registers[TZ_REGISTER_ARGS] = { 0 };
    ULONG64 SessionValue = 0;
    PHYSICAL_ADDRESS SpillPhysical;
    PVOID Spill = NULL;
    SIZE_T SpillLength;
    ULONG Count, i;
    NTSTATUS Status;
    LONG Ret;

    if (!Args || !Result)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Result, 4 * sizeof(*Result));

    Count = QCOM_TZ_PARAM_COUNT(ParamId);
    if (Count > QCOM_TZ_MAX_ARGS)
        return STATUS_INVALID_PARAMETER;

    if (Count > TZ_REGISTER_ARGS)
    {
        SpillLength = TZ_SPILL_ARGS * (Wide ? sizeof(ULONG64) : sizeof(ULONG));
        Status = TzAcquireBuffer(Device, SpillLength, &Spill, &SpillPhysical);
        if (!NT_SUCCESS(Status))
            return Status;

        RtlZeroMemory(Spill, ROUND_TO_PAGES(SpillLength));

        for (i = 0; i < TZ_REGISTER_ARGS - 1; i++)
            Registers[i] = Wide ? Args[i] : (ULONG)Args[i];
        Registers[TZ_REGISTER_ARGS - 1] = SpillPhysical.LowPart;

        for (i = TZ_REGISTER_ARGS - 1; i < Count; i++)
        {
            if (Wide)
                ((PULONG64)Spill)[i - (TZ_REGISTER_ARGS - 1)] = Args[i];
            else
                ((PULONG)Spill)[i - (TZ_REGISTER_ARGS - 1)] = (ULONG)Args[i];
        }

        TzCleanInvalidateDcache(Spill, SpillLength);
    }
    else
    {
        for (i = 0; i < Count; i++)
            Registers[i] = Wide ? Args[i] : (ULONG)Args[i];
    }

    if (Wide)
        SmcId |= QCOM_TZ_SMC64;
    else
        SmcId &= ~QCOM_TZ_SMC64;

    QC_LOG("SMC%s id 0x%08lx param 0x%lx args %lu: x2=0x%I64x x3=0x%I64x x4=0x%I64x x5=0x%I64x",
           Wide ? "64" : "32", SmcId, ParamId, Count,
           Registers[0], Registers[1], Registers[2], Registers[3]);
    Ret = TzSmcFast(SmcId, ParamId, Registers, &SessionValue);
    QC_LOG("SMC id 0x%08lx -> %ld x1=0x%I64x x2=0x%I64x x3=0x%I64x",
           SmcId, Ret, Registers[0], Registers[1], Registers[2]);
    if (!Wide)
    {
        for (i = 0; i < TZ_REGISTER_ARGS - 1; i++)
            Registers[i] = (ULONG)Registers[i];
        SessionValue = (ULONG)SessionValue;
    }

    if (Ret == 0)
    {
        Result[0] = 1;
        for (i = 0; i < TZ_REGISTER_ARGS - 1; i++)
            Result[i + 1] = Registers[i];
        Status = STATUS_SUCCESS;
    }
    else
    {
        QC_LOG("SMC 0x%08lx returned %ld", SmcId, Ret);
        if (Ret == TZ_SMC_RETURN_WITH_VALUE)
            Result[0] = Registers[0];

        /* Callers expect the monitor code itself, every failure is negative */
        Status = (NTSTATUS)Ret;
    }

    if (Session)
        *Session = SessionValue;

    TzReleaseBuffer(Device, Spill);
    return Status;
}

/* The monitor reads a command from memory and answers after a response header in the same buffer */
static
NTSTATUS
TzLegacyCall(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG SmcId,
    _Inout_updates_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(4) PULONG64 Result)
{
    PHYSICAL_ADDRESS Physical;
    PULONG Command, Response;
    PLONG Data;
    ULONG Offset, i;
    NTSTATUS Status;
    PVOID Buffer;

    if (!Args || !Result)
        return STATUS_INVALID_PARAMETER;

    Status = TzAcquireBuffer(Device, TZ_LEGACY_COMMAND_SIZE, &Buffer, &Physical);
    if (!NT_SUCCESS(Status))
        return Status;

    Command = Buffer;
    Command[0] = TZ_LEGACY_COMMAND_SIZE;
    Command[1] = TZ_LEGACY_BUFFER_OFFSET;
    Command[2] = TZ_LEGACY_RESPONSE_OFFSET;
    Command[3] = (SmcId & 0xFF) | ((SmcId & 0xFF00) << 2);
    for (i = 0; i < QCOM_TZ_MAX_ARGS; i++)
        Command[4 + i] = (ULONG)Args[i];

    TzCleanInvalidateDcache(Command, TZ_LEGACY_COMMAND_SIZE);
    Physical = MmGetPhysicalAddress(Command);

    QC_LOG("legacy SMC cmd 0x%lx (id 0x%08lx) buffer 0x%I64x", Command[3], SmcId, Physical.QuadPart);
    if (TzSmcCommandBuffer((ULONG_PTR)Physical.QuadPart) != 0)
    {
        QC_LOG("legacy SMC FAILED");
        Status = STATUS_TRUST_FAILURE;
    }

    TzInvalidateDcache(Command, TZ_LEGACY_COMMAND_SIZE);

    RtlZeroMemory(Args, QCOM_TZ_MAX_ARGS * sizeof(*Args));

    /* Result[0] is the completion word just ahead of the response payload */
    Response = (PULONG)((PUCHAR)Command + Command[2]);
    Offset = Command[2] + Response[1] - sizeof(ULONG);
    if (Response[1] < sizeof(ULONG) || Offset > TZ_LEGACY_COMMAND_SIZE - 4 * sizeof(ULONG))
    {
        RtlZeroMemory(Result, 4 * sizeof(*Result));
        if (NT_SUCCESS(Status))
            Status = STATUS_TRUST_FAILURE;
    }
    else
    {
        Data = (PLONG)((PUCHAR)Command + Offset);
        for (i = 0; i < 4; i++)
            Result[i] = (ULONG64)(LONG64)Data[i];
    }

    TzReleaseBuffer(Device, Buffer);
    return Status;
}

NTSTATUS
TzSipCall(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG SmcId,
    _In_ ULONG ParamId,
    _Inout_updates_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(4) PULONG64 Result)
{
    switch (Device->Convention)
    {
        case TzConventionSmc64:
            return TzSmcccCall(Device, TRUE, SmcId, ParamId, Args, Result, NULL);

        case TzConventionSmc32:
            return TzSmcccCall(Device, FALSE, SmcId, ParamId, Args, Result, NULL);

        default:
            return TzLegacyCall(Device, SmcId, Args, Result);
    }
}

/* No listener is registered, so every request the trusted OS makes is refused */
static
NTSTATUS
TzServiceListener(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG ListenerId)
{
    UNREFERENCED_PARAMETER(Device);

    QC_LOG("Trusted OS asked for listener %lu", ListenerId);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
TzOsCall(
    _In_ PTZ_DEVICE Device,
    _In_ ULONG SmcId,
    _In_ ULONG ParamId,
    _In_reads_(QCOM_TZ_MAX_ARGS) PULONG64 Args,
    _Out_writes_(3) PULONG64 Result)
{
    ULONG64 Local[QCOM_TZ_MAX_ARGS];
    ULONG64 Raw[4];
    ULONG64 Session;
    ULONG ListenerId;
    NTSTATUS Status;
    BOOLEAN Wide;

    /* The legacy trusted OS protocol is not implemented */
    if (Device->Convention == TzConventionLegacy)
        return STATUS_NOT_SUPPORTED;

    Wide = (Device->Convention == TzConventionSmc64);
    RtlCopyMemory(Local, Args, sizeof(Local));

    for (;;)
    {
        Status = TzSmcccCall(Device, Wide, SmcId, ParamId, Local, Raw, &Session);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Raw[1] != TZ_RESULT_INCOMPLETE || Raw[2] != TZ_RESULT_LISTENER_REQUEST)
            break;

        /* Answer the listener and resume, the remaining arguments carry over */
        ListenerId = (ULONG)Raw[3];
        QC_LOG("trusted OS wants listener %lu, refusing and resuming", ListenerId);
        Local[0] = ListenerId;
        Local[1] = NT_SUCCESS(TzServiceListener(Device, ListenerId)) ? 0 : 0xFFFFFFFF;
        SmcId = TZ_OS_LISTENER_RESPONSE;
        ParamId = 2;
    }

    Result[0] = Raw[1];
    Result[1] = Raw[2];
    Result[2] = Raw[3];
    return STATUS_SUCCESS;
}

/* Asks the information service whether a call exists, which also proves the monitor speaks SMCCC */
static
BOOLEAN
TzProbeSmccc(
    _Inout_ PTZ_DEVICE Device)
{
    ULONG64 Args[QCOM_TZ_MAX_ARGS] = { 0 };
    ULONG64 Result[4];
    NTSTATUS Status;

    Args[0] = QCOM_TZ_SIP_ID(TZ_SVC_INFO, TZ_INFO_PROBED_COMMAND);
    Status = TzSmcccCall(Device,
                         FALSE,
                         QCOM_TZ_SIP_ID(TZ_SVC_INFO, TZ_INFO_IS_CALL_AVAILABLE),
                         1,
                         Args,
                         Result,
                         NULL);
    if (!NT_SUCCESS(Status))
        return FALSE;

    RtlZeroMemory(Args, sizeof(Args));
    Args[0] = TZ_FEATURE_SMC64;
    Status = TzSmcccCall(Device,
                         FALSE,
                         QCOM_TZ_SIP_ID(TZ_SVC_INFO, TZ_INFO_FEATURE_VERSION),
                         1,
                         Args,
                         Result,
                         NULL);
    if (NT_SUCCESS(Status) && Result[1] >= TZ_SMC64_MIN_VERSION)
    {
        QC_LOG("TrustZone: 64-bit SMC interface");
        Device->Convention = TzConventionSmc64;
    }
    else
    {
        QC_LOG("TrustZone: 32-bit SMC interface");
        Device->Convention = TzConventionSmc32;
    }

    return TRUE;
}

NTSTATUS
TzInitializeMonitor(
    _Inout_ PTZ_DEVICE Device)
{
    PHYSICAL_ADDRESS Physical;
    NTSTATUS Status;
    SIZE_T Size;
    PVOID Buffer;

    Device->Spill = NULL;
    Device->SpillSize = 0;

    if (TzProbeSmccc(Device))
    {
        Size = TZ_SPILL_SIZE_SMCCC;
    }
    else
    {
        QC_LOG("TrustZone: legacy command buffer interface");
        Device->Convention = TzConventionLegacy;
        Size = TZ_SPILL_SIZE_LEGACY;
    }

    Status = TzAcquireBuffer(Device, Size, &Buffer, &Physical);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("spill buffer alloc FAILED 0x%lx", Status);
        return Status;
    }

    Device->Spill = Buffer;
    Device->SpillPhysical = Physical;
    Device->SpillSize = Size;
    QC_LOG("monitor ready, convention %u, spill %Iu bytes at 0x%I64x",
           Device->Convention, Size, Physical.QuadPart);
    return STATUS_SUCCESS;
}

VOID
TzReleaseMonitor(
    _Inout_ PTZ_DEVICE Device)
{
    PVOID Buffer = Device->Spill;

    Device->Spill = NULL;
    Device->SpillSize = 0;
    if (Buffer)
        MmFreeContiguousMemory(Buffer);
}
