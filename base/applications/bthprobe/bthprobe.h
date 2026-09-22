/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared definitions for the HCI, L2CAP, SDP and HID layers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bthdef.h>
#include <bthsdpdef.h>

#define BT_ADDRESS_LENGTH                   6
#define BT_ADDRESS_STRING                   18
#define BT_MAX_LINKS                        4
#define BT_MAX_SCAN_RESULTS                 32

#define BT_CONNECT_TIMEOUT                  20000
#define BT_PAIR_TIMEOUT                     120000
#define BT_SHORT_TIMEOUT                    10000

/* Transport buffers */
#define HCI_EVENT_READ_SIZE                 257
#define HCI_EVENT_STREAM_SIZE               1024
#define HCI_ACL_READ_SIZE                   1028
#define HCI_ACL_STREAM_SIZE                 8192
#define HCI_ACL_FRAGMENT_MAX                1021
#define HCI_COMMAND_QUEUE_LENGTH            16
#define HCI_ACL_QUEUE_LENGTH                32
#define HCI_COMMAND_TIMEOUT                 5000

#define HCI_ACL_START                       0x02
#define HCI_ACL_CONTINUE                    0x01

/* Opcode groups */
#define HCI_OGF_LINK_CONTROL                0x01
#define HCI_OGF_LINK_POLICY                 0x02
#define HCI_OGF_CONTROL_BASEBAND            0x03
#define HCI_OGF_INFORMATIONAL               0x04

#define HCI_OPCODE(Ogf, Ocf)                ((USHORT)(((Ogf) << 10) | (Ocf)))

#define HCI_INQUIRY                         HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0001)
#define HCI_INQUIRY_CANCEL                  HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0002)
#define HCI_CREATE_CONNECTION               HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0005)
#define HCI_DISCONNECT                      HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0006)
#define HCI_CREATE_CONNECTION_CANCEL        HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0008)
#define HCI_ACCEPT_CONNECTION_REQUEST       HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0009)
#define HCI_REJECT_CONNECTION_REQUEST       HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x000A)
#define HCI_LINK_KEY_REPLY                  HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x000B)
#define HCI_LINK_KEY_NEGATIVE_REPLY         HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x000C)
#define HCI_PIN_CODE_REPLY                  HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x000D)
#define HCI_PIN_CODE_NEGATIVE_REPLY         HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x000E)
#define HCI_AUTHENTICATION_REQUESTED        HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0011)
#define HCI_SET_CONNECTION_ENCRYPTION       HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0013)
#define HCI_REMOTE_NAME_REQUEST             HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0019)
#define HCI_READ_REMOTE_FEATURES            HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x001B)
#define HCI_READ_REMOTE_VERSION             HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x001D)
#define HCI_REJECT_SYNC_CONNECTION          HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002A)
#define HCI_IO_CAPABILITY_REPLY             HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002B)
#define HCI_USER_CONFIRMATION_REPLY         HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002C)
#define HCI_USER_CONFIRMATION_NEGATIVE      HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002D)
#define HCI_USER_PASSKEY_REPLY              HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002E)
#define HCI_USER_PASSKEY_NEGATIVE           HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x002F)
#define HCI_IO_CAPABILITY_NEGATIVE          HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0034)

#define HCI_WRITE_DEFAULT_LINK_POLICY       HCI_OPCODE(HCI_OGF_LINK_POLICY, 0x000F)

#define HCI_SET_EVENT_MASK                  HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0001)
#define HCI_RESET                           HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0003)
#define HCI_WRITE_LOCAL_NAME                HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0013)
#define HCI_READ_LOCAL_NAME                 HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0014)
#define HCI_WRITE_SCAN_ENABLE               HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x001A)
#define HCI_WRITE_CLASS_OF_DEVICE           HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0024)
#define HCI_WRITE_INQUIRY_MODE              HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0045)
#define HCI_WRITE_SIMPLE_PAIRING_MODE       HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0056)

