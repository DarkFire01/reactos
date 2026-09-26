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
#include <bcrypt.h>

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

/* dot11 and generic OIDs the handshake drives the adapter with */
#define OID_GEN_CURRENT_PACKET_FILTER               0x0001010E
#define OID_GEN_MEDIA_CONNECT_STATUS                0x00010114
#define OID_802_3_CURRENT_ADDRESS                   0x01010102

#define OID_DOT11_NDIS_START                        0x0D010300
#define OID_DOT11_CURRENT_OPERATION_MODE            (OID_DOT11_NDIS_START + 8)
#define OID_DOT11_OP(Seq)                           ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | (Seq))
#define OID_DOT11_DESIRED_SSID_LIST                 OID_DOT11_OP(0x7C)
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

/* dot11 values equal the WDI ones, so these carry straight down */
#define WLAN_AUTH_RSNA_PSK                          7
#define WLAN_CIPHER_NONE                            0
#define WLAN_CIPHER_TKIP                            2
#define WLAN_CIPHER_CCMP                            4

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
#define KEY_INFO_MIC                                0x0100
#define KEY_INFO_ACK                                0x0080
#define KEY_INFO_ENCRYPTED                          0x1000

/* Timeouts */
#define HANDSHAKE_STEP_TIMEOUT_MS                   3000
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

#include <pshpack1.h>
typedef struct _WLAN_KEY_MAPPING_KEY
{
    UCHAR PeerMacAddr[6];
    ULONG AlgorithmId;
    ULONG Direction;
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[32];
} WLAN_KEY_MAPPING_KEY;

typedef struct _WLAN_DEFAULT_KEY
{
    NDIS_OBJECT_HEADER Header;
    ULONG uKeyIndex;
    ULONG AlgorithmId;
    UCHAR MacAddr[6];
    BOOLEAN bDelete;
    BOOLEAN bStatic;
    USHORT usKeyLength;
    UCHAR ucKey[32];
} WLAN_DEFAULT_KEY;
#include <poppack.h>

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

/* Crypto over bcrypt */

static
BOOL
DerivePmk(
    _In_reads_bytes_(PassphraseLength) const UCHAR *Passphrase,
    _In_ ULONG PassphraseLength,
    _In_reads_bytes_(SsidLength) const UCHAR *Ssid,
    _In_ ULONG SsidLength,
    _Out_writes_bytes_(WLAN_PMK_LENGTH) PUCHAR Pmk)
{
    BCRYPT_ALG_HANDLE Algorithm;
    NTSTATUS Status;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA1_ALGORITHM,
                                                    NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return FALSE;

    Status = BCryptDeriveKeyPBKDF2(Algorithm, (PUCHAR)Passphrase, PassphraseLength,
                                   (PUCHAR)Ssid, SsidLength, 4096, Pmk, WLAN_PMK_LENGTH, 0);

    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return BCRYPT_SUCCESS(Status);
}

