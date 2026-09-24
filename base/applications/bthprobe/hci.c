/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HCI transport over the FreeBT IOCTL contract, and controller setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

#include <fbtusr.h>

/* Major device classes newer than the SDK header */
#define BT_COD_MAJOR_WEARABLE               0x07
#define BT_COD_MAJOR_TOY                    0x08
#define BT_COD_MAJOR_HEALTH                 0x09

volatile LONG BtStopRequested;

DWORD
BtRemaining(
    _In_ DWORD Deadline)
{
    LONG Left;

    Left = (LONG)(Deadline - GetTickCount());
    return (Left > 0) ? (DWORD)Left : 0;
}

BOOLEAN
BtExpired(
    _In_ DWORD Deadline)
{
    return (LONG)(Deadline - GetTickCount()) <= 0;
}

VOID
BtFormatAddress(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _Out_writes_(BT_ADDRESS_STRING) PCHAR Text)
{
    /* The address travels least significant byte first */
    sprintf(Text, "%02X:%02X:%02X:%02X:%02X:%02X",
            Address[5], Address[4], Address[3],
            Address[2], Address[1], Address[0]);
}

BOOLEAN
BtParseAddress(
    _In_z_ const CHAR *Text,
    _Out_writes_(BT_ADDRESS_LENGTH) PUCHAR Address)
{
    ULONG Index;
    ULONG Digits;
    UCHAR Value;
    CHAR Ch;

    ZeroMemory(Address, BT_ADDRESS_LENGTH);
    Digits = 0;

    for (Index = 0; Text[Index] != '\0'; Index++)
    {
        Ch = Text[Index];
        if (Ch == ':' || Ch == '-')
            continue;

        if (Ch >= '0' && Ch <= '9')
            Value = (UCHAR)(Ch - '0');
        else if (Ch >= 'a' && Ch <= 'f')
            Value = (UCHAR)(Ch - 'a' + 10);
        else if (Ch >= 'A' && Ch <= 'F')
            Value = (UCHAR)(Ch - 'A' + 10);
        else
            return FALSE;

        if (Digits == 2 * BT_ADDRESS_LENGTH)
            return FALSE;

        /* The first digit typed is the high nibble of the most significant byte */
        if (Digits % 2 == 0)
            Address[5 - Digits / 2] = (UCHAR)(Value << 4);
        else
            Address[5 - Digits / 2] |= Value;

        Digits++;
    }

    return Digits == 2 * BT_ADDRESS_LENGTH;
}

VOID
BtDescribeClass(
    _In_ ULONG ClassOfDevice,
    _Out_writes_z_(Size) PCHAR Text,
    _In_ ULONG Size)
{
    const CHAR *Major;
    const CHAR *Detail;
    ULONG Minor;

    Minor = GET_COD_MINOR(ClassOfDevice);
    Detail = "";

    switch (GET_COD_MAJOR(ClassOfDevice))
    {
        case COD_MAJOR_COMPUTER:
            Major = "Computer";
            if (Minor == COD_COMPUTER_MINOR_DESKTOP)
                Detail = " desktop";
            else if (Minor == COD_COMPUTER_MINOR_SERVER)
                Detail = " server";
            else if (Minor == COD_COMPUTER_MINOR_LAPTOP)
                Detail = " laptop";
            else if (Minor == COD_COMPUTER_MINOR_HANDHELD || Minor == COD_COMPUTER_MINOR_PALM)
                Detail = " handheld";
            else if (Minor == COD_COMPUTER_MINOR_WEARABLE)
                Detail = " wearable";
            break;

        case COD_MAJOR_PHONE:
            Major = "Phone";
            if (Minor == COD_PHONE_MINOR_CELLULAR)
                Detail = " cellular";
            else if (Minor == COD_PHONE_MINOR_CORDLESS)
                Detail = " cordless";
            else if (Minor == COD_PHONE_MINOR_SMART)
                Detail = " smartphone";
            else if (Minor == COD_PHONE_MINOR_WIRED_MODEM)
                Detail = " modem";
            break;

        case COD_MAJOR_LAN_ACCESS:
            Major = "Network access point";
            break;

        case COD_MAJOR_AUDIO:
            Major = "Audio";
            if (Minor == COD_AUDIO_MINOR_HEADSET || Minor == COD_AUDIO_MINOR_HEADSET_HANDS_FREE)
                Detail = " headset";
            else if (Minor == COD_AUDIO_MINOR_HANDS_FREE)
                Detail = " hands-free";
            else if (Minor == COD_AUDIO_MINOR_MICROPHONE)
                Detail = " microphone";
            else if (Minor == COD_AUDIO_MINOR_LOUDSPEAKER)
                Detail = " speaker";
            else if (Minor == COD_AUDIO_MINOR_HEADPHONES)
                Detail = " headphones";
            else if (Minor == COD_AUDIO_MINOR_PORTABLE_AUDIO)
                Detail = " portable";
            else if (Minor == COD_AUDIO_MINOR_CAR_AUDIO)
                Detail = " car";
            else if (Minor == COD_AUDIO_MINOR_HIFI_AUDIO)
                Detail = " hifi";
            break;

        case COD_MAJOR_PERIPHERAL:
            Major = "Peripheral";
            if ((Minor & COD_PERIPHERAL_MINOR_KEYBOARD_MASK) && (Minor & COD_PERIPHERAL_MINOR_POINTER_MASK))
                Detail = " keyboard and pointer";
            else if (Minor & COD_PERIPHERAL_MINOR_KEYBOARD_MASK)
                Detail = " keyboard";
            else if (Minor & COD_PERIPHERAL_MINOR_POINTER_MASK)
                Detail = " pointer";
            else if ((Minor & 0x0F) == COD_PERIPHERAL_MINOR_JOYSTICK)
                Detail = " joystick";
            else if ((Minor & 0x0F) == COD_PERIPHERAL_MINOR_GAMEPAD)
                Detail = " gamepad";
            else if ((Minor & 0x0F) == COD_PERIPHERAL_MINOR_REMOTE_CONTROL)
                Detail = " remote control";
            else if ((Minor & 0x0F) == COD_PERIPHERAL_MINOR_SENSING)
                Detail = " sensor";
            break;

        case COD_MAJOR_IMAGING:
            Major = "Imaging";
            if (Minor & COD_IMAGING_MINOR_PRINTER_MASK)
                Detail = " printer";
            else if (Minor & COD_IMAGING_MINOR_SCANNER_MASK)
                Detail = " scanner";
            else if (Minor & COD_IMAGING_MINOR_CAMERA_MASK)
                Detail = " camera";
            else if (Minor & COD_IMAGING_MINOR_DISPLAY_MASK)
                Detail = " display";
            break;

        case BT_COD_MAJOR_WEARABLE:
            Major = "Wearable";
            break;

        case BT_COD_MAJOR_TOY:
            Major = "Toy";
            break;

        case BT_COD_MAJOR_HEALTH:
            Major = "Health";
            break;

        case COD_MAJOR_UNCLASSIFIED:
            Major = "Uncategorized";
            break;

        default:
            Major = "Miscellaneous";
            break;
    }

    _snprintf(Text, Size, "%s%s", Major, Detail);
    Text[Size - 1] = '\0';
}

