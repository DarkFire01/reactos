/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller bring-up: link startup, NOP, device initialization
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

static
NTSTATUS
UfsSendNopOut(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    PUCHAR Ucd;
    PUCHAR Response;
    LONG Slot;
    NTSTATUS Status;

    Slot = UfsAllocSlot(Adapter);
    if (Slot < 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    Ucd = UfsSlotUcd(Adapter, Slot);
    RtlZeroMemory(Ucd, UFS_UCD_SIZE);
    Ucd[0] = UFS_UPIU_NOP_OUT;
    Ucd[3] = (UCHAR)Slot;

    UfsBuildUtrd(Adapter, Slot, UFS_UTRD_DD_NODATA, 0);
    Status = UfsRunSlotSynchronous(Adapter, Slot);

    if (NT_SUCCESS(Status))
    {
        Response = Ucd + UFS_UCD_RESP_OFFSET;
        if (Response[0] != UFS_UPIU_NOP_IN)
            Status = STATUS_UNSUCCESSFUL;
    }

    UfsFreeSlot(Adapter, Slot);
    return Status;
}

static
NTSTATUS
UfsQueryFlag(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR Function,
    _In_ UCHAR Opcode,
    _In_ UCHAR Idn,
    _Out_opt_ PUCHAR Value)
{
    PUFS_QUERY_UPIU Query;
    PUCHAR Ucd;
    PUCHAR Response;
    LONG Slot;
    NTSTATUS Status;

    Slot = UfsAllocSlot(Adapter);
    if (Slot < 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    Ucd = UfsSlotUcd(Adapter, Slot);
    RtlZeroMemory(Ucd, UFS_UCD_SIZE);

    Query = (PUFS_QUERY_UPIU)Ucd;
    Query->TransactionType = UFS_UPIU_QUERY_REQUEST;
    Query->QueryFunction = Function;
    Query->TaskTag = (UCHAR)Slot;
    Query->Opcode = Opcode;
    Query->Idn = Idn;

    UfsBuildUtrd(Adapter, Slot, UFS_UTRD_DD_NODATA, 0);
    Status = UfsRunSlotSynchronous(Adapter, Slot);

    if (NT_SUCCESS(Status))
    {
        Response = Ucd + UFS_UCD_RESP_OFFSET;
        if (Response[6] != 0)
        {
            Status = STATUS_UNSUCCESSFUL;
        }
        else if (Value != NULL)
        {
            /* The flag value lands in the last byte of the value field. */
            *Value = Response[0x17];
        }
    }

    UfsFreeSlot(Adapter, Slot);
    return Status;
}

static
BOOLEAN
UfsLinkStartup(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Attempt;
    ULONG Spin;

    for (Attempt = 0; Attempt < UFS_LINKSTARTUP_ATTEMPTS; Attempt++)
    {
        UfsSendUic(Adapter, UFS_UIC_DME_LINKSTARTUP, 0, 0, 0, NULL);

        for (Spin = 0; Spin < UFS_UIC_POLL_COUNT; Spin++)
        {
            if (UfsRead(Adapter, UFS_REG_HCS) & UFS_HCS_DP)
                return TRUE;
            StorPortStallExecution(10);
        }
    }

    DPRINT1("UFS: link startup exhausted attempts\n");
    return FALSE;
}

static
BOOLEAN
UfsProgramLists(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Spin;

    UfsWrite(Adapter, UFS_REG_UTRLRSR, 0);
    UfsWrite(Adapter, UFS_REG_UTMRLRSR, 0);

    UfsWrite(Adapter, UFS_REG_UTRLBA, Adapter->UtrdListPhysical.LowPart);
    UfsWrite(Adapter, UFS_REG_UTRLBAU, Adapter->UtrdListPhysical.HighPart);
    UfsWrite(Adapter, UFS_REG_UTMRLBA, Adapter->UtmrdListPhysical.LowPart);
    UfsWrite(Adapter, UFS_REG_UTMRLBAU, Adapter->UtmrdListPhysical.HighPart);

    UfsWrite(Adapter, UFS_REG_UTRLRSR, 1);
    UfsWrite(Adapter, UFS_REG_UTMRLRSR, 1);
    UfsWrite(Adapter, UFS_REG_IS, 0xFFFFFFFF);

    for (Spin = 0; Spin < UFS_UIC_POLL_COUNT; Spin++)
    {
        if (UfsRead(Adapter, UFS_REG_HCS) & UFS_HCS_UTRLRDY)
            return TRUE;
        StorPortStallExecution(10);
    }

    DPRINT1("UFS: UTR list did not become ready\n");
    return FALSE;
}

static
BOOLEAN
UfsDeviceInit(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Attempt;
    UCHAR Flag;
    NTSTATUS Status;

    for (Attempt = 0; Attempt < UFS_DEVINIT_SET_ATTEMPTS; Attempt++)
    {
        Status = UfsQueryFlag(Adapter,
                              UFS_QUERY_FUNC_WRITE,
                              UFS_QUERY_SET_FLAG,
                              UFS_FLAG_DEVICE_INIT,
                              NULL);
        if (NT_SUCCESS(Status))
            break;
    }
    if (Attempt >= UFS_DEVINIT_SET_ATTEMPTS)
    {
        DPRINT1("UFS: could not set fDeviceInit\n");
        return FALSE;
    }

    for (Attempt = 0; Attempt < UFS_DEVINIT_CLEAR_POLLS; Attempt++)
    {
        Status = UfsQueryFlag(Adapter,
                              UFS_QUERY_FUNC_READ,
                              UFS_QUERY_READ_FLAG,
                              UFS_FLAG_DEVICE_INIT,
                              &Flag);
        if (NT_SUCCESS(Status) && Flag == 0)
            return TRUE;

        StorPortStallExecution(3000);
    }

    DPRINT1("UFS: fDeviceInit never cleared\n");
    return FALSE;
}

BOOLEAN
UfsStartAdapter(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Attempt;

    if (!NT_SUCCESS(UfsHceReset(Adapter)))
        return FALSE;

    if (!UfsLinkStartup(Adapter))
        return FALSE;

    if (!UfsProgramLists(Adapter))
        return FALSE;

    for (Attempt = 0; Attempt < UFS_NOPOUT_ATTEMPTS; Attempt++)
    {
        if (NT_SUCCESS(UfsSendNopOut(Adapter)))
            break;
    }
    if (Attempt >= UFS_NOPOUT_ATTEMPTS)
    {
        DPRINT1("UFS: NOP OUT exhausted attempts\n");
        return FALSE;
    }

    if (!UfsDeviceInit(Adapter))
        return FALSE;

    /* Clear any latched status, then arm the interrupts the ISR services. */
    UfsWrite(Adapter, UFS_REG_IS, 0xFFFFFFFF);
    UfsWrite(Adapter, UFS_REG_IE, UFS_IE_DEFAULT);

    DPRINT1("UFS: controller initialized, %lu transfer slots\n", Adapter->Nutrs);
    return TRUE;
}
