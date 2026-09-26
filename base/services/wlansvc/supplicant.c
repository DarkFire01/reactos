/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The WPA2-PSK supplicant: the host runs the 4-way handshake and
 *              installs the keys the miniport encrypts with
 *
 * The native 802.11 miniport associates in an open, unencrypted state. This
 * code derives the PMK from the passphrase, drives the EAPOL 4-way handshake
 * over the adapter, and hands the derived pairwise and group keys down through
 * the dot11 key OIDs, which the driver turns into WDI_SET_ADD_CIPHER_KEYS.
 */

#include "precomp.h"

#include <string.h>
#include <winnls.h>
#include <strsafe.h>
#include <nuiouser.h>
#include <winioctl.h>
#include <drivers/nwifi/nwifictl.h>

#include "wlcrypto.h"

/* dot11 and generic OIDs the handshake drives the adapter with */
#define OID_GEN_CURRENT_PACKET_FILTER               0x0001010E
#define OID_GEN_MEDIA_CONNECT_STATUS                0x00010114
#define OID_802_3_CURRENT_ADDRESS                   0x01010102

#define OID_DOT11_NDIS_START                        0x0D010300
#define OID_DOT11_CURRENT_OPERATION_MODE            (OID_DOT11_NDIS_START + 8)
#define OID_DOT11_OP(Seq)                           ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | (Seq))
#define OID_DOT11_DESIRED_SSID_LIST                 OID_DOT11_OP(0x7C)
#define OID_DOT11_EXCLUDE_UNENCRYPTED               OID_DOT11_OP(130)
#define OID_DOT11_PRIVACY_EXEMPTION_LIST            OID_DOT11_OP(132)
#define OID_DOT11_CONNECT_REQUEST                   OID_DOT11_OP(0x81)
#define OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM  OID_DOT11_OP(133)
#define OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM  OID_DOT11_OP(135)
#define OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM OID_DOT11_OP(137)
#define OID_DOT11_CIPHER_DEFAULT_KEY_ID             OID_DOT11_OP(138)
#define OID_DOT11_CIPHER_DEFAULT_KEY                OID_DOT11_OP(139)
#define OID_DOT11_CIPHER_KEY_MAPPING_KEY            OID_DOT11_OP(140)

#define DOT11_OPERATION_MODE_EXTENSIBLE_STATION     0x00000004
#define NDIS_WLAN_OBJECT_TYPE_DEFAULT               0x80

#define NDIS_PACKET_TYPE_DIRECTED                   0x0001
#define NDIS_PACKET_TYPE_MULTICAST                  0x0002
#define NDIS_PACKET_TYPE_BROADCAST                  0x0004

#define DOT11_DIR_BOTH                              3

#define WLAN_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE  2
#define WLAN_EXEMPT_BOTH                            3

/* dot11 values equal the WDI ones, so these carry straight down */
#define WLAN_AUTH_RSNA_PSK                          7
#define WLAN_CIPHER_NONE                            0
#define WLAN_CIPHER_TKIP                            2
#define WLAN_CIPHER_CCMP                            4
#define WLAN_CIPHER_BIP                             6

/* The dot11 association completion the Native WiFi filter queues for us */
#define WLAN_STATUS_ASSOCIATION_COMPLETION          0x40030003
#define WLAN_ASSOC_STATUS_SUCCESS                   0
#define WLAN_INDICATION_MAX                         2048

/* Elements, KDEs and the AKM suites of the RSN element */
#define WLAN_IE_RSN                                 48
#define WLAN_IE_VENDOR                              221
#define WLAN_RSN_MAX_LENGTH                         (2 + 255)
#define WLAN_KDE_GTK                                1
#define WLAN_KDE_IGTK                               9
#define WLAN_AKM_PSK                                2
#define WLAN_AKM_PSK_SHA256                         6

/* The fixed fields ahead of the elements in these frame bodies */
#define WLAN_ASSOC_REQ_FIXED_LENGTH                 4
#define WLAN_REASSOC_REQ_FIXED_LENGTH               10
#define WLAN_BEACON_FIXED_LENGTH                    12

/* EAPOL and 802.11 constants */
#define ETH_HEADER_LENGTH                           14
#define ETHERTYPE_EAPOL                             0x888E
#define EAPOL_TYPE_KEY                              3
#define EAPOL_KEY_DESCRIPTOR_RSN                    2
#define EAPOL_FIXED_LENGTH                          99
#define WLAN_NONCE_LENGTH                           32
#define WLAN_KCK_LENGTH                             16
#define WLAN_KEK_LENGTH                             16
#define WLAN_TK_LENGTH                              16
#define WLAN_PTK_LENGTH                             48
#define WLAN_PMK_LENGTH                             32
#define WLAN_MIC_LENGTH                             16
#define WLAN_MAX_FRAME                              1024

/* Key info bits */
#define KEY_INFO_VERSION_MASK                       0x0007
#define KEY_INFO_PAIRWISE                           0x0008
#define KEY_INFO_ACK                                0x0080
#define KEY_INFO_MIC                                0x0100
#define KEY_INFO_SECURE                             0x0200
#define KEY_INFO_ENCRYPTED                          0x1000

/* Timeouts */
#define HANDSHAKE_STEP_TIMEOUT_MS                   3000
#define HANDSHAKE_TIMEOUT_MS                        10000
#define SESSION_POLL_MS                             1000
#define CONNECT_TIMEOUT_MS                          10000
#define CONNECT_POLL_MS                             250

typedef struct _WLAN_SSID_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_SSID SSIDs[1];
} WLAN_SSID_LIST, *PWLAN_SSID_LIST;

typedef struct _WLAN_ALGO_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    ULONG AlgorithmIds[1];
} WLAN_ALGO_LIST;

/* DOT11_PRIVACY_EXEMPTION_LIST with one entry */
typedef struct _WLAN_EXEMPTION_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    USHORT usEtherType;
    USHORT usExemptionActionType;
    USHORT usExemptionPacketType;
} WLAN_EXEMPTION_LIST;

/* A key in the layout the dot11 key values take for CCMP and BIP: the 48-bit
   receive counter, padding, the key length, then the key */
#define WLAN_KEY_ALGO_LENGTH_OFFSET                 8
#define WLAN_KEY_ALGO_KEY_OFFSET                    12
#define WLAN_KEY_ALGO_MAX                           (WLAN_KEY_ALGO_KEY_OFFSET + 32)

/* DOT11_BYTE_ARRAY around one DOT11_CIPHER_KEY_MAPPING_KEY_VALUE */
typedef struct _WLAN_KEY_MAPPING_KEY
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfBytes;
    ULONG uTotalNumOfBytes;
    UCHAR PeerMacAddr[6];
    ULONG AlgorithmId;
    ULONG Direction;
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[WLAN_KEY_ALGO_MAX];
} WLAN_KEY_MAPPING_KEY;

/* DOT11_CIPHER_DEFAULT_KEY_VALUE */
typedef struct _WLAN_DEFAULT_KEY
{
    NDIS_OBJECT_HEADER Header;
    ULONG uKeyIndex;
    ULONG AlgorithmId;
    UCHAR MacAddr[6];
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[WLAN_KEY_ALGO_MAX];
} WLAN_DEFAULT_KEY;