#define HCI_READ_LOCAL_VERSION              HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0001)
#define HCI_READ_LOCAL_FEATURES             HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0003)
#define HCI_READ_BUFFER_SIZE                HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0005)
#define HCI_READ_BD_ADDR                    HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0009)

#define HCI_EV_INQUIRY_COMPLETE             0x01
#define HCI_EV_INQUIRY_RESULT               0x02
#define HCI_EV_CONNECTION_COMPLETE          0x03
#define HCI_EV_CONNECTION_REQUEST           0x04
#define HCI_EV_DISCONNECTION_COMPLETE       0x05
#define HCI_EV_AUTHENTICATION_COMPLETE      0x06
#define HCI_EV_REMOTE_NAME_COMPLETE         0x07
#define HCI_EV_ENCRYPTION_CHANGE            0x08
#define HCI_EV_REMOTE_FEATURES_COMPLETE     0x0B
#define HCI_EV_REMOTE_VERSION_COMPLETE      0x0C
#define HCI_EV_COMMAND_COMPLETE             0x0E
#define HCI_EV_COMMAND_STATUS               0x0F
#define HCI_EV_HARDWARE_ERROR               0x10
#define HCI_EV_NUMBER_OF_COMPLETED_PACKETS  0x13
#define HCI_EV_PIN_CODE_REQUEST             0x16
#define HCI_EV_LINK_KEY_REQUEST             0x17
#define HCI_EV_LINK_KEY_NOTIFICATION        0x18
#define HCI_EV_DATA_BUFFER_OVERFLOW         0x1A
#define HCI_EV_INQUIRY_RESULT_RSSI          0x22
#define HCI_EV_EXTENDED_INQUIRY_RESULT      0x2F
#define HCI_EV_IO_CAPABILITY_REQUEST        0x31
#define HCI_EV_IO_CAPABILITY_RESPONSE       0x32
#define HCI_EV_USER_CONFIRMATION_REQUEST    0x33
#define HCI_EV_USER_PASSKEY_REQUEST         0x34
#define HCI_EV_SIMPLE_PAIRING_COMPLETE      0x36
#define HCI_EV_USER_PASSKEY_NOTIFICATION    0x3B
#define HCI_EV_KEYPRESS_NOTIFICATION        0x3C

/* Events a freshly reset controller reports, plus the ones this tool opts into */
#define HCI_EVENT_MASK_DEFAULT              0x00001FFFFFFFFFFFULL
#define HCI_EVENT_MASK_EXTENDED_INQUIRY     0x0000400000000000ULL
#define HCI_EVENT_MASK_SIMPLE_PAIRING       0x1CBF800000000000ULL

/* LMP feature bit numbers, page 0 */
#define LMP_FEATURE_ENCRYPTION              2
#define LMP_FEATURE_INQUIRY_RSSI            30
#define LMP_FEATURE_EXTENDED_INQUIRY        48
#define LMP_FEATURE_SIMPLE_PAIRING          51

/* Secure simple pairing parameters */
#define BT_IO_DISPLAY_ONLY                  0x00
#define BT_IO_DISPLAY_YES_NO                0x01
#define BT_IO_KEYBOARD_ONLY                 0x02
#define BT_IO_NO_INPUT_NO_OUTPUT            0x03
#define BT_AUTH_MITM_DEDICATED_BONDING      0x03
#define BT_AUTH_MITM_GENERAL_BONDING        0x05

/* L2CAP */
#define L2CAP_CID_SIGNALING                 0x0001
#define L2CAP_CID_DYNAMIC_FIRST             0x0040
#define L2CAP_LOCAL_MTU                     1024
#define L2CAP_MAX_PDU                       4096
#define L2CAP_MAX_CHANNELS                  8
#define L2CAP_MAX_LISTENERS                 4