static
BOOL
HmacSha1(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(20) PUCHAR Mac)
{
    BCRYPT_ALG_HANDLE Algorithm;
    BCRYPT_HASH_HANDLE Hash;
    BOOL Ok = FALSE;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA1_ALGORITHM,
                                                    NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return FALSE;

    if (BCRYPT_SUCCESS(BCryptCreateHash(Algorithm, &Hash, NULL, 0, (PUCHAR)Key, KeyLength, 0)))
    {
        if (BCRYPT_SUCCESS(BCryptHashData(Hash, (PUCHAR)Data, DataLength, 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(Hash, Mac, 20, 0)))
        {
            Ok = TRUE;
        }
        BCryptDestroyHash(Hash);
    }

    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Ok;
}

/* The 802.11 PRF built on HMAC-SHA1 */
static
BOOL
Prf(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_ PCSTR Label,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    UCHAR Block[128];
    UCHAR Digest[20];
    ULONG LabelLength = (ULONG)strlen(Label);
    ULONG Offset = 0;
    UCHAR Counter = 0;

    if (LabelLength + 1 + DataLength + 1 > sizeof(Block))
        return FALSE;

    while (Offset < OutputLength)
    {
        ULONG Take;

        RtlCopyMemory(Block, Label, LabelLength);
        Block[LabelLength] = 0;
        RtlCopyMemory(Block + LabelLength + 1, Data, DataLength);
        Block[LabelLength + 1 + DataLength] = Counter;

        if (!HmacSha1(Key, KeyLength, Block, LabelLength + 1 + DataLength + 1, Digest))
            return FALSE;

        Take = min(20, OutputLength - Offset);
        RtlCopyMemory(Output + Offset, Digest, Take);
        Offset += Take;
        Counter++;
    }

    return TRUE;
}

static
BOOL
AesEcbDecryptBlock(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_reads_bytes_(16) const UCHAR *In,
    _Out_writes_bytes_(16) PUCHAR Out)
{
    ULONG Done = 0;

    return BCRYPT_SUCCESS(BCryptDecrypt(Key, (PUCHAR)In, 16, NULL, NULL, 0, Out, 16, &Done, 0)) &&
           Done == 16;
}

/* RFC 3394 AES key unwrap, on AES-128 ECB. Output is InputLength - 8 bytes */
static
BOOL
AesKeyUnwrap(
    _In_reads_bytes_(16) const UCHAR *Kek,
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_(InputLength - 8) PUCHAR Output)
{
    BCRYPT_ALG_HANDLE Algorithm;
    BCRYPT_KEY_HANDLE Key;
    UCHAR A[8];
    UCHAR Buffer[16];
    ULONG Blocks;
    BOOL Ok = FALSE;
    LONG j;
    ULONG i;

    if (InputLength < 24 || (InputLength % 8) != 0)
        return FALSE;
    Blocks = InputLength / 8 - 1;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_AES_ALGORITHM, NULL, 0)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(Algorithm, BCRYPT_CHAINING_MODE,
                                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0)) ||
        !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(Algorithm, &Key, NULL, 0, (PUCHAR)Kek, 16, 0)))
    {
        BCryptCloseAlgorithmProvider(Algorithm, 0);
        return FALSE;
    }

    RtlCopyMemory(A, Input, 8);
    RtlCopyMemory(Output, Input + 8, InputLength - 8);

    for (j = 5; j >= 0; j--)
    {
        for (i = Blocks; i >= 1; i--)
        {
            ULONGLONG T = (ULONGLONG)Blocks * (ULONG)j + i;
            LONG b;

            RtlCopyMemory(Buffer, A, 8);
            RtlCopyMemory(Buffer + 8, Output + (i - 1) * 8, 8);

            /* A ^= T, over the trailing 8 bytes, big-endian */
            for (b = 0; b < 8; b++)
                Buffer[7 - b] ^= (UCHAR)(T >> (8 * b));

            if (!AesEcbDecryptBlock(Key, Buffer, Buffer))
                goto Done;

            RtlCopyMemory(A, Buffer, 8);
            RtlCopyMemory(Output + (i - 1) * 8, Buffer + 8, 8);
        }
    }

    /* The integrity check value RFC 3394 fixes */
    Ok = TRUE;
    for (i = 0; i < 8; i++)
    {
        if (A[i] != 0xA6)
        {
            Ok = FALSE;
            break;
        }
    }

Done:
    BCryptDestroyKey(Key);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Ok;
}

static
BOOL
GenerateNonce(
    _Out_writes_bytes_(WLAN_NONCE_LENGTH) PUCHAR Nonce)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, Nonce, WLAN_NONCE_LENGTH,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
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

/* The handshake */

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

/* Fills the STA RSN element M2 carries: CCMP pairwise, PSK, the given group.
   Its multi-byte fields are little-endian, unlike the EAPOL header */
static
ULONG
BuildRsnElement(
    _In_ ULONG GroupCipher,
    _Out_writes_bytes_(22) PUCHAR Element)
{
    static const UCHAR Oui[3] = { 0x00, 0x0F, 0xAC };
    PUCHAR At = Element;

    *At++ = 48;                             /* element id */
    *At++ = 20;                             /* length of the rest */
    *At++ = 1; *At++ = 0;                   /* version 1 */

    RtlCopyMemory(At, Oui, 3); At += 3;     /* group cipher suite */
    *At++ = (UCHAR)GroupCipher;

    *At++ = 1; *At++ = 0;                   /* one pairwise cipher */
    RtlCopyMemory(At, Oui, 3); At += 3;
    *At++ = (UCHAR)WLAN_CIPHER_CCMP;

    *At++ = 1; *At++ = 0;                   /* one AKM */
    RtlCopyMemory(At, Oui, 3); At += 3;
    *At++ = 2;                              /* PSK */

    *At++ = 0; *At++ = 0;                   /* RSN capabilities */
    return (ULONG)(At - Element);
}

