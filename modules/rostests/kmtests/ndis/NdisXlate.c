/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for the NDIS 5 to 6 send translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kmt_test.h>

#define NDIS60 1
#include <ndis.h>

#include "xlate.h"

#define TEST_POOL_TAG   'tlxN'
#define TEST_MDL_SIZE   128
#define TEST_PACKETS    4

typedef struct _TEST_PACKET
{
    NDIS_PACKET Packet;
    PMDL Mdl;
    PVOID Data;
} TEST_PACKET;

static
BOOLEAN
NTAPI
BuildTestPacket(
    _Out_ TEST_PACKET *Test,
    _In_ ULONG MdlCount,
    _In_ ULONG Flags)
{
    PMDL Head = NULL;
    PMDL Tail = NULL;
    PMDL Mdl;
    PVOID Data;
    ULONG i;

    RtlZeroMemory(Test, sizeof(*Test));

    for (i = 0; i < MdlCount; i++)
    {
        Data = ExAllocatePoolWithTag(NonPagedPool, TEST_MDL_SIZE, TEST_POOL_TAG);
        if (Data == NULL)
            return FALSE;

        RtlFillMemory(Data, TEST_MDL_SIZE, (UCHAR)(0x20 + i));

        Mdl = IoAllocateMdl(Data, TEST_MDL_SIZE, FALSE, FALSE, NULL);
        if (Mdl == NULL)
        {
            ExFreePoolWithTag(Data, TEST_POOL_TAG);
            return FALSE;
        }

        MmBuildMdlForNonPagedPool(Mdl);
        Mdl->Next = NULL;

        if (Tail == NULL)
            Head = Mdl;
        else
            Tail->Next = Mdl;

        Tail = Mdl;
    }

    Test->Mdl = Head;
    Test->Packet.Private.Head = Head;
    Test->Packet.Private.Tail = Tail;
    Test->Packet.Private.Flags = Flags;
    /* Deliberately wrong, the translation is expected to correct it. */
    Test->Packet.Private.TotalLength = 0xDEAD;

    return TRUE;
}

static
VOID
NTAPI
FreeTestPacket(
    _Inout_ TEST_PACKET *Test)
{
    PMDL Mdl = Test->Mdl;
    PMDL Next;
    PVOID Data;

    while (Mdl != NULL)
    {
        Next = Mdl->Next;
        Data = MmGetMdlVirtualAddress(Mdl);
        IoFreeMdl(Mdl);
        if (Data != NULL)
            ExFreePoolWithTag(Data, TEST_POOL_TAG);
        Mdl = Next;
    }

    Test->Mdl = NULL;
}

static
NDIS_HANDLE
NTAPI
CreateXlatePool(VOID)
{
    NET_BUFFER_LIST_POOL_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.fAllocateNetBuffer = TRUE;
    Parameters.PoolTag = TEST_POOL_TAG;

    return NdisAllocateNetBufferListPool(NULL, &Parameters);
}

static
VOID
NTAPI
TestPacketToNetBuffer(
    _In_ NDIS_HANDLE Pool)
{
    TEST_PACKET Test;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    if (!ok(BuildTestPacket(&Test, 3, 0), "could not build a test packet\n"))
        return;

    NetBufferList = NdisAllocateNetBufferAndNetBufferList(Pool, 0, 0, NULL, 0, 0);
    if (!ok(NetBufferList != NULL, "NdisAllocateNetBufferAndNetBufferList failed\n"))
    {
        FreeTestPacket(&Test);
        return;
    }

    NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    NdisXlatePacketToNetBuffer(&Test.Packet, NetBuffer);

    ok_eq_pointer(NET_BUFFER_FIRST_MDL(NetBuffer), Test.Packet.Private.Head);
    ok_eq_pointer(NET_BUFFER_CURRENT_MDL(NetBuffer), Test.Packet.Private.Head);
    ok_eq_ulong(NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer), 0UL);
    ok_eq_ulong(NET_BUFFER_DATA_OFFSET(NetBuffer), 0UL);

    /* Three MDLs, and the stale TotalLength must have been corrected. */
    ok_eq_ulong(NET_BUFFER_DATA_LENGTH(NetBuffer), (ULONG)(3 * TEST_MDL_SIZE));
    ok_eq_uint(Test.Packet.Private.TotalLength, (UINT)(3 * TEST_MDL_SIZE));

    NET_BUFFER_FIRST_MDL(NetBuffer) = NULL;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = NULL;
    NdisFreeNetBufferList(NetBufferList);
    FreeTestPacket(&Test);
}

