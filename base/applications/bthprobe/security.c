/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Connections, pairing and the link key store
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

#define BT_DEVICES_KEY  L"SYSTEM\\CurrentControlSet\\Services\\FreeBT\\Parameters\\Devices"

static
BOOLEAN
SecIsTarget(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    return !Radio->Security.HaveTarget ||
           memcmp(Radio->Security.Target, Address, BT_ADDRESS_LENGTH) == 0;
}

static
BOOLEAN
SecReadLine(
    _Out_writes_z_(Size) PCHAR Buffer,
    _In_ ULONG Size)
{
    ULONG Length;

    fflush(stdout);

    if (fgets(Buffer, (int)Size, stdin) == NULL)
    {
        Buffer[0] = '\0';
        return FALSE;
    }

    Length = (ULONG)strlen(Buffer);
    while (Length != 0 && (Buffer[Length - 1] == '\n' || Buffer[Length - 1] == '\r'))
        Buffer[--Length] = '\0';

    return TRUE;
}

static
const CHAR *
SecKeyTypeName(
    _In_ UCHAR Type)
{
    switch (Type)
    {
        case 0x00:
            return "legacy combination key";
        case 0x03:
            return "debug key";
        case 0x04:
            return "unauthenticated P-192 key";
        case 0x05:
            return "authenticated P-192 key";
        case 0x06:
            return "changed combination key";
        case 0x07:
            return "unauthenticated P-256 key";
        case 0x08:
            return "authenticated P-256 key";
        default:
            return "unknown key type";
    }
}

static
const CHAR *
SecIoCapabilityName(
    _In_ UCHAR Capability)
{
    switch (Capability)
    {
        case BT_IO_DISPLAY_ONLY:
            return "display only";
        case BT_IO_DISPLAY_YES_NO:
            return "display with yes and no";
        case BT_IO_KEYBOARD_ONLY:
            return "keyboard only";
        case BT_IO_NO_INPUT_NO_OUTPUT:
            return "no input or output";
        default:
            return "unknown";
    }
}

/* Link key store */

static
VOID
SecKeyName(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _Out_writes_(13) PWCHAR Name)
{
    swprintf(Name, 13, L"%02x%02x%02x%02x%02x%02x",
             Address[5], Address[4], Address[3],
             Address[2], Address[1], Address[0]);
}

static
HKEY
SecOpenDeviceKey(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Local,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _In_ BOOLEAN Create)
{
    WCHAR Path[160];
    WCHAR LocalName[13];
    WCHAR RemoteName[13];
    HKEY Key;
    LONG Error;

    SecKeyName(Local, LocalName);
    SecKeyName(Remote, RemoteName);
    swprintf(Path, sizeof(Path) / sizeof(Path[0]), L"%ls\\%ls\\%ls", BT_DEVICES_KEY, LocalName, RemoteName);

    if (Create)
        Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &Key, NULL);
    else
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key);

    return (Error == ERROR_SUCCESS) ? Key : NULL;
}

static
BOOLEAN
SecStoreKey(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _In_reads_(BTH_LINK_KEY_LENGTH) const UCHAR *LinkKey,
    _In_ UCHAR Type)
{
    DWORD KeyType;
    HKEY Key;
    LONG Error;

    Key = SecOpenDeviceKey(Radio->Address, Remote, TRUE);
    if (Key == NULL)
        return FALSE;

    KeyType = Type;
    Error = RegSetValueExW(Key, L"LinkKey", 0, REG_BINARY, LinkKey, BTH_LINK_KEY_LENGTH);
    if (Error == ERROR_SUCCESS)
        Error = RegSetValueExW(Key, L"KeyType", 0, REG_DWORD, (const BYTE *)&KeyType, sizeof(KeyType));

    RegCloseKey(Key);

    return Error == ERROR_SUCCESS;
}

BOOLEAN
BtLoadKey(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _Out_writes_(BTH_LINK_KEY_LENGTH) PUCHAR LinkKey)
{
    DWORD Type;
    DWORD Size;
    HKEY Key;
    LONG Error;

    Key = SecOpenDeviceKey(Radio->Address, Remote, FALSE);
    if (Key == NULL)
        return FALSE;

    Size = BTH_LINK_KEY_LENGTH;
    Error = RegQueryValueExW(Key, L"LinkKey", NULL, &Type, LinkKey, &Size);
    RegCloseKey(Key);

    return Error == ERROR_SUCCESS && Type == REG_BINARY && Size == BTH_LINK_KEY_LENGTH;
}

