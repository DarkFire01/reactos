/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Drives a radio over the FreeBT transport IOCTL contract
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include <fbtusr.h>

/* The transport rejects an event buffer smaller than this */
#define HCI_MAX_EVENT_SIZE          257
#define HCI_MAX_COMMAND_SIZE        258
#define HCI_EVENT_TIMEOUT           4000
#define HCI_MAX_STRAY_EVENTS        16

#define HCI_OGF_LINK_CONTROL        0x01
#define HCI_OGF_CONTROL_BASEBAND    0x03
#define HCI_OGF_INFORMATIONAL       0x04

#define HCI_OPCODE(Ogf, Ocf)        ((USHORT)(((Ogf) << 10) | (Ocf)))

#define HCI_RESET                   HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0003)
#define HCI_READ_LOCAL_NAME         HCI_OPCODE(HCI_OGF_CONTROL_BASEBAND, 0x0014)
#define HCI_READ_LOCAL_VERSION      HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0001)
#define HCI_READ_LOCAL_FEATURES     HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0003)
#define HCI_READ_BUFFER_SIZE        HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0005)
#define HCI_READ_BD_ADDR            HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0009)
#define HCI_INQUIRY                 HCI_OPCODE(HCI_OGF_LINK_CONTROL, 0x0001)

#define HCI_EVENT_INQUIRY_COMPLETE  0x01
#define HCI_EVENT_INQUIRY_RESULT    0x02
#define HCI_EVENT_COMMAND_COMPLETE  0x0E
#define HCI_EVENT_COMMAND_STATUS    0x0F
#define HCI_EVENT_INQUIRY_RSSI      0x22
#define HCI_EVENT_EXTENDED_INQUIRY  0x2F

/* General inquiry access code, least significant byte first */
static const UCHAR GeneralInquiryLap[3] = { 0x33, 0x8B, 0x9E };

static
const char *
HciErrorName(
    _In_ UCHAR Status)
{
    switch (Status)
    {
        case 0x00: return "success";
        case 0x01: return "unknown HCI command";
        case 0x02: return "no connection";
        case 0x03: return "hardware failure";
        case 0x04: return "page timeout";
        case 0x05: return "authentication failure";
        case 0x0C: return "command disallowed";
        case 0x11: return "unsupported feature or parameter";
        case 0x12: return "invalid HCI command parameters";
        case 0x1A: return "unsupported remote feature";
        default: return "unknown";
    }
}

static
const char *
ManufacturerName(
    _In_ USHORT Id)
{
    switch (Id)
    {
        case 0x0001: return "Nokia";
        case 0x0002: return "Intel";
        case 0x000A: return "Cambridge Silicon Radio";
        case 0x000F: return "Broadcom";
        case 0x001D: return "Qualcomm";
        case 0x005D: return "Realtek";
        case 0x005F: return "MediaTek";
        default: return "unknown";
    }
}

static
void
PrintAddress(
    _In_reads_(6) const UCHAR *Address)
{
    /* The address arrives least significant byte first */
    printf("%02X:%02X:%02X:%02X:%02X:%02X",
           Address[5], Address[4], Address[3],
           Address[2], Address[1], Address[0]);
}

static
void
PrintHex(
    _In_reads_(Length) const UCHAR *Buffer,
    _In_ DWORD Length)
{
    DWORD i;

    for (i = 0; i < Length; i++)
        printf("%02X ", Buffer[i]);
}

static
HANDLE
OpenRadio(
    _In_ int Instance)
{
    WCHAR Path[64];
    HANDLE Radio;

    swprintf(Path, sizeof(Path) / sizeof(Path[0]), L"\\\\.\\FbtUsb%02d", Instance);

    Radio = CreateFileW(Path,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);

    if (Radio != INVALID_HANDLE_VALUE)
        printf("Opened %ls\n\n", Path);

    return Radio;
}

static
BOOL
SendCommand(
    _In_ HANDLE Radio,
    _In_ USHORT OpCode,
    _In_reads_opt_(ParamLength) const UCHAR *Params,
    _In_ UCHAR ParamLength)
{
    UCHAR Packet[HCI_MAX_COMMAND_SIZE];
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Result;

    Packet[0] = (UCHAR)(OpCode & 0xFF);
    Packet[1] = (UCHAR)(OpCode >> 8);
    Packet[2] = ParamLength;

    if (ParamLength != 0 && Params != NULL)
        CopyMemory(&Packet[3], Params, ParamLength);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent == NULL)
        return FALSE;

    Result = DeviceIoControl(Radio,
                             IOCTL_FREEBT_HCI_SEND_CMD,
                             Packet,
                             (DWORD)(3 + ParamLength),
                             NULL,
                             0,
                             &Returned,
                             &Overlapped);

    if (!Result && GetLastError() == ERROR_IO_PENDING)
        Result = GetOverlappedResult(Radio, &Overlapped, &Returned, TRUE);

    CloseHandle(Overlapped.hEvent);

    return Result;
}

