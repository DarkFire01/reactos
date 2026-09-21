/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6 scatter gather DMA
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

typedef struct _CORE_SG_DMA
{
    PLOGICAL_ADAPTER Adapter;
    PDMA_ADAPTER DmaAdapter;
    MINIPORT_PROCESS_SG_LIST_HANDLER ProcessSGListHandler;
    MINIPORT_ALLOCATE_SHARED_MEM_COMPLETE_HANDLER SharedMemAllocateCompleteHandler;
    ULONG Flags;
    ULONG ScatterGatherListSize;
} CORE_SG_DMA, *PCORE_SG_DMA;

/*
 * What a NET_BUFFER's list was built from, kept in the NDIS reserved area
 * until NdisMFreeNetBufferSGList.
 */
#define CORE_SG_WRITE_TO_DEVICE     0x1
#define CORE_SG_BOUNCED             0x2

#define CORE_NB_SG_BOUNCE_MDL(_Nb)  (*(PMDL *)&(_Nb)->NdisReserved[0])
#define CORE_NB_SG_FLAGS(_Nb)       (*(PULONG_PTR)&(_Nb)->NdisReserved[1])

/**
 * @brief
 * Sets up bus master DMA for a 6.x miniport. The adapter also becomes the one
 * NdisMAllocateSharedMemory allocates from.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in,out] DmaDescription
 * What the device can do. ScatterGatherListSize is filled in.
 *
 * @param[out] NdisMiniportDmaHandle
 * The handle for the other DMA calls.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why DMA cannot be used.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMRegisterScatterGatherDma(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_SG_DMA_DESCRIPTION DmaDescription,
    PNDIS_HANDLE NdisMiniportDmaHandle)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;
    DEVICE_DESCRIPTION Description;
    BOOLEAN Dma64 = (DmaDescription->Flags & NDIS_SG_DMA_64_BIT_ADDRESS) != 0;
    ULONG MapRegisters = 0;
    ULONG SgMapRegisters;
    ULONG SgListSize;
    PCORE_SG_DMA SgDma;
    NTSTATUS Status;

    *NdisMiniportDmaHandle = NULL;

    if (!Driver->Ndis6Driver)
        return NDIS_STATUS_NOT_SUPPORTED;

    if (DmaDescription->Header.Revision == 0)
        return NDIS_STATUS_BAD_VERSION;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION2;

    /* 6.50 miniports can ask for the version 3 DMA interfaces, and nothing else */
    if (Driver->Characteristics6.MajorNdisVersion > 6 ||
        Driver->Characteristics6.MinorNdisVersion >= 50)
    {
        if (DmaDescription->Flags & ~(NDIS_SG_DMA_64_BIT_ADDRESS | NDIS_SG_DMA_V3_HAL_API))
            return NDIS_STATUS_INVALID_PARAMETER;

        if (DmaDescription->Flags & NDIS_SG_DMA_V3_HAL_API)
        {
            Description.Version = DEVICE_DESCRIPTION_VERSION3;
            Description.DmaAddressWidth = Dma64 ? 64 : 32;
        }
    }

    if (!(Adapter->NdisMiniportBlock.Flags & NDIS_ATTRIBUTE_BUS_MASTER))
        return NDIS_STATUS_NOT_SUPPORTED;

    if (Adapter->Core.SgDma != NULL)
        return NDIS_STATUS_RESOURCES;

    SgDma = ExAllocatePoolWithTag(NonPagedPool, sizeof(*SgDma), NDIS_TAG);
    if (SgDma == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(SgDma, sizeof(*SgDma));

    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma32BitAddresses = !Dma64;
    Description.Dma64BitAddresses = Dma64;
    Description.BusNumber = Adapter->NdisMiniportBlock.BusNumber;
    Description.InterfaceType = (INTERFACE_TYPE)Adapter->NdisMiniportBlock.BusType;
    Description.MaximumLength = 2 * DmaDescription->MaximumPhysicalMapping;

    SgDma->DmaAdapter = IoGetDmaAdapter(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                                        &Description,
                                        &MapRegisters);
    if (SgDma->DmaAdapter == NULL)
    {
        NdisWriteErrorLogEntry(Adapter, NDIS_ERROR_CODE_OUT_OF_RESOURCES, 1, 0xFFFFFFFF);
        ExFreePoolWithTag(SgDma, NDIS_TAG);
        return NDIS_STATUS_RESOURCES;
    }

    /* The largest list the adapter's map registers can describe */
    Status = SgDma->DmaAdapter->DmaOperations->CalculateScatterGatherList(SgDma->DmaAdapter,
                                                                          NULL,
                                                                          NULL,
                                                                          MapRegisters << PAGE_SHIFT,
                                                                          &SgListSize,
                                                                          &SgMapRegisters);
    if (!NT_SUCCESS(Status))
    {
        SgDma->DmaAdapter->DmaOperations->PutDmaAdapter(SgDma->DmaAdapter);
        ExFreePoolWithTag(SgDma, NDIS_TAG);
        return NDIS_STATUS_RESOURCES;
    }

    SgDma->Adapter = Adapter;
    SgDma->ProcessSGListHandler = DmaDescription->ProcessSGListHandler;
    SgDma->SharedMemAllocateCompleteHandler = DmaDescription->SharedMemAllocateCompleteHandler;
    SgDma->Flags = DmaDescription->Flags;
    SgDma->ScatterGatherListSize = SgListSize;

    Adapter->Core.SgDma = SgDma;
    Adapter->NdisMiniportBlock.SystemAdapterObject = SgDma->DmaAdapter;

    DmaDescription->ScatterGatherListSize = SgListSize;
    *NdisMiniportDmaHandle = SgDma;

    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Releases the DMA adapter from NdisMRegisterScatterGatherDma.
 *
 * @param[in] NdisMiniportDmaHandle
 * The handle it returned.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMDeregisterScatterGatherDma(
    NDIS_HANDLE NdisMiniportDmaHandle)
{
    PCORE_SG_DMA SgDma = NdisMiniportDmaHandle;
    PLOGICAL_ADAPTER Adapter = SgDma->Adapter;

    if (Adapter->NdisMiniportBlock.SystemAdapterObject == SgDma->DmaAdapter)
        Adapter->NdisMiniportBlock.SystemAdapterObject = NULL;

    if (Adapter->Core.SgDma == SgDma)
        Adapter->Core.SgDma = NULL;

    SgDma->DmaAdapter->DmaOperations->PutDmaAdapter(SgDma->DmaAdapter);
    ExFreePoolWithTag(SgDma, NDIS_TAG);
}

/* Copies between a flat buffer and an MDL chain, starting Offset bytes into the chain */
static
BOOLEAN
CoreCopyMdlChain(
    _In_ PMDL Mdl,
    _In_ ULONG Offset,
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN ToBuffer)
{
    PUCHAR Address;
    ULONG Chunk;

    for (; Mdl != NULL && Length != 0; Mdl = Mdl->Next)
    {
        if (Offset >= MmGetMdlByteCount(Mdl))
        {
            Offset -= MmGetMdlByteCount(Mdl);
            continue;
        }

        Address = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        if (Address == NULL)
            return FALSE;

        Chunk = min(MmGetMdlByteCount(Mdl) - Offset, Length);

        if (ToBuffer)
            RtlCopyMemory(Buffer, Address + Offset, Chunk);
        else
            RtlCopyMemory(Address + Offset, Buffer, Chunk);

        Buffer += Chunk;
        Length -= Chunk;
        Offset = 0;
    }

    return Length == 0;
}

/*
 * The HAL could not map the NET_BUFFER's own pages, so the data goes through
 * a buffer of NDIS's instead and is copied back when the list is freed.
 */
static
NDIS_STATUS
CoreBuildBouncedSGList(
    _In_ PCORE_SG_DMA SgDma,
    _In_ PNET_BUFFER NetBuffer,
    _In_ PVOID Context,
    _In_ ULONG Length,
    _In_ BOOLEAN WriteToDevice)
{
    PUCHAR Bounce;
    PMDL BounceMdl;
    NTSTATUS Status;

    Bounce = ExAllocatePoolWithTag(NonPagedPool, Length, NDIS_TAG);
    if (Bounce == NULL)
        return NDIS_STATUS_RESOURCES;

    BounceMdl = IoAllocateMdl(Bounce, Length, FALSE, FALSE, NULL);
    if (BounceMdl == NULL)
    {
        ExFreePoolWithTag(Bounce, NDIS_TAG);
        return NDIS_STATUS_RESOURCES;
    }

    MmBuildMdlForNonPagedPool(BounceMdl);
    BounceMdl->Next = NULL;

    if (WriteToDevice &&
        !CoreCopyMdlChain(NET_BUFFER_CURRENT_MDL(NetBuffer), 0, Bounce, Length, TRUE))
    {
        Status = NDIS_STATUS_FAILURE;
        goto Failed;
    }

    CORE_NB_SG_BOUNCE_MDL(NetBuffer) = BounceMdl;
    CORE_NB_SG_FLAGS(NetBuffer) |= CORE_SG_BOUNCED;

    Status = SgDma->DmaAdapter->DmaOperations->GetScatterGatherList(
        SgDma->DmaAdapter,
        SgDma->Adapter->NdisMiniportBlock.DeviceObject,
        BounceMdl,
        Bounce,
        Length,
        (PDRIVER_LIST_CONTROL)SgDma->ProcessSGListHandler,
        Context,
        WriteToDevice);
    if (NT_SUCCESS(Status))
        return NDIS_STATUS_SUCCESS;

    CORE_NB_SG_BOUNCE_MDL(NetBuffer) = NULL;
    CORE_NB_SG_FLAGS(NetBuffer) &= ~CORE_SG_BOUNCED;

Failed:
    IoFreeMdl(BounceMdl);
    ExFreePoolWithTag(Bounce, NDIS_TAG);
    return Status;
}

/**
 * @brief
 * Maps a NET_BUFFER for the device and hands the list to the miniport's
 * MiniportProcessSGList.
 *
 * The list starts at the beginning of the MDL the data starts in, so it
 * covers NET_BUFFER_CURRENT_MDL_OFFSET bytes before the data. The NET_BUFFER's
 * current MDL and offset are set from its data offset first.
 *
 * @param[in] NdisMiniportDmaHandle
 * The handle from NdisMRegisterScatterGatherDma.
 *
 * @param[in] NetBuffer
 * The data to map.
 *
 * @param[in] Context
 * What MiniportProcessSGList gets.
 *
 * @param[in] Flags
 * NDIS_SG_LIST_WRITE_TO_DEVICE for a send.
 *
 * @param[in] ScatterGatherListBuffer
 * Optional storage for the list.
 *
 * @param[in] ScatterGatherListBufferSize
 * Its size.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why the data could not be mapped.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMAllocateNetBufferSGList(
    NDIS_HANDLE NdisMiniportDmaHandle,
    PNET_BUFFER NetBuffer,
    PVOID Context,
    ULONG Flags,
    PVOID ScatterGatherListBuffer,
    ULONG ScatterGatherListBufferSize)
{
    PCORE_SG_DMA SgDma = NdisMiniportDmaHandle;
    PDMA_OPERATIONS Operations = SgDma->DmaAdapter->DmaOperations;
    PDEVICE_OBJECT DeviceObject = SgDma->Adapter->NdisMiniportBlock.DeviceObject;
    BOOLEAN WriteToDevice = (Flags & NDIS_SG_LIST_WRITE_TO_DEVICE) != 0;
    ULONG Offset = NET_BUFFER_DATA_OFFSET(NetBuffer);
    PMDL Mdl = NET_BUFFER_FIRST_MDL(NetBuffer);
    ULONG Length;
    NTSTATUS Status;

    /* Find the MDL the data starts in */
    while (Mdl != NULL && Offset >= MmGetMdlByteCount(Mdl))
    {
        Offset -= MmGetMdlByteCount(Mdl);
        Mdl = Mdl->Next;
    }

    if (Mdl == NULL)
        return NDIS_STATUS_FAILURE;

    NET_BUFFER_CURRENT_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = Offset;

    Length = NET_BUFFER_DATA_LENGTH(NetBuffer) + Offset;
    if (Length < NET_BUFFER_DATA_LENGTH(NetBuffer))
        return NDIS_STATUS_INVALID_LENGTH;

    CORE_NB_SG_BOUNCE_MDL(NetBuffer) = NULL;
    CORE_NB_SG_FLAGS(NetBuffer) = WriteToDevice ? CORE_SG_WRITE_TO_DEVICE : 0;

    if (ScatterGatherListBuffer != NULL)
    {
        Status = Operations->BuildScatterGatherList(SgDma->DmaAdapter,
                                                    DeviceObject,
                                                    Mdl,
                                                    MmGetMdlVirtualAddress(Mdl),
                                                    Length,
                                                    (PDRIVER_LIST_CONTROL)SgDma->ProcessSGListHandler,
                                                    Context,
                                                    WriteToDevice,
                                                    ScatterGatherListBuffer,
                                                    ScatterGatherListBufferSize);
        if (NT_SUCCESS(Status))
            return NDIS_STATUS_SUCCESS;
    }

    Status = Operations->GetScatterGatherList(SgDma->DmaAdapter,
                                              DeviceObject,
                                              Mdl,
                                              MmGetMdlVirtualAddress(Mdl),
                                              Length,
                                              (PDRIVER_LIST_CONTROL)SgDma->ProcessSGListHandler,
                                              Context,
                                              WriteToDevice);
    if (NT_SUCCESS(Status))
        return NDIS_STATUS_SUCCESS;

    Status = CoreBuildBouncedSGList(SgDma, NetBuffer, Context, Length, WriteToDevice);
    if (Status != NDIS_STATUS_SUCCESS)
        CORE_NB_SG_FLAGS(NetBuffer) = 0;

    return Status;
}

