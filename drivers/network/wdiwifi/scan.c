/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Scanning for networks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Until scans are asked for from above, one runs when the adapter comes up
 * and what it finds goes to the debug log.
 */

#include "wdiwifi.h"
#include <wditypes.h>

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * Takes a BSS list the miniport indicated, adding new networks to the
 * adapter's table and refreshing ones already in it.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Tlvs
 * The TLVs after the indication header.
 *
 * @param[in] Length
 * Their length.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiRecordBssList(
    PWDI_ADAPTER Adapter,
    const UCHAR *Tlvs,
    ULONG Length)
{
    const UCHAR *Value;
    USHORT ValueLength;
    USHORT Type;
    ULONG Offset = 0;
    WDI_BSS Bss;
    KIRQL OldIrql;
    ULONG i;

    while (WdiTlvNext(Tlvs, Length, &Offset, &Type, &Value, &ValueLength))
    {
        if (Type != WDI_TLV_BSS_ENTRY || !WdiParseBssEntry(Value, ValueLength, &Bss))
            continue;

        KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);
        for (i = 0; i < Adapter->BssCount; i++)
        {
            if (RtlEqualMemory(Adapter->Bss[i].Bssid.Address, Bss.Bssid.Address, sizeof(Bss.Bssid.Address)))
                break;
        }

        if (i < Adapter->BssCount)
        {
            /* Keep a name learned earlier when this frame had none */
            if (Bss.SsidLength == 0)
            {
                Bss.SsidLength = Adapter->Bss[i].SsidLength;
                RtlCopyMemory(Bss.Ssid, Adapter->Bss[i].Ssid, sizeof(Bss.Ssid));
            }
            Adapter->Bss[i] = Bss;
        }
        else if (Adapter->BssCount < RTL_NUMBER_OF(Adapter->Bss))
        {
            Adapter->Bss[Adapter->BssCount++] = Bss;
        }
        KeReleaseSpinLock(&Adapter->BssLock, OldIrql);
    }
}

static
PCSTR
WdiBandName(
    _In_ UINT32 BandId)
{
    switch (BandId)
    {
        case WDI_BAND_ID_2400:  return "2.4 GHz";
        case WDI_BAND_ID_5000:  return "5 GHz";
        case WDI_BAND_ID_6000:  return "6 GHz";
        case WDI_BAND_ID_60000: return "60 GHz";
        case WDI_BAND_ID_900:   return "900 MHz";
        default:                return "?";
    }
}

static
VOID
WdiLogBssTable(
    _In_ PWDI_ADAPTER Adapter)
{
    WDI_BSS Bss;
    CHAR Name[sizeof(Bss.Ssid) + 1];
    ULONG Count;
    KIRQL OldIrql;
    ULONG i;
    ULONG j;

    KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);
    Count = Adapter->BssCount;
    KeReleaseSpinLock(&Adapter->BssLock, OldIrql);

    DPRINT1("WLAN scan found %lu network%s\n", Count, Count == 1 ? "" : "s");

    for (i = 0; i < Count; i++)
    {
        KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);
        Bss = Adapter->Bss[i];
        KeReleaseSpinLock(&Adapter->BssLock, OldIrql);

        /* Hidden networks send an empty or zeroed SSID */
        for (j = 0; j < Bss.SsidLength; j++)
            Name[j] = (Bss.Ssid[j] >= 0x20 && Bss.Ssid[j] < 0x7F) ? (CHAR)Bss.Ssid[j] : '?';
        Name[j] = ANSI_NULL;
        if (Bss.SsidLength == 0 || Bss.Ssid[0] == 0)
            strcpy(Name, "<hidden>");

        DPRINT1("  %-32s %02x:%02x:%02x:%02x:%02x:%02x  ch %3lu %-7s  %4ld dBm  quality %lu\n",
                Name,
                Bss.Bssid.Address[0], Bss.Bssid.Address[1], Bss.Bssid.Address[2],
                Bss.Bssid.Address[3], Bss.Bssid.Address[4], Bss.Bssid.Address[5],
                Bss.Channel, WdiBandName(Bss.BandId), Bss.Rssi, Bss.LinkQuality);
    }
}

static
VOID
NTAPI
WdiScanWorker(
    _In_ PVOID WorkItemContext,
    _In_ NDIS_HANDLE NdisIoWorkItemHandle)
{
    PWDI_ADAPTER Adapter = WorkItemContext;
    PWDI_PORT Port = NULL;
    NDIS_STATUS Status;
    KIRQL OldIrql;
    UCHAR Tlvs[64];
    ULONG Length;
    ULONG i;

    UNREFERENCED_PARAMETER(NdisIoWorkItemHandle);

    for (i = 0; i < RTL_NUMBER_OF(Adapter->Ports); i++)
    {
        if (Adapter->Ports[i].InUse && Adapter->Ports[i].NdisPortNumber == NDIS_DEFAULT_PORT_NUMBER)
        {
            Port = &Adapter->Ports[i];
            break;
        }
    }

    if (Port == NULL)
        goto Done;

    KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);
    Adapter->BssCount = 0;
    KeReleaseSpinLock(&Adapter->BssLock, OldIrql);

    DPRINT1("WLAN scan started on port %u\n", Port->PortId);

    Length = WdiBuildScan(Tlvs);
    Status = WdiSendCommand(Adapter, WDI_TASK_SCAN, Port->PortId, Tlvs, Length, TRUE, NULL);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("WLAN scan failed (0x%x)\n", Status);
        goto Done;
    }

    /* A miniport that keeps its own BSS cache hands it over when asked */
    if (Adapter->BssCount == 0)
    {
        Length = WdiTlvPut(Tlvs, WDI_TLV_SSID, NULL, 0);
        Status = WdiSendCommand(Adapter, WDI_GET_BSS_ENTRY_LIST, Port->PortId, Tlvs, Length, FALSE, NULL);
        if (Status != NDIS_STATUS_SUCCESS)
            DPRINT1("Reading the BSS list failed (0x%x)\n", Status);
    }

    WdiLogBssTable(Adapter);

Done:
    KeSetEvent(&Adapter->ScanIdle, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * Starts a scan on the station port from a work item.
 *
 * @param[in] Adapter
 * The adapter, brought up.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiStartTestScan(
    PWDI_ADAPTER Adapter)
{
    if (Adapter->ScanWorkItem == NULL)
        return;

    KeClearEvent(&Adapter->ScanIdle);
    NdisQueueIoWorkItem(Adapter->ScanWorkItem, WdiScanWorker, Adapter);
}

/**
 * @brief
 * Waits for a scan in flight to finish.
 *
 * @param[in] Adapter
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiWaitForScan(
    PWDI_ADAPTER Adapter)
{
    KeWaitForSingleObject(&Adapter->ScanIdle, Executive, KernelMode, FALSE, NULL);
}