const CHAR *
HciErrorName(
    _In_ UCHAR Status)
{
    switch (Status)
    {
        case BTH_ERROR_SUCCESS:
            return "success";
        case BTH_ERROR_UNKNOWN_HCI_COMMAND:
            return "unknown HCI command";
        case BTH_ERROR_NO_CONNECTION:
            return "unknown connection";
        case BTH_ERROR_HARDWARE_FAILURE:
            return "hardware failure";
        case BTH_ERROR_PAGE_TIMEOUT:
            return "page timeout, the device did not answer";
        case BTH_ERROR_AUTHENTICATION_FAILURE:
            return "authentication failure";
        case BTH_ERROR_KEY_MISSING:
            return "PIN or key missing";
        case BTH_ERROR_MEMORY_FULL:
            return "controller out of memory";
        case BTH_ERROR_CONNECTION_TIMEOUT:
            return "connection timeout";
        case BTH_ERROR_MAX_NUMBER_OF_CONNECTIONS:
            return "too many connections";
        case BTH_ERROR_ACL_CONNECTION_ALREADY_EXISTS:
            return "connection already exists";
        case BTH_ERROR_COMMAND_DISALLOWED:
            return "command disallowed";
        case BTH_ERROR_HOST_REJECTED_SECURITY_REASONS:
            return "rejected for security reasons";
        case BTH_ERROR_HOST_REJECTED_PERSONAL_DEVICE:
            return "rejected, unacceptable address";
        case BTH_ERROR_HOST_TIMEOUT:
            return "accept timeout";
        case BTH_ERROR_UNSUPPORTED_FEATURE_OR_PARAMETER:
            return "unsupported feature or parameter";
        case BTH_ERROR_INVALID_HCI_PARAMETER:
            return "invalid HCI parameters";
        case BTH_ERROR_REMOTE_USER_ENDED_CONNECTION:
            return "ended by the remote user";
        case BTH_ERROR_REMOTE_LOW_RESOURCES:
            return "remote low on resources";
        case BTH_ERROR_REMOTE_POWERING_OFF:
            return "remote powering off";
        case BTH_ERROR_LOCAL_HOST_TERMINATED_CONNECTION:
            return "ended by this host";
        case BTH_ERROR_REPEATED_ATTEMPTS:
            return "repeated attempts, wait and try again";
        case BTH_ERROR_PAIRING_NOT_ALLOWED:
            return "pairing not allowed";
        case BTH_ERROR_UNSUPPORTED_REMOTE_FEATURE:
            return "unsupported remote feature";
        case BTH_ERROR_UNSPECIFIED_ERROR:
            return "unspecified error";
        case BTH_ERROR_LMP_RESPONSE_TIMEOUT:
            return "LMP response timeout";
        case BTH_ERROR_LMP_TRANSACTION_COLLISION:
            return "LMP transaction collision";
        case BTH_ERROR_ENCRYPTION_MODE_NOT_ACCEPTABLE:
            return "encryption mode not acceptable";
        case BTH_ERROR_INSTANT_PASSED:
            return "instant passed";
        case BTH_ERROR_PAIRING_WITH_UNIT_KEY_NOT_SUPPORTED:
            return "unit key pairing not supported";

        /* Codes newer than the SDK header */
        case 0x2F:
            return "insufficient security";
        case 0x37:
            return "simple pairing not supported by the host";
        case 0x38:
            return "host busy pairing";

        case BTH_ERROR_UNSPECIFIED:
            return "no response from the controller";
        default:
            return "unknown error";
    }
}