static
VOID
NTAPI
TestWholeArray(
    _In_ NDIS_HANDLE Pool)
{
    TEST_PACKET Tests[TEST_PACKETS];
    PNDIS_PACKET Packets[TEST_PACKETS];
    NDIS_SEND_XLATE Xlate;
    PNET_BUFFER_LIST NetBufferList;
    BOOLEAN More;
    ULONG Built = 0;
    ULONG i;

    for (i = 0; i < TEST_PACKETS; i++)
    {
        if (!BuildTestPacket(&Tests[i], 1, NDIS_FLAGS_DONT_LOOPBACK | 3))
            break;
        Packets[i] = &Tests[i].Packet;
        Built++;
    }

    if (!ok(Built == TEST_PACKETS, "could not build the packet array\n"))
        goto Cleanup;

    RtlZeroMemory(&Xlate, sizeof(Xlate));
    Xlate.NblPool = Pool;
    Xlate.Owner = (NDIS_HANDLE)(ULONG_PTR)0x4242;
    Xlate.Packets = Packets;
    Xlate.PacketCount = TEST_PACKETS;

    More = NdisXlatePacketArray(&Xlate);

    ok(!More, "the whole array should have translated in one pass\n");
    ok_eq_uint(Xlate.Translated, (UINT)TEST_PACKETS);
    ok_eq_uint(Xlate.NetBufferListCount, (UINT)TEST_PACKETS);
    ok(Xlate.DontLoopback, "NDIS_FLAGS_DONT_LOOPBACK was not carried\n");

    i = 0;
    for (NetBufferList = Xlate.NetBufferLists;
         NetBufferList != NULL;
         NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
    {
        ok_eq_pointer(NDIS_XLATE_PACKET(NetBufferList), Packets[i]);
        ok_eq_pointer(NetBufferList->SourceHandle, Xlate.Owner);
        ok_eq_ulong((ULONG)(ULONG_PTR)NET_BUFFER_LIST_INFO(NetBufferList,
                                                           NetBufferListFrameType), 3UL);
        ok_eq_ulong(NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(NetBufferList)),
                    (ULONG)TEST_MDL_SIZE);
        i++;
    }

    ok_eq_ulong(i, (ULONG)TEST_PACKETS);

    NdisXlateFreeNetBufferLists(Xlate.NetBufferLists);

    /* The MDLs belong to the packets and must have survived. */
    for (i = 0; i < TEST_PACKETS; i++)
        ok(Tests[i].Packet.Private.Head != NULL, "packet %lu lost its MDL chain\n", i);

Cleanup:
    for (i = 0; i < Built; i++)
        FreeTestPacket(&Tests[i]);
}

/*
 * A run stops when NDIS_FLAGS_DONT_LOOPBACK changes, because the flag applies
 * to a send as a whole. The cursor has to be left on the packet that differs.
 */
static
VOID
NTAPI
TestBatchSplit(
    _In_ NDIS_HANDLE Pool)
{
    TEST_PACKET Tests[TEST_PACKETS];
    PNDIS_PACKET Packets[TEST_PACKETS];
    NDIS_SEND_XLATE Xlate;
    BOOLEAN More;
    ULONG Built = 0;
    ULONG i;

    for (i = 0; i < TEST_PACKETS; i++)
    {
        ULONG Flags = (i < 2) ? NDIS_FLAGS_DONT_LOOPBACK : 0;

        if (!BuildTestPacket(&Tests[i], 1, Flags))
            break;
        Packets[i] = &Tests[i].Packet;
        Built++;
    }

    if (!ok(Built == TEST_PACKETS, "could not build the packet array\n"))
        goto Cleanup;

    RtlZeroMemory(&Xlate, sizeof(Xlate));
    Xlate.NblPool = Pool;
    Xlate.Packets = Packets;
    Xlate.PacketCount = TEST_PACKETS;

    /* First run: the two that do not loop back. */
    More = NdisXlatePacketArray(&Xlate);
    ok(More, "the run should have stopped at the flag change\n");
    ok_eq_uint(Xlate.Translated, 2U);
    ok_eq_uint(Xlate.NetBufferListCount, 2U);
    ok(Xlate.DontLoopback, "first run should be the DONT_LOOPBACK one\n");
    NdisXlateFreeNetBufferLists(Xlate.NetBufferLists);

    /* Second run resumes on the packet that ended the first. */
    More = NdisXlatePacketArray(&Xlate);
    ok(!More, "the second run should have finished the array\n");
    ok_eq_uint(Xlate.Translated, (UINT)TEST_PACKETS);
    ok_eq_uint(Xlate.NetBufferListCount, 2U);
    ok(!Xlate.DontLoopback, "second run should not be the DONT_LOOPBACK one\n");
    NdisXlateFreeNetBufferLists(Xlate.NetBufferLists);

Cleanup:
    for (i = 0; i < Built; i++)
        FreeTestPacket(&Tests[i]);
}

VOID
NTAPI
TestXlate(VOID)
{
    NDIS_HANDLE Pool;

    Pool = CreateXlatePool();
    if (!ok(Pool != NULL, "NdisAllocateNetBufferListPool failed\n"))
        return;

    TestPacketToNetBuffer(Pool);
    TestWholeArray(Pool);
    TestBatchSplit(Pool);

    NdisFreeNetBufferListPool(Pool);
}