static
BOOL
ReadEvent(
    _In_ HANDLE Radio,
    _Out_writes_(HCI_MAX_EVENT_SIZE) UCHAR *Buffer,
    _Out_ DWORD *Length,
    _In_ DWORD Timeout)
{
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Result;

    Returned = 0;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent == NULL)
        return FALSE;

    Result = DeviceIoControl(Radio,
                             IOCTL_FREEBT_HCI_GET_EVENT,
                             NULL,
                             0,
                             Buffer,
                             HCI_MAX_EVENT_SIZE,
                             &Returned,
                             &Overlapped);

    if (!Result && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Overlapped.hEvent, Timeout) == WAIT_OBJECT_0)
        {
            Result = GetOverlappedResult(Radio, &Overlapped, &Returned, TRUE);

        }

        else
        {
            /* Let the cancelled transfer settle so the next read starts clean */
            CancelIo(Radio);
            GetOverlappedResult(Radio, &Overlapped, &Returned, TRUE);
            SetLastError(WAIT_TIMEOUT);
            Result = FALSE;

        }

    }

    CloseHandle(Overlapped.hEvent);
    *Length = Returned;

    return Result;
}

static
void
PrintInquiryResult(
    _In_reads_(Length) const UCHAR *Event,
    _In_ DWORD Length)
{
    /* Both layouts pack one field at a time rather than one record at a time */
    UCHAR Count;
    UCHAR Stride;
    const UCHAR *Addresses;
    const UCHAR *Classes;
    DWORD i;

    if (Length < 3)
        return;

    Count = Event[2];
    Stride = 14;

    if ((DWORD)(3 + Count * Stride) > Length)
    {
        printf("  truncated inquiry result, %lu bytes for %u responses\n", Length, Count);
        return;
    }

    Addresses = &Event[3];

    if (Event[0] == HCI_EVENT_INQUIRY_RSSI)
        Classes = Addresses + Count * 6 + Count * 1 + Count * 1;

    else
        Classes = Addresses + Count * 6 + Count * 1 + Count * 2;

    for (i = 0; i < Count; i++)
    {
        printf("  found ");
        PrintAddress(&Addresses[i * 6]);
        printf("  class %02X%02X%02X",
               Classes[i * 3 + 2], Classes[i * 3 + 1], Classes[i * 3]);

        if (Event[0] == HCI_EVENT_INQUIRY_RSSI)
        {
            const UCHAR *Rssi = Classes + Count * 3 + Count * 2;
            printf("  rssi %d dBm", (signed char)Rssi[i]);

        }

        printf("\n");

    }
}

static
void
PrintStrayEvent(
    _In_reads_(Length) const UCHAR *Event,
    _In_ DWORD Length)
{
    switch (Event[0])
    {
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RSSI:
            PrintInquiryResult(Event, Length);
            break;

        case HCI_EVENT_INQUIRY_COMPLETE:
            printf("  inquiry complete\n");
            break;

        default:
            printf("  event 0x%02X, %lu bytes: ", Event[0], Length);
            PrintHex(Event, Length);
            printf("\n");
            break;

    }
}

/* Send a command and pump events until its completion comes back */
static
BOOL
Command(
    _In_ HANDLE Radio,
    _In_ USHORT OpCode,
    _In_reads_opt_(ParamLength) const UCHAR *Params,
    _In_ UCHAR ParamLength,
    _Out_writes_to_(HCI_MAX_EVENT_SIZE, *ReturnLength) UCHAR *Return,
    _Out_ DWORD *ReturnLength)
{
    UCHAR Event[HCI_MAX_EVENT_SIZE];
    DWORD Length;
    USHORT Echoed;
    int Stray;

    *ReturnLength = 0;

    if (!SendCommand(Radio, OpCode, Params, ParamLength))
    {
        printf("  command 0x%04X could not be sent, error %lu\n", OpCode, GetLastError());
        return FALSE;

    }

    for (Stray = 0; Stray < HCI_MAX_STRAY_EVENTS; Stray++)
    {
        if (!ReadEvent(Radio, Event, &Length, HCI_EVENT_TIMEOUT))
        {
            printf("  no event for command 0x%04X, error %lu\n", OpCode, GetLastError());
            return FALSE;

        }

        if (Length < 2)
            continue;

        if (Event[0] == HCI_EVENT_COMMAND_COMPLETE && Length >= 5)
        {
            Echoed = (USHORT)(Event[3] | (Event[4] << 8));
            if (Echoed == OpCode)
            {
                *ReturnLength = Length - 5;
                if (*ReturnLength != 0)
                    CopyMemory(Return, &Event[5], *ReturnLength);

                return TRUE;

            }

        }

        else if (Event[0] == HCI_EVENT_COMMAND_STATUS && Length >= 6)
        {
            Echoed = (USHORT)(Event[4] | (Event[5] << 8));
            if (Echoed == OpCode)
            {
                Return[0] = Event[2];
                *ReturnLength = 1;

                return TRUE;

            }

        }

        PrintStrayEvent(Event, Length);

    }

    printf("  gave up waiting for command 0x%04X\n", OpCode);

    return FALSE;
}