const CHAR *
HciVersionName(
    _In_ UCHAR Version)
{
    static const CHAR *const Names[] =
    {
        "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
        "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0"
    };

    if (Version < sizeof(Names) / sizeof(Names[0]))
        return Names[Version];

    return "newer than 6.0";
}

const CHAR *
HciManufacturerName(
    _In_ USHORT Id)
{
    switch (Id)
    {
        case BTH_MFG_ERICSSON:
            return "Ericsson";
        case BTH_MFG_NOKIA:
            return "Nokia";
        case BTH_MFG_INTEL:
            return "Intel";
        case BTH_MFG_MICROSOFT:
            return "Microsoft";
        case BTH_MFG_CSR:
            return "Cambridge Silicon Radio";
        case BTH_MFG_TI:
            return "Texas Instruments";
        case BTH_MFG_BROADCOM:
            return "Broadcom";
        case BTH_MFG_QUALCOMM:
            return "Qualcomm";

        /* Company identifiers assigned after the SDK header */
        case 0x0046:
            return "MediaTek";
        case 0x0048:
            return "Marvell";
        case 0x004C:
            return "Apple";
        case 0x0059:
            return "Nordic Semiconductor";
        case 0x005D:
            return "Realtek";
        case 0x0075:
            return "Samsung";
        default:
            return "unknown";
    }
}

BOOLEAN
HciHasFeature(
    _In_ PBT_RADIO Radio,
    _In_ ULONG Bit)
{
    return (Radio->Features[Bit / 8] >> (Bit % 8)) & 1;
}

static
VOID
HciDump(
    _In_z_ const CHAR *Prefix,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    ULONG Index;

    printf("%s", Prefix);
    for (Index = 0; Index < Length && Index < 32; Index++)
        printf(" %02X", Data[Index]);

    if (Length > 32)
        printf(" ... (%lu bytes)", Length);

    printf("\n");
}

PBT_RADIO
HciOpen(
    _In_ ULONG Instance,
    _In_ BOOLEAN Verbose)
{
    WCHAR Path[32];
    PBT_RADIO Radio;

    Radio = calloc(1, sizeof(*Radio));
    if (Radio == NULL)
        return NULL;

    swprintf(Path, sizeof(Path) / sizeof(Path[0]), L"\\\\.\\FbtUsb%02lu", Instance);

    Radio->Device = CreateFileW(Path,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);

    if (Radio->Device == INVALID_HANDLE_VALUE)
    {
        printf("No radio at instance %lu, error %lu\n", Instance, GetLastError());
        printf("The transport creates \\\\.\\FbtUsb00 once a dongle binds to fbtusb.sys\n");
        free(Radio);
        return NULL;
    }

    Radio->SyncEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Radio->EventOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Radio->AclOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (Radio->SyncEvent == NULL ||
        Radio->EventOverlapped.hEvent == NULL ||
        Radio->AclOverlapped.hEvent == NULL)
    {
        HciClose(Radio);
        return NULL;
    }

    Radio->Verbose = Verbose;
    Radio->CommandCredits = 1;
    Radio->NextSignalId = 1;
    Radio->NextCid = L2CAP_CID_DYNAMIC_FIRST;

    return Radio;
}

/* Wait out a canceled transfer. Returns FALSE when it never finished. */
static
BOOLEAN
HciSettle(
    _Inout_ PBT_RADIO Radio,
    _Inout_ LPOVERLAPPED Overlapped,
    _Inout_ PBOOLEAN Posted)
{
    DWORD Bytes;

    if (!*Posted)
        return TRUE;

    if (WaitForSingleObject(Overlapped->hEvent, 3000) != WAIT_OBJECT_0)
        return FALSE;

    GetOverlappedResult(Radio->Device, Overlapped, &Bytes, FALSE);
    *Posted = FALSE;

    return TRUE;
}

