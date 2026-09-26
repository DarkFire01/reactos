#ifndef __WINDOT11_H__
#define __WINDOT11_H__

#ifndef _NTDDNDIS_
#include <ntddndis.h>
#endif
#include <wlantypes.h>

/* Enumerations */

#if defined(__midl) || defined(__WIDL__)
typedef [v1_enum] enum _DOT11_PHY_TYPE {
#else
typedef enum _DOT11_PHY_TYPE {
#endif
    dot11_phy_type_unknown,
    dot11_phy_type_any,
    dot11_phy_type_fhss,
    dot11_phy_type_dsss,
    dot11_phy_type_irbaseband,
    dot11_phy_type_ofdm,
    dot11_phy_type_hrdsss,
    dot11_phy_type_erp,
    dot11_phy_type_ht,
    dot11_phy_type_IHV_start,
    dot11_phy_type_IHV_end
} DOT11_PHY_TYPE;

typedef enum _DOT11_AUTH_ALGORITHM {
    DOT11_AUTH_ALGO_80211_OPEN         = 1,
    DOT11_AUTH_ALGO_80211_SHARED_KEY,
    DOT11_AUTH_ALGO_WPA,
    DOT11_AUTH_ALGO_WPA_PSK,
    DOT11_AUTH_ALGO_WPA_NONE,
    DOT11_AUTH_ALGO_RSNA,
    DOT11_AUTH_ALGO_RSNA_PSK,
    DOT11_AUTH_ALGO_WPA3,
#if (NTDDI_VERSION >= NTDDI_WIN10_FE)
    DOT11_AUTH_ALGO_WPA3_ENT_192       = DOT11_AUTH_ALGO_WPA3,
#endif
    DOT11_AUTH_ALGO_WPA3_SAE,
#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
    DOT11_AUTH_ALGO_OWE,
#endif
#if (NTDDI_VERSION >= NTDDI_WIN10_FE)
    DOT11_AUTH_ALGO_WPA3_ENT,
#endif
    DOT11_AUTH_ALGO_IHV_START          = 0x80000000,
    DOT11_AUTH_ALGO_IHV_END            = 0xffffffff
} DOT11_AUTH_ALGORITHM;

typedef enum _DOT11_CIPHER_ALGORITHM {
    DOT11_CIPHER_ALGO_NONE            = 0x00,
    DOT11_CIPHER_ALGO_WEP40           = 0x01,
    DOT11_CIPHER_ALGO_TKIP            = 0x02,
    DOT11_CIPHER_ALGO_CCMP            = 0x04,
    DOT11_CIPHER_ALGO_WEP104          = 0x05,
    DOT11_CIPHER_ALGO_BIP             = 0x06,
    DOT11_CIPHER_ALGO_GCMP            = 0x08,
    DOT11_CIPHER_ALGO_GCMP_256        = 0x09,
    DOT11_CIPHER_ALGO_CCMP_256        = 0x0a,
    DOT11_CIPHER_ALGO_BIP_GMAC_128    = 0x0b,
    DOT11_CIPHER_ALGO_BIP_GMAC_256    = 0x0c,
    DOT11_CIPHER_ALGO_BIP_CMAC_256    = 0x0d,
    DOT11_CIPHER_ALGO_WPA_USE_GROUP   = 0x100,
    DOT11_CIPHER_ALGO_RSN_USE_GROUP   = 0x100,
    DOT11_CIPHER_ALGO_WEP             = 0x101,
    DOT11_CIPHER_ALGO_IHV_START       = 0x80000000,
    DOT11_CIPHER_ALGO_IHV_END         = 0xffffffff
} DOT11_CIPHER_ALGORITHM;

/* Types */

#if defined(__midl) || defined(__WIDL__)
typedef struct _DOT11_MAC_ADDRESS {
    UCHAR ucDot11MacAddress[6];
} DOT11_MAC_ADDRESS, *PDOT11_MAC_ADDRESS;
#else
typedef UCHAR DOT11_MAC_ADDRESS[6];
typedef DOT11_MAC_ADDRESS* PDOT11_MAC_ADDRESS;
#endif

typedef struct _DOT11_SSID {
    ULONG uSSIDLength;
    UCHAR ucSSID[DOT11_SSID_MAX_LENGTH];
} DOT11_SSID, *PDOT11_SSID;

typedef struct _DOT11_BSSID_LIST {
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
#if defined(__midl) || defined(__WIDL__)
    [size_is(uTotalNumOfEntries)] DOT11_MAC_ADDRESS BSSIDs[*];
#else
    DOT11_MAC_ADDRESS BSSIDs[1];
#endif
} DOT11_BSSID_LIST, *PDOT11_BSSID_LIST;

#define DOT11_EXTSTA_SEND_CONTEXT_REVISION_1        1

/* Out of band data on a send in extensible station mode. */
typedef struct DOT11_EXTSTA_SEND_CONTEXT {
    NDIS_OBJECT_HEADER Header;
    USHORT usExemptionActionType;
    ULONG uPhyId;
    ULONG uDelayedSleepValue;
#if defined(__midl) || defined(__WIDL__)
    ULONG_PTR pvMediaSpecificInfo;
#else
    PVOID pvMediaSpecificInfo;
#endif
    ULONG uSendFlags;
} DOT11_EXTSTA_SEND_CONTEXT, *PDOT11_EXTSTA_SEND_CONTEXT;

#define DOT11_RECV_FLAG_RAW_PACKET                  0x00000001U
#define DOT11_RECV_FLAG_RAW_PACKET_FCS_FAILURE      0x00000002U
#define DOT11_RECV_FLAG_RAW_PACKET_TIMESTAMP        0x00000004U

#define DOT11_EXTSTA_RECV_CONTEXT_REVISION_1        1

/* Out of band data on a receive in extensible station mode. */
typedef struct DOT11_EXTSTA_RECV_CONTEXT {
    NDIS_OBJECT_HEADER Header;
    ULONG uReceiveFlags;
    ULONG uPhyId;
    ULONG uChCenterFrequency;
    USHORT usNumberOfMPDUsReceived;
    LONG lRSSI;
    UCHAR ucDataRate;
    ULONG uSizeMediaSpecificInfo;
#if defined(__midl) || defined(__WIDL__)
    ULONG_PTR pvMediaSpecificInfo;
#else
    PVOID pvMediaSpecificInfo;
#endif
    ULONGLONG ullTimestamp;
} DOT11_EXTSTA_RECV_CONTEXT, *PDOT11_EXTSTA_RECV_CONTEXT;

/* Operating mode attributes a native 802.11 miniport reports. Bodies are added as they are needed. */
typedef struct DOT11_PHY_ATTRIBUTES DOT11_PHY_ATTRIBUTES, *PDOT11_PHY_ATTRIBUTES;
typedef struct DOT11_EXTSTA_ATTRIBUTES DOT11_EXTSTA_ATTRIBUTES, *PDOT11_EXTSTA_ATTRIBUTES;
typedef struct DOT11_VWIFI_ATTRIBUTES DOT11_VWIFI_ATTRIBUTES, *PDOT11_VWIFI_ATTRIBUTES;
typedef struct _DOT11_EXTAP_ATTRIBUTES DOT11_EXTAP_ATTRIBUTES, *PDOT11_EXTAP_ATTRIBUTES;
typedef struct _DOT11_WFD_ATTRIBUTES DOT11_WFD_ATTRIBUTES, *PDOT11_WFD_ATTRIBUTES;

/* Native 802.11 request OIDs */

#define OID_DOT11_NDIS_START                        0x0D010300
#define NWF_MANDATORY_OID                           0x01U
#define NWF_OPERATIONAL_OID                         0x01U
#define NWF_DEFINE_OID(Seq, o, m) \
    ((0x0E000000U) | ((o) << 16) | ((m) << 8) | (Seq))

#define OID_DOT11_OPERATION_MODE_CAPABILITY         (OID_DOT11_NDIS_START + 7)
#define OID_DOT11_CURRENT_OPERATION_MODE            (OID_DOT11_NDIS_START + 8)
#define OID_DOT11_SCAN_REQUEST                      (OID_DOT11_NDIS_START + 11)
#define OID_DOT11_RESET_REQUEST                     (OID_DOT11_NDIS_START + 16)
#define OID_DOT11_NIC_POWER_STATE                   (OID_DOT11_NDIS_START + 17)
#define OID_DOT11_ENUM_BSS_LIST                     NWF_DEFINE_OID(0x79, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_FLUSH_BSS_LIST                    NWF_DEFINE_OID(0x7A, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_DESIRED_SSID_LIST                 NWF_DEFINE_OID(0x7C, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_DESIRED_BSSID_LIST                NWF_DEFINE_OID(0x7E, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_CONNECT_REQUEST                   NWF_DEFINE_OID(0x81, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_DISCONNECT_REQUEST                NWF_DEFINE_OID(0x8E, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_HARDWARE_PHY_STATE                NWF_DEFINE_OID(0x90, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_CURRENT_PHY_ID                    NWF_DEFINE_OID(0x92, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_EXCLUDE_UNENCRYPTED               NWF_DEFINE_OID(130, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_PRIVACY_EXEMPTION_LIST            NWF_DEFINE_OID(132, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM  NWF_DEFINE_OID(133, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM  NWF_DEFINE_OID(135, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM NWF_DEFINE_OID(137, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_CIPHER_DEFAULT_KEY_ID             NWF_DEFINE_OID(138, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_CIPHER_DEFAULT_KEY                NWF_DEFINE_OID(139, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_CIPHER_KEY_MAPPING_KEY            NWF_DEFINE_OID(140, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)
#define OID_DOT11_HIDDEN_NETWORK_ENABLED            NWF_DEFINE_OID(158, NWF_OPERATIONAL_OID, NWF_MANDATORY_OID)

/* Frames exempt from ExcludeUnencrypted, by EtherType */
#define DOT11_EXEMPT_NO_EXEMPTION                   0
#define DOT11_EXEMPT_ALWAYS                         1
#define DOT11_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE 2

#define DOT11_EXEMPT_UNICAST                        1
#define DOT11_EXEMPT_MULTICAST                      2
#define DOT11_EXEMPT_BOTH                           3

typedef struct DOT11_PRIVACY_EXEMPTION
{
    USHORT usEtherType;
    USHORT usExemptionActionType;
    USHORT usExemptionPacketType;
} DOT11_PRIVACY_EXEMPTION, *PDOT11_PRIVACY_EXEMPTION;

#define DOT11_PRIVACY_EXEMPTION_LIST_REVISION_1     1

typedef struct DOT11_PRIVACY_EXEMPTION_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_PRIVACY_EXEMPTION PrivacyExemptionEntries[1];
} DOT11_PRIVACY_EXEMPTION_LIST, *PDOT11_PRIVACY_EXEMPTION_LIST;

