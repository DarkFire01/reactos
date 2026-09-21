/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for the ndis.sys NET_BUFFER_LIST clone
 *              and info copy routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kmt_test.h>

#define NDIS60 1
#include <ndis.h>

#define TEST_POOL_TAG   'tlbN'
#define TEST_DATA_SIZE  256

/* A value no real caller would produce, so a stale slot is obvious. */
#define SENTINEL(n)     ((PVOID)(ULONG_PTR)(0xA5A50000 + (n)))

static
NDIS_HANDLE
NTAPI
CreateNblPool(
    _In_ BOOLEAN AllocateNetBuffer,
    _In_ ULONG DataSize,
    _In_ UCHAR ProtocolId)
{
    NET_BUFFER_LIST_POOL_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.ProtocolId = ProtocolId;
    Parameters.fAllocateNetBuffer = AllocateNetBuffer;
    Parameters.PoolTag = TEST_POOL_TAG;
    Parameters.DataSize = DataSize;

    return NdisAllocateNetBufferListPool(NULL, &Parameters);
}

static
NDIS_HANDLE
NTAPI
CreateNbPool(VOID)
{
    NET_BUFFER_POOL_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NET_BUFFER_POOL_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_POOL_PARAMETERS_REVISION_1;
    Parameters.PoolTag = TEST_POOL_TAG;

    return NdisAllocateNetBufferPool(NULL, &Parameters);
}

/*
 * Fill the data area behind a NET_BUFFER with a position dependent pattern, so
 * a clone that maps the wrong pages shows up as a mismatch rather than as
 * plausible looking zeroes.
 */
static
VOID
NTAPI
FillPattern(
    _In_ PNET_BUFFER NetBuffer,
    _In_ UCHAR Seed)
{
    PUCHAR Data;
    ULONG i;

    Data = MmGetSystemAddressForMdlSafe(NET_BUFFER_FIRST_MDL(NetBuffer),
                                        LowPagePriority);
    if (Data == NULL)
        return;

    for (i = 0; i < TEST_DATA_SIZE; i++)
        Data[i] = (UCHAR)(Seed + i);
}

static
BOOLEAN
NTAPI
CheckPattern(
    _In_ PNET_BUFFER NetBuffer,
    _In_ UCHAR Seed)
{
    PUCHAR Data;
    ULONG i;

    Data = MmGetSystemAddressForMdlSafe(NET_BUFFER_FIRST_MDL(NetBuffer),
                                        LowPagePriority);
    if (Data == NULL)
        return FALSE;

    for (i = 0; i < TEST_DATA_SIZE; i++)
    {
        if (Data[i] != (UCHAR)(Seed + i))
            return FALSE;
    }

    return TRUE;
}

static
VOID
NTAPI
TestCloneWithOriginalMdls(
    _In_ NDIS_HANDLE NblPool,
    _In_ NDIS_HANDLE NbPool)
{
    PNET_BUFFER_LIST Original;
    PNET_BUFFER_LIST Clone;
    PNET_BUFFER OriginalNb;
    PNET_BUFFER CloneNb;

    Original = NdisAllocateNetBufferList(NblPool, 0, 0);
    if (!ok(Original != NULL, "NdisAllocateNetBufferList failed\n"))
        return;

    OriginalNb = NET_BUFFER_LIST_FIRST_NB(Original);
    ok(OriginalNb != NULL, "pool with fAllocateNetBuffer gave no NET_BUFFER\n");
    if (OriginalNb == NULL)
    {
        NdisFreeNetBufferList(Original);
        return;
    }

    FillPattern(OriginalNb, 0x10);
    Original->SourceHandle = (NDIS_HANDLE)(ULONG_PTR)0x5150;
    NET_BUFFER_LIST_INFO(Original, NblOriginalInterfaceIfIndex) = SENTINEL(16);

    Clone = NdisAllocateCloneNetBufferList(Original,
                                           NblPool,
                                           NbPool,
                                           NDIS_CLONE_FLAGS_USE_ORIGINAL_MDLS);
    if (!ok(Clone != NULL, "NdisAllocateCloneNetBufferList failed\n"))
    {
        NdisFreeNetBufferList(Original);
        return;
    }

    CloneNb = NET_BUFFER_LIST_FIRST_NB(Clone);
    ok(CloneNb != NULL, "clone has no NET_BUFFER\n");

    if (CloneNb != NULL)
    {
        /* The whole point of the flag: the descriptors are shared, not copied. */
        ok_eq_pointer(NET_BUFFER_FIRST_MDL(CloneNb), NET_BUFFER_FIRST_MDL(OriginalNb));
        ok_eq_pointer(NET_BUFFER_CURRENT_MDL(CloneNb), NET_BUFFER_CURRENT_MDL(OriginalNb));
        ok_eq_ulong(NET_BUFFER_DATA_OFFSET(CloneNb), NET_BUFFER_DATA_OFFSET(OriginalNb));
        ok_eq_ulong(NET_BUFFER_DATA_LENGTH(CloneNb), NET_BUFFER_DATA_LENGTH(OriginalNb));
        ok(CheckPattern(CloneNb, 0x10), "clone data does not match the original\n");
    }

    ok_eq_pointer(Clone->SourceHandle, Original->SourceHandle);
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Clone, NblOriginalInterfaceIfIndex), SENTINEL(16));

    NdisFreeCloneNetBufferList(Clone, NDIS_CLONE_FLAGS_USE_ORIGINAL_MDLS);

    /* Freeing the clone must not have touched the original's MDL or data. */
    ok(NET_BUFFER_FIRST_MDL(OriginalNb) != NULL, "original lost its MDL chain\n");
    ok(CheckPattern(OriginalNb, 0x10), "original data damaged by freeing the clone\n");

    NdisFreeNetBufferList(Original);
}