VOID
HciClose(
    _In_ PBT_RADIO Radio)
{
    BOOLEAN Settled;
    ULONG Index;

    Settled = TRUE;

    if (Radio->Device != INVALID_HANDLE_VALUE && Radio->Device != NULL)
    {
        /* Leave the remote side a clean disconnect rather than a supervision timeout */
        for (Index = 0; Index < BT_MAX_LINKS; Index++)
        {
            if (Radio->Links[Index].InUse)
                BtDisconnect(Radio, &Radio->Links[Index]);
        }

        CancelIo(Radio->Device);
        Settled = HciSettle(Radio, &Radio->EventOverlapped, &Radio->EventPosted) &&
                  HciSettle(Radio, &Radio->AclOverlapped, &Radio->AclPosted);

        CloseHandle(Radio->Device);
    }

    if (!Settled)
    {
        /* A transfer still owns buffers in this block, so it cannot be freed */
        printf("A transfer did not cancel, leaving its buffers in place\n");
        return;
    }

    if (Radio->SyncEvent != NULL)
        CloseHandle(Radio->SyncEvent);

    if (Radio->EventOverlapped.hEvent != NULL)
        CloseHandle(Radio->EventOverlapped.hEvent);

    if (Radio->AclOverlapped.hEvent != NULL)
        CloseHandle(Radio->AclOverlapped.hEvent);

    free(Radio);
}

static
BOOLEAN
HciWriteCommand(
    _Inout_ PBT_RADIO Radio,
    _In_ const HCI_QUEUED_COMMAND *Command)
{
    UCHAR Packet[3 + 255];
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Result;

    Packet[0] = (UCHAR)(Command->OpCode & 0xFF);
    Packet[1] = (UCHAR)(Command->OpCode >> 8);
    Packet[2] = Command->Length;
    CopyMemory(&Packet[3], Command->Params, Command->Length);

    if (Radio->Verbose)
        HciDump("  > command", Packet, 3 + Command->Length);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Radio->SyncEvent;
    ResetEvent(Radio->SyncEvent);

    Result = DeviceIoControl(Radio->Device,
                             IOCTL_FREEBT_HCI_SEND_CMD,
                             Packet,
                             (DWORD)(3 + Command->Length),
                             NULL,
                             0,
                             &Returned,
                             &Overlapped);

    if (!Result && GetLastError() == ERROR_IO_PENDING)
        Result = GetOverlappedResult(Radio->Device, &Overlapped, &Returned, TRUE);

    if (!Result)
        printf("  command 0x%04X could not be sent, error %lu\n", Command->OpCode, GetLastError());

    return Result != FALSE;
}

static
BOOLEAN
HciWriteAcl(
    _Inout_ PBT_RADIO Radio,
    _In_ const HCI_QUEUED_ACL *Entry)
{
    OVERLAPPED Overlapped;
    DWORD Written;
    BOOL Result;

    if (Radio->Verbose)
        HciDump("  > acl", Entry->Packet, Entry->Length);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Radio->SyncEvent;
    ResetEvent(Radio->SyncEvent);

    Result = WriteFile(Radio->Device, Entry->Packet, Entry->Length, &Written, &Overlapped);
    if (!Result && GetLastError() == ERROR_IO_PENDING)
        Result = GetOverlappedResult(Radio->Device, &Overlapped, &Written, TRUE);

    if (!Result)
        printf("  ACL write failed, error %lu\n", GetLastError());

    return Result != FALSE;
}

/* The controller says how many commands it will take, send no more than that */
static
VOID
HciFlushCommands(
    _Inout_ PBT_RADIO Radio)
{
    while (Radio->CommandCredits != 0 && Radio->CommandCount != 0)
    {
        HciWriteCommand(Radio, &Radio->Commands[Radio->CommandHead]);
        Radio->CommandCredits--;
        Radio->CommandHead = (Radio->CommandHead + 1) % HCI_COMMAND_QUEUE_LENGTH;
        Radio->CommandCount--;
    }
}

/* Each ACL packet spends one controller buffer until it reports it completed */
static
VOID
HciFlushAcl(
    _Inout_ PBT_RADIO Radio)
{
    while (Radio->AclCredits != 0 && Radio->AclCount != 0)
    {
        if (HciWriteAcl(Radio, &Radio->AclQueue[Radio->AclHead]))
            Radio->AclCredits--;

        Radio->AclHead = (Radio->AclHead + 1) % HCI_ACL_QUEUE_LENGTH;
        Radio->AclCount--;
    }
}

BOOLEAN
HciQueueCommand(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT OpCode,
    _In_reads_bytes_opt_(Length) const VOID *Params,
    _In_ UCHAR Length)
{
    PHCI_QUEUED_COMMAND Entry;

    if (Radio->CommandCount == HCI_COMMAND_QUEUE_LENGTH)
    {
        printf("  command queue full, dropping 0x%04X\n", OpCode);
        return FALSE;
    }

    Entry = &Radio->Commands[(Radio->CommandHead + Radio->CommandCount) % HCI_COMMAND_QUEUE_LENGTH];
    Entry->OpCode = OpCode;
    Entry->Length = Length;
    if (Length != 0 && Params != NULL)
        CopyMemory(Entry->Params, Params, Length);

    Radio->CommandCount++;
    HciFlushCommands(Radio);

    return TRUE;
}

