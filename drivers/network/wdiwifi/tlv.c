/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WDI TLVs for bring-up messages
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "wdiwifi.h"
#include <wditypes.h>

#define NDEBUG
#include <debug.h>

/* A TLV is a 16-bit type and a 16-bit value length, then the value */
#define WDI_TLV_HEADER_LENGTH               4

/* Packed WDI_TLV_INTERFACE_CAPABILITIES lengths, by the WDI version that grew it */
#define WDI_INTERFACE_CAPS_V1_1_8           54
#define WDI_INTERFACE_CAPS_V1_0_20          51
#define WDI_INTERFACE_CAPS_V1_0_1           50
#define WDI_INTERFACE_CAPS_V1_0             49

#define WDI_DATAPATH_CAPS_LENGTH            18
#define WDI_PORT_ATTRIBUTES_LENGTH          8
#define WDI_CREATE_PORT_PARAMETERS_LENGTH   6
#define WDI_LINK_QUALITY_ENTRY_LENGTH       3

static
UINT16
WdiRead16(
    _In_reads_bytes_(2) const UCHAR *Value)
{
    return (UINT16)(Value[0] | (Value[1] << 8));
}

static
UINT32
WdiRead32(
    _In_reads_bytes_(4) const UCHAR *Value)
{
    return (UINT32)Value[0] | ((UINT32)Value[1] << 8) | ((UINT32)Value[2] << 16) | ((UINT32)Value[3] << 24);
}

/**
 * @brief
 * Writes one TLV, or just measures it when there is no buffer.
 *
 * @return
 * The TLV's length, header included.
 */
_Use_decl_annotations_
ULONG
NTAPI
WdiTlvPut(
    PUCHAR Buffer,
    UINT16 Type,
    const VOID *Value,
    UINT16 Length)
{
    if (Buffer != NULL)
    {
        Buffer[0] = (UCHAR)Type;
        Buffer[1] = (UCHAR)(Type >> 8);
        Buffer[2] = (UCHAR)Length;
        Buffer[3] = (UCHAR)(Length >> 8);
        if (Length != 0)
            RtlCopyMemory(Buffer + WDI_TLV_HEADER_LENGTH, Value, Length);
    }

    return WDI_TLV_HEADER_LENGTH + Length;
}

/* Adds a TLV at the end of what a builder wrote so far */
static
VOID
WdiTlvAppend(
    _Out_writes_bytes_opt_(*Length + 4 + ValueLength) PUCHAR Buffer,
    _Inout_ PULONG Length,
    _In_ UINT16 Type,
    _In_reads_bytes_(ValueLength) const VOID *Value,
    _In_ UINT16 ValueLength)
{
    *Length += WdiTlvPut(Buffer != NULL ? Buffer + *Length : NULL, Type, Value, ValueLength);
}