static
VOID
NTAPI
TestCloneWithOwnMdls(
    _In_ NDIS_HANDLE NblPool,
    _In_ NDIS_HANDLE NbPool)
{
    PNET_BUFFER_LIST Original;
    PNET_BUFFER_LIST Clone;
    PNET_BUFFER OriginalNb;
    PNET_BUFFER CloneNb;

    Original = NdisAllocateNetBufferList(NblPool, 0, 0);
    if (!ok(Original != NULL, "NdisAllocateNetBufferList failed\n"))
        return;

    OriginalNb = NET_BUFFER_LIST_FIRST_NB(Original);
    if (OriginalNb == NULL)
    {
        NdisFreeNetBufferList(Original);
        return;
    }

    FillPattern(OriginalNb, 0x40);

    Clone = NdisAllocateCloneNetBufferList(Original, NblPool, NbPool, 0);
    if (!ok(Clone != NULL, "NdisAllocateCloneNetBufferList failed\n"))
    {
        NdisFreeNetBufferList(Original);
        return;
    }

    CloneNb = NET_BUFFER_LIST_FIRST_NB(Clone);
    ok(CloneNb != NULL, "clone has no NET_BUFFER\n");

    if (CloneNb != NULL)
    {
        /* Own descriptors over the same pages. */
        ok(NET_BUFFER_FIRST_MDL(CloneNb) != NET_BUFFER_FIRST_MDL(OriginalNb),
           "clone shares the original's MDL without the flag\n");
        ok(NET_BUFFER_FIRST_MDL(CloneNb) != NULL, "clone has no MDL chain\n");

        /* Partial MDLs start at the data, so the offset resets. */
        ok_eq_ulong(NET_BUFFER_DATA_OFFSET(CloneNb), 0UL);
        ok_eq_ulong(NET_BUFFER_DATA_LENGTH(CloneNb), NET_BUFFER_DATA_LENGTH(OriginalNb));
        ok(CheckPattern(CloneNb, 0x40), "clone data does not match the original\n");
    }

    NdisFreeCloneNetBufferList(Clone, 0);

    ok(CheckPattern(OriginalNb, 0x40), "original data damaged by freeing the clone\n");

    NdisFreeNetBufferList(Original);
}

static
VOID
NTAPI
TestCloneRejectsNullPools(
    _In_ NDIS_HANDLE NblPool,
    _In_ NDIS_HANDLE NbPool)
{
    PNET_BUFFER_LIST Original;
    PNET_BUFFER_LIST Clone;

    Original = NdisAllocateNetBufferList(NblPool, 0, 0);
    if (!ok(Original != NULL, "NdisAllocateNetBufferList failed\n"))
        return;

    /* No internal default pools here, so a null handle fails rather than faults. */
    Clone = NdisAllocateCloneNetBufferList(Original, NULL, NbPool, 0);
    ok(Clone == NULL, "clone succeeded with a null NBL pool\n");
    if (Clone != NULL)
        NdisFreeCloneNetBufferList(Clone, 0);

    Clone = NdisAllocateCloneNetBufferList(Original, NblPool, NULL, 0);
    ok(Clone == NULL, "clone succeeded with a null NET_BUFFER pool\n");
    if (Clone != NULL)
        NdisFreeCloneNetBufferList(Clone, 0);

    NdisFreeNetBufferList(Original);
}

static
VOID
NTAPI
TestCopyReceiveInfo(
    _In_ NDIS_HANDLE NblPool)
{
    PNET_BUFFER_LIST Source;
    PNET_BUFFER_LIST Dest;
    ULONG i;

    Source = NdisAllocateNetBufferList(NblPool, 0, 0);
    Dest = NdisAllocateNetBufferList(NblPool, 0, 0);
    if (Source == NULL || Dest == NULL)
    {
        ok(FALSE, "NdisAllocateNetBufferList failed\n");
        goto Cleanup;
    }

    for (i = 0; i < MaxNetBufferListInfo; i++)
        NET_BUFFER_LIST_INFO(Source, i) = SENTINEL(i);

    NdisCopyReceiveNetBufferListInfo(Dest, Source);

    /* Carried on the receive path. */
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, TcpIpChecksumNetBufferListInfo), SENTINEL(0));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, Ieee8021QNetBufferListInfo), SENTINEL(4));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListFrameType), SENTINEL(7));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListHashInfo), SENTINEL(9));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NblOriginalInterfaceIfIndex), SENTINEL(16));

    /* 64 bit builds carry the switch slots ahead of it, x86 does not. */