typedef enum _DOT11_DIRECTION
{
    DOT11_DIR_INBOUND = 1,
    DOT11_DIR_OUTBOUND = 2,
    DOT11_DIR_BOTH = 3
} DOT11_DIRECTION;

typedef struct _DOT11_AUTH_ALGORITHM_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_AUTH_ALGORITHM AlgorithmIds[1];
} DOT11_AUTH_ALGORITHM_LIST, *PDOT11_AUTH_ALGORITHM_LIST;

typedef struct _DOT11_CIPHER_ALGORITHM_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_CIPHER_ALGORITHM AlgorithmIds[1];
} DOT11_CIPHER_ALGORITHM_LIST, *PDOT11_CIPHER_ALGORITHM_LIST;

typedef struct _DOT11_CIPHER_DEFAULT_KEY_VALUE
{
    NDIS_OBJECT_HEADER Header;
    ULONG uKeyIndex;
    DOT11_CIPHER_ALGORITHM AlgorithmId;
    DOT11_MAC_ADDRESS MacAddr;
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[1];
} DOT11_CIPHER_DEFAULT_KEY_VALUE, *PDOT11_CIPHER_DEFAULT_KEY_VALUE;

typedef struct _DOT11_CIPHER_KEY_MAPPING_KEY_VALUE
{
    DOT11_MAC_ADDRESS PeerMacAddr;
    DOT11_CIPHER_ALGORITHM AlgorithmId;
    DOT11_DIRECTION Direction;
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[1];
} DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, *PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE;