/**
 * @brief
 * Finds the first TLV of a type in a run of TLVs.
 *
 * @return
 * TRUE when found. A run that overruns its length ends the search.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
WdiTlvFind(
    const UCHAR *Tlvs,
    ULONG Length,
    UINT16 Type,
    const UCHAR **Value,
    PUSHORT ValueLength)
{
    ULONG Offset = 0;
    UINT16 ThisType;
    UINT16 ThisLength;

    *Value = NULL;
    *ValueLength = 0;

    while (Length - Offset >= WDI_TLV_HEADER_LENGTH)
    {
        ThisType = WdiRead16(Tlvs + Offset);
        ThisLength = WdiRead16(Tlvs + Offset + 2);
        Offset += WDI_TLV_HEADER_LENGTH;

        if (ThisLength > Length - Offset)
        {
            DPRINT1("TLV 0x%x claims %u bytes, %lu left\n", ThisType, ThisLength, Length - Offset);
            return FALSE;
        }

        if (ThisType == Type)
        {
            *Value = Tlvs + Offset;
            *ValueLength = ThisLength;
            return TRUE;
        }

        Offset += ThisLength;
    }

    return FALSE;
}

static
ULONG
WdiInterfaceCapsLength(
    _In_ ULONG PeerVersion)
{
    if (PeerVersion >= WDI_VERSION_1_1_8)
        return WDI_INTERFACE_CAPS_V1_1_8;
    if (PeerVersion >= WDI_VERSION_1_0_20)
        return WDI_INTERFACE_CAPS_V1_0_20;
    if (PeerVersion >= WDI_VERSION_1_0_1)
        return WDI_INTERFACE_CAPS_V1_0_1;
    return WDI_INTERFACE_CAPS_V1_0;
}

/**
 * @brief
 * Pulls what bring-up needs out of a WDI_GET_ADAPTER_CAPABILITIES response.
 *
 * @param[in] PeerVersion
 * The miniport's WDI version, which picks the layout of versioned TLVs.
 *
 * @param[in] Tlvs
 * The TLVs after the response header.
 *
 * @param[in] Length
 * Their length.
 *
 * @param[out] Caps
 * The capabilities.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or NDIS_STATUS_INVALID_DATA when a required TLV is
 * missing or the wrong size.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiParseCapabilities(
    ULONG PeerVersion,
    const UCHAR *Tlvs,
    ULONG Length,
    PWDI_CAPABILITIES Caps)
{
    const UCHAR *Attributes;
    const UCHAR *Value;
    USHORT AttributesLength;
    USHORT ValueLength;
    CHAR Firmware[64];

    RtlZeroMemory(Caps, sizeof(*Caps));

    if (!WdiTlvFind(Tlvs, Length, WDI_TLV_INTERFACE_ATTRIBUTES, &Attributes, &AttributesLength))
    {
        DPRINT1("No interface attributes\n");
        return NDIS_STATUS_INVALID_DATA;
    }

    if (!WdiTlvFind(Attributes, AttributesLength, WDI_TLV_INTERFACE_CAPABILITIES, &Value, &ValueLength) ||
        ValueLength != WdiInterfaceCapsLength(PeerVersion))
    {
        DPRINT1("Interface capabilities missing or %u bytes for WDI 0x%lx\n", ValueLength, PeerVersion);
        return NDIS_STATUS_INVALID_DATA;
    }

    Caps->MtuSize = WdiRead32(Value + 0);
    Caps->MaxMulticastListSize = WdiRead32(Value + 4);
    Caps->BackFillSize = WdiRead16(Value + 8);
    RtlCopyMemory(Caps->PermanentAddress.Address, Value + 10, sizeof(Caps->PermanentAddress.Address));
    Caps->MaxTxRate = WdiRead32(Value + 16);
    Caps->MaxRxRate = WdiRead32(Value + 20);
    Caps->HardwareRadioOn = Value[24] != 0;
    Caps->SoftwareRadioOn = Value[25] != 0;
    Caps->ActionFramesSupported = Value[28] != 0;
    Caps->NonWdiOidsSupported = Value[45] != 0;

    if (!WdiTlvFind(Attributes, AttributesLength, WDI_TLV_FIRMWARE_VERSION, &Value, &ValueLength))
    {
        DPRINT1("No firmware version\n");
        return NDIS_STATUS_INVALID_DATA;
    }

    RtlZeroMemory(Firmware, sizeof(Firmware));
    RtlCopyMemory(Firmware, Value, min(ValueLength, sizeof(Firmware) - 1));
    DPRINT1("WLAN firmware %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            Firmware,
            Caps->PermanentAddress.Address[0], Caps->PermanentAddress.Address[1],
            Caps->PermanentAddress.Address[2], Caps->PermanentAddress.Address[3],
            Caps->PermanentAddress.Address[4], Caps->PermanentAddress.Address[5]);

    if (!WdiTlvFind(Tlvs, Length, WDI_TLV_STATION_ATTRIBUTES, &Value, &ValueLength))
    {
        DPRINT1("No station attributes\n");
        return NDIS_STATUS_INVALID_DATA;
    }

    /* The data path description is optional */
    if (WdiTlvFind(Tlvs, Length, WDI_TLV_DATAPATH_ATTRIBUTES, &Attributes, &AttributesLength) &&
        WdiTlvFind(Attributes, AttributesLength, WDI_TLV_DATAPATH_CAPABILITIES, &Value, &ValueLength))
    {
        if (ValueLength != WDI_DATAPATH_CAPS_LENGTH)
        {
            DPRINT1("Data path capabilities are %u bytes\n", ValueLength);
            return NDIS_STATUS_INVALID_DATA;
        }

        Caps->HasDataPath = TRUE;
        Caps->InterconnectType = WdiRead32(Value + 0);
        Caps->MaxNumPeers = Value[4];
        Caps->TxTargetPriorityQueueing = Value[5];
        Caps->TxMaxScatterGatherElements = WdiRead16(Value + 6);
        Caps->TxExplicitSendComplete = Value[8];
        Caps->TxMinEffectiveFrameSize = WdiRead16(Value + 9);
        Caps->TxFrameSizeGranularity = WdiRead16(Value + 11);
        Caps->RxTxForwarding = Value[13];
        Caps->RxMaxThroughput = WdiRead32(Value + 14);
    }

    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Builds the TLVs of WDI_SET_ADAPTER_CONFIGURATION, or measures them.
 *
 * @param[in] Adapter
 * The adapter, for its registry knobs and WDI version.
 *
 * @param[in] ConfiguredAddress
 * A locally administered address from the registry, if one is set.
 *
 * @param[out] Buffer
 * Where the TLVs go, or NULL to measure.
 *
 * @return
 * The length of the TLVs.
 */