#if defined(_M_AMD64) || defined(_M_ARM64)
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, TcpRecvSegCoalesceInfo), SENTINEL(22));
#else
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, TcpRecvSegCoalesceInfo), SENTINEL(19));
#endif

    /* Not carried. WFP state and the correlation id belong to the destination. */
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, WfpNetBufferListInfo), NULL);
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListCorrelationId), NULL);

    /* The cancel id only follows when the source carries the loopback flag. */
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListCancelId), NULL);

    NET_BUFFER_LIST_NBL_FLAGS(Source) |= NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET;
    NdisCopyReceiveNetBufferListInfo(Dest, Source);

    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListCancelId), SENTINEL(5));
    ok((NET_BUFFER_LIST_NBL_FLAGS(Dest) & NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET) != 0,
       "NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET was not carried\n");

Cleanup:
    if (Source != NULL)
        NdisFreeNetBufferList(Source);
    if (Dest != NULL)
        NdisFreeNetBufferList(Dest);
}

static
VOID
NTAPI
TestCopySendInfo(
    _In_ NDIS_HANDLE NblPool,
    _In_ UCHAR ProtocolId)
{
    PNET_BUFFER_LIST Source;
    PNET_BUFFER_LIST Dest;
    ULONG_PTR FrameType;
    ULONG i;

    Source = NdisAllocateNetBufferList(NblPool, 0, 0);
    Dest = NdisAllocateNetBufferList(NblPool, 0, 0);
    if (Source == NULL || Dest == NULL)
    {
        ok(FALSE, "NdisAllocateNetBufferList failed\n");
        goto Cleanup;
    }

    for (i = 0; i < MaxNetBufferListInfo; i++)
        NET_BUFFER_LIST_INFO(Source, i) = SENTINEL(i);

    NdisCopySendNetBufferListInfo(Dest, Source);

    /* Carried on the send path. */
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, TcpIpChecksumNetBufferListInfo), SENTINEL(0));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, ClassificationHandleNetBufferListInfo), SENTINEL(3));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListCancelId), SENTINEL(5));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListHashValue), SENTINEL(8));
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, TcpSendOffloadsSupplementalNetBufferListInfo),
                  SENTINEL(20));

    /* Not carried on the send path, unlike receive. */
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NetBufferListHashInfo), NULL);
    ok_eq_pointer(NET_BUFFER_LIST_INFO(Dest, NblOriginalInterfaceIfIndex), NULL);

    /*
     * The frame type slot takes the source's protocol id in its low byte
     * rather than a copy of the source slot.
     */
    FrameType = (ULONG_PTR)NET_BUFFER_LIST_INFO(Dest, NetBufferListFrameType);
    ok_eq_ulong((ULONG)(FrameType & 0xFF), (ULONG)ProtocolId);
    ok(FrameType != (ULONG_PTR)SENTINEL(7),
       "frame type slot was copied instead of set to the protocol id\n");

Cleanup:
    if (Source != NULL)
        NdisFreeNetBufferList(Source);
    if (Dest != NULL)
        NdisFreeNetBufferList(Dest);
}

VOID
NTAPI
TestNbl(VOID)
{
    NDIS_HANDLE DataPool;
    NDIS_HANDLE PlainPool;
    NDIS_HANDLE NbPool;
    const UCHAR ProtocolId = 6;

    /* Carries an inline NET_BUFFER and a data area, so clones have real bytes. */
    DataPool = CreateNblPool(TRUE, TEST_DATA_SIZE, ProtocolId);
    if (!ok(DataPool != NULL, "NdisAllocateNetBufferListPool failed\n"))
        return;

    PlainPool = CreateNblPool(FALSE, 0, ProtocolId);
    NbPool = CreateNbPool();

    if (ok(PlainPool != NULL, "plain NBL pool failed\n") &&
        ok(NbPool != NULL, "NET_BUFFER pool failed\n"))
    {
        TestCloneWithOriginalMdls(DataPool, NbPool);
        TestCloneWithOwnMdls(DataPool, NbPool);
        TestCloneRejectsNullPools(DataPool, NbPool);
        TestCopyReceiveInfo(PlainPool);
        TestCopySendInfo(PlainPool, ProtocolId);
    }

    if (NbPool != NULL)
        NdisFreeNetBufferPool(NbPool);
    if (PlainPool != NULL)
        NdisFreeNetBufferListPool(PlainPool);
    NdisFreeNetBufferListPool(DataPool);
}
