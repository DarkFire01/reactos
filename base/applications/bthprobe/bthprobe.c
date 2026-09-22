/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Scan, pair, connect and inspect devices over the FreeBT transport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

typedef enum _BT_COMMAND
{
    BtCommandInfo = 0,
    BtCommandScan,
    BtCommandPair,
    BtCommandConnect,
    BtCommandSdp,
    BtCommandHid,
    BtCommandList,
    BtCommandUnpair
} BT_COMMAND;

typedef struct _BT_OPTIONS
{
    BT_COMMAND Command;
    BOOLEAN InfoThenScan;
    BOOLEAN Verbose;
    BOOLEAN AutoConfirm;
    BOOLEAN SkipNames;
    BOOLEAN HavePin;
    UCHAR InquiryLength;
    ULONG Instance;
    UCHAR Address[BT_ADDRESS_LENGTH];
    CHAR Pin[BTH_MAX_PIN_SIZE + 1];
} BT_OPTIONS, *PBT_OPTIONS;

/* General inquiry access code, least significant byte first */
static const UCHAR BtGeneralInquiryLap[3] = { 0x33, 0x8B, 0x9E };

static
BOOL
WINAPI
BtConsoleHandler(
    _In_ DWORD CtrlType)
{
    if (CtrlType == CTRL_C_EVENT || CtrlType == CTRL_BREAK_EVENT)
    {
        InterlockedExchange(&BtStopRequested, 1);
        return TRUE;
    }

    return FALSE;
}

static
ULONG
BtRead24(
    _In_reads_(3) const UCHAR *Data)
{
    return (ULONG)Data[0] | ((ULONG)Data[1] << 8) | ((ULONG)Data[2] << 16);
}

static
PBT_SCAN_RESULT
BtScanEntry(
    _Inout_updates_(BT_MAX_SCAN_RESULTS) PBT_SCAN_RESULT Results,
    _Inout_ PULONG Count,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    CHAR Text[BT_ADDRESS_STRING];
    ULONG Index;

    for (Index = 0; Index < *Count; Index++)
    {
        if (memcmp(Results[Index].Address, Address, BT_ADDRESS_LENGTH) == 0)
            return &Results[Index];
    }

    if (*Count == BT_MAX_SCAN_RESULTS)
        return NULL;

    ZeroMemory(&Results[*Count], sizeof(Results[*Count]));
    CopyMemory(Results[*Count].Address, Address, BT_ADDRESS_LENGTH);

    BtFormatAddress(Address, Text);
    printf("  found %s\n", Text);

    return &Results[(*Count)++];
}

static
VOID
BtParseEirName(
    _In_reads_(Length) const UCHAR *Eir,
    _In_ ULONG Length,
    _Inout_ PBT_SCAN_RESULT Result)
{
    ULONG Offset;
    ULONG Field;
    ULONG Size;

    for (Offset = 0; Offset < Length; Offset += 1 + Field)
    {
        Field = Eir[Offset];
        if (Field == 0 || Offset + 1 + Field > Length)
            break;

        /* 0x09 is the complete name, 0x08 a shortened one */
        if (Eir[Offset + 1] == 0x09 || (Eir[Offset + 1] == 0x08 && !Result->HasName))
        {
            Size = min(Field - 1, (ULONG)BTH_MAX_NAME_SIZE);
            CopyMemory(Result->Name, &Eir[Offset + 2], Size);
            Result->Name[Size] = '\0';
            Result->HasName = Size != 0;
        }
    }
}