_Use_decl_annotations_
ULONG
NTAPI
WdiBuildAdapterConfiguration(
    PWDI_ADAPTER Adapter,
    PCWDI_MAC_ADDRESS ConfiguredAddress,
    PUCHAR Buffer)
{
    /* Signal bar 0 below link quality 50, bar 1 from there up */
    static const UCHAR LinkQualityBars[2][WDI_LINK_QUALITY_ENTRY_LENGTH] =
    {
        { 0, 49, 0 },
        { 50, 100, 1 }
    };
    UINT32 Threshold = Adapter->UnreachableThreshold;
    UINT32 NloScanMode = WDI_SCAN_TYPE_AUTO;
    UCHAR PldrSupport = FALSE;
    ULONG Length = 0;

    if (ConfiguredAddress != NULL)
    {
        WdiTlvAppend(Buffer, &Length, WDI_TLV_CONFIGURED_MAC_ADDRESS,
                     ConfiguredAddress->Address, sizeof(ConfiguredAddress->Address));
    }

    WdiTlvAppend(Buffer, &Length, WDI_TLV_UNREACHABLE_DETECTION_THRESHOLD, &Threshold, sizeof(Threshold));
    WdiTlvAppend(Buffer, &Length, WDI_TLV_LINK_QUALITY_BAR_MAP, LinkQualityBars, sizeof(LinkQualityBars));
    WdiTlvAppend(Buffer, &Length, WDI_TLV_ADAPTER_NLO_SCAN_MODE, &NloScanMode, sizeof(NloScanMode));

    if (Adapter->PeerVersion >= WDI_VERSION_1_0_10)
        WdiTlvAppend(Buffer, &Length, WDI_TLV_PLDR_SUPPORT, &PldrSupport, sizeof(PldrSupport));

    return Length;
}

/**
 * @brief
 * Builds the TLVs of WDI_TASK_CREATE_PORT, or measures them.
 *
 * @return
 * The length of the TLVs.
 */
_Use_decl_annotations_
ULONG
NTAPI
WdiBuildCreatePort(
    UINT16 OpModeMask,
    NDIS_PORT_NUMBER NdisPortNumber,
    PUCHAR Buffer)
{
    UCHAR Parameters[WDI_CREATE_PORT_PARAMETERS_LENGTH];

    Parameters[0] = (UCHAR)OpModeMask;
    Parameters[1] = (UCHAR)(OpModeMask >> 8);
    Parameters[2] = (UCHAR)NdisPortNumber;
    Parameters[3] = (UCHAR)(NdisPortNumber >> 8);
    Parameters[4] = (UCHAR)(NdisPortNumber >> 16);
    Parameters[5] = (UCHAR)(NdisPortNumber >> 24);

    return WdiTlvPut(Buffer, WDI_TLV_CREATE_PORT_PARAMETERS, Parameters, sizeof(Parameters));
}

/**
 * @brief
 * Reads the port a WDI_INDICATION_CREATE_PORT_COMPLETE describes.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or NDIS_STATUS_INVALID_DATA without port attributes.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiParsePortAttributes(
    const UCHAR *Tlvs,
    ULONG Length,
    PWDI_MAC_ADDRESS Address,
    WDI_PORT_ID *PortId)
{
    const UCHAR *Value;
    USHORT ValueLength;

    if (!WdiTlvFind(Tlvs, Length, WDI_TLV_PORT_ATTRIBUTES, &Value, &ValueLength) ||
        ValueLength != WDI_PORT_ATTRIBUTES_LENGTH)
    {
        return NDIS_STATUS_INVALID_DATA;
    }

    RtlCopyMemory(Address->Address, Value, sizeof(Address->Address));
    *PortId = WdiRead16(Value + 6);
    return NDIS_STATUS_SUCCESS;
}