BOOLEAN
BtStoreName(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote,
    _In_z_ const CHAR *Name)
{
    WCHAR Wide[BTH_MAX_NAME_SIZE + 1];
    HKEY Key;
    LONG Error;
    int Length;

    /* Device names are UTF-8 on the air */
    Length = MultiByteToWideChar(CP_UTF8, 0, Name, -1, Wide, BTH_MAX_NAME_SIZE + 1);
    if (Length == 0)
        return FALSE;

    Key = SecOpenDeviceKey(Radio->Address, Remote, TRUE);
    if (Key == NULL)
        return FALSE;

    Error = RegSetValueExW(Key, L"Name", 0, REG_SZ, (const BYTE *)Wide, (DWORD)(Length * sizeof(WCHAR)));
    RegCloseKey(Key);

    return Error == ERROR_SUCCESS;
}

static
BOOLEAN
SecKeyTextToAddress(
    _In_z_ const WCHAR *Name,
    _Out_writes_(BT_ADDRESS_LENGTH) PUCHAR Address)
{
    CHAR Narrow[13];
    ULONG Index;

    for (Index = 0; Index < 12; Index++)
    {
        if (Name[Index] == L'\0' || Name[Index] > 0x7F)
            return FALSE;

        Narrow[Index] = (CHAR)Name[Index];
    }

    Narrow[12] = '\0';

    return Name[12] == L'\0' && BtParseAddress(Narrow, Address);
}

