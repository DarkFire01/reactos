/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI data blocks of adapters a WDF class extension registers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include <ndiswdf.h>
#include <wmistr.h>
#include <ndisguid.h>

/* The class extension answers WMI for its device, and asks NDIS for the data blocks */

/* NDIS itself answers this block, no OID behind it */
#define CORE_WMI_NDIS_DATA          0x80000000

typedef struct _CORE_WMI_BLOCK
{
    const GUID *Guid;
    ULONG Id;
    ULONG Size;
    ULONG Flags;
} CORE_WMI_BLOCK, *PCORE_WMI_BLOCK;

#define CORE_WMI_READ(_Guid, _Oid, _Size, _Flags) \
    { &(_Guid), (_Oid), (_Size), fNDIS_GUID_TO_OID | fNDIS_GUID_ALLOW_READ | (_Flags) }
#define CORE_WMI_STATUS(_Guid, _Status, _Size, _Flags) \
    { &(_Guid), (_Status), (_Size), fNDIS_GUID_TO_STATUS | (_Flags) }

/* Every adapter has these */
static const CORE_WMI_BLOCK CoreWmiNdisBlocks[] =
{
    { &GUID_NDIS_ENUMERATE_ADAPTER, 0, (ULONG)-1,
      CORE_WMI_NDIS_DATA | fNDIS_GUID_UNICODE_STRING | fNDIS_GUID_ALLOW_READ },
    { &GUID_NDIS_NOTIFY_ADAPTER_REMOVAL, 0, (ULONG)-1, CORE_WMI_NDIS_DATA | fNDIS_GUID_TO_STATUS },
    { &GUID_NDIS_NOTIFY_ADAPTER_ARRIVAL, 0, (ULONG)-1, CORE_WMI_NDIS_DATA | fNDIS_GUID_TO_STATUS },
};

/* Offered when the miniport supports the OID */
static const CORE_WMI_BLOCK CoreWmiOidBlocks[] =
{
    CORE_WMI_READ(GUID_NDIS_GEN_HARDWARE_STATUS, OID_GEN_HARDWARE_STATUS, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_SUPPORTED, sizeof(ULONG), fNDIS_GUID_ARRAY),
    CORE_WMI_READ(GUID_NDIS_GEN_MEDIA_IN_USE, OID_GEN_MEDIA_IN_USE, sizeof(ULONG), fNDIS_GUID_ARRAY),
    CORE_WMI_READ(GUID_NDIS_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_LOOKAHEAD, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_MAXIMUM_FRAME_SIZE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_LINK_SPEED, OID_GEN_LINK_SPEED, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_TRANSMIT_BUFFER_SPACE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_TRANSMIT_BLOCK_SIZE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_VENDOR_ID, OID_GEN_VENDOR_ID, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DESCRIPTION, (ULONG)-1, fNDIS_GUID_ANSI_STRING),
    CORE_WMI_READ(GUID_NDIS_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_PACKET_FILTER, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_CURRENT_LOOKAHEAD, OID_GEN_CURRENT_LOOKAHEAD, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_DRIVER_VERSION, OID_GEN_DRIVER_VERSION, sizeof(USHORT), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAXIMUM_TOTAL_SIZE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MAC_OPTIONS, OID_GEN_MAC_OPTIONS, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MEDIA_CONNECT_STATUS, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_MAXIMUM_SEND_PACKETS, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_VENDOR_DRIVER_VERSION, OID_GEN_VENDOR_DRIVER_VERSION, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_VLAN_ID, OID_GEN_VLAN_ID, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_PHYSICAL_MEDIUM, OID_GEN_PHYSICAL_MEDIUM, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_XMIT_OK, OID_GEN_XMIT_OK, sizeof(ULONG64), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_RCV_OK, OID_GEN_RCV_OK, sizeof(ULONG64), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_XMIT_ERROR, OID_GEN_XMIT_ERROR, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_RCV_ERROR, OID_GEN_RCV_ERROR, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_GEN_RCV_NO_BUFFER, OID_GEN_RCV_NO_BUFFER, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_802_3_PERMANENT_ADDRESS, OID_802_3_PERMANENT_ADDRESS, 6, 0),
    CORE_WMI_READ(GUID_NDIS_802_3_CURRENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, 6, 0),
    CORE_WMI_READ(GUID_NDIS_802_3_MULTICAST_LIST, OID_802_3_MULTICAST_LIST, 6, fNDIS_GUID_ARRAY),
    CORE_WMI_READ(GUID_NDIS_802_3_MAXIMUM_LIST_SIZE, OID_802_3_MAXIMUM_LIST_SIZE, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_802_3_MAC_OPTIONS, OID_802_3_MAC_OPTIONS, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_RCV_ERROR_ALIGNMENT, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_ONE_COLLISION, sizeof(ULONG), 0),
    CORE_WMI_READ(GUID_NDIS_802_3_XMIT_MORE_COLLISIONS, OID_802_3_XMIT_MORE_COLLISIONS, sizeof(ULONG), 0),
};

