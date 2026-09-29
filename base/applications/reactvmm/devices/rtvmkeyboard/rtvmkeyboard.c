/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The controller that keys arrive through, and the gate it also holds
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * This is not optional in the way a display or a disk is. Something polling a
 * controller that is not there reads all ones off an empty bus, and all ones
 * says a key is waiting: the poll never ends, and it is usually done with
 * interrupts off, so the machine stops dead with no fault to show for it.
 *
 * Answering "nothing is waiting" is therefore the single most useful thing this
 * device does. Carrying keys is the second.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

#define KEYBOARD_DATA       0x0060
#define KEYBOARD_STATUS     0x0064   /* Read */
#define KEYBOARD_COMMAND    0x0064   /* Write */

#define KEYBOARD_LINE       1
#define MOUSE_LINE          12

/* What the status register says */
#define STATUS_OUTPUT_FULL  0x01
#define STATUS_INPUT_FULL   0x02
#define STATUS_SYSTEM       0x04
#define STATUS_COMMAND      0x08
#define STATUS_TIMEOUT      0x40
#define STATUS_PARITY       0x80

/* Commands the controller itself takes */
#define COMMAND_READ_CONFIG     0x20
#define COMMAND_WRITE_CONFIG    0x60
#define COMMAND_DISABLE_MOUSE   0xA7
#define COMMAND_ENABLE_MOUSE    0xA8
#define COMMAND_TEST_MOUSE      0xA9
#define COMMAND_SELF_TEST       0xAA
#define COMMAND_TEST_KEYBOARD   0xAB
#define COMMAND_DISABLE         0xAD
#define COMMAND_ENABLE          0xAE
#define COMMAND_READ_OUTPUT     0xD0
#define COMMAND_WRITE_OUTPUT    0xD1
#define COMMAND_RESET           0xFE

/* What the keyboard itself takes */
#define KEYBOARD_RESET          0xFF
#define KEYBOARD_ECHO           0xEE
#define KEYBOARD_IDENTIFY       0xF2
#define KEYBOARD_SET_RATE       0xF3
#define KEYBOARD_ENABLE         0xF4
#define KEYBOARD_DISABLE        0xF5
#define KEYBOARD_SET_LEDS       0xED
#define KEYBOARD_SET_SCANCODES  0xF0

/* What it answers with */
#define REPLY_ACKNOWLEDGE       0xFA
#define REPLY_SELF_TEST_OK      0xAA
#define REPLY_RESEND            0xFE

/* The bit of the output port that is not about keys at all */
#define OUTPUT_A20              0x02
#define OUTPUT_RESET            0x01

#define QUEUE_SIZE              32

/* TYPES **********************************************************************/

typedef struct _KEYBOARD_DEVICE
{
    RTVM_DEVICE Device;

    /* What the guest has not read yet */
    UCHAR Queue[QUEUE_SIZE];
    ULONG Count;
    ULONG Read;
    ULONG Write;

    UCHAR Status;
    UCHAR Config;
    UCHAR OutputPort;

    /* A command that is waiting for the byte that goes with it */
    UCHAR Pending;

    BOOLEAN KeyboardEnabled;
    BOOLEAN LineAsserted;

    /* Where keys come from, if anywhere */
    HANDLE Input;
    HANDLE Reader;
    volatile LONG Stopping;

    CRITICAL_SECTION Lock;
    CHAR Backend[MAX_PATH];
} KEYBOARD_DEVICE, *PKEYBOARD_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE KeyboardVtable;

/*
 * Enough of the first scan code set to carry what someone would type at a
 * machine coming up. A character arrives as the code for the key that would
 * have produced it, because that is the only thing a keyboard can say.
 */
static const UCHAR KeyboardCodes[128] =
{
    /* 0x00 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x08 */ 0x0E, 0x0F, 0x1C, 0,    0,    0x1C, 0,    0,
    /* 0x10 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x18 */ 0,    0,    0,    0x01, 0,    0,    0,    0,
    /* 0x20 */ 0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28,
    /* 0x28 */ 0x0A, 0x0B, 0x09, 0x0D, 0x33, 0x0C, 0x34, 0x35,
    /* 0x30 */ 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    /* 0x38 */ 0x09, 0x0A, 0x27, 0x27, 0x33, 0x0D, 0x34, 0x35,
    /* 0x40 */ 0x03, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,
    /* 0x48 */ 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
    /* 0x50 */ 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,
    /* 0x58 */ 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x07, 0x0C,
    /* 0x60 */ 0x29, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,
    /* 0x68 */ 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
    /* 0x70 */ 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,
    /* 0x78 */ 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x29, 0
};