#define L2CAP_COMMAND_REJECT                0x01
#define L2CAP_CONNECTION_REQUEST            0x02
#define L2CAP_CONNECTION_RESPONSE           0x03
#define L2CAP_CONFIGURE_REQUEST             0x04
#define L2CAP_CONFIGURE_RESPONSE            0x05
#define L2CAP_DISCONNECTION_REQUEST         0x06
#define L2CAP_DISCONNECTION_RESPONSE        0x07
#define L2CAP_ECHO_REQUEST                  0x08
#define L2CAP_ECHO_RESPONSE                 0x09
#define L2CAP_INFORMATION_REQUEST           0x0A
#define L2CAP_INFORMATION_RESPONSE          0x0B

#define L2CAP_PSM_SDP                       0x0001
#define L2CAP_PSM_HID_CONTROL               0x0011
#define L2CAP_PSM_HID_INTERRUPT             0x0013

/* HCI and L2CAP fields are little endian */
#define BT_READ16(Data)                     ((USHORT)((Data)[0] | ((Data)[1] << 8)))
#define BT_READ32(Data)                     ((ULONG)(Data)[0] | ((ULONG)(Data)[1] << 8) | \
                                             ((ULONG)(Data)[2] << 16) | ((ULONG)(Data)[3] << 24))

FORCEINLINE
VOID
BtWrite16(
    _Out_writes_(2) PUCHAR Data,
    _In_ USHORT Value)
{
    Data[0] = (UCHAR)(Value & 0xFF);
    Data[1] = (UCHAR)(Value >> 8);
}

typedef enum _HCI_ITEM_KIND
{
    HciItemNone = 0,
    HciItemEvent,
    HciItemAcl,
    HciItemError
} HCI_ITEM_KIND;

typedef struct _HCI_QUEUED_COMMAND
{
    USHORT OpCode;
    UCHAR Length;
    UCHAR Params[255];
} HCI_QUEUED_COMMAND, *PHCI_QUEUED_COMMAND;

typedef struct _HCI_QUEUED_ACL
{
    ULONG Length;
    UCHAR Packet[4 + HCI_ACL_FRAGMENT_MAX];
} HCI_QUEUED_ACL, *PHCI_QUEUED_ACL;

typedef struct _BT_LINK
{
    BOOLEAN InUse;
    BOOLEAN Authenticated;
    BOOLEAN Encrypted;
    UCHAR RemoteIoCapability;
    USHORT Handle;
    UCHAR Address[BT_ADDRESS_LENGTH];

    /* L2CAP reassembly across ACL fragments */
    ULONG RxLength;
    UCHAR Rx[4 + L2CAP_MAX_PDU];
} BT_LINK, *PBT_LINK;

struct _BT_RADIO;
struct _L2CAP_CHANNEL;

typedef
VOID
L2CAP_RECEIVE_ROUTINE(
    _Inout_ struct _BT_RADIO *Radio,
    _In_ struct _L2CAP_CHANNEL *Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length);
typedef L2CAP_RECEIVE_ROUTINE *PL2CAP_RECEIVE_ROUTINE;

typedef enum _L2CAP_STATE
{
    L2capClosed = 0,
    L2capWaitConnect,
    L2capConfig,
    L2capOpen,
    L2capWaitDisconnect
} L2CAP_STATE;

typedef struct _L2CAP_CHANNEL
{
    BOOLEAN InUse;
    BOOLEAN LocalConfigDone;
    BOOLEAN RemoteConfigDone;
    BOOLEAN ConfigRetried;
    L2CAP_STATE State;
    USHORT Handle;
    USHORT Psm;
    USHORT LocalCid;
    USHORT RemoteCid;
    USHORT RemoteMtu;
    USHORT Result;
    UCHAR PendingId;
    PL2CAP_RECEIVE_ROUTINE Receive;
    PVOID Context;
} L2CAP_CHANNEL, *PL2CAP_CHANNEL;

