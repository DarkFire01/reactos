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

/* Gear 3 is the highest UFS 2.x links run */
#define UFS_MAX_HS_GEAR 3
#define UFS_HS_SERIES_A 1
#define UFS_HS_SERIES_B 2

/**
 * @brief Asks the link which power mode and HS series the firmware ran it in, before a reset forgets them.
 */
static
VOID
UfsReadFirmwareLink(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    Adapter->FirmwareLinkRead = TRUE;

    if (!(UfsRead(Adapter, UFS_REG_HCE) & UFS_HCE_ENABLE) || !(UfsRead(Adapter, UFS_REG_HCS) & UFS_HCS_DP))
    {
        DPRINT1("UFS: firmware left the controller off\n");
        return;
    }

    if (!UfsDmeGet(Adapter, UFS_PA_PWR_MODE, &Adapter->FirmwareMode) ||
        !UfsDmeGet(Adapter, UFS_PA_HS_SERIES, &Adapter->FirmwareSeries))
    {
        Adapter->FirmwareMode = 0;
        Adapter->FirmwareSeries = 0;
    }

    DPRINT1("UFS: firmware left the link in mode 0x%lx, series %lu\n",
            Adapter->FirmwareMode, Adapter->FirmwareSeries);
}

/* The PHY is set up for one series, the firmware's when it ran HS, otherwise B on Qualcomm */
static
ULONG
UfsPickHsSeries(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG LinkSeries)
{
    if (Adapter->FirmwareMode == UFS_PWR_MODE(UFS_PWR_FAST_MODE, UFS_PWR_FAST_MODE) &&
        (Adapter->FirmwareSeries == UFS_HS_SERIES_A || Adapter->FirmwareSeries == UFS_HS_SERIES_B))
    {
        return Adapter->FirmwareSeries;
    }

    return Adapter->Qualcomm ? UFS_HS_SERIES_B : LinkSeries;
}

/**
 * @brief Moves the link out of the PWM gear link startup leaves it in, to the fastest gear both ends take.
 *
 * @return FALSE when the link may no longer carry traffic and has to be brought up again.
 */
static
BOOLEAN
UfsEnterHighSpeed(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG HostGear, DeviceGear, Gear, TxLanes, RxLanes, Series, Status, Spin, OldMode = 0;

    UfsDmeGet(Adapter, UFS_PA_PWR_MODE, &OldMode);
    if (!UfsDmeGet(Adapter, UFS_PA_MAX_RX_HS_GEAR, &HostGear) ||
        !UfsDmePeerGet(Adapter, UFS_PA_MAX_RX_HS_GEAR, &DeviceGear) ||
        !UfsDmeGet(Adapter, UFS_PA_CONNECTED_TX_LANES, &TxLanes) ||
        !UfsDmeGet(Adapter, UFS_PA_CONNECTED_RX_LANES, &RxLanes) ||
        !UfsDmeGet(Adapter, UFS_PA_HS_SERIES, &Series))
    {
        DPRINT1("UFS: link attributes unreadable, staying in PWM\n");
        return TRUE;
    }

    Gear = min(min(HostGear, DeviceGear), UFS_MAX_HS_GEAR);
    if (Gear == 0 || TxLanes == 0 || RxLanes == 0)
    {
        DPRINT1("UFS: no HS gear (host %lu, device %lu, lanes %lu/%lu), staying in PWM\n",
                HostGear, DeviceGear, TxLanes, RxLanes);
        return TRUE;
    }

    Series = UfsPickHsSeries(Adapter, Series);
    if (!UfsDmeSet(Adapter, UFS_PA_TX_GEAR, Gear) ||
        !UfsDmeSet(Adapter, UFS_PA_RX_GEAR, Gear) ||
        !UfsDmeSet(Adapter, UFS_PA_ACTIVE_TX_LANES, TxLanes) ||
        !UfsDmeSet(Adapter, UFS_PA_ACTIVE_RX_LANES, RxLanes) ||
        !UfsDmeSet(Adapter, UFS_PA_TX_TERMINATION, TRUE) ||
        !UfsDmeSet(Adapter, UFS_PA_RX_TERMINATION, TRUE) ||
        !UfsDmeSet(Adapter, UFS_PA_HS_SERIES, Series))
    {
        DPRINT1("UFS: HS parameters rejected, staying in PWM\n");
        return TRUE;
    }

    UfsWrite(Adapter, UFS_REG_IS, UFS_IS_UPMS);
    if (!UfsDmeSet(Adapter, UFS_PA_PWR_MODE, UFS_PWR_MODE(UFS_PWR_FAST_MODE, UFS_PWR_FAST_MODE)))
    {
        DPRINT1("UFS: power mode change rejected, staying in PWM\n");
        return TRUE;
    }

    for (Spin = 0; Spin < UFS_UIC_POLL_COUNT; Spin++)
    {
        if (UfsRead(Adapter, UFS_REG_IS) & UFS_IS_UPMS)
            break;
        StorPortStallExecution(10);
    }
    UfsWrite(Adapter, UFS_REG_IS, UFS_IS_UPMS);

    Status = (UfsRead(Adapter, UFS_REG_HCS) & UFS_HCS_UPMCRS_MASK) >> UFS_HCS_UPMCRS_SHIFT;
    if (Spin >= UFS_UIC_POLL_COUNT || Status != UFS_UPMCRS_PWR_LOCAL)
    {
        DPRINT1("UFS: power mode change failed, status %lu\n", Status);
        return FALSE;
    }

    /* The change can complete on a PHY that cannot run the series, only traffic tells */
    if (!NT_SUCCESS(UfsSendNopOut(Adapter)))
    {
        DPRINT1("UFS: HS-G%lu series %c does not answer a NOP\n", Gear, (Series == UFS_HS_SERIES_B) ? 'B' : 'A');
        return FALSE;
    }

    DPRINT1("UFS: link at HS-G%lu series %c, %lu TX and %lu RX lanes, was mode 0x%lx\n",
            Gear, (Series == UFS_HS_SERIES_B) ? 'B' : 'A', TxLanes, RxLanes, OldMode);
    return TRUE;
}

/* Reset, link startup and device init, leaving the link in the startup gear */
static
BOOLEAN
UfsBringUpLink(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Attempt;

    if (!NT_SUCCESS(UfsHceReset(Adapter)))
        return FALSE;

    if (Adapter->Qualcomm)
        UfsQcomBeforeLinkStartup(Adapter);

    if (!UfsLinkStartup(Adapter))
        return FALSE;

    if (Adapter->Qualcomm)
        UfsQcomAfterLinkStartup(Adapter);

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

    return UfsDeviceInit(Adapter);
}

BOOLEAN
UfsStartAdapter(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    if (!Adapter->FirmwareLinkRead)
        UfsReadFirmwareLink(Adapter);

    if (!UfsBringUpLink(Adapter))
        return FALSE;

    if (!Adapter->PwmOnly && !UfsEnterHighSpeed(Adapter))
    {
        DPRINT1("UFS: bringing the link up again in PWM\n");
        Adapter->PwmOnly = TRUE;
        if (!UfsBringUpLink(Adapter))
            return FALSE;
    }

    /* Clear any latched status, then arm the interrupts the ISR services. */
    UfsWrite(Adapter, UFS_REG_IS, 0xFFFFFFFF);
    UfsWrite(Adapter, UFS_REG_IE, UFS_IE_DEFAULT);

    DPRINT1("UFS: controller initialized, %lu transfer slots\n", Adapter->Nutrs);
    return TRUE;
}