/* Default keys 4 and 5 hold the IGTK when management frames are protected */
#define DOT11_MAX_NUM_DEFAULT_KEY                   4
#define DOT11_MAX_NUM_DEFAULT_KEY_MFP               (DOT11_MAX_NUM_DEFAULT_KEY + 2)

/* The ucKey layouts of the key values above, by cipher. WEP keys are raw */
typedef struct DOT11_KEY_ALGO_TKIP_MIC
{
    UCHAR ucIV48Counter[6];
    ULONG ulTKIPKeyLength;
    ULONG ulMICKeyLength;
    UCHAR ucTKIPMICKeys[1];
} DOT11_KEY_ALGO_TKIP_MIC, *PDOT11_KEY_ALGO_TKIP_MIC;

typedef struct DOT11_KEY_ALGO_CCMP
{
    UCHAR ucIV48Counter[6];
    ULONG ulCCMPKeyLength;
    UCHAR ucCCMPKey[1];
} DOT11_KEY_ALGO_CCMP, *PDOT11_KEY_ALGO_CCMP;

typedef struct DOT11_KEY_ALGO_GCMP
{
    UCHAR ucIV48Counter[6];
    ULONG ulGCMPKeyLength;
    UCHAR ucGCMPKey[1];
} DOT11_KEY_ALGO_GCMP, *PDOT11_KEY_ALGO_GCMP;