typedef struct _L2CAP_LISTENER
{
    BOOLEAN InUse;
    USHORT Psm;
    UCHAR Address[BT_ADDRESS_LENGTH];
    PL2CAP_RECEIVE_ROUTINE Receive;
    PVOID Context;
} L2CAP_LISTENER, *PL2CAP_LISTENER;

/* What the security handler may do on its own while a command runs */
typedef struct _BT_SECURITY
{
    BOOLEAN AllowPairing;
    BOOLEAN UseStoredKeys;
    BOOLEAN AutoConfirm;
    BOOLEAN AcceptIncoming;
    BOOLEAN HaveTarget;
    BOOLEAN PinGiven;
    BOOLEAN KeyStored;
    UCHAR KeyType;
    UCHAR PinLength;
    UCHAR AuthRequirements;
    UCHAR Target[BT_ADDRESS_LENGTH];
    CHAR Pin[BTH_MAX_PIN_SIZE + 1];
} BT_SECURITY, *PBT_SECURITY;

typedef struct _BT_RADIO
{
    HANDLE Device;
    HANDLE SyncEvent;
    BOOLEAN Verbose;
    BOOLEAN AclBroken;

    /* An event read and an ACL read stay posted while the radio is open */
    BOOLEAN EventPosted;
    BOOLEAN AclPosted;
    OVERLAPPED EventOverlapped;
    OVERLAPPED AclOverlapped;
    ULONG EventStreamLength;
    ULONG AclStreamLength;
    UCHAR EventRead[HCI_EVENT_READ_SIZE];
    UCHAR EventStream[HCI_EVENT_STREAM_SIZE];
    UCHAR AclRead[HCI_ACL_READ_SIZE];
    UCHAR AclStream[HCI_ACL_STREAM_SIZE];

    /* The event or ACL packet the last pump handed back */
    ULONG ItemLength;
    UCHAR Item[HCI_ACL_STREAM_SIZE];

    /* Command flow control */
    UCHAR CommandCredits;
    ULONG CommandHead;
    ULONG CommandCount;
    HCI_QUEUED_COMMAND Commands[HCI_COMMAND_QUEUE_LENGTH];

    /* ACL flow control */
    USHORT AclMtu;
    USHORT AclPackets;
    USHORT AclCredits;
    ULONG AclHead;
    ULONG AclCount;
    HCI_QUEUED_ACL AclQueue[HCI_ACL_QUEUE_LENGTH];

    /* Controller facts */
    UCHAR Address[BT_ADDRESS_LENGTH];
    UCHAR Features[8];
    UCHAR HciVersion;
    UCHAR LmpVersion;
    UCHAR ScoMtu;
    USHORT HciRevision;
    USHORT LmpSubversion;
    USHORT Manufacturer;
    USHORT ScoPackets;

    BT_SECURITY Security;
    BT_LINK Links[BT_MAX_LINKS];

    UCHAR NextSignalId;
    USHORT NextCid;
    L2CAP_CHANNEL Channels[L2CAP_MAX_CHANNELS];
    L2CAP_LISTENER Listeners[L2CAP_MAX_LISTENERS];
} BT_RADIO, *PBT_RADIO;

typedef struct _BT_SCAN_RESULT
{
    BOOLEAN HasRssi;
    BOOLEAN HasName;
    CHAR Rssi;
    UCHAR PageScanRepetitionMode;
    USHORT ClockOffset;
    ULONG ClassOfDevice;
    UCHAR Address[BT_ADDRESS_LENGTH];
    CHAR Name[BTH_MAX_NAME_SIZE + 1];
} BT_SCAN_RESULT, *PBT_SCAN_RESULT;

extern volatile LONG BtStopRequested;

/* hci.c */
PBT_RADIO
HciOpen(
    _In_ ULONG Instance,
    _In_ BOOLEAN Verbose);