/* Finds the GTK in the unwrapped key data of message 3 */
static
BOOL
FindGtk(
    _In_reads_bytes_(Length) const UCHAR *KeyData,
    _In_ ULONG Length,
    _Out_writes_bytes_to_(32, *GtkLength) PUCHAR Gtk,
    _Out_ PULONG GtkLength,
    _Out_ PULONG KeyId)
{
    static const UCHAR Rsn[3] = { 0x00, 0x0F, 0xAC };
    ULONG Offset = 0;

    *GtkLength = 0;
    *KeyId = 0;

    while (Offset + 2 <= Length)
    {
        UCHAR Type = KeyData[Offset];
        UCHAR ElementLength = KeyData[Offset + 1];

        if (Type == 0)
            break;
        if (Offset + 2 + ElementLength > Length)
            break;

        /* A KDE: 0xDD, len, OUI(3), data type(1), then the payload */
        if (Type == 0xDD && ElementLength >= 6 &&
            RtlEqualMemory(KeyData + Offset + 2, Rsn, 3) &&
            KeyData[Offset + 5] == 1)
        {
            ULONG KeyBytes = (ULONG)ElementLength - 6;

            if (KeyBytes == 0 || KeyBytes > 32)
                return FALSE;
            *KeyId = KeyData[Offset + 6] & 0x03;
            RtlCopyMemory(Gtk, KeyData + Offset + 8, KeyBytes);
            *GtkLength = KeyBytes;
            return TRUE;
        }

        Offset += 2 + ElementLength;
    }

    return FALSE;
}

static
DWORD
InstallKeys(
    _In_ HANDLE Interface,
    _In_reads_bytes_(6) const UCHAR *Bssid,
    _In_reads_bytes_(WLAN_TK_LENGTH) const UCHAR *Tk,
    _In_reads_bytes_(GtkLength) const UCHAR *Gtk,
    _In_ ULONG GtkLength,
    _In_ ULONG GtkKeyId)
{
    WLAN_KEY_MAPPING_KEY Pairwise;
    WLAN_DEFAULT_KEY Group;
    ULONG KeyId = GtkKeyId;
    DWORD Error;

    RtlZeroMemory(&Pairwise, sizeof(Pairwise));
    RtlCopyMemory(Pairwise.PeerMacAddr, Bssid, 6);
    Pairwise.AlgorithmId = WLAN_CIPHER_CCMP;
    Pairwise.Direction = DOT11_DIR_BOTH;
    Pairwise.usKeyLength = WLAN_TK_LENGTH;
    RtlCopyMemory(Pairwise.ucKey, Tk, WLAN_TK_LENGTH);

    Error = WlanSetOid(Interface, OID_DOT11_CIPHER_KEY_MAPPING_KEY, &Pairwise,
                       FIELD_OFFSET(WLAN_KEY_MAPPING_KEY, ucKey) + WLAN_TK_LENGTH);
    if (Error != ERROR_SUCCESS)
        return Error;

    RtlZeroMemory(&Group, sizeof(Group));
    Group.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Group.Header.Revision = 1;
    Group.Header.Size = sizeof(Group);
    Group.uKeyIndex = GtkKeyId;
    Group.AlgorithmId = WLAN_CIPHER_CCMP;
    RtlFillMemory(Group.MacAddr, 6, 0xFF);
    Group.usKeyLength = (USHORT)GtkLength;
    RtlCopyMemory(Group.ucKey, Gtk, GtkLength);

    Error = WlanSetOid(Interface, OID_DOT11_CIPHER_DEFAULT_KEY, &Group,
                       FIELD_OFFSET(WLAN_DEFAULT_KEY, ucKey) + GtkLength);
    if (Error != ERROR_SUCCESS)
        return Error;

    return WlanSetOid(Interface, OID_DOT11_CIPHER_DEFAULT_KEY_ID, &KeyId, sizeof(KeyId));
}