/* Every adapter can raise these */
static const CORE_WMI_BLOCK CoreWmiStatusBlocks[] =
{
    CORE_WMI_STATUS(GUID_NDIS_STATUS_RESET_START, NDIS_STATUS_RESET_START, 0, 0),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_RESET_END, NDIS_STATUS_RESET_END, 0, 0),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_MEDIA_CONNECT, NDIS_STATUS_MEDIA_CONNECT, 0, 0),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_MEDIA_DISCONNECT, NDIS_STATUS_MEDIA_DISCONNECT, 0, 0),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_MEDIA_SPECIFIC_INDICATION, NDIS_STATUS_MEDIA_SPECIFIC_INDICATION, 1,
                    fNDIS_GUID_ARRAY),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_LINK_SPEED_CHANGE, NDIS_STATUS_LINK_SPEED_CHANGE, sizeof(NDIS_LINK_SPEED), 0),
    CORE_WMI_STATUS(GUID_NDIS_STATUS_LINK_STATE, NDIS_STATUS_LINK_STATE, sizeof(NDIS_LINK_STATE),
                    fNDIS_GUID_SUPPORT_COMMON_HEADER),
};

static
PCORE_WMI_BLOCK
CoreWmiFindBlock(
    _In_ LPCGUID Guid)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiNdisBlocks); i++)
    {
        if (IsEqualGUID(Guid, CoreWmiNdisBlocks[i].Guid))
            return (PCORE_WMI_BLOCK)&CoreWmiNdisBlocks[i];
    }

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiOidBlocks); i++)
    {
        if (IsEqualGUID(Guid, CoreWmiOidBlocks[i].Guid))
            return (PCORE_WMI_BLOCK)&CoreWmiOidBlocks[i];
    }

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiStatusBlocks); i++)
    {
        if (IsEqualGUID(Guid, CoreWmiStatusBlocks[i].Guid))
            return (PCORE_WMI_BLOCK)&CoreWmiStatusBlocks[i];
    }

    return NULL;
}

static
VOID
CoreWmiEmit(
    _Out_writes_opt_(Capacity) PNDIS_GUID Map,
    _In_ USHORT Capacity,
    _Inout_ PUSHORT Count,
    _In_ const CORE_WMI_BLOCK *Block)
{
    if (Map != NULL && *Count < Capacity)
    {
        Map[*Count].Guid = *Block->Guid;
        Map[*Count].Oid = Block->Id;
        Map[*Count].Size = Block->Size;
        Map[*Count].Flags = Block->Flags & ~CORE_WMI_NDIS_DATA;
    }

    (*Count)++;
}

/**
 * @brief
 * Lists the WMI data blocks an adapter with a set of OIDs has: the ones NDIS
 * answers, the ones its OIDs back and the status indications.
 *
 * @param[in] OidList
 * The OIDs the miniport supports.
 *
 * @param[in] OidCount
 * How many there are.
 *
 * @param[out] GuidToOidMap
 * Receives the blocks, or NULL to count them.
 *
 * @param[in,out] GuidToOidCount
 * In, the room in GuidToOidMap. Out, how many blocks there are.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfGetGuidToOidMap(
    PNDIS_OID OidList,
    USHORT OidCount,
    PNDIS_GUID GuidToOidMap,
    PUSHORT GuidToOidCount)
{
    USHORT Capacity = (GuidToOidMap != NULL) ? *GuidToOidCount : 0;
    USHORT Count = 0;
    ULONG i, j;

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiNdisBlocks); i++)
        CoreWmiEmit(GuidToOidMap, Capacity, &Count, &CoreWmiNdisBlocks[i]);

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiOidBlocks); i++)
    {
        for (j = 0; j < OidCount; j++)
        {
            if (OidList[j] == CoreWmiOidBlocks[i].Id)
            {
                CoreWmiEmit(GuidToOidMap, Capacity, &Count, &CoreWmiOidBlocks[i]);
                break;
            }
        }
    }

    for (i = 0; i < RTL_NUMBER_OF(CoreWmiStatusBlocks); i++)
        CoreWmiEmit(GuidToOidMap, Capacity, &Count, &CoreWmiStatusBlocks[i]);

    *GuidToOidCount = Count;
}

/* The one instance name an adapter answers to */
static
PCUNICODE_STRING
CoreWmiInstanceName(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    if (Adapter->Wdf.InstanceName.Buffer != NULL)
        return &Adapter->Wdf.InstanceName;

    return &Adapter->NdisMiniportBlock.MiniportName;
}