BOOLEAN
HciSendAcl(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PHCI_QUEUED_ACL Entry;
    ULONG Fragment;
    ULONG Offset;
    ULONG Chunk;
    UCHAR Boundary;

    Fragment = min(Radio->AclMtu, HCI_ACL_FRAGMENT_MAX);
    if (Fragment == 0)
        return FALSE;

    Boundary = HCI_ACL_START;

    for (Offset = 0; Offset < Length; Offset += Chunk)
    {
        if (Radio->AclCount == HCI_ACL_QUEUE_LENGTH)
        {
            printf("  ACL queue full, dropping data\n");
            return FALSE;
        }

        Chunk = min(Length - Offset, Fragment);
        Entry = &Radio->AclQueue[(Radio->AclHead + Radio->AclCount) % HCI_ACL_QUEUE_LENGTH];

        Entry->Packet[0] = (UCHAR)(Handle & 0xFF);
        Entry->Packet[1] = (UCHAR)(((Handle >> 8) & 0x0F) | (Boundary << 4));
        BtWrite16(&Entry->Packet[2], (USHORT)Chunk);
        CopyMemory(&Entry->Packet[4], Data + Offset, Chunk);
        Entry->Length = 4 + Chunk;

        Radio->AclCount++;
        Boundary = HCI_ACL_CONTINUE;
    }

    HciFlushAcl(Radio);

    return TRUE;
}

static
VOID
HciHandleEvent(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(Length) const UCHAR *Event,
    _In_ ULONG Length)
{
    ULONG Index;
    ULONG Freed;

    if (Radio->Verbose)
        HciDump("  < event", Event, Length);

    switch (Event[0])
    {
        case HCI_EV_COMMAND_COMPLETE:
            if (Length >= 3)
                Radio->CommandCredits = Event[2];
            HciFlushCommands(Radio);
            break;

        case HCI_EV_COMMAND_STATUS:
            if (Length >= 4)
                Radio->CommandCredits = Event[3];
            HciFlushCommands(Radio);
            break;

        case HCI_EV_NUMBER_OF_COMPLETED_PACKETS:
            /* Handle and count pairs, flow control only needs the counts */
            Freed = 0;
            for (Index = 0; Length >= 3 && Index < Event[2] && 3 + (Index + 1) * 4 <= Length; Index++)
                Freed += BT_READ16(&Event[3 + Index * 4 + 2]);

            Radio->AclCredits = (USHORT)min(Radio->AclPackets, Radio->AclCredits + Freed);
            HciFlushAcl(Radio);
            break;

        case HCI_EV_HARDWARE_ERROR:
            printf("  controller reported hardware error 0x%02X\n", (Length >= 3) ? Event[2] : 0);
            break;

        case HCI_EV_DATA_BUFFER_OVERFLOW:
            printf("  controller dropped ACL data, its buffers overflowed\n");
            break;

        default:
            SecHandleEvent(Radio, Event, Length);
            break;
    }
}