/* One connection's keys and the handles they are driven through */
typedef struct _WLAN_KEY_SESSION
{
    LIST_ENTRY ListEntry;
    GUID InterfaceGuid;
    HANDLE Thread;
    HANDLE StopEvent;

    HANDLE Device;          /* NDISUIO, for the EAPOL frames */
    HANDLE Interface;       /* nativewifip, for the dot11 OIDs */
    UCHAR OwnMac[6];
    UCHAR Bssid[6];

    ULONG Akm;
    ULONG GroupCipher;

    /* Ours as the association request carried it, and the AP's beacon one */
    ULONG RsnLength;
    UCHAR Rsn[WLAN_RSN_MAX_LENGTH];
    ULONG ApRsnLength;
    UCHAR ApRsn[WLAN_RSN_MAX_LENGTH];

    UCHAR Pmk[WLAN_PMK_LENGTH];
    UCHAR Ptk[WLAN_PTK_LENGTH];     /* KCK, KEK, then TK */
    BOOLEAN PairwiseInstalled;

    /* The handshake in progress, its PTK unused until message 3 checks out */
    UCHAR ANonce[WLAN_NONCE_LENGTH];
    UCHAR SNonce[WLAN_NONCE_LENGTH];
    UCHAR Tptk[WLAN_PTK_LENGTH];
    BOOLEAN TptkValid;

    /* The last signed frame taken, and the group key in use */
    BOOLEAN ReplayValid;
    UCHAR ReplayCounter[8];
    ULONG GtkKeyId;
    ULONG GtkLength;
    UCHAR Gtk[32];

    /* The IGTK in use when management frames are protected */
    ULONG IgtkKeyId;
    ULONG IgtkLength;
    UCHAR Igtk[32];
} WLAN_KEY_SESSION, *PWLAN_KEY_SESSION;

/* Big-endian helpers, since EAPOL fields are on the wire big-endian */
static
USHORT
ReadBe16(
    _In_reads_bytes_(2) const UCHAR *Buffer)
{
    return (USHORT)((Buffer[0] << 8) | Buffer[1]);
}

static
VOID
WriteBe16(
    _Out_writes_bytes_(2) PUCHAR Buffer,
    _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)(Value >> 8);
    Buffer[1] = (UCHAR)Value;
}

/* Overlapped adapter I/O on one exclusive handle */

static
HANDLE
SupplicantOpen(
    _In_ const GUID *InterfaceGuid)
{
    WCHAR Name[64];
    HANDLE Device;
    DWORD Returned;

    Device = CreateFileW(L"\\\\.\\Ndisuio", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return NULL;

    StringCchPrintfW(Name, ARRAYSIZE(Name),
                     L"\\DEVICE\\{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                     InterfaceGuid->Data1, InterfaceGuid->Data2, InterfaceGuid->Data3,
                     InterfaceGuid->Data4[0], InterfaceGuid->Data4[1], InterfaceGuid->Data4[2],
                     InterfaceGuid->Data4[3], InterfaceGuid->Data4[4], InterfaceGuid->Data4[5],
                     InterfaceGuid->Data4[6], InterfaceGuid->Data4[7]);

    /* The length must exclude the terminator to match the bound device name */
    if (!DeviceIoControl(Device, IOCTL_NDISUIO_OPEN_DEVICE,
                         Name, (DWORD)(wcslen(Name) * sizeof(WCHAR)),
                         NULL, 0, &Returned, NULL))
    {
        CloseHandle(Device);
        return NULL;
    }

    return Device;
}

/* Waits on an overlapped operation, canceling it if the timeout elapses */
static
DWORD
SupplicantWait(
    _In_ HANDLE Device,
    _Inout_ LPOVERLAPPED Overlapped,
    _In_ DWORD Timeout,
    _Out_ PDWORD Transferred)
{
    *Transferred = 0;

    if (GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Overlapped->hEvent, Timeout) != WAIT_OBJECT_0)
        {
            CancelIo(Device);
            WaitForSingleObject(Overlapped->hEvent, INFINITE);
            return ERROR_TIMEOUT;
        }
    }

    if (!GetOverlappedResult(Device, Overlapped, Transferred, FALSE))
        return GetLastError();

    return ERROR_SUCCESS;
}

static
DWORD
SupplicantIoctl(
    _In_ HANDLE Device,
    _In_ DWORD Code,
    _Inout_updates_bytes_(Size) PVOID Buffer,
    _In_ DWORD Size,
    _Out_ PDWORD Returned)
{
    OVERLAPPED Overlapped;
    DWORD Error;

    RtlZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    SetLastError(ERROR_SUCCESS);
    DeviceIoControl(Device, Code, Buffer, Size, Buffer, Size, Returned, &Overlapped);
    Error = SupplicantWait(Device, &Overlapped, INFINITE, Returned);

    CloseHandle(Overlapped.hEvent);
    return Error;
}

static
DWORD
SupplicantSetOid(
    _In_ HANDLE Device,
    _In_ NDIS_OID Oid,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    PNDISUIO_SET_OID Set;
    ULONG Size = FIELD_OFFSET(NDISUIO_SET_OID, Data) + Length;
    DWORD Returned;
    DWORD Error;

    Set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Set == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    Set->Oid = Oid;
    RtlCopyMemory(Set->Data, Data, Length);
    Error = SupplicantIoctl(Device, IOCTL_NDISUIO_SET_OID_VALUE, Set, Size, &Returned);
    HeapFree(GetProcessHeap(), 0, Set);
    return Error;
}

static
DWORD
SupplicantSendFrame(
    _In_ HANDLE Device,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length)
{
    OVERLAPPED Overlapped;
    DWORD Transferred;
    DWORD Error;

    RtlZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    SetLastError(ERROR_SUCCESS);
    WriteFile(Device, Frame, Length, &Transferred, &Overlapped);
    Error = SupplicantWait(Device, &Overlapped, HANDSHAKE_STEP_TIMEOUT_MS, &Transferred);

    CloseHandle(Overlapped.hEvent);
    return Error;
}