/**
 * @brief
 * Releases a list from NdisMAllocateNetBufferSGList, copying received data
 * into the NET_BUFFER when it went through NDIS's own buffer.
 *
 * @param[in] NdisMiniportDmaHandle
 * The handle from NdisMRegisterScatterGatherDma.
 *
 * @param[in] pSGL
 * The list.
 *
 * @param[in] NetBuffer
 * The NET_BUFFER it maps.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMFreeNetBufferSGList(
    NDIS_HANDLE NdisMiniportDmaHandle,
    PSCATTER_GATHER_LIST pSGL,
    PNET_BUFFER NetBuffer)
{
    PCORE_SG_DMA SgDma = NdisMiniportDmaHandle;
    ULONG_PTR SgFlags = CORE_NB_SG_FLAGS(NetBuffer);
    BOOLEAN WriteToDevice = (SgFlags & CORE_SG_WRITE_TO_DEVICE) != 0;
    PMDL BounceMdl;
    PUCHAR Bounce;
    ULONG Offset;

    SgDma->DmaAdapter->DmaOperations->PutScatterGatherList(SgDma->DmaAdapter, pSGL, WriteToDevice);

    if (SgFlags & CORE_SG_BOUNCED)
    {
        BounceMdl = CORE_NB_SG_BOUNCE_MDL(NetBuffer);
        Bounce = MmGetMdlVirtualAddress(BounceMdl);
        Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);

        if (!WriteToDevice)
        {
            CoreCopyMdlChain(NET_BUFFER_CURRENT_MDL(NetBuffer),
                             Offset,
                             Bounce + Offset,
                             NET_BUFFER_DATA_LENGTH(NetBuffer),
                             FALSE);
        }

        IoFreeMdl(BounceMdl);
        ExFreePoolWithTag(Bounce, NDIS_TAG);
    }

    CORE_NB_SG_BOUNCE_MDL(NetBuffer) = NULL;
    CORE_NB_SG_FLAGS(NetBuffer) = 0;
}