/*
 * Reads a block into Buffer, or with a NULL Buffer only finds its size in the
 * WMI layout: arrays lead with a count, strings with a USHORT byte length.
 */
static
NTSTATUS
CoreWmiReadBlock(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_WMI_BLOCK Block,
    _Out_writes_bytes_opt_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Needed)
{
    PCUNICODE_STRING Name;
    PUCHAR Raw = NULL;
    ULONG RawLength;
    ULONG Written;
    ULONG BytesNeeded;
    NDIS_STATUS NdisStatus;
    NTSTATUS Status = STATUS_SUCCESS;

    *Needed = 0;

    if (Block->Flags & CORE_WMI_NDIS_DATA)
    {
        if (!(Block->Flags & fNDIS_GUID_ALLOW_READ))
            return STATUS_WMI_GUID_NOT_FOUND;

        /* The only data block NDIS answers itself is the adapter's device name */
        Name = &Adapter->NdisMiniportBlock.MiniportName;
        *Needed = sizeof(USHORT) + Name->Length;
        if (Buffer != NULL)
        {
            if (Length < *Needed)
                return STATUS_BUFFER_TOO_SMALL;

            *(PUSHORT)Buffer = Name->Length;
            RtlCopyMemory(Buffer + sizeof(USHORT), Name->Buffer, Name->Length);
        }

        return STATUS_SUCCESS;
    }

    if (!(Block->Flags & fNDIS_GUID_TO_OID))
        return STATUS_WMI_GUID_NOT_FOUND;

    /* Fixed size blocks are known, the rest the miniport has to say */
    RawLength = Block->Size;
    if (RawLength == (ULONG)-1 || (Block->Flags & fNDIS_GUID_ARRAY))
    {
        NdisStatus = CoreQueryInformationEx(Adapter, Block->Id, NULL, 0, &Written, &BytesNeeded);
        if (NdisStatus == NDIS_STATUS_SUCCESS)
            RawLength = Written;
        else if (NdisStatus == NDIS_STATUS_INVALID_LENGTH || NdisStatus == NDIS_STATUS_BUFFER_TOO_SHORT)
            RawLength = BytesNeeded;
        else
            return NdisConvertNdisStatusToNtStatus(NdisStatus);
    }

    if (Block->Flags & fNDIS_GUID_ARRAY)
        *Needed = sizeof(ULONG) + RawLength;
    else if (Block->Flags & fNDIS_GUID_ANSI_STRING)
        *Needed = sizeof(USHORT) + RawLength * sizeof(WCHAR);
    else if (Block->Flags & fNDIS_GUID_UNICODE_STRING)
        *Needed = sizeof(USHORT) + RawLength;
    else
        *Needed = RawLength;

    if (Buffer == NULL)
        return STATUS_SUCCESS;

    if (Length < *Needed)
        return STATUS_BUFFER_TOO_SMALL;

    if (RawLength != 0)
    {
        Raw = ExAllocatePoolWithTag(NonPagedPool, RawLength, NDIS_TAG);
        if (Raw == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    Written = 0;
    NdisStatus = CoreQueryInformation(Adapter, Block->Id, Raw, RawLength, &Written);
    if (NdisStatus != NDIS_STATUS_SUCCESS)
    {
        Status = NdisConvertNdisStatusToNtStatus(NdisStatus);
        goto Exit;
    }

    Written = min(Written, RawLength);

    if (Block->Flags & fNDIS_GUID_ARRAY)
    {
        *(PULONG)Buffer = (Block->Size != 0 && Block->Size != (ULONG)-1) ? Written / Block->Size : Written;
        RtlCopyMemory(Buffer + sizeof(ULONG), Raw, Written);
        *Needed = sizeof(ULONG) + Written;
    }
    else if (Block->Flags & fNDIS_GUID_ANSI_STRING)
    {
        ANSI_STRING Ansi;
        UNICODE_STRING Unicode;

        /* The terminator is not part of the string */
        Ansi.Buffer = (PCHAR)Raw;
        Ansi.Length = Ansi.MaximumLength = (USHORT)Written;
        while (Ansi.Length != 0 && Ansi.Buffer[Ansi.Length - 1] == ANSI_NULL)
            Ansi.Length--;

        Unicode.Buffer = (PWCH)(Buffer + sizeof(USHORT));
        Unicode.Length = 0;
        Unicode.MaximumLength = (USHORT)(Length - sizeof(USHORT));
        Status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, FALSE);
        if (NT_SUCCESS(Status))
        {
            *(PUSHORT)Buffer = Unicode.Length;
            *Needed = sizeof(USHORT) + Unicode.Length;
        }
    }
    else if (Block->Flags & fNDIS_GUID_UNICODE_STRING)
    {
        *(PUSHORT)Buffer = (USHORT)Written;
        RtlCopyMemory(Buffer + sizeof(USHORT), Raw, Written);
        *Needed = sizeof(USHORT) + Written;
    }
    else
    {
        RtlCopyMemory(Buffer, Raw, Written);
        *Needed = Written;
    }

Exit:
    if (Raw != NULL)
        ExFreePoolWithTag(Raw, NDIS_TAG);

    return Status;
}

/**
 * @brief
 * Reads a data block of every instance of an adapter, which is the adapter.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] NdisGuid
 * The block, from NdisWdfGetGuidToOidMap.
 *
 * @param[in] Guid
 * The block's GUID.
 *
 * @param[out] DataBuffer
 * A WNODE_ALL_DATA.
 *
 * @param[in] BufferSize
 * Its size.
 *
 * @param[out] ReturnSize
 * How much of it was used.
 *
 * @return
 * STATUS_SUCCESS, also when only a WNODE_TOO_SMALL fit, or why the block
 * could not be read.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfQueryAllData(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_GUID NdisGuid,
    LPCGUID Guid,
    PVOID DataBuffer,
    ULONG BufferSize,
    PULONG ReturnSize)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PWNODE_ALL_DATA Wnode = DataBuffer;
    PCUNICODE_STRING Name = CoreWmiInstanceName(Adapter);
    PCORE_WMI_BLOCK Block;
    ULONG DataSize;
    ULONG DataRoom;
    ULONG NameOffset;
    ULONG Total;
    PUCHAR Base = DataBuffer;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(NdisGuid);

    *ReturnSize = 0;

    if (BufferSize < sizeof(WNODE_TOO_SMALL))
    {
        *ReturnSize = sizeof(ULONG);
        return STATUS_BUFFER_TOO_SMALL;
    }

    Block = CoreWmiFindBlock(Guid);
    if (Block == NULL)
        return STATUS_WMI_GUID_NOT_FOUND;

    Status = CoreWmiReadBlock(Adapter, Block, NULL, 0, &DataSize);
    if (!NT_SUCCESS(Status))
        return Status;

    /* One instance: data, then the name offset table, then the name */
    DataRoom = ALIGN_UP_BY(DataSize, sizeof(ULONG));
    NameOffset = sizeof(WNODE_ALL_DATA) + DataRoom;
    Total = NameOffset + sizeof(ULONG) + sizeof(USHORT) + Name->Length;

    if (BufferSize < Total)
    {
        PWNODE_TOO_SMALL TooSmall = DataBuffer;

        TooSmall->WnodeHeader.Flags |= WNODE_FLAG_TOO_SMALL;
        TooSmall->WnodeHeader.BufferSize = sizeof(WNODE_TOO_SMALL);
        TooSmall->SizeNeeded = Total;
        *ReturnSize = sizeof(WNODE_TOO_SMALL);
        return STATUS_SUCCESS;
    }

    Status = CoreWmiReadBlock(Adapter, Block, Base + sizeof(WNODE_ALL_DATA), DataRoom, &DataSize);
    if (!NT_SUCCESS(Status))
        return Status;

    KeQuerySystemTime(&Wnode->WnodeHeader.TimeStamp);
    Wnode->WnodeHeader.Flags |= WNODE_FLAG_FIXED_INSTANCE_SIZE;
    Wnode->WnodeHeader.BufferSize = Total;
    Wnode->DataBlockOffset = sizeof(WNODE_ALL_DATA);
    Wnode->InstanceCount = 1;
    Wnode->OffsetInstanceNameOffsets = NameOffset;
    Wnode->FixedInstanceSize = DataSize;

    *(PULONG)(Base + NameOffset) = NameOffset + sizeof(ULONG);
    *(PUSHORT)(Base + NameOffset + sizeof(ULONG)) = Name->Length;
    RtlCopyMemory(Base + NameOffset + sizeof(ULONG) + sizeof(USHORT), Name->Buffer, Name->Length);

    *ReturnSize = Total;
    return STATUS_SUCCESS;
}

