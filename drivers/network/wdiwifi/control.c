/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The native 802.11 control path: dot11 OIDs and their indications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The management service drives the adapter with the dot11 OIDs. Each one is
 * turned into the matching WDI command, and the WDI results come back up as
 * the dot11 status indications the service waits on.
 */

#include "wdiwifi.h"
#include <wditypes.h>

#define NDEBUG
#include <debug.h>

/* Status indications */

static
VOID
NTAPI
WdiIndicateDot11(
    _In_ PWDI_ADAPTER Adapter,
    _In_ NDIS_STATUS StatusCode,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    NDIS_STATUS_INDICATION Indication;

    RtlZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    Indication.SourceHandle = Adapter->MiniportAdapterHandle;
    Indication.StatusCode = StatusCode;
    Indication.StatusBuffer = Buffer;
    Indication.StatusBufferSize = Length;

    NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
}

_Use_decl_annotations_
VOID
NTAPI
WdiIndicateScanConfirm(
    PWDI_ADAPTER Adapter,
    NDIS_STATUS ScanStatus)
{
    DOT11_BYTE_ARRAY ByteArray;

    RtlZeroMemory(&ByteArray, sizeof(ByteArray));
    ByteArray.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ByteArray.Header.Revision = DOT11_BSS_ENTRY_BYTE_ARRAY_REVISION_1;
    ByteArray.Header.Size = sizeof(ByteArray);

    UNREFERENCED_PARAMETER(ScanStatus);
    WdiIndicateDot11(Adapter, NDIS_STATUS_DOT11_SCAN_CONFIRM, &ByteArray, sizeof(ByteArray));
}

_Use_decl_annotations_
VOID
NTAPI
WdiIndicateAssociation(
    PWDI_ADAPTER Adapter,
    PCWDI_MAC_ADDRESS Bssid,
    ULONG AssocStatus)
{
    DOT11_ASSOCIATION_COMPLETION_PARAMETERS Params;

    RtlZeroMemory(&Params, sizeof(Params));
    Params.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Params.Header.Revision = DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1;
    Params.Header.Size = sizeof(Params);
    RtlCopyMemory(Params.MacAddr, Bssid->Address, sizeof(Params.MacAddr));
    Params.uStatus = AssocStatus;
    Params.bPortAuthorized = (AssocStatus == DOT11_ASSOC_STATUS_SUCCESS);

    WdiIndicateDot11(Adapter, NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION, &Params, sizeof(Params));
}

_Use_decl_annotations_
VOID
NTAPI
WdiIndicateConnectionComplete(
    PWDI_ADAPTER Adapter,
    ULONG ConnectStatus)
{
    DOT11_CONNECTION_COMPLETION_PARAMETERS Params;

    RtlZeroMemory(&Params, sizeof(Params));
    Params.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Params.Header.Revision = DOT11_CONNECTION_COMPLETION_PARAMETERS_REVISION_1;
    Params.Header.Size = sizeof(Params);
    Params.uStatus = ConnectStatus;

    WdiIndicateDot11(Adapter, NDIS_STATUS_DOT11_CONNECTION_COMPLETION, &Params, sizeof(Params));
}

_Use_decl_annotations_
VOID
NTAPI
WdiIndicateDisassociation(
    PWDI_ADAPTER Adapter,
    PCWDI_MAC_ADDRESS Bssid,
    ULONG Reason)
{
    DOT11_DISASSOCIATION_PARAMETERS Params;

    RtlZeroMemory(&Params, sizeof(Params));
    Params.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Params.Header.Revision = DOT11_DISASSOCIATION_PARAMETERS_REVISION_1;
    Params.Header.Size = sizeof(Params);
    RtlCopyMemory(Params.MacAddr, Bssid->Address, sizeof(Params.MacAddr));
    Params.uReason = Reason;

    WdiIndicateDot11(Adapter, NDIS_STATUS_DOT11_DISASSOCIATION, &Params, sizeof(Params));
}

/* Query OIDs */

