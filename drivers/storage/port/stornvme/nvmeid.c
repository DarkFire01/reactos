/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Controller and namespace identification
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Runs one Identify command into the scratch page.
 */
static
BOOLEAN
NvmpIdentify(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NamespaceId,
    _In_ UCHAR ControllerOrNamespace)
{
    NVME_COMMAND Command;

    RtlZeroMemory(&Command, sizeof(Command));
    RtlZeroMemory(Adapter->ScratchBuffer, PAGE_SIZE);

    Command.CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    Command.NSID = NamespaceId;
    Command.PRP1 = Adapter->ScratchAddress.QuadPart;
    Command.u.IDENTIFY.CDW10.CNS = ControllerOrNamespace;

    return NvmpIssueAdminCommand(Adapter, &Command, NULL);
}


/**
 * @brief Works out the largest transfer the controller will take.
 *
 * MDTS is an exponent over the smallest page the controller supports, and a
 * value of zero means it has no limit of its own.
 */
static
ULONG
NvmpMaximumTransfer(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_IDENTIFY_CONTROLLER_DATA Data)
{
    ULONG MinimumPage;
    ULONG Length;

    if (Data->MDTS == 0 || Data->MDTS >= 32)
        return NVME_MAX_TRANSFER_LENGTH;

    MinimumPage = 1UL << (NVME_MIN_PAGE_SHIFT + Adapter->Capabilities.MPSMIN);

    /* Guard against a controller claiming more than the host can describe */
    if (Data->MDTS >= 31 || (ULONG)Data->MDTS > 31 - NVME_MIN_PAGE_SHIFT)
        return NVME_MAX_TRANSFER_LENGTH;

    Length = MinimumPage << Data->MDTS;
    if (Length > NVME_MAX_TRANSFER_LENGTH)
        Length = NVME_MAX_TRANSFER_LENGTH;

    return Length;
}


BOOLEAN
NvmpIdentifyController(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    PNVME_IDENTIFY_CONTROLLER_DATA Data;

    if (!NvmpIdentify(Adapter, 0, NVME_IDENTIFY_CNS_CONTROLLER))
    {
        DPRINT1("Identify controller failed\n");
        return FALSE;
    }

    Data = Adapter->ScratchBuffer;

    Adapter->NamespaceCount = Data->NN;
    Adapter->VolatileWriteCache = (Data->VWC.Present != 0);
    Adapter->MaximumTransferLength = NvmpMaximumTransfer(Adapter, Data);

    RtlCopyMemory(Adapter->SerialNumber, Data->SN, sizeof(Adapter->SerialNumber));
    RtlCopyMemory(Adapter->ModelNumber, Data->MN, sizeof(Adapter->ModelNumber));
    RtlCopyMemory(Adapter->FirmwareRevision, Data->FR, sizeof(Adapter->FirmwareRevision));

    DPRINT1("Controller has %lu namespaces, transfers up to %lu bytes%s\n",
            Adapter->NamespaceCount,
            Adapter->MaximumTransferLength,
            Adapter->VolatileWriteCache ? ", with a write cache" : "");

    /*
     * A controller that reports no namespaces has nothing to offer, and one
     * that reports more than we can address would need a bigger LUN space.
     */
    if (Adapter->NamespaceCount == 0)
    {
        DPRINT1("Controller reports no namespaces\n");
        return FALSE;
    }

    if (Adapter->NamespaceCount > NVME_MAX_NAMESPACES)
        Adapter->NamespaceCount = NVME_MAX_NAMESPACES;

    return TRUE;
}


BOOLEAN
NvmpIdentifyNamespace(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NamespaceId,
    _Out_ PNVME_NAMESPACE Namespace)
{
    PNVME_IDENTIFY_NAMESPACE_DATA Data;
    ULONG Format;

    RtlZeroMemory(Namespace, sizeof(*Namespace));

    if (!NvmpIdentify(Adapter, NamespaceId, NVME_IDENTIFY_CNS_SPECIFIC_NAMESPACE))
    {
        DPRINT1("Identify namespace %lu failed\n", NamespaceId);
        return FALSE;
    }

    Data = Adapter->ScratchBuffer;

    /* A namespace of no size is one that is not attached to this controller */
    if (Data->NSZE == 0)
        return FALSE;

    Format = Data->FLBAS.LbaFormatIndex;
    if (Format > Data->NLBAF)
    {
        DPRINT1("Namespace %lu names format %lu of %u\n",
                NamespaceId, Format, Data->NLBAF);
        return FALSE;
    }

    /*
     * A format with metadata needs the extra bytes carried in or out of band,
     * neither of which the translation below knows how to do.
     */
    if (Data->LBAF[Format].MS != 0)
    {
        DPRINT1("Namespace %lu wants %u metadata bytes per block\n",
                NamespaceId, Data->LBAF[Format].MS);
        return FALSE;
    }

    Namespace->NamespaceId = NamespaceId;
    Namespace->BlockCount = Data->NSZE;
    Namespace->BlockShift = Data->LBAF[Format].LBADS;
    Namespace->BlockSize = 1UL << Namespace->BlockShift;
    Namespace->Present = TRUE;

    DPRINT1("Namespace %lu holds %I64u blocks of %lu bytes\n",
            NamespaceId, Namespace->BlockCount, Namespace->BlockSize);

    return TRUE;
}


/**
 * @brief Finds every namespace the controller has attached.
 */
BOOLEAN
NvmpEnumerateNamespaces(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    ULONG NamespaceId;
    ULONG Found = 0;

    /*
     * Namespace identifiers start at one and run to the count the controller
     * reported, with gaps where a namespace is not attached.
     */
    for (NamespaceId = 1; NamespaceId <= Adapter->NamespaceCount; NamespaceId++)
    {
        if (NvmpIdentifyNamespace(Adapter,
                                  NamespaceId,
                                  &Adapter->Namespaces[NamespaceId - 1]))
        {
            Found++;
        }
    }

    DPRINT1("Found %lu attached namespaces\n", Found);

    return (Found != 0);
}