/* A dynamic instance name has to be the adapter's */
static
BOOLEAN
CoreWmiIsOurInstance(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PWNODE_SINGLE_INSTANCE Wnode)
{
    UNICODE_STRING Requested;
    PUSHORT Counted;

    if (Wnode->WnodeHeader.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES)
        return TRUE;

    Counted = (PUSHORT)((PUCHAR)Wnode + Wnode->OffsetInstanceName);
    Requested.Length = Requested.MaximumLength = *Counted;
    Requested.Buffer = (PWCH)(Counted + 1);

    return RtlEqualUnicodeString(CoreWmiInstanceName(Adapter), &Requested, TRUE);
}

/**
 * @brief
 * Reads a data block of the adapter's one instance.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] NdisGuid
 * The block, from NdisWdfGetGuidToOidMap.
 *
 * @param[in,out] Wnode
 * A WNODE_SINGLE_INSTANCE naming the instance.
 *
 * @param[in] BufferSize
 * The size of Wnode.
 *
 * @param[out] ReturnSize
 * How much of it was used.
 *
 * @return
 * STATUS_SUCCESS, also when only a WNODE_TOO_SMALL fit, or why the block
 * could not be read.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfQuerySingleInstance(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_GUID NdisGuid,
    PVOID Wnode,
    ULONG BufferSize,
    PULONG ReturnSize)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PWNODE_SINGLE_INSTANCE Instance = Wnode;
    PCORE_WMI_BLOCK Block;
    ULONG DataSize;
    ULONG Total;
    NTSTATUS Status;

    *ReturnSize = 0;

    if (!CoreWmiIsOurInstance(Adapter, Instance))
        return STATUS_WMI_INSTANCE_NOT_FOUND;

    Block = CoreWmiFindBlock((NdisGuid != NULL) ? &NdisGuid->Guid : &Instance->WnodeHeader.Guid);
    if (Block == NULL)
        return STATUS_WMI_GUID_NOT_FOUND;

    Status = CoreWmiReadBlock(Adapter, Block, NULL, 0, &DataSize);
    if (!NT_SUCCESS(Status))
        return Status;

    Total = Instance->DataBlockOffset + DataSize;
    if (Total < DataSize)
        return STATUS_UNSUCCESSFUL;

    if (BufferSize < Total)
    {
        PWNODE_TOO_SMALL TooSmall = Wnode;

        if (BufferSize < sizeof(WNODE_TOO_SMALL))
        {
            *ReturnSize = sizeof(ULONG);
            return STATUS_BUFFER_TOO_SMALL;
        }

        TooSmall->WnodeHeader.Flags |= WNODE_FLAG_TOO_SMALL;
        TooSmall->WnodeHeader.BufferSize = sizeof(WNODE_TOO_SMALL);
        TooSmall->SizeNeeded = Total;
        *ReturnSize = sizeof(WNODE_TOO_SMALL);
        return STATUS_SUCCESS;
    }

    Status = CoreWmiReadBlock(Adapter,
                              Block,
                              (PUCHAR)Wnode + Instance->DataBlockOffset,
                              BufferSize - Instance->DataBlockOffset,
                              &DataSize);
    if (!NT_SUCCESS(Status))
        return Status;

    KeQuerySystemTime(&Instance->WnodeHeader.TimeStamp);
    Instance->SizeDataBlock = DataSize;
    Instance->WnodeHeader.BufferSize = Instance->DataBlockOffset + DataSize;

    *ReturnSize = Instance->WnodeHeader.BufferSize;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Writes a data block of the adapter's one instance to its OID.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] NdisGuid
 * The block, from NdisWdfGetGuidToOidMap.
 *
 * @param[in] Wnode
 * A WNODE_SINGLE_INSTANCE with the new data.
 *
 * @return
 * STATUS_SUCCESS, or why the block could not be written.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfChangeSingleInstance(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_GUID NdisGuid,
    PVOID Wnode)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PWNODE_SINGLE_INSTANCE Instance = Wnode;
    PCORE_WMI_BLOCK Block;
    PUCHAR Data;
    ULONG Length;
    ULONG BytesRead;
    NDIS_STATUS NdisStatus;

    if (!CoreWmiIsOurInstance(Adapter, Instance))
        return STATUS_WMI_INSTANCE_NOT_FOUND;

    Block = CoreWmiFindBlock((NdisGuid != NULL) ? &NdisGuid->Guid : &Instance->WnodeHeader.Guid);
    if (Block == NULL)
        return STATUS_WMI_GUID_NOT_FOUND;

    if (!(Block->Flags & fNDIS_GUID_TO_OID) || (Block->Flags & (fNDIS_GUID_ANSI_STRING | fNDIS_GUID_UNICODE_STRING)))
        return STATUS_INVALID_DEVICE_REQUEST;

    Data = (PUCHAR)Wnode + Instance->DataBlockOffset;
    Length = Instance->SizeDataBlock;

    /* An array's count is WMI's, the OID takes the elements */
    if (Block->Flags & fNDIS_GUID_ARRAY)
    {
        if (Length < sizeof(ULONG))
            return STATUS_INVALID_PARAMETER;

        Data += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }

    NdisStatus = CoreSetInformation(Adapter, Block->Id, Data, Length, &BytesRead);
    return NdisConvertNdisStatusToNtStatus(NdisStatus);
}