/* Inquiry results pack one field at a time across all responses, not one response at a time */
static
VOID
BtScanEvent(
    _In_reads_(Length) const UCHAR *Event,
    _In_ ULONG Length,
    _Inout_updates_(BT_MAX_SCAN_RESULTS) PBT_SCAN_RESULT Results,
    _Inout_ PULONG Count)
{
    PBT_SCAN_RESULT Entry;
    const UCHAR *Params;
    ULONG ParamLength;
    ULONG Responses;
    ULONG Index;

    Params = Event + 2;
    ParamLength = Length - 2;
    if (ParamLength < 1)
        return;

    Responses = Params[0];

    switch (Event[0])
    {
        case HCI_EV_INQUIRY_RESULT:
            if (1 + Responses * 14 > ParamLength)
                return;

            for (Index = 0; Index < Responses; Index++)
            {
                Entry = BtScanEntry(Results, Count, &Params[1 + Index * 6]);
                if (Entry == NULL)
                    continue;

                Entry->PageScanRepetitionMode = Params[1 + Responses * 6 + Index];
                Entry->ClassOfDevice = BtRead24(&Params[1 + Responses * 9 + Index * 3]);
                Entry->ClockOffset = BT_READ16(&Params[1 + Responses * 12 + Index * 2]);
            }
            break;

        case HCI_EV_INQUIRY_RESULT_RSSI:
            if (1 + Responses * 14 > ParamLength)
                return;

            for (Index = 0; Index < Responses; Index++)
            {
                Entry = BtScanEntry(Results, Count, &Params[1 + Index * 6]);
                if (Entry == NULL)
                    continue;

                Entry->PageScanRepetitionMode = Params[1 + Responses * 6 + Index];
                Entry->ClassOfDevice = BtRead24(&Params[1 + Responses * 8 + Index * 3]);
                Entry->ClockOffset = BT_READ16(&Params[1 + Responses * 11 + Index * 2]);
                Entry->Rssi = (CHAR)Params[1 + Responses * 13 + Index];
                Entry->HasRssi = TRUE;
            }
            break;

        case HCI_EV_EXTENDED_INQUIRY_RESULT:
            /* Always a single response, followed by 240 bytes of extended data */
            if (ParamLength < 15)
                return;

            Entry = BtScanEntry(Results, Count, &Params[1]);
            if (Entry == NULL)
                return;

            Entry->PageScanRepetitionMode = Params[7];
            Entry->ClassOfDevice = BtRead24(&Params[9]);
            Entry->ClockOffset = BT_READ16(&Params[12]);
            Entry->Rssi = (CHAR)Params[14];
            Entry->HasRssi = TRUE;
            BtParseEirName(&Params[15], ParamLength - 15, Entry);
            break;

        default:
            break;
    }
}

static
BOOLEAN
BtScan(
    _Inout_ PBT_RADIO Radio,
    _In_ UCHAR InquiryLength,
    _In_ BOOLEAN ResolveNames)
{
    CHAR ClassText[40];
    CHAR RssiText[8];
    CHAR Text[BT_ADDRESS_STRING];
    PBT_SCAN_RESULT Results;
    HCI_ITEM_KIND Kind;
    UCHAR Params[5];
    DWORD Deadline;
    ULONG Count;
    ULONG Index;
    UCHAR Status;

    Results = calloc(BT_MAX_SCAN_RESULTS, sizeof(*Results));
    if (Results == NULL)
        return FALSE;

    Count = 0;

    CopyMemory(Params, BtGeneralInquiryLap, sizeof(BtGeneralInquiryLap));
    Params[3] = InquiryLength;
    Params[4] = 0x00;

    /* The inquiry length counts 1.28 second units */
    printf("Scanning for about %u seconds, Ctrl+C stops early\n", InquiryLength * 128 / 100);

    Status = HciCommand(Radio, HCI_INQUIRY, Params, sizeof(Params), NULL, 0, NULL);
    if (Status != BTH_ERROR_SUCCESS)
    {
        printf("  inquiry refused, %s (0x%02X)\n", HciErrorName(Status), Status);
        free(Results);
        return FALSE;
    }

    Deadline = GetTickCount() + InquiryLength * 1280 + 5000;

    for (;;)
    {
        Kind = HciPump(Radio, BtRemaining(Deadline));
        if (Kind == HciItemError)
            break;

        if (Kind == HciItemEvent)
        {
            if (Radio->Item[0] == HCI_EV_INQUIRY_COMPLETE)
                break;

            BtScanEvent(Radio->Item, Radio->ItemLength, Results, &Count);
        }

        if (BtStopRequested)
        {
            HciCommand(Radio, HCI_INQUIRY_CANCEL, NULL, 0, NULL, 0, NULL);
            break;
        }

        if (BtExpired(Deadline))
        {
            printf("  the controller never reported the inquiry finished\n");
            break;
        }
    }

    /* Devices that did not send a name in their extended response have to be asked */
    for (Index = 0; Index < Count && ResolveNames && !BtStopRequested; Index++)
    {
        if (Results[Index].HasName)
            continue;

        Results[Index].HasName = BtRemoteName(Radio,
                                              Results[Index].Address,
                                              Results[Index].PageScanRepetitionMode,
                                              Results[Index].ClockOffset | 0x8000,
                                              Results[Index].Name,
                                              sizeof(Results[Index].Name));
    }

    if (Count == 0)
    {
        printf("\nNo devices found. Is the device in pairing or discoverable mode?\n");
        free(Results);
        return TRUE;
    }

    printf("\n  %-17s  %5s  %-26s  %s\n", "Address", "RSSI", "Class", "Name");

    for (Index = 0; Index < Count; Index++)
    {
        BtFormatAddress(Results[Index].Address, Text);
        BtDescribeClass(Results[Index].ClassOfDevice, ClassText, sizeof(ClassText));

        if (Results[Index].HasRssi)
            sprintf(RssiText, "%4d", Results[Index].Rssi);
        else
            strcpy(RssiText, "    -");

        printf("  %s  %5s  %-26s  %s\n",
               Text,
               RssiText,
               ClassText,
               Results[Index].HasName ? Results[Index].Name : "");
    }

    printf("\n");
    free(Results);

    return TRUE;
}