/* FUNCTIONS ******************************************************************/

static
VOID
KeyboardSetLine(
    _Inout_ PKEYBOARD_DEVICE Keyboard)
{
    const RTVM_HOST_INTERFACE *Host = Keyboard->Device.Host;
    BOOLEAN Wanted;

    /* Only when there is something to read and the guest asked to be told */
    Wanted = (Keyboard->Count != 0) && ((Keyboard->Config & 0x01) != 0);

    if (Wanted == Keyboard->LineAsserted)
        return;

    Keyboard->LineAsserted = Wanted;

    if (Host->SetInterruptLine != NULL)
        Host->SetInterruptLine(Host->Context, KEYBOARD_LINE, Wanted);
}

static
VOID
KeyboardPut(
    _Inout_ PKEYBOARD_DEVICE Keyboard,
    _In_ UCHAR Value)
{
    if (Keyboard->Count >= QUEUE_SIZE)
        return;

    Keyboard->Queue[Keyboard->Write] = Value;
    Keyboard->Write = (Keyboard->Write + 1) % QUEUE_SIZE;
    Keyboard->Count++;

    Keyboard->Status |= STATUS_OUTPUT_FULL;
    KeyboardSetLine(Keyboard);
}

/* A character, as the press and release of whatever key would make it */
static
VOID
KeyboardType(
    _Inout_ PKEYBOARD_DEVICE Keyboard,
    _In_ UCHAR Character)
{
    UCHAR Code;

    if (Character >= RTL_NUMBER_OF(KeyboardCodes))
        return;

    Code = KeyboardCodes[Character];

    if (Code == 0)
        return;

    KeyboardPut(Keyboard, Code);
    KeyboardPut(Keyboard, (UCHAR)(Code | 0x80));
}

static
DWORD
WINAPI
KeyboardReader(
    _In_ LPVOID Parameter)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Parameter;
    UCHAR Buffer[32];
    DWORD Read;
    DWORD Index;

    while (InterlockedCompareExchange(&Keyboard->Stopping, 0, 0) == 0)
    {
        if (!ReadFile(Keyboard->Input, Buffer, sizeof(Buffer), &Read, NULL) ||
            (Read == 0))
        {
            break;
        }

        EnterCriticalSection(&Keyboard->Lock);

        for (Index = 0; Index < Read; Index++)
            KeyboardType(Keyboard, Buffer[Index]);

        LeaveCriticalSection(&Keyboard->Lock);
    }

    return 0;
}

/* THE DEVICE *****************************************************************/

static
RTVM_STATUS
RTVMAPI
KeyboardStart(
    _In_ PRTVM_DEVICE Device)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    Status = Host->ClaimPortRange(Host->Context, Device, KEYBOARD_DATA, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, KEYBOARD_STATUS, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    if (Keyboard->Input != INVALID_HANDLE_VALUE)
        Keyboard->Reader = CreateThread(NULL, 0, KeyboardReader, Keyboard, 0, NULL);

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: controller at %04x on line %u, %s\n",
              Device->Name,
              KEYBOARD_DATA,
              KEYBOARD_LINE,
              Keyboard->Backend);

    return RtvmOk;
}

static
VOID
RTVMAPI
KeyboardStop(
    _In_ PRTVM_DEVICE Device)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;

    InterlockedExchange(&Keyboard->Stopping, 1);

    if (Keyboard->Input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Keyboard->Input);
        Keyboard->Input = INVALID_HANDLE_VALUE;
    }

    if (Keyboard->Reader != NULL)
    {
        WaitForSingleObject(Keyboard->Reader, 2000);
        CloseHandle(Keyboard->Reader);
        Keyboard->Reader = NULL;
    }
}

static
VOID
RTVMAPI
KeyboardReset(
    _In_ PRTVM_DEVICE Device)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;

    Keyboard->Count = 0;
    Keyboard->Read = 0;
    Keyboard->Write = 0;
    Keyboard->Pending = 0;

    /*
     * Empty, and saying so. This is the value that matters: all ones would say
     * a key is waiting, and something polling for one would never stop.
     */
    Keyboard->Status = STATUS_SYSTEM;

    /* Interrupts on for the keyboard, and the translation everything expects */
    Keyboard->Config = 0x45;

    /* The gate starts closed, as it does everywhere else */
    Keyboard->OutputPort = 0x01;

    Keyboard->KeyboardEnabled = TRUE;
    Keyboard->LineAsserted = FALSE;
}