static
BOOLEAN
HciPostEventRead(
    _Inout_ PBT_RADIO Radio)
{
    DWORD Returned;

    if (Radio->EventPosted)
        return TRUE;

    ResetEvent(Radio->EventOverlapped.hEvent);

    if (!DeviceIoControl(Radio->Device,
                         IOCTL_FREEBT_HCI_GET_EVENT,
                         NULL,
                         0,
                         Radio->EventRead,
                         sizeof(Radio->EventRead),
                         &Returned,
                         &Radio->EventOverlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        printf("  event read failed, error %lu\n", GetLastError());
        return FALSE;
    }

    /* A read that finished at once still signals its event, the pump reaps it */
    Radio->EventPosted = TRUE;

    return TRUE;
}

static
VOID
HciPostAclRead(
    _Inout_ PBT_RADIO Radio)
{
    DWORD Read;

    if (Radio->AclPosted || Radio->AclBroken)
        return;

    ResetEvent(Radio->AclOverlapped.hEvent);

    if (!ReadFile(Radio->Device,
                  Radio->AclRead,
                  sizeof(Radio->AclRead),
                  &Read,
                  &Radio->AclOverlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        printf("  ACL read failed, error %lu, continuing without ACL data\n", GetLastError());
        Radio->AclBroken = TRUE;
        return;
    }

    Radio->AclPosted = TRUE;
}

/* Move a completed read into its stream, where packets may straddle reads */
static
BOOLEAN
HciHarvest(
    _Inout_ PBT_RADIO Radio,
    _Inout_ LPOVERLAPPED Overlapped,
    _Inout_ PBOOLEAN Posted,
    _In_ const UCHAR *Read,
    _Inout_updates_(StreamSize) PUCHAR Stream,
    _In_ ULONG StreamSize,
    _Inout_ PULONG StreamLength)
{
    DWORD Bytes;

    Bytes = 0;
    *Posted = FALSE;

    if (!GetOverlappedResult(Radio->Device, Overlapped, &Bytes, FALSE))
    {
        if (GetLastError() == ERROR_OPERATION_ABORTED)
            return TRUE;

        printf("  transfer failed, error %lu\n", GetLastError());
        return FALSE;
    }

    if (Bytes > StreamSize - *StreamLength)
    {
        printf("  receive stream overflowed, resynchronizing\n");
        *StreamLength = 0;

        if (Bytes > StreamSize)
            return TRUE;
    }

    CopyMemory(Stream + *StreamLength, Read, Bytes);
    *StreamLength += Bytes;

    return TRUE;
}

static
BOOLEAN
HciDeliverEvent(
    _Inout_ PBT_RADIO Radio)
{
    ULONG Length;

    if (Radio->EventStreamLength < 2)
        return FALSE;

    Length = 2 + Radio->EventStream[1];
    if (Radio->EventStreamLength < Length)
        return FALSE;

    CopyMemory(Radio->Item, Radio->EventStream, Length);
    Radio->ItemLength = Length;

    Radio->EventStreamLength -= Length;
    MoveMemory(Radio->EventStream, Radio->EventStream + Length, Radio->EventStreamLength);

    HciHandleEvent(Radio, Radio->Item, Radio->ItemLength);

    return TRUE;
}

static
BOOLEAN
HciDeliverAcl(
    _Inout_ PBT_RADIO Radio)
{
    ULONG Length;

    if (Radio->AclStreamLength < 4)
        return FALSE;

    Length = 4 + BT_READ16(&Radio->AclStream[2]);
    if (Length > sizeof(Radio->Item))
    {
        printf("  ACL packet of %lu bytes cannot be right, resynchronizing\n", Length);
        Radio->AclStreamLength = 0;
        return FALSE;
    }

    if (Radio->AclStreamLength < Length)
        return FALSE;

    CopyMemory(Radio->Item, Radio->AclStream, Length);
    Radio->ItemLength = Length;

    Radio->AclStreamLength -= Length;
    MoveMemory(Radio->AclStream, Radio->AclStream + Length, Radio->AclStreamLength);

    if (Radio->Verbose)
        HciDump("  < acl", Radio->Item, Radio->ItemLength);

    L2capReceiveAcl(Radio, Radio->Item, Radio->ItemLength);

    return TRUE;
}

HCI_ITEM_KIND
HciPump(
    _Inout_ PBT_RADIO Radio,
    _In_ DWORD Timeout)
{
    HANDLE Handles[2];
    DWORD Count;
    DWORD Wait;

    if (HciDeliverEvent(Radio))
        return HciItemEvent;

    if (HciDeliverAcl(Radio))
        return HciItemAcl;

    if (!HciPostEventRead(Radio))
        return HciItemError;

    HciPostAclRead(Radio);

    Handles[0] = Radio->EventOverlapped.hEvent;
    Count = 1;
    if (Radio->AclPosted)
        Handles[Count++] = Radio->AclOverlapped.hEvent;

    Wait = WaitForMultipleObjects(Count, Handles, FALSE, Timeout);
    if (Wait == WAIT_TIMEOUT)
        return HciItemNone;

    if (Wait >= WAIT_OBJECT_0 + Count)
        return HciItemError;

    if (WaitForSingleObject(Radio->EventOverlapped.hEvent, 0) == WAIT_OBJECT_0 &&
        !HciHarvest(Radio,
                    &Radio->EventOverlapped,
                    &Radio->EventPosted,
                    Radio->EventRead,
                    Radio->EventStream,
                    sizeof(Radio->EventStream),
                    &Radio->EventStreamLength))
    {
        return HciItemError;
    }

    if (Radio->AclPosted &&
        WaitForSingleObject(Radio->AclOverlapped.hEvent, 0) == WAIT_OBJECT_0 &&
        !HciHarvest(Radio,
                    &Radio->AclOverlapped,
                    &Radio->AclPosted,
                    Radio->AclRead,
                    Radio->AclStream,
                    sizeof(Radio->AclStream),
                    &Radio->AclStreamLength))
    {
        printf("  continuing without ACL data\n");
        Radio->AclBroken = TRUE;
    }

    if (HciDeliverEvent(Radio))
        return HciItemEvent;

    if (HciDeliverAcl(Radio))
        return HciItemAcl;

    return HciItemNone;
}

UCHAR
HciCommand(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT OpCode,
    _In_reads_bytes_opt_(Length) const VOID *Params,
    _In_ UCHAR Length,
    _Out_writes_bytes_opt_(ReturnSize) PVOID Return,
    _In_ ULONG ReturnSize,
    _Out_opt_ PULONG ReturnLength)
{
    const UCHAR *Event;
    HCI_ITEM_KIND Kind;
    DWORD Deadline;
    ULONG Size;

    if (ReturnLength != NULL)
        *ReturnLength = 0;

    if (!HciQueueCommand(Radio, OpCode, Params, Length))
        return BTH_ERROR_UNSPECIFIED;

    Deadline = GetTickCount() + HCI_COMMAND_TIMEOUT;

    for (;;)
    {
        Kind = HciPump(Radio, BtRemaining(Deadline));
        if (Kind == HciItemError)
            return BTH_ERROR_UNSPECIFIED;

        Event = Radio->Item;

        if (Kind == HciItemEvent &&
            Event[0] == HCI_EV_COMMAND_COMPLETE &&
            Radio->ItemLength >= 5 &&
            BT_READ16(&Event[3]) == OpCode)
        {
            Size = Radio->ItemLength - 5;
            if (Return != NULL)
                CopyMemory(Return, &Event[5], min(Size, ReturnSize));

            if (ReturnLength != NULL)
                *ReturnLength = Size;

            /* Every command this tool sends returns its status first */
            return (Size != 0) ? Event[5] : BTH_ERROR_SUCCESS;
        }

        if (Kind == HciItemEvent &&
            Event[0] == HCI_EV_COMMAND_STATUS &&
            Radio->ItemLength >= 6 &&
            BT_READ16(&Event[4]) == OpCode)
        {
            return Event[2];
        }

        if (BtExpired(Deadline))
        {
            printf("  no answer to command 0x%04X\n", OpCode);
            return BTH_ERROR_UNSPECIFIED;
        }
    }
}

/* Optional setup steps only warn, the radio still works without them */
static
VOID
HciOptional(
    _Inout_ PBT_RADIO Radio,
    _In_z_ const CHAR *What,
    _In_ USHORT OpCode,
    _In_reads_bytes_(Length) const VOID *Params,
    _In_ UCHAR Length)
{
    UCHAR Status;

    Status = HciCommand(Radio, OpCode, Params, Length, NULL, 0, NULL);
    if (Status != BTH_ERROR_SUCCESS)
        printf("  note: %s failed, %s (0x%02X)\n", What, HciErrorName(Status), Status);
}

BOOLEAN
HciInitialize(
    _Inout_ PBT_RADIO Radio,
    _In_ BOOLEAN PageScan)
{
    UCHAR Return[HCI_EVENT_READ_SIZE];
    UCHAR Params[BTH_MAX_NAME_SIZE];
    ULONGLONG Mask;
    ULONG Length;
    UCHAR Status;
    ULONG Index;

    Status = HciCommand(Radio, HCI_RESET, NULL, 0, Return, sizeof(Return), &Length);
    if (Status != BTH_ERROR_SUCCESS)
    {
        printf("Reset failed, %s (0x%02X)\n", HciErrorName(Status), Status);
        return FALSE;
    }

    if (HciCommand(Radio, HCI_READ_LOCAL_VERSION, NULL, 0, Return, sizeof(Return), &Length) == BTH_ERROR_SUCCESS &&
        Length >= 9)
    {
        Radio->HciVersion = Return[1];
        Radio->HciRevision = BT_READ16(&Return[2]);
        Radio->LmpVersion = Return[4];
        Radio->Manufacturer = BT_READ16(&Return[5]);
        Radio->LmpSubversion = BT_READ16(&Return[7]);
    }

    if (HciCommand(Radio, HCI_READ_LOCAL_FEATURES, NULL, 0, Return, sizeof(Return), &Length) == BTH_ERROR_SUCCESS &&
        Length >= 9)
    {
        CopyMemory(Radio->Features, &Return[1], sizeof(Radio->Features));
    }

    Status = HciCommand(Radio, HCI_READ_BUFFER_SIZE, NULL, 0, Return, sizeof(Return), &Length);
    if (Status != BTH_ERROR_SUCCESS || Length < 8)
    {
        printf("Read Buffer Size failed, %s (0x%02X)\n", HciErrorName(Status), Status);
        return FALSE;
    }

    Radio->AclMtu = BT_READ16(&Return[1]);
    Radio->ScoMtu = Return[3];
    Radio->AclPackets = BT_READ16(&Return[4]);
    Radio->ScoPackets = BT_READ16(&Return[6]);
    Radio->AclCredits = Radio->AclPackets;

    if (HciCommand(Radio, HCI_READ_BD_ADDR, NULL, 0, Return, sizeof(Return), &Length) == BTH_ERROR_SUCCESS &&
        Length >= 7)
    {
        CopyMemory(Radio->Address, &Return[1], BT_ADDRESS_LENGTH);
    }

    /* Events outside the reset default have to be asked for */
    Mask = HCI_EVENT_MASK_DEFAULT;
    if (HciHasFeature(Radio, LMP_FEATURE_EXTENDED_INQUIRY))
        Mask |= HCI_EVENT_MASK_EXTENDED_INQUIRY;
    if (HciHasFeature(Radio, LMP_FEATURE_SIMPLE_PAIRING))
        Mask |= HCI_EVENT_MASK_SIMPLE_PAIRING;

    /* Shifted by a constant, so this needs no 64 bit shift helper from the runtime */
    for (Index = 0; Index < 8; Index++)
    {
        Params[Index] = (UCHAR)Mask;
        Mask >>= 8;
    }

    HciOptional(Radio, "Set Event Mask", HCI_SET_EVENT_MASK, Params, 8);

    if (HciHasFeature(Radio, LMP_FEATURE_SIMPLE_PAIRING))
    {
        Params[0] = 0x01;
        HciOptional(Radio, "Write Simple Pairing Mode", HCI_WRITE_SIMPLE_PAIRING_MODE, Params, 1);
    }

    /* Extended results carry the device name, which saves a name lookup per device */
    if (HciHasFeature(Radio, LMP_FEATURE_EXTENDED_INQUIRY))
        Params[0] = 0x02;
    else if (HciHasFeature(Radio, LMP_FEATURE_INQUIRY_RSSI))
        Params[0] = 0x01;
    else
        Params[0] = 0x00;

    if (Params[0] != 0x00)
        HciOptional(Radio, "Write Inquiry Mode", HCI_WRITE_INQUIRY_MODE, Params, 1);

    /* Uncategorized computer */
    Params[0] = 0x00;
    Params[1] = COD_MAJOR_COMPUTER;
    Params[2] = 0x00;
    HciOptional(Radio, "Write Class Of Device", HCI_WRITE_CLASS_OF_DEVICE, Params, 3);

    ZeroMemory(Params, sizeof(Params));
    strcpy((PCHAR)Params, "ReactOS");
    HciOptional(Radio, "Write Local Name", HCI_WRITE_LOCAL_NAME, Params, BTH_MAX_NAME_SIZE);

    /* Allow role switch and sniff mode, input devices lean on sniff to save power */
    BtWrite16(Params, 0x0005);
    HciOptional(Radio, "Write Default Link Policy", HCI_WRITE_DEFAULT_LINK_POLICY, Params, 2);

    /* Page scan lets a paired device connect back to us */
    if (PageScan)
    {
        Params[0] = 0x02;
        HciOptional(Radio, "Write Scan Enable", HCI_WRITE_SCAN_ENABLE, Params, 1);
    }

    return TRUE;
}

VOID
HciPrintSummary(
    _In_ PBT_RADIO Radio)
{
    CHAR Text[BT_ADDRESS_STRING];

    BtFormatAddress(Radio->Address, Text);
    printf("Radio %s, %s, Bluetooth %s%s\n\n",
           Text,
           HciManufacturerName(Radio->Manufacturer),
           HciVersionName(Radio->HciVersion),
           HciHasFeature(Radio, LMP_FEATURE_SIMPLE_PAIRING) ? ", secure simple pairing" : ", legacy pairing only");
}

VOID
HciPrintInfo(
    _Inout_ PBT_RADIO Radio)
{
    UCHAR Return[HCI_EVENT_READ_SIZE];
    CHAR Text[BT_ADDRESS_STRING];
    ULONG Length;
    ULONG Index;

    BtFormatAddress(Radio->Address, Text);

    printf("Address          %s\n", Text);
    printf("Manufacturer     %u (%s)\n", Radio->Manufacturer, HciManufacturerName(Radio->Manufacturer));
    printf("HCI version      %s, revision %u\n", HciVersionName(Radio->HciVersion), Radio->HciRevision);
    printf("LMP version      %s, subversion %u\n", HciVersionName(Radio->LmpVersion), Radio->LmpSubversion);
    printf("ACL buffers      %u bytes x %u\n", Radio->AclMtu, Radio->AclPackets);
    printf("SCO buffers      %u bytes x %u\n", Radio->ScoMtu, Radio->ScoPackets);

    printf("LMP features    ");
    for (Index = 0; Index < sizeof(Radio->Features); Index++)
        printf(" %02X", Radio->Features[Index]);
    printf("\n");

    printf("Supports         %s%s%s%s\n",
           HciHasFeature(Radio, LMP_FEATURE_ENCRYPTION) ? "encryption " : "",
           HciHasFeature(Radio, LMP_FEATURE_INQUIRY_RSSI) ? "inquiry-rssi " : "",
           HciHasFeature(Radio, LMP_FEATURE_EXTENDED_INQUIRY) ? "extended-inquiry " : "",
           HciHasFeature(Radio, LMP_FEATURE_SIMPLE_PAIRING) ? "secure-simple-pairing" : "");

    if (HciCommand(Radio, HCI_READ_LOCAL_NAME, NULL, 0, Return, sizeof(Return) - 1, &Length) == BTH_ERROR_SUCCESS &&
        Length >= 2)
    {
        Return[min(Length, sizeof(Return) - 1)] = '\0';
        printf("Local name       \"%s\"\n", (const CHAR *)&Return[1]);
    }

    printf("\n");
}