/* Runs the 4-way handshake to completion and installs the derived keys */
static
DWORD
FourWayHandshake(
    _In_ HANDLE Device,
    _In_ HANDLE Interface,
    _In_reads_bytes_(WLAN_PMK_LENGTH) const UCHAR *Pmk,
    _In_reads_bytes_(6) const UCHAR *OwnMac)
{
    UCHAR Frame[WLAN_MAX_FRAME];
    UCHAR Message2[ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + 64];
    UCHAR Message4[ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH];
    UCHAR Bssid[6];
    UCHAR ANonce[WLAN_NONCE_LENGTH];
    UCHAR SNonce[WLAN_NONCE_LENGTH];
    UCHAR Ptk[WLAN_PTK_LENGTH];
    UCHAR PtkInput[2 * 6 + 2 * WLAN_NONCE_LENGTH];
    UCHAR KeyData[WLAN_MAX_FRAME];
    UCHAR Gtk[32];
    UCHAR Mic[20];
    PUCHAR At;
    PUCHAR Eapol;
    PUCHAR Key = Ptk;             /* KCK = Ptk[0..15] */
    ULONG Length;
    ULONG RsnLength;
    ULONG KeyDataLength;
    ULONG GtkLength;
    ULONG GtkKeyId;
    USHORT KeyInfo;
    DWORD Error;

    /* Message 1: the AP sends its nonce */
    Error = SupplicantReadEapol(Device, Frame, sizeof(Frame), &Length, HANDSHAKE_STEP_TIMEOUT_MS);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (Length < ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH)
        return ERROR_INVALID_DATA;

    Eapol = Frame + ETH_HEADER_LENGTH;
    KeyInfo = ReadBe16(Eapol + 5);
    if (!(KeyInfo & KEY_INFO_ACK) || (KeyInfo & KEY_INFO_MIC))
        return ERROR_INVALID_DATA;

    RtlCopyMemory(Bssid, Frame + 6, 6);
    RtlCopyMemory(ANonce, Eapol + 17, WLAN_NONCE_LENGTH);

    if (!GenerateNonce(SNonce))
        return ERROR_GEN_FAILURE;

    /* PTK = PRF(PMK, "Pairwise key expansion", min||max MAC, min||max nonce) */
    At = PtkInput;
    AppendOrdered(&At, OwnMac, Bssid, 6);
    AppendOrdered(&At, ANonce, SNonce, WLAN_NONCE_LENGTH);
    if (!Prf(Pmk, WLAN_PMK_LENGTH, "Pairwise key expansion", PtkInput, sizeof(PtkInput),
             Ptk, WLAN_PTK_LENGTH))
        return ERROR_GEN_FAILURE;

    /* Message 2: our nonce and RSN element, signed with the KCK */
    RtlZeroMemory(Message2, sizeof(Message2));
    RtlCopyMemory(Message2 + 0, Bssid, 6);
    RtlCopyMemory(Message2 + 6, OwnMac, 6);
    WriteBe16(Message2 + 12, ETHERTYPE_EAPOL);

    Eapol = Message2 + ETH_HEADER_LENGTH;
    RsnLength = BuildRsnElement(WLAN_CIPHER_CCMP, Eapol + EAPOL_FIXED_LENGTH);
    Eapol[0] = 2;                               /* EAPOL version */
    Eapol[1] = EAPOL_TYPE_KEY;
    WriteBe16(Eapol + 2, (USHORT)(95 + RsnLength));
    Eapol[4] = EAPOL_KEY_DESCRIPTOR_RSN;
    WriteBe16(Eapol + 5, (USHORT)(0x0002 | 0x0008 | KEY_INFO_MIC));   /* ver 2, pairwise, MIC */
    WriteBe16(Eapol + 7, WLAN_TK_LENGTH);
    RtlCopyMemory(Eapol + 9, Frame + ETH_HEADER_LENGTH + 9, 8);       /* replay counter from M1 */
    RtlCopyMemory(Eapol + 17, SNonce, WLAN_NONCE_LENGTH);
    WriteBe16(Eapol + 97, (USHORT)RsnLength);

    if (!HmacSha1(Key, WLAN_KCK_LENGTH, Eapol, EAPOL_FIXED_LENGTH + RsnLength, Mic))
        return ERROR_GEN_FAILURE;
    RtlCopyMemory(Eapol + 81, Mic, WLAN_MIC_LENGTH);

    Error = SupplicantSendFrame(Device, Message2, ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + RsnLength);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Message 3: the AP's keys, signed and with the group key wrapped */
    Error = SupplicantReadEapol(Device, Frame, sizeof(Frame), &Length, HANDSHAKE_STEP_TIMEOUT_MS);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (Length < ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH)
        return ERROR_INVALID_DATA;

    Eapol = Frame + ETH_HEADER_LENGTH;
    KeyInfo = ReadBe16(Eapol + 5);
    if (!(KeyInfo & KEY_INFO_MIC) || !(KeyInfo & KEY_INFO_ACK))
        return ERROR_INVALID_DATA;

    /* Verify the MIC with the MIC field cleared */
    KeyDataLength = ReadBe16(Eapol + 97);
    if (ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH + KeyDataLength > Length ||
        KeyDataLength > sizeof(KeyData))
        return ERROR_INVALID_DATA;

    {
        UCHAR Received[WLAN_MIC_LENGTH];

        RtlCopyMemory(Received, Eapol + 81, WLAN_MIC_LENGTH);
        RtlZeroMemory(Eapol + 81, WLAN_MIC_LENGTH);
        if (!HmacSha1(Key, WLAN_KCK_LENGTH, Eapol, EAPOL_FIXED_LENGTH + KeyDataLength, Mic))
            return ERROR_GEN_FAILURE;
        if (!RtlEqualMemory(Received, Mic, WLAN_MIC_LENGTH))
            return ERROR_ACCESS_DENIED;
    }

    /* The group key rides in the encrypted key data, unwrapped with the KEK */
    if (KeyInfo & KEY_INFO_ENCRYPTED)
    {
        if (!AesKeyUnwrap(Ptk + WLAN_KCK_LENGTH, Eapol + 99, KeyDataLength, KeyData))
            return ERROR_ACCESS_DENIED;
        KeyDataLength -= 8;
    }
    else
    {
        RtlCopyMemory(KeyData, Eapol + 99, KeyDataLength);
    }

    if (!FindGtk(KeyData, KeyDataLength, Gtk, &GtkLength, &GtkKeyId))
        return ERROR_ACCESS_DENIED;

    /* Message 4: acknowledge, signed with the KCK */
    RtlZeroMemory(Message4, sizeof(Message4));
    RtlCopyMemory(Message4 + 0, Bssid, 6);
    RtlCopyMemory(Message4 + 6, OwnMac, 6);
    WriteBe16(Message4 + 12, ETHERTYPE_EAPOL);

    Eapol = Message4 + ETH_HEADER_LENGTH;
    Eapol[0] = 2;
    Eapol[1] = EAPOL_TYPE_KEY;
    WriteBe16(Eapol + 2, 95);
    Eapol[4] = EAPOL_KEY_DESCRIPTOR_RSN;
    WriteBe16(Eapol + 5, (USHORT)(0x0002 | 0x0008 | KEY_INFO_MIC | 0x0200));  /* ver2, pairwise, MIC, secure */
    RtlCopyMemory(Eapol + 9, Frame + ETH_HEADER_LENGTH + 9, 8);               /* replay counter from M3 */
    WriteBe16(Eapol + 97, 0);

    if (!HmacSha1(Key, WLAN_KCK_LENGTH, Eapol, EAPOL_FIXED_LENGTH, Mic))
        return ERROR_GEN_FAILURE;
    RtlCopyMemory(Eapol + 81, Mic, WLAN_MIC_LENGTH);

    Error = SupplicantSendFrame(Device, Message4, ETH_HEADER_LENGTH + EAPOL_FIXED_LENGTH);
    if (Error != ERROR_SUCCESS)
        return Error;

    return InstallKeys(Interface, Bssid, Ptk + WLAN_KCK_LENGTH + WLAN_KEK_LENGTH,
                       Gtk, GtkLength, GtkKeyId);
}