static
VOID
RTVMAPI
KeyboardDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;

    KeyboardStop(Device);
    DeleteCriticalSection(&Keyboard->Lock);
    free(Keyboard);
}

static
RTVM_STATUS
RTVMAPI
KeyboardIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;
    UCHAR Result = 0;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Keyboard->Lock);

    if (Port == KEYBOARD_STATUS)
    {
        Result = Keyboard->Status;
    }
    else
    {
        if (Keyboard->Count != 0)
        {
            Result = Keyboard->Queue[Keyboard->Read];
            Keyboard->Read = (Keyboard->Read + 1) % QUEUE_SIZE;
            Keyboard->Count--;
        }

        if (Keyboard->Count == 0)
            Keyboard->Status &= (UCHAR)~STATUS_OUTPUT_FULL;

        KeyboardSetLine(Keyboard);
    }

    LeaveCriticalSection(&Keyboard->Lock);

    *Value = Result;
    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
KeyboardIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PKEYBOARD_DEVICE Keyboard = (PKEYBOARD_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    UCHAR Byte = (UCHAR)(Value & 0xFF);

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Keyboard->Lock);

    if (Port == KEYBOARD_COMMAND)
    {
        switch (Byte)
        {
            case COMMAND_SELF_TEST:
                KeyboardPut(Keyboard, 0x55);
                break;

            case COMMAND_TEST_KEYBOARD:
            case COMMAND_TEST_MOUSE:
                KeyboardPut(Keyboard, 0x00);
                break;

            case COMMAND_READ_CONFIG:
                KeyboardPut(Keyboard, Keyboard->Config);
                break;

            case COMMAND_READ_OUTPUT:
                KeyboardPut(Keyboard, Keyboard->OutputPort);
                break;

            case COMMAND_DISABLE:
                Keyboard->KeyboardEnabled = FALSE;
                break;

            case COMMAND_ENABLE:
                Keyboard->KeyboardEnabled = TRUE;
                break;

            case COMMAND_DISABLE_MOUSE:
            case COMMAND_ENABLE_MOUSE:
                /* There is no second port, and refusing would only confuse */
                break;

            case COMMAND_RESET:
                Host->Log(Host->Context, RtvmLogWarning,
                          "%s: a reset was asked for and is not carried out\n",
                          Device->Name);
                break;

            case COMMAND_WRITE_CONFIG:
            case COMMAND_WRITE_OUTPUT:
                /* These take the byte that comes next */
                Keyboard->Pending = Byte;
                break;

            default:
                break;
        }

        LeaveCriticalSection(&Keyboard->Lock);
        return RtvmOk;
    }

    /* A byte for whichever command asked for one */
    if (Keyboard->Pending == COMMAND_WRITE_CONFIG)
    {
        Keyboard->Config = Byte;
        Keyboard->Pending = 0;
        KeyboardSetLine(Keyboard);

        LeaveCriticalSection(&Keyboard->Lock);
        return RtvmOk;
    }

    if (Keyboard->Pending == COMMAND_WRITE_OUTPUT)
    {
        const BOOLEAN Was = (Keyboard->OutputPort & OUTPUT_A20) != 0;
        const BOOLEAN Now = (Byte & OUTPUT_A20) != 0;

        Keyboard->OutputPort = Byte;
        Keyboard->Pending = 0;

        /*
         * The gate on the twenty first address line is held here as well as at
         * the other port, because this is the way it was reached first and
         * anything old enough still reaches it this way.
         */
        if (Was != Now)
        {
            Host->Log(Host->Context, RtvmLogTrace,
                      "%s: the gate is %s\n",
                      Device->Name, Now ? "open" : "closed");
        }

        LeaveCriticalSection(&Keyboard->Lock);
        return RtvmOk;
    }

    /* Otherwise it is meant for the keyboard itself */
    switch (Byte)
    {
        case KEYBOARD_RESET:
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            KeyboardPut(Keyboard, REPLY_SELF_TEST_OK);
            break;

        case KEYBOARD_ECHO:
            KeyboardPut(Keyboard, KEYBOARD_ECHO);
            break;

        case KEYBOARD_IDENTIFY:
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            KeyboardPut(Keyboard, 0xAB);
            KeyboardPut(Keyboard, 0x83);
            break;

        case KEYBOARD_ENABLE:
            Keyboard->KeyboardEnabled = TRUE;
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            break;

        case KEYBOARD_DISABLE:
            Keyboard->KeyboardEnabled = FALSE;
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            break;

        case KEYBOARD_SET_LEDS:
        case KEYBOARD_SET_RATE:
        case KEYBOARD_SET_SCANCODES:
            /* Each takes a byte after it, which is taken and ignored */
            Keyboard->Pending = Byte;
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            break;

        default:
            /*
             * Anything else is acknowledged rather than refused. A keyboard
             * that says resend to something it does not know sends the sender
             * round again, and it will not know it the second time either.
             */
            KeyboardPut(Keyboard, REPLY_ACKNOWLEDGE);
            break;
    }

    LeaveCriticalSection(&Keyboard->Lock);
    return RtvmOk;
}