static
NDIS_STATUS
NTAPI
WdiQueryBssList(
    _In_ PWDI_ADAPTER Adapter,
    _Out_writes_bytes_to_(BufferLength, *Written) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Written,
    _Out_ PULONG Needed)
{
    PDOT11_BYTE_ARRAY Array = (PDOT11_BYTE_ARRAY)Buffer;
    PUCHAR At;
    ULONG Total;
    ULONG EntrySize;
    KIRQL OldIrql;
    ULONG i;

    *Written = 0;
    *Needed = 0;

    EntrySize = FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer);

    KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);

    Total = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer) + Adapter->BssCount * EntrySize;
    if (BufferLength < Total)
    {
        KeReleaseSpinLock(&Adapter->BssLock, OldIrql);
        *Needed = Total;
        return NDIS_STATUS_BUFFER_OVERFLOW;
    }

    Array->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Array->Header.Revision = DOT11_BSS_ENTRY_BYTE_ARRAY_REVISION_1;
    Array->Header.Size = sizeof(DOT11_BYTE_ARRAY);
    Array->uNumOfBytes = Adapter->BssCount * EntrySize;
    Array->uTotalNumOfBytes = Adapter->BssCount * EntrySize;

    At = Array->ucBuffer;
    for (i = 0; i < Adapter->BssCount; i++)
    {
        PDOT11_BSS_ENTRY Entry = (PDOT11_BSS_ENTRY)At;
        PWDI_BSS Bss = &Adapter->Bss[i];

        RtlZeroMemory(Entry, EntrySize);
        Entry->uPhyId = Bss->BandId;
        Entry->PhySpecificInfo.uChCenterFrequency = Bss->Channel;
        RtlCopyMemory(Entry->dot11BSSID, Bss->Bssid.Address, sizeof(Entry->dot11BSSID));
        Entry->dot11BSSType = dot11_BSS_type_infrastructure;
        Entry->lRSSI = Bss->Rssi;
        Entry->uLinkQuality = Bss->LinkQuality;
        Entry->bInRegDomain = TRUE;
        Entry->uBufferLength = 0;
        At += EntrySize;
    }

    KeReleaseSpinLock(&Adapter->BssLock, OldIrql);

    *Written = Total;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