static
BOOL
ProbeRadio(
    _In_ HANDLE Radio)
{
    UCHAR Return[HCI_MAX_EVENT_SIZE];
    DWORD Length;
    USHORT Manufacturer;

    printf("Reset\n");
    if (!Command(Radio, HCI_RESET, NULL, 0, Return, &Length) || Length < 1)
        return FALSE;

    printf("  %s\n\n", HciErrorName(Return[0]));
    if (Return[0] != 0x00)
        return FALSE;

    printf("Read BD_ADDR\n");
    if (Command(Radio, HCI_READ_BD_ADDR, NULL, 0, Return, &Length) && Length >= 7)
    {
        printf("  address ");
        PrintAddress(&Return[1]);
        printf("\n\n");

    }

    printf("Read Local Version Information\n");
    if (Command(Radio, HCI_READ_LOCAL_VERSION, NULL, 0, Return, &Length) && Length >= 9)
    {
        Manufacturer = (USHORT)(Return[4] | (Return[5] << 8));
        printf("  HCI version %u revision %u\n", Return[1], (USHORT)(Return[2] | (Return[3] << 8)));
        printf("  LMP version %u subversion %u\n", Return[6], (USHORT)(Return[7] | (Return[8] << 8)));
        printf("  manufacturer %u (%s)\n\n", Manufacturer, ManufacturerName(Manufacturer));

    }

    printf("Read Buffer Size\n");
    if (Command(Radio, HCI_READ_BUFFER_SIZE, NULL, 0, Return, &Length) && Length >= 8)
    {
        printf("  ACL %u bytes x %u packets\n",
               (USHORT)(Return[1] | (Return[2] << 8)),
               (USHORT)(Return[4] | (Return[5] << 8)));
        printf("  SCO %u bytes x %u packets\n\n",
               Return[3],
               (USHORT)(Return[6] | (Return[7] << 8)));

    }

    printf("Read Local Supported Features\n");
    if (Command(Radio, HCI_READ_LOCAL_FEATURES, NULL, 0, Return, &Length) && Length >= 9)
    {
        printf("  LMP features ");
        PrintHex(&Return[1], 8);
        printf("\n\n");

    }

    printf("Read Local Name\n");
    if (Command(Radio, HCI_READ_LOCAL_NAME, NULL, 0, Return, &Length) && Length >= 2)
    {
        Return[Length - 1] = '\0';
        printf("  name \"%s\"\n\n", (const char *)&Return[1]);

    }

    return TRUE;
}

static
void
RunInquiry(
    _In_ HANDLE Radio,
    _In_ UCHAR Seconds)
{
    UCHAR Params[5];
    UCHAR Event[HCI_MAX_EVENT_SIZE];
    UCHAR Return[HCI_MAX_EVENT_SIZE];
    DWORD Length;

    /* The inquiry length counts 1.28 second units */
    CopyMemory(Params, GeneralInquiryLap, sizeof(GeneralInquiryLap));
    Params[3] = (UCHAR)(Seconds > 0 ? Seconds : 8);
    Params[4] = 0;

    printf("Inquiry for about %u seconds\n", (unsigned)(Params[3] * 128 / 100));

    if (!Command(Radio, HCI_INQUIRY, Params, sizeof(Params), Return, &Length) || Length < 1)
        return;

    if (Return[0] != 0x00)
    {
        printf("  refused, %s\n", HciErrorName(Return[0]));
        return;

    }

    /* Results trickle in until the controller reports the inquiry finished */
    for (;;)
    {
        if (!ReadEvent(Radio, Event, &Length, (DWORD)(Params[3] * 1280 + HCI_EVENT_TIMEOUT)))
        {
            printf("  inquiry timed out, error %lu\n", GetLastError());
            return;

        }

        if (Length < 2)
            continue;

        if (Event[0] == HCI_EVENT_INQUIRY_COMPLETE)
        {
            printf("  inquiry complete\n");
            return;

        }

        PrintStrayEvent(Event, Length);

    }
}

int
main(
    int argc,
    char *argv[])
{
    HANDLE Radio;
    int Instance;
    int i;
    BOOL Inquiry;
    UCHAR InquiryLength;

    Instance = 0;
    Inquiry = FALSE;
    InquiryLength = 8;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-i") == 0)
            Inquiry = TRUE;

        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            Instance = atoi(argv[++i]);

        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            InquiryLength = (UCHAR)atoi(argv[++i]);

        else
        {
            printf("Usage: bthprobe [-d instance] [-i] [-l units]\n");
            printf("  -d  radio instance, default 0\n");
            printf("  -i  run a general inquiry after the probe\n");
            printf("  -l  inquiry length in 1.28 second units, default 8\n");
            return 1;

        }

    }

    Radio = OpenRadio(Instance);
    if (Radio == INVALID_HANDLE_VALUE)
    {
        printf("No radio at instance %d, error %lu\n", Instance, GetLastError());
        printf("The transport creates \\\\.\\FbtUsb00 once a dongle binds to fbtusb.sys\n");
        return 1;

    }

    if (!ProbeRadio(Radio))
    {
        printf("Probe failed\n");
        CloseHandle(Radio);
        return 1;

    }

    if (Inquiry)
        RunInquiry(Radio, InquiryLength);

    CloseHandle(Radio);

    return 0;
}