typedef struct DOT11_KEY_ALGO_BIP
{
    UCHAR ucIPN[6];
    ULONG ulBIPKeyLength;
    UCHAR ucBIPKey[1];
} DOT11_KEY_ALGO_BIP, *PDOT11_KEY_ALGO_BIP;

/* Operation modes reported and set through the operation mode OIDs */
#define DOT11_OPERATION_MODE_UNKNOWN                0x00000000
#define DOT11_OPERATION_MODE_STATION               0x00000001
#define DOT11_OPERATION_MODE_EXTENSIBLE_STATION    0x00000004

typedef struct _DOT11_CURRENT_OPERATION_MODE
{
    ULONG uReserved;
    ULONG uCurrentOpMode;
} DOT11_CURRENT_OPERATION_MODE, *PDOT11_CURRENT_OPERATION_MODE;

/* Scan */

typedef enum _DOT11_SCAN_TYPE
{
    dot11_scan_type_active = 1,
    dot11_scan_type_passive = 2,
    dot11_scan_type_auto = 3,
    dot11_scan_type_forced = 0x80000000
} DOT11_SCAN_TYPE, *PDOT11_SCAN_TYPE;

typedef struct _DOT11_SCAN_REQUEST_V2
{
    DOT11_BSS_TYPE dot11BSSType;
    DOT11_MAC_ADDRESS dot11BSSID;
    DOT11_SCAN_TYPE dot11ScanType;
    BOOLEAN bRestrictedScan;
    ULONG udot11SSIDsOffset;
    ULONG uNumOfdot11SSIDs;
    BOOLEAN bUseRequestIE;
    ULONG uRequestIDsOffset;
    ULONG uNumOfRequestIDs;
    ULONG uPhyTypeInfosOffset;
    ULONG uNumOfPhyTypeInfos;
    ULONG uIEsOffset;
    ULONG uIEsLength;
    UCHAR ucBuffer[1];
} DOT11_SCAN_REQUEST_V2, *PDOT11_SCAN_REQUEST_V2;

/* Enumerated networks */

typedef struct DOT11_BYTE_ARRAY
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfBytes;
    ULONG uTotalNumOfBytes;
    UCHAR ucBuffer[1];
} DOT11_BYTE_ARRAY, *PDOT11_BYTE_ARRAY;

#define DOT11_BSS_ENTRY_BYTE_ARRAY_REVISION_1       1

typedef union DOT11_BSS_ENTRY_PHY_SPECIFIC_INFO
{
    ULONG uChCenterFrequency;
    struct
    {
        ULONG uHopPattern;
        ULONG uHopSet;
        ULONG uDwellTime;
    } FHSS;
} DOT11_BSS_ENTRY_PHY_SPECIFIC_INFO, *PDOT11_BSS_ENTRY_PHY_SPECIFIC_INFO;