/* Reads the next EAPOL frame, discarding anything that is not one */
static
DWORD
SupplicantReadEapol(
    _In_ HANDLE Device,
    _Out_writes_bytes_to_(Size, *Length) PUCHAR Buffer,
    _In_ ULONG Size,
    _Out_ PULONG Length,
    _In_ DWORD Timeout)
{
    ULONG Deadline = GetTickCount() + Timeout;

    *Length = 0;
    for (;;)
    {
        OVERLAPPED Overlapped;
        DWORD Transferred;
        DWORD Error;
        DWORD Remaining;
        ULONG Now = GetTickCount();

        Remaining = (Now >= Deadline) ? 0 : (Deadline - Now);
        if (Remaining == 0)
            return ERROR_TIMEOUT;

        RtlZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (Overlapped.hEvent == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;

        SetLastError(ERROR_SUCCESS);
        ReadFile(Device, Buffer, Size, &Transferred, &Overlapped);
        Error = SupplicantWait(Device, &Overlapped, Remaining, &Transferred);
        CloseHandle(Overlapped.hEvent);

        if (Error != ERROR_SUCCESS)
            return Error;

        if (Transferred >= ETH_HEADER_LENGTH + 4 &&
            ReadBe16(Buffer + 12) == ETHERTYPE_EAPOL &&
            Buffer[ETH_HEADER_LENGTH + 1] == EAPOL_TYPE_KEY)
        {
            *Length = Transferred;
            return ERROR_SUCCESS;
        }
    }
}

/* Elements and the RSN element */

/* Finds an element in a run of them, returning it with its header */
static
const UCHAR *
FindElement(
    _In_reads_bytes_(Length) const UCHAR *Elements,
    _In_ ULONG Length,
    _In_ UCHAR Id)
{
    ULONG Offset = 0;

    while (Offset + 2 <= Length)
    {
        ULONG ElementLength = Elements[Offset + 1];

        if (Offset + 2 + ElementLength > Length)
            break;
        if (Elements[Offset] == Id)
            return Elements + Offset;

        Offset += 2 + ElementLength;
    }

    return NULL;
}

/* Reads what the supplicant needs from an RSN element: the group cipher and
   the AKM suites, as a mask of the 00-0F-AC suite types */
static
BOOL
ParseRsnElement(
    _In_reads_bytes_(Length) const UCHAR *Element,
    _In_ ULONG Length,
    _Out_ PULONG GroupCipher,
    _Out_ PULONG AkmMask)
{
    static const UCHAR Oui[3] = { 0x00, 0x0F, 0xAC };
    const UCHAR *At = Element + 2;
    const UCHAR *End;
    USHORT Count;

    *GroupCipher = WLAN_CIPHER_CCMP;
    *AkmMask = 0;

    if (Length < 2 || Element[0] != WLAN_IE_RSN || (ULONG)Element[1] + 2 > Length)
        return FALSE;
    End = At + Element[1];

    /* Version, then the group suite */
    if (End - At < 6)
        return FALSE;
    At += 2;
    if (RtlEqualMemory(At, Oui, sizeof(Oui)))
        *GroupCipher = At[3];
    At += 4;

    /* The pairwise suites, skipped */
    if (End - At < 2)
        return TRUE;
    Count = At[0] | (At[1] << 8);
    At += 2;
    if (End - At < 4 * Count)
        return FALSE;
    At += 4 * Count;

    /* The AKM suites */
    if (End - At < 2)
        return TRUE;
    Count = At[0] | (At[1] << 8);
    At += 2;
    if (End - At < 4 * Count)
        return FALSE;
    while (Count-- > 0)
    {
        if (RtlEqualMemory(At, Oui, sizeof(Oui)) && At[3] < 32)
            *AkmMask |= 1UL << At[3];
        At += 4;
    }

    return TRUE;
}

/* Builds the STA RSN element: CCMP pairwise, one AKM, the given group. Its
   multi-byte fields are little-endian, unlike the EAPOL header */
static
ULONG
BuildRsnElement(
    _In_ ULONG GroupCipher,
    _In_ ULONG Akm,
    _Out_writes_bytes_(22) PUCHAR Element)
{
    static const UCHAR Oui[3] = { 0x00, 0x0F, 0xAC };
    PUCHAR At = Element;

    *At++ = WLAN_IE_RSN;
    *At++ = 20;                             /* length of the rest */
    *At++ = 1; *At++ = 0;                   /* version 1 */

    RtlCopyMemory(At, Oui, 3); At += 3;     /* group cipher suite */
    *At++ = (UCHAR)GroupCipher;

    *At++ = 1; *At++ = 0;                   /* one pairwise cipher */
    RtlCopyMemory(At, Oui, 3); At += 3;
    *At++ = (UCHAR)WLAN_CIPHER_CCMP;

    *At++ = 1; *At++ = 0;                   /* one AKM */
    RtlCopyMemory(At, Oui, 3); At += 3;
    *At++ = (UCHAR)Akm;

    *At++ = 0; *At++ = 0;                   /* RSN capabilities */
    return (ULONG)(At - Element);
}

/* Takes the RSN elements out of an association completion: ours from the
   association request, and the AP's from its beacon or probe response */
static
VOID
TakeAssociation(
    _Inout_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    PDOT11_ASSOCIATION_COMPLETION_PARAMETERS Params = (PDOT11_ASSOCIATION_COMPLETION_PARAMETERS)Buffer;
    const UCHAR *Element;
    ULONG Fixed;
    ULONG Mask;

    if (Length < sizeof(*Params) || Params->uStatus != WLAN_ASSOC_STATUS_SUCCESS)
        return;

    RtlCopyMemory(Session->Bssid, &Params->MacAddr, sizeof(Session->Bssid));

    Fixed = Params->bReAssocReq ? WLAN_REASSOC_REQ_FIXED_LENGTH : WLAN_ASSOC_REQ_FIXED_LENGTH;
    if (Params->uAssocReqOffset <= Length &&
        Params->uAssocReqSize <= Length - Params->uAssocReqOffset &&
        Params->uAssocReqSize > Fixed)
    {
        Element = FindElement(Buffer + Params->uAssocReqOffset + Fixed,
                              Params->uAssocReqSize - Fixed, WLAN_IE_RSN);
        if (Element != NULL &&
            ParseRsnElement(Element, Element[1] + 2, &Session->GroupCipher, &Mask) &&
            (Mask & ((1UL << WLAN_AKM_PSK_SHA256) | (1UL << WLAN_AKM_PSK))))
        {
            /* The association request names exactly one AKM */
            Session->Akm = (Mask & (1UL << WLAN_AKM_PSK_SHA256)) ? WLAN_AKM_PSK_SHA256 : WLAN_AKM_PSK;
            Session->RsnLength = Element[1] + 2;
            RtlCopyMemory(Session->Rsn, Element, Session->RsnLength);
        }
    }

    if (Params->uBeaconOffset <= Length &&
        Params->uBeaconSize <= Length - Params->uBeaconOffset &&
        Params->uBeaconSize > WLAN_BEACON_FIXED_LENGTH)
    {
        Element = FindElement(Buffer + Params->uBeaconOffset + WLAN_BEACON_FIXED_LENGTH,
                              Params->uBeaconSize - WLAN_BEACON_FIXED_LENGTH, WLAN_IE_RSN);
        if (Element != NULL)
        {
            Session->ApRsnLength = Element[1] + 2;
            RtlCopyMemory(Session->ApRsn, Element, Session->ApRsnLength);

            /* Our own element was not reported, so the group cipher comes from the AP */
            if (Session->RsnLength == 0)
                ParseRsnElement(Element, Session->ApRsnLength, &Session->GroupCipher, &Mask);
        }
    }
}

/* Hands back the oldest queued dot11 indication of the adapter, if any */
static
BOOL
NextIndication(
    _In_ HANDLE Interface,
    _Out_writes_bytes_(Size) PNWIFI_INDICATION Indication,
    _In_ ULONG Size)
{
    DWORD Got;

    return DeviceIoControl(Interface, IOCTL_NWIFI_GET_INDICATION, NULL, 0, Indication, Size, &Got, NULL) &&
           Got >= FIELD_OFFSET(NWIFI_INDICATION, Data) &&
           Indication->Length <= Got - FIELD_OFFSET(NWIFI_INDICATION, Data);
}

/* Waits for the open association, keeping what its completion reports */
static
DWORD
WaitForAssociation(
    _Inout_ PWLAN_KEY_SESSION Session)
{
    UCHAR Buffer[FIELD_OFFSET(NWIFI_INDICATION, Data) + WLAN_INDICATION_MAX];
    PNWIFI_INDICATION Indication = (PNWIFI_INDICATION)Buffer;
    ULONG Elapsed;
    ULONG Got;

    for (Elapsed = 0; Elapsed < CONNECT_TIMEOUT_MS; Elapsed += CONNECT_POLL_MS)
    {
        ULONG Status = 0;

        while (NextIndication(Session->Interface, Indication, sizeof(Buffer)))
        {
            if (Indication->StatusCode == WLAN_STATUS_ASSOCIATION_COMPLETION)
                TakeAssociation(Session, Indication->Data, Indication->Length);
        }

        if (WlanQueryOid(Session->Interface, OID_GEN_MEDIA_CONNECT_STATUS,
                         &Status, sizeof(Status), &Got) == ERROR_SUCCESS &&
            Status == NdisMediaStateConnected)
        {
            return ERROR_SUCCESS;
        }

        Sleep(CONNECT_POLL_MS);
    }

    return ERROR_TIMEOUT;
}

/* The key hierarchy the AKM selects */

/* The key descriptor version: HMAC-SHA1 for PSK, AES-CMAC for PSK-SHA256 */
static
USHORT
KeyVersion(
    _In_ PWLAN_KEY_SESSION Session)
{
    return (Session->Akm == WLAN_AKM_PSK_SHA256) ? 3 : 2;
}

static
BOOL
ComputeMic(
    _In_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(WLAN_KCK_LENGTH) const UCHAR *Kck,
    _In_reads_bytes_(Length) const UCHAR *Eapol,
    _In_ ULONG Length,
    _Out_writes_bytes_(WLAN_MIC_LENGTH) PUCHAR Mic)
{
    UCHAR Digest[WLAN_SHA1_LENGTH];

    if (Session->Akm == WLAN_AKM_PSK_SHA256)
        return WlanCryptoAesCmac(Kck, Eapol, Length, Mic);

    WlanCryptoHmacSha1(Kck, WLAN_KCK_LENGTH, Eapol, Length, Digest);
    RtlCopyMemory(Mic, Digest, WLAN_MIC_LENGTH);
    return TRUE;
}

/* Checks the MIC of a received EAPOL-Key frame, which is computed with the
   MIC field cleared */
static
BOOL
CheckMic(
    _In_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(WLAN_KCK_LENGTH) const UCHAR *Kck,
    _Inout_updates_bytes_(Length) PUCHAR Eapol,
    _In_ ULONG Length)
{
    UCHAR Received[WLAN_MIC_LENGTH];
    UCHAR Mic[WLAN_MIC_LENGTH];

    RtlCopyMemory(Received, Eapol + 81, WLAN_MIC_LENGTH);
    RtlZeroMemory(Eapol + 81, WLAN_MIC_LENGTH);
    if (!ComputeMic(Session, Kck, Eapol, Length, Mic))
        return FALSE;

    return RtlEqualMemory(Received, Mic, WLAN_MIC_LENGTH);
}

/* min||max of two equal length buffers, as the PTK derivation orders them */
static
VOID
AppendOrdered(
    _Inout_ PUCHAR *At,
    _In_reads_bytes_(Length) const UCHAR *A,
    _In_reads_bytes_(Length) const UCHAR *B,
    _In_ ULONG Length)
{
    int Order = memcmp(A, B, Length);
    const UCHAR *Low = (Order <= 0) ? A : B;
    const UCHAR *High = (Order <= 0) ? B : A;

    RtlCopyMemory(*At, Low, Length);
    *At += Length;
    RtlCopyMemory(*At, High, Length);
    *At += Length;
}

/* PTK = PRF or KDF(PMK, "Pairwise key expansion", min||max MAC, min||max nonce),
   derived into the temporary PTK until message 3 proves it */
static
BOOL
DerivePtk(
    _Inout_ PWLAN_KEY_SESSION Session)
{
    UCHAR Input[2 * 6 + 2 * WLAN_NONCE_LENGTH];
    PUCHAR At = Input;

    AppendOrdered(&At, Session->OwnMac, Session->Bssid, 6);
    AppendOrdered(&At, Session->ANonce, Session->SNonce, WLAN_NONCE_LENGTH);

    if (Session->Akm == WLAN_AKM_PSK_SHA256)
    {
        return WlanCryptoKdfSha256(Session->Pmk, WLAN_PMK_LENGTH, "Pairwise key expansion",
                                   Input, sizeof(Input), Session->Tptk, WLAN_PTK_LENGTH);
    }

    return WlanCryptoPrfSha1(Session->Pmk, WLAN_PMK_LENGTH, "Pairwise key expansion",
                             Input, sizeof(Input), Session->Tptk, WLAN_PTK_LENGTH);
}

/* Key data */

/* Finds a KDE in unwrapped key data: 0xDD, length, 00-0F-AC, type, payload */
static
BOOL
FindKde(
    _In_reads_bytes_(Length) const UCHAR *KeyData,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Outptr_result_bytebuffer_(*PayloadLength) const UCHAR **Payload,
    _Out_ PULONG PayloadLength)
{
    static const UCHAR Oui[3] = { 0x00, 0x0F, 0xAC };
    ULONG Offset = 0;

    *Payload = NULL;
    *PayloadLength = 0;

    while (Offset + 2 <= Length)
    {
        ULONG ElementLength = KeyData[Offset + 1];

        /* Padding starts with 0xDD and a zero length */
        if (KeyData[Offset] == WLAN_IE_VENDOR && ElementLength == 0)
            break;
        if (Offset + 2 + ElementLength > Length)
            break;

        if (KeyData[Offset] == WLAN_IE_VENDOR && ElementLength >= 4 &&
            RtlEqualMemory(KeyData + Offset + 2, Oui, sizeof(Oui)) &&
            KeyData[Offset + 5] == Type)
        {
            *Payload = KeyData + Offset + 6;
            *PayloadLength = ElementLength - 4;
            return TRUE;
        }

        Offset += 2 + ElementLength;
    }

    return FALSE;
}

/* The GTK KDE: key id and Tx bits, a reserved byte, then the key */
static
BOOL
TakeGtk(
    _In_reads_bytes_(Length) const UCHAR *KeyData,
    _In_ ULONG Length,
    _Out_writes_bytes_to_(32, *GtkLength) PUCHAR Gtk,
    _Out_ PULONG GtkLength,
    _Out_ PULONG KeyId)
{
    const UCHAR *Payload;
    ULONG PayloadLength;

    *GtkLength = 0;
    *KeyId = 0;

    if (!FindKde(KeyData, Length, WLAN_KDE_GTK, &Payload, &PayloadLength) ||
        PayloadLength <= 2 || PayloadLength - 2 > 32)
    {
        return FALSE;
    }

    *KeyId = Payload[0] & 0x03;
    *GtkLength = PayloadLength - 2;
    RtlCopyMemory(Gtk, Payload + 2, *GtkLength);
    return TRUE;
}

/* The IGTK KDE, there when management frames are protected: the key id (4
   or 5), the 48-bit IPN, then the key */
static
BOOL
TakeIgtk(
    _In_reads_bytes_(Length) const UCHAR *KeyData,
    _In_ ULONG Length,
    _Out_writes_bytes_(6) PUCHAR Ipn,
    _Out_writes_bytes_to_(32, *IgtkLength) PUCHAR Igtk,
    _Out_ PULONG IgtkLength,
    _Out_ PULONG KeyId)
{
    const UCHAR *Payload;
    ULONG PayloadLength;

    *IgtkLength = 0;
    *KeyId = 0;

    if (!FindKde(KeyData, Length, WLAN_KDE_IGTK, &Payload, &PayloadLength) ||
        PayloadLength <= 8 || PayloadLength - 8 > 32)
    {
        return FALSE;
    }

    *KeyId = Payload[0] | (Payload[1] << 8);
    if (*KeyId != 4 && *KeyId != 5)
        return FALSE;

    RtlCopyMemory(Ipn, Payload + 2, 6);
    *IgtkLength = PayloadLength - 8;
    RtlCopyMemory(Igtk, Payload + 8, *IgtkLength);
    return TRUE;
}

/* Unwraps or copies the key data of a received EAPOL-Key frame */
static
BOOL
TakeKeyData(
    _In_reads_bytes_(WLAN_KEK_LENGTH) const UCHAR *Kek,
    _In_reads_bytes_(Length) const UCHAR *Eapol,
    _In_ ULONG Length,
    _Out_writes_bytes_to_(Size, *KeyDataLength) PUCHAR KeyData,
    _In_ ULONG Size,
    _Out_ PULONG KeyDataLength)
{
    ULONG Wrapped = ReadBe16(Eapol + 97);

    *KeyDataLength = 0;
    if (EAPOL_FIXED_LENGTH + Wrapped > Length || Wrapped > Size)
        return FALSE;

    if (!(ReadBe16(Eapol + 5) & KEY_INFO_ENCRYPTED))
    {
        RtlCopyMemory(KeyData, Eapol + EAPOL_FIXED_LENGTH, Wrapped);
        *KeyDataLength = Wrapped;
        return TRUE;
    }

    return WlanCryptoAesUnwrap(Kek, WLAN_KEK_LENGTH, Eapol + EAPOL_FIXED_LENGTH, Wrapped,
                               KeyData, Size, KeyDataLength);
}

/* Lays a key out the way the CCMP and BIP key values carry it */
static
USHORT
PackKey(
    _Out_writes_bytes_(WLAN_KEY_ALGO_MAX) PUCHAR Value,
    _In_reads_bytes_opt_(6) const UCHAR *Rsc,
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength)
{
    RtlZeroMemory(Value, WLAN_KEY_ALGO_KEY_OFFSET);
    if (Rsc != NULL)
        RtlCopyMemory(Value, Rsc, 6);

    Value[WLAN_KEY_ALGO_LENGTH_OFFSET + 0] = (UCHAR)KeyLength;
    Value[WLAN_KEY_ALGO_LENGTH_OFFSET + 1] = (UCHAR)(KeyLength >> 8);
    RtlCopyMemory(Value + WLAN_KEY_ALGO_KEY_OFFSET, Key, KeyLength);

    return (USHORT)(WLAN_KEY_ALGO_KEY_OFFSET + KeyLength);
}

static
DWORD
InstallPairwiseKey(
    _In_ PWLAN_KEY_SESSION Session)
{
    WLAN_KEY_MAPPING_KEY Pairwise;
    DWORD Error;

    RtlZeroMemory(&Pairwise, sizeof(Pairwise));
    Pairwise.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Pairwise.Header.Revision = 1;
    Pairwise.Header.Size = sizeof(Pairwise);
    RtlCopyMemory(Pairwise.PeerMacAddr, Session->Bssid, 6);
    Pairwise.AlgorithmId = WLAN_CIPHER_CCMP;
    Pairwise.Direction = DOT11_DIR_BOTH;
    Pairwise.usKeyLength = PackKey(Pairwise.ucKey, NULL,
                                   Session->Ptk + WLAN_KCK_LENGTH + WLAN_KEK_LENGTH, WLAN_TK_LENGTH);
    Pairwise.uNumOfBytes = FIELD_OFFSET(WLAN_KEY_MAPPING_KEY, ucKey) -
                           FIELD_OFFSET(WLAN_KEY_MAPPING_KEY, PeerMacAddr) + Pairwise.usKeyLength;
    Pairwise.uTotalNumOfBytes = Pairwise.uNumOfBytes;

    Error = WlanSetOid(Session->Interface, OID_DOT11_CIPHER_KEY_MAPPING_KEY, &Pairwise,
                       FIELD_OFFSET(WLAN_KEY_MAPPING_KEY, ucKey) + Pairwise.usKeyLength);

    WlanCryptoWipe(&Pairwise, sizeof(Pairwise));
    return Error;
}

/* Installs a group key, unless it is the one already in use, so a replayed
   message cannot reset the receive counters that go with it */
static
DWORD
InstallGroupKey(
    _Inout_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(6) const UCHAR *Rsc,
    _In_reads_bytes_(GtkLength) const UCHAR *Gtk,
    _In_ ULONG GtkLength,
    _In_ ULONG GtkKeyId)
{
    WLAN_DEFAULT_KEY Group;
    ULONG KeyId = GtkKeyId;
    DWORD Error;

    if (Session->GtkLength == GtkLength && Session->GtkKeyId == GtkKeyId &&
        RtlEqualMemory(Session->Gtk, Gtk, GtkLength))
    {
        return ERROR_SUCCESS;
    }

    RtlZeroMemory(&Group, sizeof(Group));
    Group.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Group.Header.Revision = 1;
    Group.Header.Size = sizeof(Group);
    Group.uKeyIndex = GtkKeyId;
    Group.AlgorithmId = Session->GroupCipher;
    RtlFillMemory(Group.MacAddr, 6, 0xFF);
    Group.usKeyLength = PackKey(Group.ucKey, Rsc, Gtk, GtkLength);

    Error = WlanSetOid(Session->Interface, OID_DOT11_CIPHER_DEFAULT_KEY, &Group,
                       FIELD_OFFSET(WLAN_DEFAULT_KEY, ucKey) + Group.usKeyLength);
    WlanCryptoWipe(&Group, sizeof(Group));
    if (Error == ERROR_SUCCESS)
        Error = WlanSetOid(Session->Interface, OID_DOT11_CIPHER_DEFAULT_KEY_ID, &KeyId, sizeof(KeyId));
    if (Error != ERROR_SUCCESS)
        return Error;

    RtlCopyMemory(Session->Gtk, Gtk, GtkLength);
    Session->GtkLength = GtkLength;
    Session->GtkKeyId = GtkKeyId;
    return ERROR_SUCCESS;
}

/* Installs the IGTK as a BIP default key, again only when it is a new one */
static
DWORD
InstallIgtk(
    _Inout_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(6) const UCHAR *Ipn,
    _In_reads_bytes_(IgtkLength) const UCHAR *Igtk,
    _In_ ULONG IgtkLength,
    _In_ ULONG IgtkKeyId)
{
    WLAN_DEFAULT_KEY Group;
    DWORD Error;

    if (Session->IgtkLength == IgtkLength && Session->IgtkKeyId == IgtkKeyId &&
        RtlEqualMemory(Session->Igtk, Igtk, IgtkLength))
    {
        return ERROR_SUCCESS;
    }

    RtlZeroMemory(&Group, sizeof(Group));
    Group.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Group.Header.Revision = 1;
    Group.Header.Size = sizeof(Group);
    Group.uKeyIndex = IgtkKeyId;
    Group.AlgorithmId = WLAN_CIPHER_BIP;
    RtlFillMemory(Group.MacAddr, 6, 0xFF);
    Group.usKeyLength = PackKey(Group.ucKey, Ipn, Igtk, IgtkLength);

    Error = WlanSetOid(Session->Interface, OID_DOT11_CIPHER_DEFAULT_KEY, &Group,
                       FIELD_OFFSET(WLAN_DEFAULT_KEY, ucKey) + Group.usKeyLength);
    WlanCryptoWipe(&Group, sizeof(Group));
    if (Error != ERROR_SUCCESS)
        return Error;

    RtlCopyMemory(Session->Igtk, Igtk, IgtkLength);
    Session->IgtkLength = IgtkLength;
    Session->IgtkKeyId = IgtkKeyId;
    return ERROR_SUCCESS;
}

/* The exchanges */

/* Fills the Ethernet and EAPOL-Key headers of a frame to the AP */
static
VOID
BuildKeyFrame(
    _In_ PWLAN_KEY_SESSION Session,
    _Out_writes_bytes_(ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + KeyDataLength) PUCHAR Frame,
    _In_ USHORT KeyInfo,
    _In_reads_bytes_(8) const UCHAR *ReplayCounter,
    _In_reads_bytes_opt_(WLAN_NONCE_LENGTH) const UCHAR *Nonce,
    _In_reads_bytes_opt_(KeyDataLength) const UCHAR *KeyData,
    _In_ ULONG KeyDataLength)
{
    PUCHAR Eapol = Frame + ETH_HEADER_LENGTH;

    RtlZeroMemory(Frame, ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH);
    RtlCopyMemory(Frame + 0, Session->Bssid, 6);
    RtlCopyMemory(Frame + 6, Session->OwnMac, 6);
    WriteBe16(Frame + 12, ETHERTYPE_EAPOL);

    Eapol[0] = 2;                                           /* EAPOL version */
    Eapol[1] = EAPOL_TYPE_KEY;
    WriteBe16(Eapol + 2, (USHORT)(EAPOL_FIXED_LENGTH - 4 + KeyDataLength));
    Eapol[4] = EAPOL_KEY_DESCRIPTOR_RSN;
    WriteBe16(Eapol + 5, (USHORT)(KeyInfo | KeyVersion(Session)));
    RtlCopyMemory(Eapol + 9, ReplayCounter, 8);
    if (Nonce != NULL)
        RtlCopyMemory(Eapol + 17, Nonce, WLAN_NONCE_LENGTH);
    WriteBe16(Eapol + 97, (USHORT)KeyDataLength);
    if (KeyDataLength != 0)
        RtlCopyMemory(Eapol + EAPOL_FIXED_LENGTH, KeyData, KeyDataLength);
}

/* Signs a built frame with the given KCK and sends it */
static
DWORD
SendKeyFrame(
    _In_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(WLAN_KCK_LENGTH) const UCHAR *Kck,
    _Inout_updates_bytes_(ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + KeyDataLength) PUCHAR Frame,
    _In_ ULONG KeyDataLength)
{
    PUCHAR Eapol = Frame + ETH_HEADER_LENGTH;

    if (!ComputeMic(Session, Kck, Eapol, EAPOL_FIXED_LENGTH + KeyDataLength, Eapol + 81))
        return ERROR_GEN_FAILURE;

    return SupplicantSendFrame(Session->Device, Frame, ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + KeyDataLength);
}

/* Message 1 of the 4-way handshake: the AP's nonce. Answered with message 2 */
static
DWORD
PairwiseMessage1(
    _Inout_ PWLAN_KEY_SESSION Session,
    _In_reads_bytes_(Length) const UCHAR *Eapol,
    _In_ ULONG Length)
{
    UCHAR Reply[ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + WLAN_RSN_MAX_LENGTH];
    USHORT KeyInfo = ReadBe16(Eapol + 5);

    UNREFERENCED_PARAMETER(Length);

    /* Without the association request to go by, the descriptor version the AP
       chose says which of the two PSK AKMs it runs */
    if (Session->RsnLength == 0)
    {
        Session->Akm = ((KeyInfo & KEY_INFO_VERSION_MASK) == 3) ? WLAN_AKM_PSK_SHA256 : WLAN_AKM_PSK;
        Session->RsnLength = BuildRsnElement(Session->GroupCipher, Session->Akm, Session->Rsn);
    }

    if ((KeyInfo & KEY_INFO_VERSION_MASK) != KeyVersion(Session))
        return ERROR_INVALID_DATA;

    /* A retransmitted message 1 keeps our nonce, a new one starts over */
    if (!Session->TptkValid || !RtlEqualMemory(Session->ANonce, Eapol + 17, WLAN_NONCE_LENGTH))
    {
        RtlCopyMemory(Session->ANonce, Eapol + 17, WLAN_NONCE_LENGTH);
        if (!WlanCryptoRandom(Session->SNonce, sizeof(Session->SNonce)) || !DerivePtk(Session))
            return ERROR_GEN_FAILURE;
        Session->TptkValid = TRUE;
    }

    BuildKeyFrame(Session, Reply, KEY_INFO_PAIRWISE | KEY_INFO_MIC, Eapol + 9,
                  Session->SNonce, Session->Rsn, Session->RsnLength);
    return SendKeyFrame(Session, Session->Tptk, Reply, Session->RsnLength);
}

/* Message 3 of the 4-way handshake: the AP's proof and the group key.
   Answered with message 4, then the keys go in */
static
DWORD
PairwiseMessage3(
    _Inout_ PWLAN_KEY_SESSION Session,
    _Inout_updates_bytes_(Length) PUCHAR Eapol,
    _In_ ULONG Length)
{
    UCHAR Reply[ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH];
    UCHAR KeyData[WLAN_MAX_FRAME];
    UCHAR Gtk[32];
    UCHAR Igtk[32];
    UCHAR Ipn[6];
    const UCHAR *ApRsn;
    ULONG KeyDataLength;
    ULONG GtkLength;
    ULONG GtkKeyId;
    ULONG IgtkLength;
    ULONG IgtkKeyId;
    BOOL HaveIgtk;
    DWORD Error;

    if (!Session->TptkValid || !RtlEqualMemory(Eapol + 17, Session->ANonce, WLAN_NONCE_LENGTH))
        return ERROR_INVALID_DATA;

    if (!CheckMic(Session, Session->Tptk, Eapol, Length))
        return ERROR_ACCESS_DENIED;

    RtlCopyMemory(Session->ReplayCounter, Eapol + 9, 8);
    Session->ReplayValid = TRUE;

    if (!TakeKeyData(Session->Tptk + WLAN_KCK_LENGTH, Eapol, Length, KeyData, sizeof(KeyData), &KeyDataLength))
        return ERROR_ACCESS_DENIED;

    /* The AP repeats its RSN element here. It has to match the beacon, or
       something rewrote the beacon to talk us down */
    ApRsn = FindElement(KeyData, KeyDataLength, WLAN_IE_RSN);
    if (Session->ApRsnLength != 0 &&
        (ApRsn == NULL || (ULONG)ApRsn[1] + 2 != Session->ApRsnLength ||
         !RtlEqualMemory(ApRsn, Session->ApRsn, Session->ApRsnLength)))
    {
        WlanCryptoWipe(KeyData, sizeof(KeyData));
        return ERROR_ACCESS_DENIED;
    }

    if (!TakeGtk(KeyData, KeyDataLength, Gtk, &GtkLength, &GtkKeyId))
    {
        WlanCryptoWipe(KeyData, sizeof(KeyData));
        return ERROR_ACCESS_DENIED;
    }
    HaveIgtk = TakeIgtk(KeyData, KeyDataLength, Ipn, Igtk, &IgtkLength, &IgtkKeyId);
    WlanCryptoWipe(KeyData, sizeof(KeyData));

    BuildKeyFrame(Session, Reply, KEY_INFO_PAIRWISE | KEY_INFO_MIC | KEY_INFO_SECURE, Eapol + 9,
                  NULL, NULL, 0);
    Error = SendKeyFrame(Session, Session->Tptk, Reply, 0);

    /* A retransmitted message 3 is answered again, but the key it carries is
       already in and is not installed twice */
    if (Error == ERROR_SUCCESS &&
        (!Session->PairwiseInstalled || !RtlEqualMemory(Session->Ptk, Session->Tptk, WLAN_PTK_LENGTH)))
    {
        RtlCopyMemory(Session->Ptk, Session->Tptk, WLAN_PTK_LENGTH);
        Error = InstallPairwiseKey(Session);
        Session->PairwiseInstalled = (Error == ERROR_SUCCESS);
    }

    /* The Key RSC field starts the group key's receive counter */
    if (Error == ERROR_SUCCESS)
        Error = InstallGroupKey(Session, Eapol + 65, Gtk, GtkLength, GtkKeyId);
    if (Error == ERROR_SUCCESS && HaveIgtk)
        Error = InstallIgtk(Session, Ipn, Igtk, IgtkLength, IgtkKeyId);

    WlanCryptoWipe(Gtk, sizeof(Gtk));
    WlanCryptoWipe(Igtk, sizeof(Igtk));
    return Error;
}

/* Message 1 of the group key handshake: a new group key. Answered with message 2 */
static
DWORD
GroupMessage1(
    _Inout_ PWLAN_KEY_SESSION Session,
    _Inout_updates_bytes_(Length) PUCHAR Eapol,
    _In_ ULONG Length)
{
    UCHAR Reply[ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH];
    UCHAR KeyData[WLAN_MAX_FRAME];
    UCHAR Gtk[32];
    UCHAR Igtk[32];
    UCHAR Ipn[6];
    ULONG KeyDataLength;
    ULONG GtkLength;
    ULONG GtkKeyId;
    ULONG IgtkLength;
    ULONG IgtkKeyId;
    BOOL HaveIgtk;
    DWORD Error;

    if (!Session->PairwiseInstalled || !(ReadBe16(Eapol + 5) & KEY_INFO_ENCRYPTED))
        return ERROR_INVALID_DATA;

    if (!CheckMic(Session, Session->Ptk, Eapol, Length))
        return ERROR_ACCESS_DENIED;

    RtlCopyMemory(Session->ReplayCounter, Eapol + 9, 8);
    Session->ReplayValid = TRUE;

    if (!TakeKeyData(Session->Ptk + WLAN_KCK_LENGTH, Eapol, Length, KeyData, sizeof(KeyData), &KeyDataLength) ||
        !TakeGtk(KeyData, KeyDataLength, Gtk, &GtkLength, &GtkKeyId))
    {
        WlanCryptoWipe(KeyData, sizeof(KeyData));
        return ERROR_ACCESS_DENIED;
    }
    HaveIgtk = TakeIgtk(KeyData, KeyDataLength, Ipn, Igtk, &IgtkLength, &IgtkKeyId);
    WlanCryptoWipe(KeyData, sizeof(KeyData));

    BuildKeyFrame(Session, Reply, KEY_INFO_MIC | KEY_INFO_SECURE, Eapol + 9, NULL, NULL, 0);
    Error = SendKeyFrame(Session, Session->Ptk, Reply, 0);
    if (Error == ERROR_SUCCESS)
        Error = InstallGroupKey(Session, Eapol + 65, Gtk, GtkLength, GtkKeyId);
    if (Error == ERROR_SUCCESS && HaveIgtk)
        Error = InstallIgtk(Session, Ipn, Igtk, IgtkLength, IgtkKeyId);

    WlanCryptoWipe(Gtk, sizeof(Gtk));
    WlanCryptoWipe(Igtk, sizeof(Igtk));
    return Error;
}

/* Hands a received EAPOL-Key frame to the step of the exchange it belongs to */
static
DWORD
ProcessKeyFrame(
    _Inout_ PWLAN_KEY_SESSION Session,
    _Inout_updates_bytes_(Length) PUCHAR Frame,
    _In_ ULONG Length)
{
    static const UCHAR ZeroMac[6] = { 0 };
    PUCHAR Eapol = Frame + ETH_HEADER_LENGTH;
    USHORT KeyInfo;
    ULONG EapolLength;

    if (Length < ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH)
        return ERROR_INVALID_DATA;

    /* The MIC covers the key frame itself, not any padding after it */
    EapolLength = EAPOL_FIXED_LENGTH + ReadBe16(Eapol + 97);
    if (EapolLength > Length - ETH_HEADER_LENGTH || Eapol[4] != EAPOL_KEY_DESCRIPTOR_RSN)
        return ERROR_INVALID_DATA;

    /* Only the AP we associated with, which the completion may not have named */
    if (RtlEqualMemory(Session->Bssid, ZeroMac, 6))
        RtlCopyMemory(Session->Bssid, Frame + 6, 6);
    else if (!RtlEqualMemory(Frame + 6, Session->Bssid, 6))
        return ERROR_INVALID_DATA;

    /* Once a signed frame was taken, anything not newer is a replay */
    if (Session->ReplayValid && memcmp(Eapol + 9, Session->ReplayCounter, 8) <= 0)
        return ERROR_INVALID_DATA;

    KeyInfo = ReadBe16(Eapol + 5);
    if (!(KeyInfo & KEY_INFO_ACK))
        return ERROR_INVALID_DATA;

    if (KeyInfo & KEY_INFO_PAIRWISE)
    {
        if (!(KeyInfo & KEY_INFO_MIC))
            return PairwiseMessage1(Session, Eapol, EapolLength);
        return PairwiseMessage3(Session, Eapol, EapolLength);
    }

    if (KeyInfo & KEY_INFO_MIC)
        return GroupMessage1(Session, Eapol, EapolLength);

    return ERROR_INVALID_DATA;
}

/* Runs the 4-way handshake until the keys are in. Frames that fit no step are
   dropped, a failed MIC ends it */
static
DWORD
FourWayHandshake(
    _Inout_ PWLAN_KEY_SESSION Session)
{
    UCHAR Frame[WLAN_MAX_FRAME];
    ULONG Deadline = GetTickCount() + HANDSHAKE_TIMEOUT_MS;
    ULONG Length;
    DWORD Error;

    while (!Session->PairwiseInstalled || Session->GtkLength == 0)
    {
        ULONG Now = GetTickCount();

        if ((LONG)(Deadline - Now) <= 0)
            return ERROR_TIMEOUT;

        Error = SupplicantReadEapol(Session->Device, Frame, sizeof(Frame), &Length, Deadline - Now);
        if (Error != ERROR_SUCCESS)
            return Error;

        Error = ProcessKeyFrame(Session, Frame, Length);
        if (Error != ERROR_SUCCESS && Error != ERROR_INVALID_DATA)
            return Error;
    }

    return ERROR_SUCCESS;
}

/* Sessions */

static CRITICAL_SECTION SessionLock;
static LIST_ENTRY SessionList;

/* Answers the AP for as long as the connection lasts: group key updates,
   PTK rekeys and a message 3 whose message 4 got lost */
static
DWORD
WINAPI
SupplicantThread(
    _In_ LPVOID Context)
{
    PWLAN_KEY_SESSION Session = Context;
    UCHAR Frame[WLAN_MAX_FRAME];
    ULONG Length;
    DWORD Error;

    while (WaitForSingleObject(Session->StopEvent, 0) == WAIT_TIMEOUT)
    {
        Error = SupplicantReadEapol(Session->Device, Frame, sizeof(Frame), &Length, SESSION_POLL_MS);
        if (Error == ERROR_SUCCESS)
            ProcessKeyFrame(Session, Frame, Length);
        else if (Error != ERROR_TIMEOUT)
            WaitForSingleObject(Session->StopEvent, SESSION_POLL_MS);
    }

    return 0;
}

static
VOID
FreeSession(
    _In_ _Post_invalid_ PWLAN_KEY_SESSION Session)
{
    if (Session->Thread != NULL)
        CloseHandle(Session->Thread);
    if (Session->StopEvent != NULL)
        CloseHandle(Session->StopEvent);
    if (Session->Interface != NULL)
        CloseHandle(Session->Interface);
    if (Session->Device != NULL)
        CloseHandle(Session->Device);

    WlanCryptoWipe(Session, sizeof(*Session));
    HeapFree(GetProcessHeap(), 0, Session);
}

VOID
WlanSupplicantInitialize(VOID)
{
    InitializeCriticalSection(&SessionLock);
    InitializeListHead(&SessionList);
}

/**
 * @brief
 * Ends the supplicant session of an interface, if it has one, before the
 * interface disconnects or connects somewhere else.
 */
VOID
WlanStopSupplicant(
    _In_ const GUID *InterfaceGuid)
{
    PWLAN_KEY_SESSION Session = NULL;
    PLIST_ENTRY Entry;

    EnterCriticalSection(&SessionLock);
    for (Entry = SessionList.Flink; Entry != &SessionList; Entry = Entry->Flink)
    {
        PWLAN_KEY_SESSION This = CONTAINING_RECORD(Entry, WLAN_KEY_SESSION, ListEntry);

        if (IsEqualGUID(&This->InterfaceGuid, InterfaceGuid))
        {
            RemoveEntryList(&This->ListEntry);
            Session = This;
            break;
        }
    }
    LeaveCriticalSection(&SessionLock);

    if (Session == NULL)
        return;

    SetEvent(Session->StopEvent);
    WaitForSingleObject(Session->Thread, INFINITE);
    FreeSession(Session);
}

/**
 * @brief
 * Connects to a WPA2-PSK network: associates open, runs the host handshake,
 * installs the keys, and stays to answer the AP's later key updates.
 */
DWORD
WlanConnectWpa(
    _In_ const GUID *InterfaceGuid,
    _In_ PDOT11_SSID Ssid,
    _In_ PCWSTR Passphrase)
{
    UCHAR IndicationBuffer[FIELD_OFFSET(NWIFI_INDICATION, Data) + WLAN_INDICATION_MAX];
    PWLAN_KEY_SESSION Session;
    UCHAR PassphraseBytes[64];
    UCHAR ListBuffer[FIELD_OFFSET(WLAN_SSID_LIST, SSIDs) + sizeof(DOT11_SSID)];
    PWLAN_SSID_LIST List = (PWLAN_SSID_LIST)ListBuffer;
    WLAN_ALGO_LIST Algo;
    WLAN_EXEMPTION_LIST Exemptions;
    BOOLEAN Exclude = TRUE;
    ULONG PassphraseLength;
    ULONG Filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_BROADCAST;
    ULONG Mode = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    ULONG Connect = 0;
    ULONG Got;
    int Converted;
    DWORD Error;

    Converted = WideCharToMultiByte(CP_UTF8, 0, Passphrase, -1,
                                    (char *)PassphraseBytes, sizeof(PassphraseBytes), NULL, NULL);
    if (Converted <= 1)
        return ERROR_INVALID_PARAMETER;
    PassphraseLength = (ULONG)Converted - 1;

    Session = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Session));
    if (Session == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Session->InterfaceGuid = *InterfaceGuid;
    Session->GroupCipher = WLAN_CIPHER_CCMP;

    WlanCryptoInitialize();
    Error = ERROR_GEN_FAILURE;
    if (!WlanCryptoPbkdf2Sha1(PassphraseBytes, PassphraseLength, Ssid->ucSSID, Ssid->uSSIDLength,
                              4096, Session->Pmk, WLAN_PMK_LENGTH))
    {
        goto Cleanup;
    }

    /* EAPOL frames go through NDISUIO, the dot11 OIDs through the Native WiFi filter */
    Error = ERROR_BAD_UNIT;
    Session->Device = SupplicantOpen(InterfaceGuid);
    if (Session->Device == NULL)
        goto Cleanup;

    Session->Interface = WlanOpenInterface(InterfaceGuid);
    if (Session->Interface == NULL)
        goto Cleanup;

    Error = ERROR_NOT_ENOUGH_MEMORY;
    Session->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Session->StopEvent == NULL)
        goto Cleanup;

    SupplicantSetOid(Session->Device, OID_GEN_CURRENT_PACKET_FILTER, &Filter, sizeof(Filter));
    WlanSetOid(Session->Interface, OID_DOT11_CURRENT_OPERATION_MODE, &Mode, sizeof(Mode));

    RtlZeroMemory(&Algo, sizeof(Algo));
    Algo.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Algo.Header.Revision = 1;
    Algo.Header.Size = sizeof(Algo);
    Algo.uNumOfEntries = 1;
    Algo.uTotalNumOfEntries = 1;

    /* Plaintext data is dropped, except EAPOL until the pairwise key is in */
    RtlZeroMemory(&Exemptions, sizeof(Exemptions));
    Exemptions.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Exemptions.Header.Revision = 1;
    Exemptions.Header.Size = sizeof(Exemptions);
    Exemptions.uNumOfEntries = 1;
    Exemptions.uTotalNumOfEntries = 1;
    WriteBe16((PUCHAR)&Exemptions.usEtherType, ETHERTYPE_EAPOL);
    Exemptions.usExemptionActionType = WLAN_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE;
    Exemptions.usExemptionPacketType = WLAN_EXEMPT_BOTH;
    WlanSetOid(Session->Interface, OID_DOT11_PRIVACY_EXEMPTION_LIST, &Exemptions, sizeof(Exemptions));
    WlanSetOid(Session->Interface, OID_DOT11_EXCLUDE_UNENCRYPTED, &Exclude, sizeof(Exclude));

    Algo.AlgorithmIds[0] = WLAN_AUTH_RSNA_PSK;
    WlanSetOid(Session->Interface, OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM, &Algo, sizeof(Algo));
    Algo.AlgorithmIds[0] = WLAN_CIPHER_CCMP;
    WlanSetOid(Session->Interface, OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM, &Algo, sizeof(Algo));
    WlanSetOid(Session->Interface, OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM, &Algo, sizeof(Algo));

    RtlZeroMemory(ListBuffer, sizeof(ListBuffer));
    List->Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    List->Header.Revision = 1;
    List->Header.Size = sizeof(ListBuffer);
    List->uNumOfEntries = 1;
    List->uTotalNumOfEntries = 1;
    List->SSIDs[0] = *Ssid;
    WlanSetOid(Session->Interface, OID_DOT11_DESIRED_SSID_LIST, List, sizeof(ListBuffer));

    Error = WlanQueryOid(Session->Interface, OID_802_3_CURRENT_ADDRESS,
                         Session->OwnMac, sizeof(Session->OwnMac), &Got);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* Whatever an earlier connection left queued is not about this one */
    while (NextIndication(Session->Interface, (PNWIFI_INDICATION)IndicationBuffer, sizeof(IndicationBuffer)))
        continue;

    Error = WlanSetOid(Session->Interface, OID_DOT11_CONNECT_REQUEST, &Connect, sizeof(Connect));
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    Error = WaitForAssociation(Session);
    if (Error == ERROR_SUCCESS)
        Error = FourWayHandshake(Session);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    Session->Thread = CreateThread(NULL, 0, SupplicantThread, Session, 0, NULL);
    if (Session->Thread == NULL)
    {
        /* The keys are in, only later rekeys go unanswered */
        Error = ERROR_SUCCESS;
        goto Cleanup;
    }

    EnterCriticalSection(&SessionLock);
    InsertTailList(&SessionList, &Session->ListEntry);
    LeaveCriticalSection(&SessionLock);
    Session = NULL;

Cleanup:
    WlanCryptoWipe(PassphraseBytes, sizeof(PassphraseBytes));
    if (Session != NULL)
        FreeSession(Session);
    return Error;
}