VOID
BtListKeys(VOID)
{
    WCHAR RadioName[64];
    WCHAR DeviceName[64];
    WCHAR Name[BTH_MAX_NAME_SIZE + 1];
    UCHAR RadioAddress[BT_ADDRESS_LENGTH];
    UCHAR DeviceAddress[BT_ADDRESS_LENGTH];
    CHAR RadioText[BT_ADDRESS_STRING];
    CHAR DeviceText[BT_ADDRESS_STRING];
    HKEY Devices;
    HKEY RadioKey;
    HKEY DeviceKey;
    DWORD RadioIndex;
    DWORD DeviceIndex;
    DWORD Size;
    DWORD Type;
    DWORD KeyType;
    ULONG Count;

    Count = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BT_DEVICES_KEY, 0, KEY_READ, &Devices) != ERROR_SUCCESS)
    {
        printf("No stored pairings\n");
        return;
    }

    for (RadioIndex = 0; ; RadioIndex++)
    {
        Size = sizeof(RadioName) / sizeof(RadioName[0]);
        if (RegEnumKeyExW(Devices, RadioIndex, RadioName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (!SecKeyTextToAddress(RadioName, RadioAddress) ||
            RegOpenKeyExW(Devices, RadioName, 0, KEY_READ, &RadioKey) != ERROR_SUCCESS)
        {
            continue;
        }

        BtFormatAddress(RadioAddress, RadioText);

        for (DeviceIndex = 0; ; DeviceIndex++)
        {
            Size = sizeof(DeviceName) / sizeof(DeviceName[0]);
            if (RegEnumKeyExW(RadioKey, DeviceIndex, DeviceName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            if (!SecKeyTextToAddress(DeviceName, DeviceAddress) ||
                RegOpenKeyExW(RadioKey, DeviceName, 0, KEY_READ, &DeviceKey) != ERROR_SUCCESS)
            {
                continue;
            }

            Size = sizeof(Name) - sizeof(WCHAR);
            ZeroMemory(Name, sizeof(Name));
            if (RegQueryValueExW(DeviceKey, L"Name", NULL, &Type, (PBYTE)Name, &Size) != ERROR_SUCCESS ||
                Type != REG_SZ)
            {
                Name[0] = L'\0';
            }

            Size = sizeof(KeyType);
            if (RegQueryValueExW(DeviceKey, L"KeyType", NULL, &Type, (PBYTE)&KeyType, &Size) != ERROR_SUCCESS ||
                Type != REG_DWORD)
            {
                KeyType = 0xFF;
            }

            RegCloseKey(DeviceKey);

            if (Count == 0)
                printf("  Radio              Device             Key                          Name\n");

            BtFormatAddress(DeviceAddress, DeviceText);
            printf("  %s  %s  %-27s  %ls\n", RadioText, DeviceText, SecKeyTypeName((UCHAR)KeyType), Name);
            Count++;
        }

        RegCloseKey(RadioKey);
    }

    RegCloseKey(Devices);

    if (Count == 0)
        printf("No stored pairings\n");
}

BOOLEAN
BtDeleteKey(
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Remote)
{
    WCHAR RadioName[64];
    WCHAR DeviceName[13];
    CHAR Text[BT_ADDRESS_STRING];
    HKEY Devices;
    HKEY RadioKey;
    DWORD RadioIndex;
    DWORD Size;
    ULONG Removed;

    Removed = 0;
    SecKeyName(Remote, DeviceName);
    BtFormatAddress(Remote, Text);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BT_DEVICES_KEY, 0, KEY_READ, &Devices) == ERROR_SUCCESS)
    {
        for (RadioIndex = 0; ; RadioIndex++)
        {
            Size = sizeof(RadioName) / sizeof(RadioName[0]);
            if (RegEnumKeyExW(Devices, RadioIndex, RadioName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            if (RegOpenKeyExW(Devices, RadioName, 0, KEY_READ | KEY_WRITE, &RadioKey) != ERROR_SUCCESS)
                continue;

            if (RegDeleteKeyW(RadioKey, DeviceName) == ERROR_SUCCESS)
                Removed++;

            RegCloseKey(RadioKey);
        }

        RegCloseKey(Devices);
    }

    if (Removed == 0)
    {
        printf("No stored pairing for %s\n", Text);
        return FALSE;
    }

    printf("Forgot %s\n", Text);

    return TRUE;
}

/* Link table */

PBT_LINK
BtFindLinkByHandle(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle)
{
    ULONG Index;

    for (Index = 0; Index < BT_MAX_LINKS; Index++)
    {
        if (Radio->Links[Index].InUse && Radio->Links[Index].Handle == Handle)
            return &Radio->Links[Index];
    }

    return NULL;
}

PBT_LINK
BtFindLinkByAddress(
    _In_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    ULONG Index;

    for (Index = 0; Index < BT_MAX_LINKS; Index++)
    {
        if (Radio->Links[Index].InUse &&
            memcmp(Radio->Links[Index].Address, Address, BT_ADDRESS_LENGTH) == 0)
        {
            return &Radio->Links[Index];
        }
    }

    return NULL;
}

static
PBT_LINK
SecAllocateLink(
    _Inout_ PBT_RADIO Radio)
{
    ULONG Index;

    for (Index = 0; Index < BT_MAX_LINKS; Index++)
    {
        if (!Radio->Links[Index].InUse)
        {
            ZeroMemory(&Radio->Links[Index], sizeof(Radio->Links[Index]));
            Radio->Links[Index].InUse = TRUE;
            Radio->Links[Index].RemoteIoCapability = 0xFF;
            return &Radio->Links[Index];
        }
    }

    return NULL;
}

/* Security events, answered on the spot from whatever command is running */

static
VOID
SecConnectionComplete(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(11) const UCHAR *Params)
{
    PBT_LINK Link;

    /* Only ACL links matter here, voice links are refused up front */
    if (Params[0] != BTH_ERROR_SUCCESS || Params[9] != 0x01)
        return;

    Link = BtFindLinkByAddress(Radio, &Params[3]);
    if (Link == NULL)
        Link = SecAllocateLink(Radio);

    if (Link == NULL)
    {
        printf("  no room to track another link\n");
        return;
    }

    Link->Handle = BT_READ16(&Params[1]) & 0x0FFF;
    Link->Encrypted = Params[10] != 0;
    Link->Authenticated = FALSE;
    Link->RxLength = 0;
    CopyMemory(Link->Address, &Params[3], BT_ADDRESS_LENGTH);
}

static
VOID
SecConnectionRequest(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(10) const UCHAR *Params)
{
    CHAR Text[BT_ADDRESS_STRING];
    UCHAR Reply[7];

    BtFormatAddress(Params, Text);
    CopyMemory(Reply, Params, BT_ADDRESS_LENGTH);

    if (Params[9] == 0x01 && Radio->Security.AcceptIncoming && SecIsTarget(Radio, Params))
    {
        /* Stay slave, some input devices refuse a role switch */
        printf("  Accepting a connection from %s\n", Text);
        Reply[6] = 0x01;
        HciQueueCommand(Radio, HCI_ACCEPT_CONNECTION_REQUEST, Reply, sizeof(Reply));
        return;
    }

    if (Radio->Verbose)
        printf("  Refusing a connection from %s\n", Text);

    Reply[6] = BTH_ERROR_HOST_REJECTED_PERSONAL_DEVICE;

    if (Params[9] == 0x02)
        HciQueueCommand(Radio, HCI_REJECT_SYNC_CONNECTION, Reply, sizeof(Reply));
    else
        HciQueueCommand(Radio, HCI_REJECT_CONNECTION_REQUEST, Reply, sizeof(Reply));
}

static
VOID
SecDisconnectionComplete(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(4) const UCHAR *Params)
{
    CHAR Text[BT_ADDRESS_STRING];
    PBT_LINK Link;

    if (Params[0] != BTH_ERROR_SUCCESS)
        return;

    Link = BtFindLinkByHandle(Radio, BT_READ16(&Params[1]) & 0x0FFF);
    if (Link == NULL)
        return;

    L2capLinkClosed(Radio, Link->Handle);

    BtFormatAddress(Link->Address, Text);
    printf("  Link to %s closed, %s\n", Text, HciErrorName(Params[3]));

    Link->InUse = FALSE;
}

static
VOID
SecLinkKeyRequest(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    UCHAR Reply[BT_ADDRESS_LENGTH + BTH_LINK_KEY_LENGTH];

    CopyMemory(Reply, Address, BT_ADDRESS_LENGTH);

    if (Radio->Security.UseStoredKeys && BtLoadKey(Radio, Address, &Reply[BT_ADDRESS_LENGTH]))
    {
        if (Radio->Verbose)
            printf("  Answering the link key request from the store\n");

        HciQueueCommand(Radio, HCI_LINK_KEY_REPLY, Reply, sizeof(Reply));
        SecureZeroMemory(Reply, sizeof(Reply));
        return;
    }

    /* Saying no makes the controller fall back to pairing */
    HciQueueCommand(Radio, HCI_LINK_KEY_NEGATIVE_REPLY, Address, BT_ADDRESS_LENGTH);
}

static
VOID
SecLinkKeyNotification(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(23) const UCHAR *Params)
{
    UCHAR Type;

    Type = Params[BT_ADDRESS_LENGTH + BTH_LINK_KEY_LENGTH];

    if (!Radio->Security.AllowPairing && !Radio->Security.UseStoredKeys)
        return;

    if (!SecStoreKey(Radio, Params, &Params[BT_ADDRESS_LENGTH], Type))
    {
        printf("  Could not store the link key\n");
        return;
    }

    Radio->Security.KeyStored = TRUE;
    Radio->Security.KeyType = Type;
    printf("  Stored a %s\n", SecKeyTypeName(Type));
}

static
VOID
SecPinCodeRequest(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    UCHAR Reply[BT_ADDRESS_LENGTH + 1 + BTH_MAX_PIN_SIZE];
    CHAR Pin[BTH_MAX_PIN_SIZE + 2];
    CHAR Text[BT_ADDRESS_STRING];
    ULONG Length;

    BtFormatAddress(Address, Text);

    if (!Radio->Security.AllowPairing || !SecIsTarget(Radio, Address))
    {
        printf("  %s asked for a PIN, refusing since no pairing is running\n", Text);
        HciQueueCommand(Radio, HCI_PIN_CODE_NEGATIVE_REPLY, Address, BT_ADDRESS_LENGTH);
        return;
    }

    if (Radio->Security.PinGiven)
    {
        strcpy(Pin, Radio->Security.Pin);
    }
    else
    {
        printf("  Legacy pairing. Enter a PIN for %s, or press Enter for 0000.\n", Text);
        printf("  A keyboard needs the same digits typed on it, followed by its Enter key.\n");
        printf("  PIN: ");
        SecReadLine(Pin, sizeof(Pin));

        if (Pin[0] == '\0')
            strcpy(Pin, "0000");
    }

    Length = (ULONG)strlen(Pin);
    if (Length > BTH_MAX_PIN_SIZE)
        Length = BTH_MAX_PIN_SIZE;

    ZeroMemory(Reply, sizeof(Reply));
    CopyMemory(Reply, Address, BT_ADDRESS_LENGTH);
    Reply[BT_ADDRESS_LENGTH] = (UCHAR)Length;
    CopyMemory(&Reply[BT_ADDRESS_LENGTH + 1], Pin, Length);

    HciQueueCommand(Radio, HCI_PIN_CODE_REPLY, Reply, sizeof(Reply));
}

static
VOID
SecIoCapabilityRequest(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    UCHAR Reply[BT_ADDRESS_LENGTH + 3];

    CopyMemory(Reply, Address, BT_ADDRESS_LENGTH);

    if (!Radio->Security.AllowPairing || !SecIsTarget(Radio, Address))
    {
        printf("  Refusing to pair, no pairing is running\n");
        Reply[BT_ADDRESS_LENGTH] = BTH_ERROR_PAIRING_NOT_ALLOWED;
        HciQueueCommand(Radio, HCI_IO_CAPABILITY_NEGATIVE, Reply, BT_ADDRESS_LENGTH + 1);
        return;
    }

    /* A console can show a number and read a yes or no */
    Reply[BT_ADDRESS_LENGTH] = BT_IO_DISPLAY_YES_NO;
    Reply[BT_ADDRESS_LENGTH + 1] = 0x00;
    Reply[BT_ADDRESS_LENGTH + 2] = Radio->Security.AuthRequirements;

    HciQueueCommand(Radio, HCI_IO_CAPABILITY_REPLY, Reply, sizeof(Reply));
}

static
VOID
SecIoCapabilityResponse(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(9) const UCHAR *Params)
{
    PBT_LINK Link;

    Link = BtFindLinkByAddress(Radio, Params);
    if (Link != NULL)
        Link->RemoteIoCapability = Params[BT_ADDRESS_LENGTH];

    printf("  Secure simple pairing, the device has %s\n", SecIoCapabilityName(Params[BT_ADDRESS_LENGTH]));
}

static
VOID
SecUserConfirmation(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(10) const UCHAR *Params)
{
    CHAR Answer[16];
    PBT_LINK Link;
    ULONG Value;
    BOOLEAN Accept;

    Value = BT_READ32(&Params[BT_ADDRESS_LENGTH]);
    Link = BtFindLinkByAddress(Radio, Params);

    if (!Radio->Security.AllowPairing || !SecIsTarget(Radio, Params))
    {
        HciQueueCommand(Radio, HCI_USER_CONFIRMATION_NEGATIVE, Params, BT_ADDRESS_LENGTH);
        return;
    }

    if (Link != NULL && Link->RemoteIoCapability == BT_IO_NO_INPUT_NO_OUTPUT)
    {
        /* Nothing to compare against on a device without a display, just works */
        printf("  Confirming automatically, the device has no display\n");
        Accept = TRUE;
    }
    else if (Radio->Security.AutoConfirm)
    {
        printf("  Confirming %06lu automatically\n", Value);
        Accept = TRUE;
    }
    else
    {
        printf("  Does the device show %06lu? [y/N] ", Value);
        SecReadLine(Answer, sizeof(Answer));
        Accept = Answer[0] == 'y' || Answer[0] == 'Y';
    }

    HciQueueCommand(Radio,
                    Accept ? HCI_USER_CONFIRMATION_REPLY : HCI_USER_CONFIRMATION_NEGATIVE,
                    Params,
                    BT_ADDRESS_LENGTH);
}

static
VOID
SecUserPasskeyRequest(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    UCHAR Reply[BT_ADDRESS_LENGTH + 4];
    CHAR Answer[16];
    ULONG Value;

    if (!Radio->Security.AllowPairing || !SecIsTarget(Radio, Address))
    {
        HciQueueCommand(Radio, HCI_USER_PASSKEY_NEGATIVE, Address, BT_ADDRESS_LENGTH);
        return;
    }

    printf("  Enter the 6 digit passkey the device shows: ");
    SecReadLine(Answer, sizeof(Answer));

    if (Answer[0] == '\0')
    {
        HciQueueCommand(Radio, HCI_USER_PASSKEY_NEGATIVE, Address, BT_ADDRESS_LENGTH);
        return;
    }

    Value = strtoul(Answer, NULL, 10);
    CopyMemory(Reply, Address, BT_ADDRESS_LENGTH);
    Reply[6] = (UCHAR)(Value & 0xFF);
    Reply[7] = (UCHAR)((Value >> 8) & 0xFF);
    Reply[8] = (UCHAR)((Value >> 16) & 0xFF);
    Reply[9] = (UCHAR)((Value >> 24) & 0xFF);

    HciQueueCommand(Radio, HCI_USER_PASSKEY_REPLY, Reply, sizeof(Reply));
}

VOID
SecHandleEvent(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(Length) const UCHAR *Event,
    _In_ ULONG Length)
{
    const UCHAR *Params;
    ULONG ParamLength;
    PBT_LINK Link;

    Params = Event + 2;
    ParamLength = Length - 2;

    switch (Event[0])
    {
        case HCI_EV_CONNECTION_COMPLETE:
            if (ParamLength >= 11)
                SecConnectionComplete(Radio, Params);
            break;

        case HCI_EV_CONNECTION_REQUEST:
            if (ParamLength >= 10)
                SecConnectionRequest(Radio, Params);
            break;

        case HCI_EV_DISCONNECTION_COMPLETE:
            if (ParamLength >= 4)
                SecDisconnectionComplete(Radio, Params);
            break;

        case HCI_EV_AUTHENTICATION_COMPLETE:
            if (ParamLength >= 3 && (Link = BtFindLinkByHandle(Radio, BT_READ16(&Params[1]) & 0x0FFF)) != NULL)
                Link->Authenticated = Params[0] == BTH_ERROR_SUCCESS;
            break;

        case HCI_EV_ENCRYPTION_CHANGE:
            if (ParamLength >= 4 && (Link = BtFindLinkByHandle(Radio, BT_READ16(&Params[1]) & 0x0FFF)) != NULL)
                Link->Encrypted = Params[0] == BTH_ERROR_SUCCESS && Params[3] != 0;
            break;

        case HCI_EV_LINK_KEY_REQUEST:
            if (ParamLength >= BT_ADDRESS_LENGTH)
                SecLinkKeyRequest(Radio, Params);
            break;

        case HCI_EV_LINK_KEY_NOTIFICATION:
            if (ParamLength >= BT_ADDRESS_LENGTH + BTH_LINK_KEY_LENGTH + 1)
                SecLinkKeyNotification(Radio, Params);
            break;

        case HCI_EV_PIN_CODE_REQUEST:
            if (ParamLength >= BT_ADDRESS_LENGTH)
                SecPinCodeRequest(Radio, Params);
            break;

        case HCI_EV_IO_CAPABILITY_REQUEST:
            if (ParamLength >= BT_ADDRESS_LENGTH)
                SecIoCapabilityRequest(Radio, Params);
            break;

        case HCI_EV_IO_CAPABILITY_RESPONSE:
            if (ParamLength >= BT_ADDRESS_LENGTH + 3)
                SecIoCapabilityResponse(Radio, Params);
            break;

        case HCI_EV_USER_CONFIRMATION_REQUEST:
            if (ParamLength >= BT_ADDRESS_LENGTH + 4)
                SecUserConfirmation(Radio, Params);
            break;

        case HCI_EV_USER_PASSKEY_REQUEST:
            if (ParamLength >= BT_ADDRESS_LENGTH)
                SecUserPasskeyRequest(Radio, Params);
            break;

        case HCI_EV_USER_PASSKEY_NOTIFICATION:
            if (ParamLength >= BT_ADDRESS_LENGTH + 4)
                printf("  Type %06lu on the device, then press its Enter key\n", BT_READ32(&Params[BT_ADDRESS_LENGTH]));
            break;

        case HCI_EV_KEYPRESS_NOTIFICATION:
            if (ParamLength >= BT_ADDRESS_LENGTH + 1 && Params[BT_ADDRESS_LENGTH] == 0x01)
            {
                printf("*");
                fflush(stdout);
            }
            break;

        case HCI_EV_SIMPLE_PAIRING_COMPLETE:
            if (ParamLength >= 1 + BT_ADDRESS_LENGTH && Params[0] != BTH_ERROR_SUCCESS)
                printf("\n  Secure simple pairing failed, %s (0x%02X)\n", HciErrorName(Params[0]), Params[0]);
            break;

        default:
            break;
    }
}

/* Pump until an event with Code arrives whose parameters hold Key at Offset */
static
const UCHAR *
SecWaitFor(
    _Inout_ PBT_RADIO Radio,
    _In_ UCHAR Code,
    _In_ ULONG Offset,
    _In_reads_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_opt_ PBT_LINK Link,
    _In_ DWORD Timeout)
{
    HCI_ITEM_KIND Kind;
    DWORD Deadline;

    Deadline = GetTickCount() + Timeout;

    for (;;)
    {
        Kind = HciPump(Radio, BtRemaining(Deadline));
        if (Kind == HciItemError)
            return NULL;

        if (Kind == HciItemEvent &&
            Radio->Item[0] == Code &&
            Radio->ItemLength >= 2 + Offset + KeyLength &&
            memcmp(&Radio->Item[2 + Offset], Key, KeyLength) == 0)
        {
            return &Radio->Item[2];
        }

        /* A link that went away will never answer */
        if (Link != NULL && !Link->InUse)
            return NULL;

        if (BtStopRequested || BtExpired(Deadline))
            return NULL;
    }
}

PBT_LINK
BtConnect(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    CHAR Text[BT_ADDRESS_STRING];
    const UCHAR *Complete;
    UCHAR Params[13];
    PBT_LINK Link;
    UCHAR Status;

    Link = BtFindLinkByAddress(Radio, Address);
    if (Link != NULL)
        return Link;

    BtFormatAddress(Address, Text);
    printf("Connecting to %s\n", Text);

    CopyMemory(Params, Address, BT_ADDRESS_LENGTH);
    BtWrite16(&Params[6], 0xCC18);  /* DM1 DH1 DM3 DH3 DM5 DH5 */
    Params[8] = 0x02;               /* page scan repetition mode R2, covers the others */
    Params[9] = 0x00;
    BtWrite16(&Params[10], 0x0000); /* clock offset unknown */
    Params[12] = 0x01;              /* allow a role switch */

    Status = HciCommand(Radio, HCI_CREATE_CONNECTION, Params, sizeof(Params), NULL, 0, NULL);
    if (Status != BTH_ERROR_SUCCESS)
    {
        printf("  refused, %s (0x%02X)\n", HciErrorName(Status), Status);
        return NULL;
    }

    Complete = SecWaitFor(Radio, HCI_EV_CONNECTION_COMPLETE, 3, Address, BT_ADDRESS_LENGTH, NULL, BT_CONNECT_TIMEOUT);
    if (Complete == NULL)
    {
        printf("  no answer, giving up\n");
        HciQueueCommand(Radio, HCI_CREATE_CONNECTION_CANCEL, Address, BT_ADDRESS_LENGTH);
        SecWaitFor(Radio, HCI_EV_CONNECTION_COMPLETE, 3, Address, BT_ADDRESS_LENGTH, NULL, 3000);
        return NULL;
    }

    if (Complete[0] != BTH_ERROR_SUCCESS)
    {
        printf("  failed, %s (0x%02X)\n", HciErrorName(Complete[0]), Complete[0]);
        return NULL;
    }

    Link = BtFindLinkByAddress(Radio, Address);
    if (Link != NULL)
        printf("  connected, handle 0x%03X\n", Link->Handle);

    return Link;
}

BOOLEAN
BtAuthenticate(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ DWORD Timeout)
{
    const UCHAR *Complete;
    UCHAR Params[2];
    UCHAR Status;

    BtWrite16(Params, Link->Handle);

    Status = HciCommand(Radio, HCI_AUTHENTICATION_REQUESTED, Params, sizeof(Params), NULL, 0, NULL);
    if (Status != BTH_ERROR_SUCCESS)
    {
        printf("  authentication refused, %s (0x%02X)\n", HciErrorName(Status), Status);
        return FALSE;
    }

    Complete = SecWaitFor(Radio, HCI_EV_AUTHENTICATION_COMPLETE, 1, Params, sizeof(Params), Link, Timeout);
    if (Complete == NULL)
    {
        printf("  authentication %s\n", Link->InUse ? "timed out" : "ended when the link dropped");
        return FALSE;
    }

    if (Complete[0] != BTH_ERROR_SUCCESS)
    {
        printf("  authentication failed, %s (0x%02X)\n", HciErrorName(Complete[0]), Complete[0]);
        return FALSE;
    }

    printf("  authenticated\n");

    return TRUE;
}

BOOLEAN
BtEncrypt(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link)
{
    const UCHAR *Change;
    UCHAR Params[3];
    UCHAR Status;

    BtWrite16(Params, Link->Handle);
    Params[2] = 0x01;

    Status = HciCommand(Radio, HCI_SET_CONNECTION_ENCRYPTION, Params, sizeof(Params), NULL, 0, NULL);
    if (Status != BTH_ERROR_SUCCESS)
    {
        printf("  encryption refused, %s (0x%02X)\n", HciErrorName(Status), Status);
        return FALSE;
    }

    Change = SecWaitFor(Radio, HCI_EV_ENCRYPTION_CHANGE, 1, Params, 2, Link, BT_SHORT_TIMEOUT);
    if (Change == NULL || Change[0] != BTH_ERROR_SUCCESS || Change[3] == 0)
    {
        printf("  encryption did not come on\n");
        return FALSE;
    }

    printf("  encrypted\n");

    return TRUE;
}

VOID
BtDisconnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link)
{
    UCHAR Params[3];

    if (!Link->InUse)
        return;

    BtWrite16(Params, Link->Handle);
    Params[2] = BTH_ERROR_REMOTE_USER_ENDED_CONNECTION;

    if (HciCommand(Radio, HCI_DISCONNECT, Params, sizeof(Params), NULL, 0, NULL) == BTH_ERROR_SUCCESS)
        SecWaitFor(Radio, HCI_EV_DISCONNECTION_COMPLETE, 1, Params, 2, NULL, 5000);

    /* The controller forgets the handle either way */
    if (Link->InUse)
    {
        L2capLinkClosed(Radio, Link->Handle);
        Link->InUse = FALSE;
    }
}

BOOLEAN
BtRemoteName(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _In_ UCHAR PageScanRepetitionMode,
    _In_ USHORT ClockOffset,
    _Out_writes_z_(Size) PCHAR Name,
    _In_ ULONG Size)
{
    const UCHAR *Complete;
    UCHAR Params[10];
    ULONG Length;

    Name[0] = '\0';

    CopyMemory(Params, Address, BT_ADDRESS_LENGTH);
    Params[6] = PageScanRepetitionMode;
    Params[7] = 0x00;
    BtWrite16(&Params[8], ClockOffset);

    if (HciCommand(Radio, HCI_REMOTE_NAME_REQUEST, Params, sizeof(Params), NULL, 0, NULL) != BTH_ERROR_SUCCESS)
        return FALSE;

    Complete = SecWaitFor(Radio, HCI_EV_REMOTE_NAME_COMPLETE, 1, Address, BT_ADDRESS_LENGTH, NULL, BT_SHORT_TIMEOUT);
    if (Complete == NULL || Complete[0] != BTH_ERROR_SUCCESS)
        return FALSE;

    /* The name field is NUL padded unless all 248 bytes are used */
    Length = min(Size - 1, (ULONG)BTH_MAX_NAME_SIZE);
    CopyMemory(Name, &Complete[7], Length);
    Name[Length] = '\0';

    return Name[0] != '\0';
}

VOID
BtPrintRemoteInfo(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link)
{
    CHAR Name[BTH_MAX_NAME_SIZE + 1];
    const UCHAR *Complete;
    UCHAR Params[2];
    ULONG Index;

    if (BtRemoteName(Radio, Link->Address, 0x02, 0x0000, Name, sizeof(Name)))
        printf("  Name             %s\n", Name);

    BtWrite16(Params, Link->Handle);

    if (HciCommand(Radio, HCI_READ_REMOTE_VERSION, Params, sizeof(Params), NULL, 0, NULL) == BTH_ERROR_SUCCESS)
    {
        Complete = SecWaitFor(Radio, HCI_EV_REMOTE_VERSION_COMPLETE, 1, Params, 2, Link, BT_SHORT_TIMEOUT);
        if (Complete != NULL && Complete[0] == BTH_ERROR_SUCCESS)
        {
            printf("  Version          Bluetooth %s, subversion %u\n",
                   HciVersionName(Complete[3]),
                   BT_READ16(&Complete[6]));
            printf("  Manufacturer     %u (%s)\n",
                   BT_READ16(&Complete[4]),
                   HciManufacturerName(BT_READ16(&Complete[4])));
        }
    }

    if (HciCommand(Radio, HCI_READ_REMOTE_FEATURES, Params, sizeof(Params), NULL, 0, NULL) == BTH_ERROR_SUCCESS)
    {
        Complete = SecWaitFor(Radio, HCI_EV_REMOTE_FEATURES_COMPLETE, 1, Params, 2, Link, BT_SHORT_TIMEOUT);
        if (Complete != NULL && Complete[0] == BTH_ERROR_SUCCESS)
        {
            printf("  LMP features    ");
            for (Index = 0; Index < 8; Index++)
                printf(" %02X", Complete[3 + Index]);
            printf("\n");
        }
    }
}