WdiQuery(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    NDIS_OID Oid = OidRequest->DATA.QUERY_INFORMATION.Oid;
    PVOID Buffer = OidRequest->DATA.QUERY_INFORMATION.InformationBuffer;
    ULONG BufferLength = OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength;
    ULONG Value;

    switch (Oid)
    {
        case OID_DOT11_ENUM_BSS_LIST:
            return WdiQueryBssList(Adapter,
                                   Buffer,
                                   BufferLength,
                                   &OidRequest->DATA.QUERY_INFORMATION.BytesWritten,
                                   &OidRequest->DATA.QUERY_INFORMATION.BytesNeeded);

        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            DOT11_CURRENT_OPERATION_MODE Mode;

            if (BufferLength < sizeof(Mode))
            {
                OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(Mode);
                return NDIS_STATUS_BUFFER_OVERFLOW;
            }
            RtlZeroMemory(&Mode, sizeof(Mode));
            Mode.uCurrentOpMode = Adapter->OperationMode;
            RtlCopyMemory(Buffer, &Mode, sizeof(Mode));
            OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(Mode);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_HARDWARE_PHY_STATE:
        case OID_DOT11_NIC_POWER_STATE:
            Value = TRUE;
            if (BufferLength < sizeof(Value))
            {
                OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(Value);
                return NDIS_STATUS_BUFFER_OVERFLOW;
            }
            RtlCopyMemory(Buffer, &Value, sizeof(Value));
            OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(Value);
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

/* Set OIDs */

/* Little-endian 32-bit store for the packed WDI wire fields */
static
VOID
WdiPutLe32(
    _Out_writes_bytes_(4) PUCHAR Buffer,
    _In_ UINT32 Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
    Buffer[2] = (UCHAR)(Value >> 16);
    Buffer[3] = (UCHAR)(Value >> 24);
}

/* Picks the cached scan entry to connect to: the desired BSSID when one is
   set, otherwise the strongest network whose SSID matches the desired one */
static
BOOLEAN
WdiFindConnectBss(
    _In_ PWDI_ADAPTER Adapter,
    _Out_ PWDI_BSS Target)
{
    KIRQL OldIrql;
    BOOLEAN Found = FALSE;
    ULONG i;

    KeAcquireSpinLock(&Adapter->BssLock, &OldIrql);
    for (i = 0; i < Adapter->BssCount; i++)
    {
        PWDI_BSS Bss = &Adapter->Bss[i];

        if (Adapter->HasDesiredBssid)
        {
            if (!RtlEqualMemory(Bss->Bssid.Address, Adapter->DesiredBssid.Address,
                                sizeof(Bss->Bssid.Address)))
                continue;
        }
        else if (Bss->SsidLength != Adapter->DesiredSsidLength ||
                 !RtlEqualMemory(Bss->Ssid, Adapter->DesiredSsid, Adapter->DesiredSsidLength))
        {
            continue;
        }

        if (!Found || Bss->Rssi > Target->Rssi)
        {
            *Target = *Bss;
            Found = TRUE;
        }
    }
    KeReleaseSpinLock(&Adapter->BssLock, OldIrql);

    return Found;
}

static
VOID
NTAPI
WdiConnectWorker(
    _In_ PVOID WorkItemContext,
    _In_ NDIS_HANDLE NdisIoWorkItemHandle)
{
    PWDI_ADAPTER Adapter = WorkItemContext;
    PWDI_PORT Port = NULL;
    NDIS_STATUS Status;
    UCHAR Message[256];
    UCHAR Parameters[128];
    UCHAR Entry[64];
    UCHAR Settings[WDI_CONNECTION_SETTINGS_MAX_LENGTH];
    UCHAR SignalInfo[WDI_SIGNAL_INFO_LENGTH];
    UCHAR ChannelInfo[WDI_CHANNEL_INFO_LENGTH];
    ULONG MessageLength = 0;
    ULONG ParametersLength = 0;
    ULONG EntryLength = 0;
    ULONG SettingsLength;
    UINT32 Auth;
    UINT32 Cipher;
    WDI_BSS Target;
    BOOLEAN HaveTarget;
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
    {
        WdiIndicateConnectionComplete(Adapter, DOT11_CONNECTION_STATUS_FAILURE);
        goto Done;
    }

    /* WDI_TLV_CONNECT_PARAMETERS holds the connection settings, the SSID and
       the auth and cipher lists. A fresh connect to an open network for now,
       so the settings are all left clear. The packed settings grew with the
       WDI version */
    if (Adapter->PeerVersion >= WDI_VERSION_1_1_13)
        SettingsLength = 15;
    else if (Adapter->PeerVersion >= WDI_VERSION_1_0_1)
        SettingsLength = 14;
    else
        SettingsLength = 13;

    RtlZeroMemory(Settings, sizeof(Settings));
    ParametersLength += WdiTlvPut(Parameters + ParametersLength, WDI_TLV_CONNECTION_SETTINGS,
                                  Settings, (UINT16)SettingsLength);

    ParametersLength += WdiTlvPut(Parameters + ParametersLength, WDI_TLV_SSID,
                                  Adapter->DesiredSsid, Adapter->DesiredSsidLength);

    Auth = WDI_AUTH_ALGO_80211_OPEN;
    ParametersLength += WdiTlvPut(Parameters + ParametersLength, WDI_TLV_AUTH_ALGO_LIST,
                                  &Auth, sizeof(Auth));

    Cipher = WDI_CIPHER_ALGO_NONE;
    ParametersLength += WdiTlvPut(Parameters + ParametersLength, WDI_TLV_MULTICAST_CIPHER_ALGO_LIST,
                                  &Cipher, sizeof(Cipher));
    ParametersLength += WdiTlvPut(Parameters + ParametersLength, WDI_TLV_UNICAST_CIPHER_ALGO_LIST,
                                  &Cipher, sizeof(Cipher));

    MessageLength += WdiTlvPut(Message + MessageLength, WDI_TLV_CONNECT_PARAMETERS,
                               Parameters, (UINT16)ParametersLength);

    /* The candidate from our own scan gives the miniport the BSSID, signal
       and channel it needs to associate */
    HaveTarget = WdiFindConnectBss(Adapter, &Target);
    if (HaveTarget)
    {
        EntryLength += WdiTlvPut(Entry + EntryLength, WDI_TLV_BSSID,
                                 Target.Bssid.Address, sizeof(Target.Bssid.Address));

        WdiPutLe32(SignalInfo + 0, (UINT32)Target.Rssi);
        WdiPutLe32(SignalInfo + 4, Target.LinkQuality);
        EntryLength += WdiTlvPut(Entry + EntryLength, WDI_TLV_BSS_ENTRY_SIGNAL_INFO,
                                 SignalInfo, sizeof(SignalInfo));

        WdiPutLe32(ChannelInfo + 0, Target.Channel);
        WdiPutLe32(ChannelInfo + 4, Target.BandId);
        EntryLength += WdiTlvPut(Entry + EntryLength, WDI_TLV_BSS_ENTRY_CHANNEL_INFO,
                                 ChannelInfo, sizeof(ChannelInfo));

        MessageLength += WdiTlvPut(Message + MessageLength, WDI_TLV_CONNECT_BSS_ENTRY,
                                   Entry, (UINT16)EntryLength);
    }

    DPRINT1("WLAN connecting to a %u byte SSID on port %u, candidate %s\n",
            Adapter->DesiredSsidLength, Port->PortId, HaveTarget ? "found" : "none");

    Status = WdiSendCommand(Adapter, WDI_TASK_CONNECT, Port->PortId, Message, MessageLength, TRUE, NULL);

    /* The association indication carries the BSSID from the peer creation, so
       this only needs to report whether the task itself finished */
    WdiIndicateConnectionComplete(Adapter,
                                  Status == NDIS_STATUS_SUCCESS ?
                                  DOT11_CONNECTION_STATUS_SUCCESS : DOT11_CONNECTION_STATUS_FAILURE);

Done:
    Adapter->Connecting = FALSE;
    KeSetEvent(&Adapter->ConnectIdle, IO_NO_INCREMENT, FALSE);
}

static
NDIS_STATUS
NTAPI
WdiSet(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    NDIS_OID Oid = OidRequest->DATA.SET_INFORMATION.Oid;
    PVOID Buffer = OidRequest->DATA.SET_INFORMATION.InformationBuffer;
    ULONG BufferLength = OidRequest->DATA.SET_INFORMATION.InformationBufferLength;

    switch (Oid)
    {
        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            PDOT11_CURRENT_OPERATION_MODE Mode = Buffer;

            if (BufferLength < sizeof(*Mode))
                return NDIS_STATUS_INVALID_LENGTH;
            Adapter->OperationMode = Mode->uCurrentOpMode;
            OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(*Mode);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_DESIRED_SSID_LIST:
        {
            PDOT11_SSID_LIST List = Buffer;

            if (BufferLength < FIELD_OFFSET(DOT11_SSID_LIST, SSIDs) + sizeof(DOT11_SSID))
                return NDIS_STATUS_INVALID_LENGTH;
            if (List->uNumOfEntries == 0 || List->SSIDs[0].uSSIDLength > sizeof(Adapter->DesiredSsid))
                return NDIS_STATUS_INVALID_DATA;

            Adapter->DesiredSsidLength = (UCHAR)List->SSIDs[0].uSSIDLength;
            RtlCopyMemory(Adapter->DesiredSsid, List->SSIDs[0].ucSSID, Adapter->DesiredSsidLength);
            Adapter->HasDesiredSsid = TRUE;
            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_DESIRED_BSSID_LIST:
        {
            PDOT11_BSSID_LIST List = Buffer;

            if (BufferLength < FIELD_OFFSET(DOT11_BSSID_LIST, BSSIDs) + sizeof(DOT11_MAC_ADDRESS))
            {
                Adapter->HasDesiredBssid = FALSE;
                OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
                return NDIS_STATUS_SUCCESS;
            }
            RtlCopyMemory(Adapter->DesiredBssid.Address, &List->BSSIDs[0], sizeof(Adapter->DesiredBssid.Address));
            Adapter->HasDesiredBssid = TRUE;
            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_SCAN_REQUEST:
            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            WdiStartScan(Adapter);
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_CONNECT_REQUEST:
            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            if (!Adapter->HasDesiredSsid)
                return NDIS_STATUS_INVALID_STATE;
            if (Adapter->ConnectWorkItem == NULL || Adapter->Connecting)
                return NDIS_STATUS_NOT_ACCEPTED;
            Adapter->Connecting = TRUE;
            KeClearEvent(&Adapter->ConnectIdle);
            NdisQueueIoWorkItem(Adapter->ConnectWorkItem, WdiConnectWorker, Adapter);
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_DISCONNECT_REQUEST:
        {
            PWDI_PORT Port = NULL;
            ULONG i;

            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            for (i = 0; i < RTL_NUMBER_OF(Adapter->Ports); i++)
            {
                if (Adapter->Ports[i].InUse &&
                    Adapter->Ports[i].NdisPortNumber == NDIS_DEFAULT_PORT_NUMBER)
                {
                    Port = &Adapter->Ports[i];
                    break;
                }
            }
            if (Port == NULL)
                return NDIS_STATUS_INVALID_STATE;

            Adapter->HasDesiredSsid = FALSE;
            WdiSendCommand(Adapter, WDI_TASK_DISCONNECT, Port->PortId, NULL, 0, TRUE, NULL);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_RESET_REQUEST:
        case OID_DOT11_FLUSH_BSS_LIST:
            OidRequest->DATA.SET_INFORMATION.BytesRead = BufferLength;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

/**
 * @brief
 * The dot11 OIDs the management service sends the adapter.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiHandleOidRequest(
    PWDI_ADAPTER Adapter,
    PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return WdiQuery(Adapter, OidRequest);

        case NdisRequestSetInformation:
            return WdiSet(Adapter, OidRequest);

        case NdisRequestMethod:
            /* The method OIDs here read from and write to the same buffer */
            OidRequest->DATA.SET_INFORMATION.Oid = OidRequest->DATA.METHOD_INFORMATION.Oid;
            OidRequest->DATA.SET_INFORMATION.InformationBuffer = OidRequest->DATA.METHOD_INFORMATION.InformationBuffer;
            OidRequest->DATA.SET_INFORMATION.InformationBufferLength = OidRequest->DATA.METHOD_INFORMATION.InputBufferLength;
            return WdiSet(Adapter, OidRequest);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}