/**
 * @brief
 * Connects to a WPA2-PSK network: associates open, runs the host handshake and
 * installs the keys.
 */
DWORD
WlanConnectWpa(
    _In_ const GUID *InterfaceGuid,
    _In_ PDOT11_SSID Ssid,
    _In_ PCWSTR Passphrase)
{
    HANDLE Device;
    HANDLE Interface;
    UCHAR Pmk[WLAN_PMK_LENGTH];
    UCHAR OwnMac[6];
    UCHAR PassphraseBytes[64];
    UCHAR ListBuffer[FIELD_OFFSET(WLAN_SSID_LIST, SSIDs) + sizeof(DOT11_SSID)];
    PWLAN_SSID_LIST List = (PWLAN_SSID_LIST)ListBuffer;
    WLAN_ALGO_LIST Algo;
    ULONG PassphraseLength;
    ULONG Filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_BROADCAST;
    ULONG Mode = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    ULONG Connect = 0;
    ULONG Elapsed;
    ULONG Got;
    int Converted;
    DWORD Error;

    Converted = WideCharToMultiByte(CP_UTF8, 0, Passphrase, -1,
                                    (char *)PassphraseBytes, sizeof(PassphraseBytes), NULL, NULL);
    if (Converted <= 1)
        return ERROR_INVALID_PARAMETER;
    PassphraseLength = (ULONG)Converted - 1;

    if (!DerivePmk(PassphraseBytes, PassphraseLength, Ssid->ucSSID, Ssid->uSSIDLength, Pmk))
        return ERROR_GEN_FAILURE;

    /* EAPOL frames go through NDISUIO, the dot11 OIDs through the Native WiFi filter */
    Device = SupplicantOpen(InterfaceGuid);
    if (Device == NULL)
        return ERROR_BAD_UNIT;

    Interface = WlanOpenInterface(InterfaceGuid);
    if (Interface == NULL)
    {
        CloseHandle(Device);
        return ERROR_BAD_UNIT;
    }

    SupplicantSetOid(Device, OID_GEN_CURRENT_PACKET_FILTER, &Filter, sizeof(Filter));
    WlanSetOid(Interface, OID_DOT11_CURRENT_OPERATION_MODE, &Mode, sizeof(Mode));

    RtlZeroMemory(&Algo, sizeof(Algo));
    Algo.Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    Algo.Header.Revision = 1;
    Algo.Header.Size = sizeof(Algo);
    Algo.uNumOfEntries = 1;
    Algo.uTotalNumOfEntries = 1;

    Algo.AlgorithmIds[0] = WLAN_AUTH_RSNA_PSK;
    WlanSetOid(Interface, OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM, &Algo, sizeof(Algo));
    Algo.AlgorithmIds[0] = WLAN_CIPHER_CCMP;
    WlanSetOid(Interface, OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM, &Algo, sizeof(Algo));
    WlanSetOid(Interface, OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM, &Algo, sizeof(Algo));

    RtlZeroMemory(ListBuffer, sizeof(ListBuffer));
    List->Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    List->Header.Revision = 1;
    List->Header.Size = sizeof(ListBuffer);
    List->uNumOfEntries = 1;
    List->uTotalNumOfEntries = 1;
    List->SSIDs[0] = *Ssid;
    WlanSetOid(Interface, OID_DOT11_DESIRED_SSID_LIST, List, sizeof(ListBuffer));

    Error = WlanQueryOid(Interface, OID_802_3_CURRENT_ADDRESS, OwnMac, sizeof(OwnMac), &Got);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    Error = WlanSetOid(Interface, OID_DOT11_CONNECT_REQUEST, &Connect, sizeof(Connect));
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* Wait for the open association before the handshake */
    Error = ERROR_TIMEOUT;
    for (Elapsed = 0; Elapsed < CONNECT_TIMEOUT_MS; Elapsed += CONNECT_POLL_MS)
    {
        ULONG Status = 0;

        if (WlanQueryOid(Interface, OID_GEN_MEDIA_CONNECT_STATUS, &Status, sizeof(Status), &Got) == ERROR_SUCCESS &&
            Status == NdisMediaStateConnected)
        {
            Error = ERROR_SUCCESS;
            break;
        }
        Sleep(CONNECT_POLL_MS);
    }

    if (Error == ERROR_SUCCESS)
        Error = FourWayHandshake(Device, Interface, Pmk, OwnMac);

Cleanup:
    CloseHandle(Interface);
    CloseHandle(Device);
    return Error;
}