/**
 * @brief
 * Runs a WMI method of an adapter. None of the blocks here has methods.
 *
 * @return
 * STATUS_INVALID_DEVICE_REQUEST.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfExecuteMethod(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_GUID NdisGuid,
    PVOID Wnode,
    ULONG BufferSize,
    PULONG ReturnSize)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);
    UNREFERENCED_PARAMETER(NdisGuid);
    UNREFERENCED_PARAMETER(Wnode);
    UNREFERENCED_PARAMETER(BufferSize);

    *ReturnSize = 0;
    return STATUS_INVALID_DEVICE_REQUEST;
}

/**
 * @brief
 * Raises the WMI event that an adapter arrived, carrying its device name.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfNotifyWmiAdapterArrival(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PCUNICODE_STRING Name = CoreWmiInstanceName(Adapter);
    PCUNICODE_STRING DeviceName = &Adapter->NdisMiniportBlock.MiniportName;
    PWNODE_SINGLE_INSTANCE Event;
    ULONG DataOffset;
    ULONG DataSize;
    ULONG Total;
    NTSTATUS Status;

    ASSERT(MINIPORT_IS_WDF(Adapter));

    DataOffset = sizeof(WNODE_SINGLE_INSTANCE) + ALIGN_UP_BY(sizeof(USHORT) + Name->Length, 8);
    DataSize = sizeof(USHORT) + DeviceName->Length;
    Total = DataOffset + DataSize;

    Event = ExAllocatePoolWithTag(NonPagedPool, Total, NDIS_TAG);
    if (Event == NULL)
        return;

    RtlZeroMemory(Event, Total);
    Event->WnodeHeader.BufferSize = Total;
    Event->WnodeHeader.ProviderId = IoWMIDeviceObjectToProviderId(Adapter->NdisMiniportBlock.DeviceObject);
    Event->WnodeHeader.Version = 1;
    KeQuerySystemTime(&Event->WnodeHeader.TimeStamp);
    Event->WnodeHeader.Guid = GUID_NDIS_NOTIFY_ADAPTER_ARRIVAL;
    Event->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE | WNODE_FLAG_EVENT_ITEM;
    Event->OffsetInstanceName = sizeof(WNODE_SINGLE_INSTANCE);
    Event->DataBlockOffset = DataOffset;
    Event->SizeDataBlock = DataSize;

    *(PUSHORT)((PUCHAR)Event + sizeof(WNODE_SINGLE_INSTANCE)) = Name->Length;
    RtlCopyMemory((PUCHAR)Event + sizeof(WNODE_SINGLE_INSTANCE) + sizeof(USHORT), Name->Buffer, Name->Length);

    *(PUSHORT)((PUCHAR)Event + DataOffset) = DeviceName->Length;
    RtlCopyMemory((PUCHAR)Event + DataOffset + sizeof(USHORT), DeviceName->Buffer, DeviceName->Length);

    /* WMI owns the event once it takes it */
    Status = IoWMIWriteEvent(Event);
    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("IoWMIWriteEvent failed (0x%lx).\n", Status));
        ExFreePoolWithTag(Event, NDIS_TAG);
    }
}
