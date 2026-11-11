/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UFSHCI register access, slots, UIC commands and reset
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

PUCHAR
UfsSlotUcd(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot)
{
    return Adapter->UcdArray + (Slot * UFS_UCD_SIZE);
}

STOR_PHYSICAL_ADDRESS
UfsSlotUcdPhysical(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot)
{
    STOR_PHYSICAL_ADDRESS Address = Adapter->UcdArrayPhysical;

    Address.QuadPart += (ULONGLONG)Slot * UFS_UCD_SIZE;
    return Address;
}

LONG
UfsAllocSlot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    LONG Free;
    LONG Bit;
    LONG Mask;

    for (;;)
    {
        Free = *(volatile LONG *)&Adapter->FreeSlots;
        if (Free == 0)
            return -1;

        for (Bit = 0; Bit < (LONG)Adapter->Nutrs; Bit++)
        {
            Mask = 1 << Bit;
            if (Free & Mask)
                break;
        }

        if (Bit >= (LONG)Adapter->Nutrs)
            return -1;

        if (InterlockedCompareExchange((volatile LONG *)&Adapter->FreeSlots,
                                       Free & ~Mask,
                                       Free) == Free)
        {
            return Bit;
        }
    }
}

VOID
UfsFreeSlot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot)
{
    InterlockedOr((volatile LONG *)&Adapter->FreeSlots, 1 << Slot);
}

VOID
UfsBuildUtrd(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot,
    _In_ ULONG DataDirection,
    _In_ USHORT PrdtEntries)
{
    PUFS_UTRD Utrd = &Adapter->UtrdList[Slot];
    STOR_PHYSICAL_ADDRESS Ucd = UfsSlotUcdPhysical(Adapter, Slot);

    Utrd->Dw0Flags = UFS_UTRD_DW0_BASE | (DataDirection << UFS_UTRD_DD_SHIFT);
    Utrd->Dw1 = 0;
    Utrd->Dw2Ocs = UFS_OCS_INVALID;
    Utrd->Dw3 = 0;
    Utrd->UcdbaLower = Ucd.LowPart;
    Utrd->UcdbaUpper = Ucd.HighPart;
    Utrd->ResponseOffset = UFS_RESP_OFFSET_DW;
    Utrd->ResponseLength = (UFS_UCD_PRDT_OFFSET - UFS_UCD_RESP_OFFSET) / sizeof(ULONG);
    Utrd->PrdtOffset = (PrdtEntries != 0) ? UFS_PRDT_OFFSET_DW : 0;
    Utrd->PrdtLength = (USHORT)(PrdtEntries * (sizeof(UFS_PRDT_ENTRY) / sizeof(ULONG)));
}

VOID
UfsRingDoorbell(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot)
{
    UfsWrite(Adapter, UFS_REG_UTRLDBR, 1 << Slot);
}

NTSTATUS
UfsSendUic(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Opcode,
    _In_ ULONG Arg1,
    _In_ ULONG Arg2,
    _In_ ULONG Arg3,
    _Out_opt_ PULONG Result)
{
    ULONG Spin;
    ULONG Code;

    /* The controller must be ready to take a UIC command. */
    for (Spin = 0; Spin < UFS_UIC_POLL_COUNT; Spin++)
    {
        if (UfsRead(Adapter, UFS_REG_HCS) & UFS_HCS_UCRDY)
            break;
        StorPortStallExecution(10);
    }
    if (Spin >= UFS_UIC_POLL_COUNT)
        return STATUS_IO_TIMEOUT;

    UfsWrite(Adapter, UFS_REG_IS, UFS_IS_UCCS);
    UfsWrite(Adapter, UFS_REG_UCMDARG1, Arg1);
    UfsWrite(Adapter, UFS_REG_UCMDARG2, Arg2);
    UfsWrite(Adapter, UFS_REG_UCMDARG3, Arg3);
    UfsWrite(Adapter, UFS_REG_UICCMD, Opcode);

    for (Spin = 0; Spin < UFS_UIC_POLL_COUNT; Spin++)
    {
        if (UfsRead(Adapter, UFS_REG_IS) & UFS_IS_UCCS)
            break;
        StorPortStallExecution(10);
    }
    if (Spin >= UFS_UIC_POLL_COUNT)
        return STATUS_IO_TIMEOUT;

    UfsWrite(Adapter, UFS_REG_IS, UFS_IS_UCCS);

    Code = UfsRead(Adapter, UFS_REG_UCMDARG2) & UFS_UIC_RESULT_MASK;
    if (Result != NULL)
        *Result = Code;

    return (Code == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS
UfsHceReset(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Spin;

    UfsWrite(Adapter, UFS_REG_HCE, 0);
    for (Spin = 0; Spin < UFS_HCE_POLL_COUNT; Spin++)
    {
        if ((UfsRead(Adapter, UFS_REG_HCE) & UFS_HCE_ENABLE) == 0)
            break;
        StorPortStallExecution(10);
    }
    if (Spin >= UFS_HCE_POLL_COUNT)
    {
        DPRINT1("UFS: host controller did not clear enable\n");
        return STATUS_IO_TIMEOUT;
    }

    UfsWrite(Adapter, UFS_REG_HCE, UFS_HCE_ENABLE);
    for (Spin = 0; Spin < UFS_HCE_POLL_COUNT; Spin++)
    {
        if (UfsRead(Adapter, UFS_REG_HCE) & UFS_HCE_ENABLE)
            break;
        StorPortStallExecution(10);
    }
    if (Spin >= UFS_HCE_POLL_COUNT)
    {
        DPRINT1("UFS: host controller did not enable\n");
        return STATUS_IO_TIMEOUT;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
UfsRunSlotSynchronous(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot)
{
    ULONG Spin;
    UCHAR Ocs;

    UfsRingDoorbell(Adapter, Slot);

    for (Spin = 0; Spin < UFS_SLOT_POLL_COUNT; Spin++)
    {
        if ((UfsRead(Adapter, UFS_REG_UTRLDBR) & (1 << Slot)) == 0)
            break;
        StorPortStallExecution(10);
    }

    if (Spin >= UFS_SLOT_POLL_COUNT)
    {
        /* Clear the stuck slot so the list can keep running. */
        UfsWrite(Adapter, UFS_REG_UTRLCLR, ~(1 << Slot));
        DPRINT1("UFS: slot %lu timed out\n", Slot);
        return STATUS_IO_TIMEOUT;
    }

    Ocs = (UCHAR)(Adapter->UtrdList[Slot].Dw2Ocs & 0xFF);
    if (Ocs != UFS_OCS_SUCCESS)
    {
        DPRINT1("UFS: slot %lu OCS 0x%02x\n", Slot, Ocs);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}