static
BOOLEAN
BtPair(
    _Inout_ PBT_RADIO Radio,
    _In_ const BT_OPTIONS *Options)
{
    CHAR Name[BTH_MAX_NAME_SIZE + 1];
    CHAR Text[BT_ADDRESS_STRING];
    PBT_SECURITY Security;
    PBT_LINK Link;
    BOOLEAN Paired;

    Security = &Radio->Security;
    Security->AllowPairing = TRUE;
    Security->UseStoredKeys = FALSE;
    Security->AutoConfirm = Options->AutoConfirm;
    Security->HaveTarget = TRUE;
    Security->AuthRequirements = BT_AUTH_MITM_DEDICATED_BONDING;
    CopyMemory(Security->Target, Options->Address, BT_ADDRESS_LENGTH);

    if (Options->HavePin)
    {
        Security->PinGiven = TRUE;
        strcpy(Security->Pin, Options->Pin);
    }

    BtFormatAddress(Options->Address, Text);
    printf("Pairing with %s, put it in pairing mode if it is not already\n", Text);

    Link = BtConnect(Radio, Options->Address);
    if (Link == NULL)
        return FALSE;

    if (BtRemoteName(Radio, Options->Address, 0x02, 0x0000, Name, sizeof(Name)))
        printf("  name %s\n", Name);

    /* Refusing the stored key forces a fresh pairing */
    Paired = BtAuthenticate(Radio, Link, BT_PAIR_TIMEOUT) && Security->KeyStored;

    if (Paired)
    {
        if (Name[0] != '\0')
            BtStoreName(Radio, Options->Address, Name);

        if (Link->InUse)
            BtEncrypt(Radio, Link);

        printf("Paired with %s\n", Text);
    }
    else
    {
        printf("Pairing with %s failed\n", Text);
    }

    if (Link->InUse)
        BtDisconnect(Radio, Link);

    return Paired;
}

