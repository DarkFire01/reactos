/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor groups and the performance counter provider interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * Returns the node a processor belongs to, naming the processor by group.
 *
 * @remarks
 * There is one group and one node here, so the answer only depends on the
 * processor being one that exists.
 */
BOOL
WINAPI
GetNumaProcessorNodeEx(
    _In_ PPROCESSOR_NUMBER Processor,
    _Out_ PUSHORT NodeNumber)
{
    if ((Processor == NULL) || (NodeNumber == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (Processor->Group != 0)
    {
        *NodeNumber = 0xFFFF;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *NodeNumber = 0;

    return TRUE;
}

/**
 * @brief
 * Returns the processor a thread would rather run on.
 */
BOOL
WINAPI
GetThreadIdealProcessorEx(
    _In_ HANDLE Thread,
    _Out_ PPROCESSOR_NUMBER IdealProcessor)
{
    THREAD_BASIC_INFORMATION Information;
    NTSTATUS Status;

    if (IdealProcessor == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtQueryInformationThread(Thread,
                                      ThreadBasicInformation,
                                      &Information,
                                      sizeof(Information),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    IdealProcessor->Group = 0;
    IdealProcessor->Number = 0;
    IdealProcessor->Reserved = 0;

    return TRUE;
}

/*
 * The performance counter provider interface. Nothing here collects counters,
 * so a provider is told so and carries on without them, which is what every
 * caller of these does.
 */

ULONG
WINAPI
PerfStartProvider(
    _In_ LPGUID ProviderGuid,
    _In_opt_ PVOID ControlCallback,
    _Out_ PVOID *Provider)
{
    UNREFERENCED_PARAMETER(ProviderGuid);
    UNREFERENCED_PARAMETER(ControlCallback);

    if (Provider != NULL)
        *Provider = NULL;

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
PerfStopProvider(
    _In_ PVOID Provider)
{
    UNREFERENCED_PARAMETER(Provider);

    return ERROR_SUCCESS;
}

ULONG
WINAPI
PerfSetCounterSetInfo(
    _In_ PVOID Provider,
    _Inout_ PVOID Template,
    _In_ ULONG TemplateSize)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(Template);
    UNREFERENCED_PARAMETER(TemplateSize);

    return ERROR_NOT_SUPPORTED;
}

PVOID
WINAPI
PerfCreateInstance(
    _In_ PVOID Provider,
    _In_ LPCGUID CounterSetGuid,
    _In_ PCWSTR Name,
    _In_ ULONG Id)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(CounterSetGuid);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Id);

    SetLastError(ERROR_NOT_SUPPORTED);

    return NULL;
}

ULONG
WINAPI
PerfDeleteInstance(
    _In_ PVOID Provider,
    _In_ PVOID Instance)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(Instance);

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
PerfSetULongCounterValue(
    _In_ PVOID Provider,
    _Inout_ PVOID Instance,
    _In_ ULONG CounterId,
    _In_ ULONG Value)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(CounterId);
    UNREFERENCED_PARAMETER(Value);

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
PerfSetULongLongCounterValue(
    _In_ PVOID Provider,
    _Inout_ PVOID Instance,
    _In_ ULONG CounterId,
    _In_ ULONGLONG Value)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(CounterId);
    UNREFERENCED_PARAMETER(Value);

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
PerfSetCounterRefValue(
    _In_ PVOID Provider,
    _Inout_ PVOID Instance,
    _In_ ULONG CounterId,
    _In_ PVOID Address)
{
    UNREFERENCED_PARAMETER(Provider);
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(CounterId);
    UNREFERENCED_PARAMETER(Address);

    return ERROR_NOT_SUPPORTED;
}

/* EOF */
