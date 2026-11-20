/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management performance handling
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Serializes processor registration and every performance request to the plug-in */
static KMUTEX PpmPerfLock;

static PPM_PEP_PROCESSOR PpmPepProcessors[MAXIMUM_PROCESSORS];
static ULONG PpmPepProcessorCount;
static volatile LONG PpmPepProcessorsPending;
static BOOLEAN PpmPepFailureLogged;

static PPM_PERF_DOMAIN PpmPerfDomains[MAXIMUM_PROCESSORS];
static ULONG PpmPerfDomainCount;

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
PpmAcquirePerfLock(VOID)
{
    KeWaitForSingleObject(&PpmPerfLock, Executive, KernelMode, FALSE, NULL);
}

static
VOID
PpmReleasePerfLock(VOID)
{
    KeReleaseMutex(&PpmPerfLock, FALSE);
}

/**
 * @brief Maps an ACPI processor device to a processor number through its instance, the _UID.
 *
 * @return The processor number, MAXULONG when the _UID names no running processor.
 */
static
ULONG
PpmProcessorNumberFromInstance(
    _In_ PCUNICODE_STRING InstancePath)
{
    UNICODE_STRING Instance;
    USHORT Start;
    ULONG Number;

    Start = InstancePath->Length / sizeof(WCHAR);
    while (Start > 0 && InstancePath->Buffer[Start - 1] != L'\\')
        Start--;

    Instance.Buffer = &InstancePath->Buffer[Start];
    Instance.Length = InstancePath->Length - Start * sizeof(WCHAR);
    Instance.MaximumLength = Instance.Length;

    if (!NT_SUCCESS(RtlUnicodeStringToInteger(&Instance, 10, &Number)) ||
        Number >= (ULONG)KeNumberProcessors)
    {
        return MAXULONG;
    }

    return Number;
}

/**
 * @brief Adds a processor to the domain the plug-in reported, creating the domain on first sight.
 *
 * @remarks The plug-in starts every domain at its nominal level, which becomes the first request.
 */
static
ULONG
PpmJoinPerfDomain(
    _In_ PPEP_PPM_QUERY_PERF_CAPABILITIES Capabilities)
{
    PPPM_PERF_DOMAIN Domain;
    ULONG Index;

    for (Index = 0; Index < PpmPerfDomainCount; Index++)
    {
        if (PpmPerfDomains[Index].DomainId == Capabilities->DomainId)
            return Index;
    }

    if (PpmPerfDomainCount == RTL_NUMBER_OF(PpmPerfDomains))
        return MAXULONG;

    Domain = &PpmPerfDomains[PpmPerfDomainCount];
    Domain->DomainId = Capabilities->DomainId;
    Domain->Members = Capabilities->DomainMembers;
    Domain->Highest = Capabilities->HighestPerformance;
    Domain->Nominal = Capabilities->NominalPerformance;
    Domain->LowestNonlinear = Capabilities->LowestNonlinearPerformance;
    Domain->Lowest = Capabilities->LowestPerformance;
    Domain->Requested.Minimum = Domain->Lowest;
    Domain->Requested.Maximum = Domain->Highest;
    Domain->Requested.Desired = Domain->Nominal;
    Domain->Current = Domain->Requested;

    DPRINT1("PPM: perf domain %lu, %lu processors, lowest %lu nominal %lu highest %lu\n",
            Domain->DomainId, Domain->Members, Domain->Lowest, Domain->Nominal, Domain->Highest);

    return PpmPerfDomainCount++;
}

/**
 * @brief Registers one processor device with the plug-in that prepares it and queries its performance.
 *
 * @return NULL on success, else why the processor got no performance control.
 */
static
PCSTR
PpmRegisterPepProcessor(
    _Inout_ PPPM_PEP_PROCESSOR Processor)
{
    PEP_PPM_QUERY_PERF_CAPABILITIES PerfCapabilities;
    PEP_PPM_QUERY_CAPABILITIES Capabilities;
    PEP_DEVICE_REGISTER_V2 Register;
    PEP_REGISTER_DEVICE_V2 Device;
    PEP_PREPARE_DEVICE Prepare;
    PPOP_FX_PLUGIN Plugin;

    if (!NT_SUCCESS(PopFxQueryDeviceId(Processor->Pdo, &Processor->DeviceId)))
        return "no device id";

    for (Plugin = PopFxNextPlugin(NULL); Plugin; Plugin = PopFxNextPlugin(Plugin))
    {
        if (!Plugin->AcceptProcessorNotification)
            continue;

        Prepare.DeviceId = &Processor->DeviceId;
        Prepare.DeviceAccepted = FALSE;
        if (PopFxNotifyPlugin(Plugin, PEP_DPM_PREPARE_DEVICE, &Prepare) && Prepare.DeviceAccepted)
            break;
    }

    if (!Plugin)
        return "not prepared";

    /* Processors carry no components, the plug-in only needs a kernel handle to call back with */
    RtlZeroMemory(&Register, sizeof(Register));
    RtlZeroMemory(&Device, sizeof(Device));
    Device.DeviceId = &Processor->DeviceId;
    Device.KernelHandle = (POHANDLE)Processor;
    Device.Register = &Register;
    if (!PopFxNotifyPlugin(Plugin, PEP_DPM_REGISTER_DEVICE, &Device) ||
        Device.DeviceAccepted != PepDeviceAccepted ||
        !Device.DeviceHandle)
    {
        return "not registered";
    }

    Processor->Plugin = Plugin;
    Processor->PepHandle = Device.DeviceHandle;

    /* The plug-in brings up a processor's performance tables when it answers this */
    RtlZeroMemory(&Capabilities, sizeof(Capabilities));
    if (!Plugin->AcceptProcessorNotification(Processor->PepHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Capabilities))
        return "no capabilities";

    if (!Capabilities.PerformanceStatesSupported)
        return "no performance states";

    RtlZeroMemory(&PerfCapabilities, sizeof(PerfCapabilities));
    if (!Plugin->AcceptProcessorNotification(Processor->PepHandle,
                                             PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES,
                                             &PerfCapabilities) ||
        !PerfCapabilities.HighestPerformance ||
        PerfCapabilities.LowestPerformance > PerfCapabilities.HighestPerformance)
    {
        return "no performance range";
    }

    Processor->Domain = PpmJoinPerfDomain(&PerfCapabilities);
    if (Processor->Domain == MAXULONG)
        return "too many domains";

    return NULL;
}