typedef struct DOT11_BSS_ENTRY
{
    ULONG uPhyId;
    DOT11_BSS_ENTRY_PHY_SPECIFIC_INFO PhySpecificInfo;
    DOT11_MAC_ADDRESS dot11BSSID;
    DOT11_BSS_TYPE dot11BSSType;
    LONG lRSSI;
    ULONG uLinkQuality;
    BOOLEAN bInRegDomain;
    USHORT usBeaconPeriod;
    ULONGLONG ullTimestamp;
    ULONGLONG ullHostTimestamp;
    USHORT usCapabilityInformation;
    ULONG uBufferLength;
    UCHAR ucBuffer[1];
} DOT11_BSS_ENTRY, *PDOT11_BSS_ENTRY;

/* Connection request parameters */

#define DOT11_SSID_LIST_REVISION_1                  1

typedef struct DOT11_SSID_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_SSID SSIDs[1];
} DOT11_SSID_LIST, *PDOT11_SSID_LIST;

/* Connection and association indications */

typedef ULONG DOT11_ASSOC_STATUS;

#define DOT11_ASSOC_STATUS_SUCCESS                  0
#define DOT11_ASSOC_STATUS_FAILURE                  0x00000001U
#define DOT11_ASSOC_STATUS_UNREACHABLE             0x00000002U
#define DOT11_ASSOC_STATUS_DISASSOCIATED_BY_OS     0x00000007U

#define DOT11_CONNECTION_STATUS_SUCCESS            DOT11_ASSOC_STATUS_SUCCESS
#define DOT11_CONNECTION_STATUS_FAILURE            DOT11_ASSOC_STATUS_FAILURE

typedef enum DOT11_DS_INFO
{
    DOT11_DS_CHANGED,
    DOT11_DS_UNCHANGED,
    DOT11_DS_UNKNOWN
} DOT11_DS_INFO, *PDOT11_DS_INFO;

#define DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1  1

typedef struct DOT11_ASSOCIATION_COMPLETION_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    DOT11_MAC_ADDRESS MacAddr;
    DOT11_ASSOC_STATUS uStatus;
    BOOLEAN bReAssocReq;
    BOOLEAN bReAssocResp;
    ULONG uAssocReqOffset, uAssocReqSize;
    ULONG uAssocRespOffset, uAssocRespSize;
    ULONG uBeaconOffset, uBeaconSize;
    ULONG uIHVDataOffset, uIHVDataSize;
    DOT11_AUTH_ALGORITHM AuthAlgo;
    DOT11_CIPHER_ALGORITHM UnicastCipher;
    DOT11_CIPHER_ALGORITHM MulticastCipher;
    ULONG uActivePhyListOffset, uActivePhyListSize;
    BOOLEAN bFourAddressSupported;
    BOOLEAN bPortAuthorized;
    UCHAR ucActiveQoSProtocol;
    DOT11_DS_INFO DSInfo;
    ULONG uEncapTableOffset, uEncapTableSize;
} DOT11_ASSOCIATION_COMPLETION_PARAMETERS, *PDOT11_ASSOCIATION_COMPLETION_PARAMETERS;

#define DOT11_CONNECTION_COMPLETION_PARAMETERS_REVISION_1  1

typedef struct DOT11_CONNECTION_COMPLETION_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    DOT11_ASSOC_STATUS uStatus;
} DOT11_CONNECTION_COMPLETION_PARAMETERS, *PDOT11_CONNECTION_COMPLETION_PARAMETERS;

#define DOT11_DISASSOCIATION_PARAMETERS_REVISION_1  1

typedef struct DOT11_DISASSOCIATION_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    DOT11_MAC_ADDRESS MacAddr;
    DOT11_ASSOC_STATUS uReason;
    ULONG uIHVDataOffset, uIHVDataSize;
} DOT11_DISASSOCIATION_PARAMETERS, *PDOT11_DISASSOCIATION_PARAMETERS;

/* DOT11_STATUS_INDICATION::uStatusType */
#define DOT11_STATUS_SUCCESS                        0x00000001
#define DOT11_STATUS_SCAN_CONFIRM                   1
#define DOT11_STATUS_JOIN_CONFIRM                   2
#define DOT11_STATUS_START_CONFIRM                  3
#define DOT11_STATUS_RESET_CONFIRM                  4

/* What a method OID_DOT11_RESET_REQUEST hands back */
typedef struct _DOT11_STATUS_INDICATION
{
    ULONG uStatusType;
    NDIS_STATUS ndisStatus;
} DOT11_STATUS_INDICATION, *PDOT11_STATUS_INDICATION;


#endif