static
BOOLEAN
BtConnectAndShow(
    _Inout_ PBT_RADIO Radio,
    _In_ const BT_OPTIONS *Options)
{
    UCHAR LinkKey[BTH_LINK_KEY_LENGTH];
    CHAR Text[BT_ADDRESS_STRING];
    PBT_LINK Link;
    BOOLEAN Paired;
    BOOLEAN Secured;

    BtFormatAddress(Options->Address, Text);

    Radio->Security.UseStoredKeys = TRUE;
    Radio->Security.HaveTarget = TRUE;
    CopyMemory(Radio->Security.Target, Options->Address, BT_ADDRESS_LENGTH);

    Paired = BtLoadKey(Radio, Options->Address, LinkKey);
    SecureZeroMemory(LinkKey, sizeof(LinkKey));

    if (!Paired)
        printf("No stored pairing for %s, the link will stay unauthenticated\n", Text);

    Link = BtConnect(Radio, Options->Address);
    if (Link == NULL)
        return FALSE;

    BtPrintRemoteInfo(Radio, Link);

    Secured = FALSE;
    if (Paired && Link->InUse && BtAuthenticate(Radio, Link, BT_SHORT_TIMEOUT) && Link->InUse)
        Secured = BtEncrypt(Radio, Link);

    printf("Connected to %s, %s\n", Text, Secured ? "authenticated and encrypted" : "not secured");

    if (Link->InUse)
        BtDisconnect(Radio, Link);

    return TRUE;
}

static
BOOLEAN
BtShowServices(
    _Inout_ PBT_RADIO Radio,
    _In_ const BT_OPTIONS *Options)
{
    PBT_LINK Link;
    BOOLEAN Result;

    /* SDP rarely needs security, but answer with the stored key if asked */
    Radio->Security.UseStoredKeys = TRUE;
    Radio->Security.HaveTarget = TRUE;
    CopyMemory(Radio->Security.Target, Options->Address, BT_ADDRESS_LENGTH);

    Link = BtConnect(Radio, Options->Address);
    if (Link == NULL)
        return FALSE;

    Result = SdpBrowse(Radio, Link);

    if (Link->InUse)
        BtDisconnect(Radio, Link);

    return Result;
}

static
VOID
BtUsage(VOID)
{
    printf("Usage: bthprobe [options] [command] [address]\n\n");
    printf("Commands:\n");
    printf("  info               controller details, the default\n");
    printf("  scan               find nearby devices\n");
    printf("  pair <address>     pair with a device and store its link key\n");
    printf("  connect <address>  connect with the stored key and show device details\n");
    printf("  sdp <address>      list the services a device offers\n");
    printf("  hid <address>      show input from a paired keyboard or mouse\n");
    printf("  list               show stored pairings\n");
    printf("  unpair <address>   forget a stored pairing\n\n");
    printf("Options:\n");
    printf("  -d n      radio instance, default 0\n");
    printf("  -t units  inquiry length in 1.28 second units, default 8\n");
    printf("  -n        skip name lookups while scanning\n");
    printf("  -p pin    PIN for legacy pairing\n");
    printf("  -y        accept pairing confirmations without asking\n");
    printf("  -v        print every HCI packet and L2CAP signal\n");
    printf("  -i        info followed by a scan\n\n");
    printf("Addresses look like 00:1A:7D:DA:71:13\n");
}