/* PUBLIC FUNCTIONS ***********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PpmPerfInitialize(VOID)
{
    KeInitializeMutex(&PpmPerfLock, 0);
}

/**
 * @brief Takes an ACPI processor device for the PPM, which registers it with the plug-in itself.
 *
 * @return TRUE when the device is a processor, the caller then leaves it alone.
 */
BOOLEAN
NTAPI
PpmPepTrackProcessor(
    _In_ PDEVICE_OBJECT Pdo)
{
    static const UNICODE_STRING ProcessorPrefix = RTL_CONSTANT_STRING(L"ACPI\\ACPI0007\\");
    PPPM_PEP_PROCESSOR Processor;
    PDEVICE_NODE DeviceNode;
    BOOLEAN Known = FALSE;
    ULONG Index;

    PAGED_CODE();

    DeviceNode = IopGetDeviceNode(Pdo);
    if (!DeviceNode || !RtlPrefixUnicodeString(&ProcessorPrefix, &DeviceNode->InstancePath, TRUE))
        return FALSE;

    PpmAcquirePerfLock();
    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        if (PpmPepProcessors[Index].Pdo == Pdo)
        {
            Known = TRUE;
            break;
        }
    }

    if (!Known && PpmPepProcessorCount < RTL_NUMBER_OF(PpmPepProcessors))
    {
        Processor = &PpmPepProcessors[PpmPepProcessorCount++];
        ObReferenceObject(Pdo);
        Processor->Pdo = Pdo;
        Processor->Number = PpmProcessorNumberFromInstance(&DeviceNode->InstancePath);
        Processor->Domain = MAXULONG;
        InterlockedIncrement(&PpmPepProcessorsPending);
    }
    PpmReleasePerfLock();

    return TRUE;
}

/**
 * @brief Registers the processors not yet offered to a plug-in.
 *
 * @remarks Called once a plug-in is known. Each processor gets one try, a refusal
 * leaves its clocks with the plug-in's own defaults.
 */
VOID
NTAPI
PpmPepRegisterProcessors(VOID)
{
    PPPM_PEP_PROCESSOR Processor;
    PCSTR Failure;
    ULONG Index;

    PAGED_CODE();

    if (!ReadNoFence(&PpmPepProcessorsPending))
        return;

    PpmAcquirePerfLock();
    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        Processor = &PpmPepProcessors[Index];
        if (Processor->Attempted)
            continue;

        Processor->Attempted = TRUE;
        InterlockedDecrement(&PpmPepProcessorsPending);

        Failure = PpmRegisterPepProcessor(Processor);
        if (!Failure)
            continue;

        Processor->Domain = MAXULONG;
        if (!PpmPepFailureLogged)
        {
            PpmPepFailureLogged = TRUE;
            DPRINT1("PPM: %wZ has no performance control, %s\n", &Processor->DeviceId, Failure);
        }
    }
    PpmReleasePerfLock();
}

/**
 * @brief
 * DPC routine that fires periodically to sample processor performance state
 * and adjust throttling accordingly.
 *
 * @param[in] Dpc
 * The DPC object associated with this performance timer.
 *
 * @param[in] DeferredContext
 * A pointer to the PKPRCB (Processor Control Block) for the target processor.
 *
 * @param[in] SystemArgument1
 * Unused.
 *
 * @param[in] SystemArgument2
 * Unused.
 *
 * @remarks
 * A full implementation would evaluate the current processor idle time vs.
 * busy time, look up the appropriate P-state in the processor's performance
 * state table, and request a frequency/voltage transition if needed. This
 * requires HAL/ACPI _PSS support which is not yet available. For now the
 * routine only updates the performance accounting timestamps so that the
 * system does not accumulate stale data.
 */
VOID
NTAPI
PpmPerfIdleDpcRoutine(
    _In_ PKDPC Dpc,
    _In_ PVOID DeferredContext,
    _In_ PVOID SystemArgument1,
    _In_ PVOID SystemArgument2)
{
    PKPRCB Prcb = (PKPRCB)DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (Prcb == NULL)
    {
        return;
    }

    /*
     * Record the current performance timer tick. A complete performance
     * governor would compare KernelTime, UserTime and IdleTime over successive
     * samples to determine utilization, then walk the _PSS P-state table and
     * issue an ACPI _PPC/_PCT write to adjust the processor frequency/voltage.
     *
     * FIXME: Implement P-state evaluation and transition once HAL exposes
     * processor performance controls via HalSetSystemInformation.
     */
    Prcb->PowerState.LastSysTime = KeQueryInterruptTime();
}

/* EOF */