VOID
HciClose(
    _In_ PBT_RADIO Radio);

BOOLEAN
HciInitialize(
    _Inout_ PBT_RADIO Radio,
    _In_ BOOLEAN PageScan);

HCI_ITEM_KIND
HciPump(
    _Inout_ PBT_RADIO Radio,
    _In_ DWORD Timeout);

BOOLEAN
HciQueueCommand(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT OpCode,
    _In_reads_bytes_opt_(Length) const VOID *Params,
    _In_ UCHAR Length);

UCHAR
HciCommand(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT OpCode,
    _In_reads_bytes_opt_(Length) const VOID *Params,
    _In_ UCHAR Length,
    _Out_writes_bytes_opt_(ReturnSize) PVOID Return,
    _In_ ULONG ReturnSize,
    _Out_opt_ PULONG ReturnLength);

BOOLEAN
HciSendAcl(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length);

BOOLEAN
HciHasFeature(
    _In_ PBT_RADIO Radio,
    _In_ ULONG Bit);

const CHAR *
HciErrorName(
    _In_ UCHAR Status);

const CHAR *
HciVersionName(
    _In_ UCHAR Version);

const CHAR *
HciManufacturerName(
    _In_ USHORT Id);

VOID
HciPrintInfo(
    _Inout_ PBT_RADIO Radio);

VOID
HciPrintSummary(
    _In_ PBT_RADIO Radio);

DWORD
BtRemaining(
    _In_ DWORD Deadline);

BOOLEAN
BtExpired(
    _In_ DWORD Deadline);

VOID
BtFormatAddress(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _Out_writes_(BT_ADDRESS_STRING) PCHAR Text);

BOOLEAN
BtParseAddress(
    _In_z_ const CHAR *Text,
    _Out_writes_(BT_ADDRESS_LENGTH) PUCHAR Address);

VOID
BtDescribeClass(
    _In_ ULONG ClassOfDevice,
    _Out_writes_z_(Size) PCHAR Text,
    _In_ ULONG Size);

/* security.c */
VOID
SecHandleEvent(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(Length) const UCHAR *Event,
    _In_ ULONG Length);

PBT_LINK
BtFindLinkByHandle(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle);

PBT_LINK
BtFindLinkByAddress(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address);

PBT_LINK
BtConnect(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address);

BOOLEAN
BtAuthenticate(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ DWORD Timeout);

BOOLEAN
BtEncrypt(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link);

VOID
BtDisconnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link);

BOOLEAN
BtRemoteName(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _In_ UCHAR PageScanRepetitionMode,
    _In_ USHORT ClockOffset,
    _Out_writes_z_(Size) PCHAR Name,
    _In_ ULONG Size);

VOID
BtPrintRemoteInfo(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link);

BOOLEAN
BtLoadKey(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _Out_writes_(BTH_LINK_KEY_LENGTH) PUCHAR Key);

BOOLEAN
BtStoreName(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _In_z_ const CHAR *Name);

VOID
BtListKeys(VOID);

BOOLEAN
BtDeleteKey(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote);

/* l2cap.c */
VOID
L2capReceiveAcl(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(Length) const UCHAR *Packet,
    _In_ ULONG Length);

VOID
L2capLinkClosed(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle);

BOOLEAN
L2capListen(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Psm,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _In_ PL2CAP_RECEIVE_ROUTINE Receive,
    _In_opt_ PVOID Context);

PL2CAP_CHANNEL
L2capConnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ USHORT Psm,
    _In_ PL2CAP_RECEIVE_ROUTINE Receive,
    _In_opt_ PVOID Context);

PL2CAP_CHANNEL
L2capFindOpen(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ USHORT Psm);

BOOLEAN
L2capSend(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length);

VOID
L2capDisconnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel);

/* sdp.c */
BOOLEAN
SdpBrowse(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link);

/* hid.c */
VOID
HidRun(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Target);