static const RTVM_DEVICE_VTABLE KeyboardVtable =
{
    sizeof(KeyboardVtable),
    KeyboardStart,
    KeyboardStop,
    KeyboardReset,
    KeyboardDestroy,
    KeyboardIoRead,
    KeyboardIoWrite,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

static
PCSTR
KeyboardSetting(
    _In_opt_ PCSTR Parameters,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Walk = Parameters;

    while ((Walk != NULL) && (*Walk != '\0'))
    {
        if ((_strnicmp(Walk, Name, Length) == 0) &&
            ((Walk[Length] == '=') || (Walk[Length] == ',') || (Walk[Length] == '\0')))
        {
            return (Walk[Length] == '=') ? &Walk[Length + 1] : &Walk[Length];
        }

        Walk = strchr(Walk, ',');
        if (Walk != NULL)
            Walk++;
    }

    return NULL;
}

static
RTVM_STATUS
RTVMAPI
KeyboardCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PKEYBOARD_DEVICE Keyboard;
    PCSTR Value;

    Keyboard = (PKEYBOARD_DEVICE)calloc(1, sizeof(*Keyboard));
    if (Keyboard == NULL)
        return RtvmNoMemory;

    Keyboard->Device.Size = sizeof(Keyboard->Device);
    Keyboard->Device.Vtable = &KeyboardVtable;
    Keyboard->Device.Host = Host;
    Keyboard->Device.DeviceContext = Keyboard;
    Keyboard->Input = INVALID_HANDLE_VALUE;
    StringCchCopyA(Keyboard->Device.Name, sizeof(Keyboard->Device.Name), "keyboard");

    Value = KeyboardSetting(Parameters, "pipe");
    if ((Value != NULL) && (*Value != '\0'))
    {
        CHAR Name[MAX_PATH];
        CHAR Path[MAX_PATH];
        SIZE_T Index = 0;

        while ((Index + 1 < sizeof(Path)) && (Value[Index] != '\0') &&
               (Value[Index] != ','))
        {
            Path[Index] = Value[Index];
            Index++;
        }

        Path[Index] = '\0';
        StringCchPrintfA(Name, sizeof(Name), "\\\\.\\pipe\\%s", Path);

        Keyboard->Input = CreateNamedPipeA(Name,
                                          PIPE_ACCESS_INBOUND,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                          1,
                                          0,
                                          4096,
                                          0,
                                          NULL);
        if (Keyboard->Input != INVALID_HANDLE_VALUE)
        {
            StringCchPrintfA(Keyboard->Backend, sizeof(Keyboard->Backend),
                             "keys from %s", Name);
        }
    }

    if (Keyboard->Input == INVALID_HANDLE_VALUE)
    {
        StringCchCopyA(Keyboard->Backend, sizeof(Keyboard->Backend),
                       "nothing typing at it");
    }

    InitializeCriticalSection(&Keyboard->Lock);
    KeyboardReset(&Keyboard->Device);

    *Device = &Keyboard->Device;
    return RtvmOk;
}

/* WHAT THIS MODULE HAS *******************************************************/

static const RTVM_DEVICE_CLASS KeyboardClasses[] =
{
    {
        "keyboard",
        "Keyboard controller, and the gate it holds",
        KeyboardCreate
    }
};

static const RTVM_DEVICE_MODULE KeyboardModule =
{
    sizeof(KeyboardModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmkeyboard",
    "Keyboards",
    RTL_NUMBER_OF(KeyboardClasses),
    KeyboardClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &KeyboardModule;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/* EOF */
