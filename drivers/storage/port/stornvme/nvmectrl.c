/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Controller reset, configuration and admin queue setup
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Waits for the controller ready bit to reach the wanted state.
 *
 * The specification gives a controller CAP.TO half seconds to get there, so
 * that is how long we are prepared to wait.
 */
static
BOOLEAN
NvmpWaitForReady(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ BOOLEAN Ready)
{
    NVME_CONTROLLER_STATUS Status;
    ULONG Attempts;
    ULONG Limit;

    Limit = Adapter->ReadyTimeout / (NVME_POLL_INTERVAL_US / 1000);

    for (Attempts = 0; ; Attempts++)
    {
        Status.AsUlong = StorPortReadRegisterUlong(Adapter,
                                                   &Adapter->Registers->CSTS.AsUlong);

        /* A controller that has given up will never reach the wanted state */
        if (Status.CFS)
        {
            DPRINT1("Controller reported a fatal status\n");
            return FALSE;
        }

        if (Status.RDY == (Ready ? 1u : 0u))
            return TRUE;

        if (Attempts >= Limit)
            break;

        StorPortStallExecution(NVME_POLL_INTERVAL_US);
    }

    DPRINT1("Timed out waiting for ready to become %u\n", Ready);
    return FALSE;
}


BOOLEAN
NvmpDisableController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    NVME_CONTROLLER_CONFIGURATION Config;
    NVME_CONTROLLER_STATUS Status;

    Config.AsUlong = StorPortReadRegisterUlong(Adapter,
                                               &Adapter->Registers->CC.AsUlong);
    Status.AsUlong = StorPortReadRegisterUlong(Adapter,
                                               &Adapter->Registers->CSTS.AsUlong);

    /*
     * Taking the enable bit away while the controller is still coming up has
     * undefined results, so let it finish first.
     */
    if (Config.EN && !Status.RDY)
    {
        if (!NvmpWaitForReady(Adapter, TRUE))
            return FALSE;
    }

    Config.EN = 0;
    StorPortWriteRegisterUlong(Adapter,
                               &Adapter->Registers->CC.AsUlong,
                               Config.AsUlong);

    return NvmpWaitForReady(Adapter, FALSE);
}


BOOLEAN
NvmpEnableController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    NVME_CONTROLLER_CONFIGURATION Config;

    Config.AsUlong = 0;
    Config.CSS = NVME_CSS_NVM_COMMAND_SET;
    Config.MPS = Adapter->PageShift - NVME_MIN_PAGE_SHIFT;
    Config.AMS = NVME_AMS_ROUND_ROBIN;
    Config.IOSQES = NVME_SQ_ENTRY_SHIFT;
    Config.IOCQES = NVME_CQ_ENTRY_SHIFT;

    /*
     * Settle the configuration before the enable bit goes in, because the
     * controller latches it the moment it sees the bit.
     */
    StorPortWriteRegisterUlong(Adapter,
                               &Adapter->Registers->CC.AsUlong,
                               Config.AsUlong);

    Config.EN = 1;
    StorPortWriteRegisterUlong(Adapter,
                               &Adapter->Registers->CC.AsUlong,
                               Config.AsUlong);

    return NvmpWaitForReady(Adapter, TRUE);
}


/**
 * @brief Writes a physical address into one of the 64 bit registers.
 */
static
VOID
NvmpWriteRegister64(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PULONGLONG Register,
    _In_ PHYSICAL_ADDRESS Value)
{
    PULONG Half = (PULONG)Register;

    StorPortWriteRegisterUlong(Adapter, Half, Value.LowPart);
    StorPortWriteRegisterUlong(Adapter, Half + 1, Value.HighPart);
}


BOOLEAN
NvmpCreateAdminQueues(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    PNVME_QUEUE_PAIR Queue = &Adapter->AdminQueue;
    NVME_ADMIN_QUEUE_ATTRIBUTES Attributes;
    ULONG SubmissionSize;
    ULONG CompletionSize;
    ULONG Length;

    /* Both queues have to start on a page boundary of their own */
    SubmissionSize = ROUND_TO_PAGES(Adapter->AdminQueueDepth * sizeof(NVME_COMMAND));
    CompletionSize = ROUND_TO_PAGES(Adapter->AdminQueueDepth * sizeof(NVME_COMPLETION_ENTRY));
    Length = SubmissionSize + CompletionSize;

    Adapter->QueueMemory = StorPortGetUncachedExtension(Adapter, ConfigInfo, Length);
    if (Adapter->QueueMemory == NULL)
    {
        DPRINT1("Could not obtain %lu bytes for the admin queues\n", Length);
        return FALSE;
    }

    Adapter->QueueMemorySize = Length;
    RtlZeroMemory(Adapter->QueueMemory, Length);

    Adapter->QueueMemoryAddress = StorPortGetPhysicalAddress(Adapter,
                                                             NULL,
                                                             Adapter->QueueMemory,
                                                             &Length);

    RtlZeroMemory(Queue, sizeof(*Queue));

    Queue->QueueId = NVME_ADMINQ_ID;
    Queue->Depth = Adapter->AdminQueueDepth;
    Queue->Phase = 1;

    Queue->SubmissionQueue = Adapter->QueueMemory;
    Queue->SubmissionAddress = Adapter->QueueMemoryAddress;

    Queue->CompletionQueue = (PNVME_COMPLETION_ENTRY)((PUCHAR)Adapter->QueueMemory + SubmissionSize);
    Queue->CompletionAddress.QuadPart = Adapter->QueueMemoryAddress.QuadPart + SubmissionSize;

    /* Both queue sizes are reported one less than their real depth */
    Attributes.AsUlong = 0;
    Attributes.ASQS = Queue->Depth - 1;
    Attributes.ACQS = Queue->Depth - 1;

    StorPortWriteRegisterUlong(Adapter,
                               &Adapter->Registers->AQA.AsUlong,
                               Attributes.AsUlong);

    NvmpWriteRegister64(Adapter,
                        &Adapter->Registers->ASQ.AsUlonglong,
                        Queue->SubmissionAddress);
    NvmpWriteRegister64(Adapter,
                        &Adapter->Registers->ACQ.AsUlonglong,
                        Queue->CompletionAddress);

    DPRINT1("Admin queues of %lu entries at 0x%I64x and 0x%I64x\n",
            Queue->Depth,
            Queue->SubmissionAddress.QuadPart,
            Queue->CompletionAddress.QuadPart);

    return TRUE;
}


/**
 * @brief Takes the controller from whatever state it was in to ready.
 */
BOOLEAN
NvmpStartController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    if (!NvmpDisableController(Adapter))
    {
        DPRINT1("Controller would not go idle\n");
        return FALSE;
    }

    if (!NvmpCreateAdminQueues(Adapter, ConfigInfo))
        return FALSE;

    if (!NvmpEnableController(Adapter))
    {
        DPRINT1("Controller would not come ready\n");
        return FALSE;
    }

    DPRINT1("Controller is ready\n");

    return TRUE;
}