static
BOOLEAN
BtParseOptions(
    _In_ int argc,
    _In_reads_(argc) char *argv[],
    _Out_ PBT_OPTIONS Options)
{
    const CHAR *Command;
    const CHAR *Address;
    BOOLEAN NeedsAddress;
    int Index;

    ZeroMemory(Options, sizeof(*Options));
    Options->InquiryLength = 8;
    Command = NULL;
    Address = NULL;

    for (Index = 1; Index < argc; Index++)
    {
        if (strcmp(argv[Index], "-v") == 0)
            Options->Verbose = TRUE;
        else if (strcmp(argv[Index], "-y") == 0)
            Options->AutoConfirm = TRUE;
        else if (strcmp(argv[Index], "-n") == 0)
            Options->SkipNames = TRUE;
        else if (strcmp(argv[Index], "-i") == 0)
            Options->InfoThenScan = TRUE;
        else if (strcmp(argv[Index], "-d") == 0 && Index + 1 < argc)
            Options->Instance = strtoul(argv[++Index], NULL, 10);
        else if ((strcmp(argv[Index], "-t") == 0 || strcmp(argv[Index], "-l") == 0) && Index + 1 < argc)
            Options->InquiryLength = (UCHAR)strtoul(argv[++Index], NULL, 10);
        else if (strcmp(argv[Index], "-p") == 0 && Index + 1 < argc)
        {
            Options->HavePin = TRUE;
            strncpy(Options->Pin, argv[++Index], BTH_MAX_PIN_SIZE);
            Options->Pin[BTH_MAX_PIN_SIZE] = '\0';
        }
        else if (argv[Index][0] == '-')
            return FALSE;
        else if (Command == NULL)
            Command = argv[Index];
        else if (Address == NULL)
            Address = argv[Index];
        else
            return FALSE;
    }

    /* The spec caps an inquiry at 0x30 units */
    if (Options->InquiryLength == 0 || Options->InquiryLength > 0x30)
        Options->InquiryLength = 8;

    NeedsAddress = TRUE;

    if (Command == NULL || strcmp(Command, "info") == 0)
    {
        Options->Command = BtCommandInfo;
        NeedsAddress = FALSE;
    }
    else if (strcmp(Command, "scan") == 0)
    {
        Options->Command = BtCommandScan;
        NeedsAddress = FALSE;
    }
    else if (strcmp(Command, "list") == 0)
    {
        Options->Command = BtCommandList;
        NeedsAddress = FALSE;
    }
    else if (strcmp(Command, "pair") == 0)
        Options->Command = BtCommandPair;
    else if (strcmp(Command, "connect") == 0)
        Options->Command = BtCommandConnect;
    else if (strcmp(Command, "sdp") == 0)
        Options->Command = BtCommandSdp;
    else if (strcmp(Command, "hid") == 0)
        Options->Command = BtCommandHid;
    else if (strcmp(Command, "unpair") == 0)
        Options->Command = BtCommandUnpair;
    else
    {
        printf("Unknown command %s\n\n", Command);
        return FALSE;
    }

    if (!NeedsAddress)
        return Address == NULL;

    if (Address == NULL || !BtParseAddress(Address, Options->Address))
    {
        printf("%s needs a device address\n\n", Command);
        return FALSE;
    }

    return TRUE;
}

int
main(
    _In_ int argc,
    _In_reads_(argc) char *argv[])
{
    BT_OPTIONS Options;
    PBT_RADIO Radio;
    BOOLEAN Result;

    if (!BtParseOptions(argc, argv, &Options))
    {
        BtUsage();
        return 1;
    }

    /* The key store needs no radio */
    if (Options.Command == BtCommandList)
    {
        BtListKeys();
        return 0;
    }

    if (Options.Command == BtCommandUnpair)
        return BtDeleteKey(Options.Address) ? 0 : 1;

    SetConsoleCtrlHandler(BtConsoleHandler, TRUE);

    Radio = HciOpen(Options.Instance, Options.Verbose);
    if (Radio == NULL)
        return 1;

    /* Only the HID viewer needs the device to be able to page us */
    if (!HciInitialize(Radio, Options.Command == BtCommandHid))
    {
        printf("Controller setup failed\n");
        HciClose(Radio);
        return 1;
    }

    if (Options.Command == BtCommandInfo)
        HciPrintInfo(Radio);
    else
        HciPrintSummary(Radio);

    switch (Options.Command)
    {
        case BtCommandInfo:
            Result = !Options.InfoThenScan || BtScan(Radio, Options.InquiryLength, !Options.SkipNames);
            break;

        case BtCommandScan:
            Result = BtScan(Radio, Options.InquiryLength, !Options.SkipNames);
            break;

        case BtCommandPair:
            Result = BtPair(Radio, &Options);
            break;

        case BtCommandConnect:
            Result = BtConnectAndShow(Radio, &Options);
            break;

        case BtCommandSdp:
            Result = BtShowServices(Radio, &Options);
            break;

        case BtCommandHid:
            HidRun(Radio, Options.Address);
            Result = TRUE;
            break;

        default:
            Result = FALSE;
            break;
    }

    HciClose(Radio);

    return Result ? 0 : 1;
}
