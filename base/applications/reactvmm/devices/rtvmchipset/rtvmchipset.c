/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The parts of the board that were never on a card
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The interval timer, the clock that keeps running with the machine off, and
 * the gate that decides whether an address may carry its twenty first bit.
 * None of them is interesting on its own and nothing boots without all three.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

/* The interval timer: three counters and the register that picks one */
#define PIT_COUNTER0        0x0040
#define PIT_COUNTER1        0x0041
#define PIT_COUNTER2        0x0042
#define PIT_CONTROL         0x0043
#define PIT_LINE            0

/* What it counts at, which is a third of the old colour burst and always has been */
#define PIT_FREQUENCY       1193182

/* The clock, reached by naming a location and then reading it */
#define CMOS_ADDRESS        0x0070
#define CMOS_DATA           0x0071
#define CMOS_LINE           8

/* The one bit of the keyboard controller that was never about keys */
#define SYSTEM_CONTROL      0x0092
#define SYSTEM_A20          0x02

/* The port a machine writes to say which of the two chips it is talking to */
#define PIT_SELECT_SHIFT    6
#define PIT_ACCESS_SHIFT    4
#define PIT_ACCESS_LATCH    0
#define PIT_ACCESS_LOW      1
#define PIT_ACCESS_HIGH     2
#define PIT_ACCESS_BOTH     3

/* Which of the locations behind the clock mean something */
#define CMOS_SECONDS        0x00
#define CMOS_MINUTES        0x02
#define CMOS_HOURS          0x04
#define CMOS_WEEKDAY        0x06
#define CMOS_DAY            0x07
#define CMOS_MONTH          0x08
#define CMOS_YEAR           0x09
#define CMOS_STATUS_A       0x0A
#define CMOS_STATUS_B       0x0B
#define CMOS_STATUS_C       0x0C
#define CMOS_STATUS_D       0x0D
#define CMOS_FLOPPY_TYPE    0x10
#define CMOS_EQUIPMENT      0x14
#define CMOS_BASE_LOW       0x15
#define CMOS_BASE_HIGH      0x16
#define CMOS_EXTENDED_LOW   0x17
#define CMOS_EXTENDED_HIGH  0x18
#define CMOS_EXTENDED2_LOW  0x30
#define CMOS_EXTENDED2_HIGH 0x31
#define CMOS_CENTURY        0x32
#define CMOS_MEMORY_HIGH_LOW  0x34
#define CMOS_MEMORY_HIGH_HIGH 0x35

#define CMOS_SIZE           128

/* Where the address register's top bit says not to interrupt */
#define CMOS_ADDRESS_MASK   0x7F

/* TYPES **********************************************************************/

typedef struct _PIT_COUNTER
{
    USHORT Reload;
    USHORT Latched;
    UCHAR Access;
    UCHAR Mode;
    BOOLEAN Latch;
    BOOLEAN ReadHigh;
    BOOLEAN WriteHigh;
    BOOLEAN Running;
} PIT_COUNTER, *PPIT_COUNTER;

typedef struct _CHIPSET_DEVICE
{
    RTVM_DEVICE Device;

    PIT_COUNTER Counter[3];

    /* Where the clock is pointing, and what is behind it */
    UCHAR CmosAddress;
    UCHAR Cmos[CMOS_SIZE];

    /* How much memory the machine has, because the clock is asked for it */
    ULONG64 MemorySize;

    BOOLEAN A20Enabled;

    /* The tick, kept by a thread because nothing else here has a clock */
    HANDLE Ticker;
    volatile LONG Stopping;
    ULONG TickHertz;
    volatile LONG LineUp;

    CRITICAL_SECTION Lock;
} CHIPSET_DEVICE, *PCHIPSET_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE ChipsetVtable;

/* FUNCTIONS ******************************************************************/

/* The clock keeps its numbers as two digits a byte, not as a count */
static
UCHAR
ChipsetToPacked(
    _In_ ULONG Value)
{
    return (UCHAR)(((Value / 10) << 4) | (Value % 10));
}

/**
 * @brief
 * Fills in what the clock holds, from the time the machine was started.
 *
 * @remarks
 * The memory sizes live here too. They were put behind the clock because it
 * was the only thing on the board that kept anything with the power off, and
 * every operating system since has looked for them there.
 */
static
VOID
ChipsetFillCmos(
    _Inout_ PCHIPSET_DEVICE Chipset)
{
    SYSTEMTIME Time;
    ULONG64 Kilobytes;
    ULONG64 Extended;

    GetLocalTime(&Time);

    Chipset->Cmos[CMOS_SECONDS] = ChipsetToPacked(Time.wSecond);
    Chipset->Cmos[CMOS_MINUTES] = ChipsetToPacked(Time.wMinute);
    Chipset->Cmos[CMOS_HOURS] = ChipsetToPacked(Time.wHour);
    Chipset->Cmos[CMOS_WEEKDAY] = ChipsetToPacked(Time.wDayOfWeek + 1);
    Chipset->Cmos[CMOS_DAY] = ChipsetToPacked(Time.wDay);
    Chipset->Cmos[CMOS_MONTH] = ChipsetToPacked(Time.wMonth);
    Chipset->Cmos[CMOS_YEAR] = ChipsetToPacked(Time.wYear % 100);
    Chipset->Cmos[CMOS_CENTURY] = ChipsetToPacked(Time.wYear / 100);

    /* Counting in the usual way, and the clock is running */
    Chipset->Cmos[CMOS_STATUS_A] = 0x26;
    Chipset->Cmos[CMOS_STATUS_B] = 0x02;
    Chipset->Cmos[CMOS_STATUS_C] = 0x00;
    Chipset->Cmos[CMOS_STATUS_D] = 0x80;

    /* No floppy drives of any kind */
    Chipset->Cmos[CMOS_FLOPPY_TYPE] = 0x00;

    /* A display that is already in the mode it will stay in, and a disk */
    Chipset->Cmos[CMOS_EQUIPMENT] = 0x01;

    /* The first six hundred and forty kilobytes */
    Chipset->Cmos[CMOS_BASE_LOW] = 640 & 0xFF;
    Chipset->Cmos[CMOS_BASE_HIGH] = (640 >> 8) & 0xFF;

    /*
     * What there is above the first megabyte. The field only reaches sixty
     * three megabytes, so it is held there and the wider pair below carries
     * the real number, which is what anything written since will read.
     */
    Kilobytes = (Chipset->MemorySize > (1024 * 1024))
              ? ((Chipset->MemorySize - (1024 * 1024)) / 1024)
              : 0;

    Extended = (Kilobytes > 0xFC00) ? 0xFC00 : Kilobytes;

    Chipset->Cmos[CMOS_EXTENDED_LOW] = (UCHAR)(Extended & 0xFF);
    Chipset->Cmos[CMOS_EXTENDED_HIGH] = (UCHAR)((Extended >> 8) & 0xFF);
    Chipset->Cmos[CMOS_EXTENDED2_LOW] = (UCHAR)(Extended & 0xFF);
    Chipset->Cmos[CMOS_EXTENDED2_HIGH] = (UCHAR)((Extended >> 8) & 0xFF);

    /* Anything past sixteen megabytes, counted in sixty four kilobyte pieces */
    if (Chipset->MemorySize > (16 * 1024 * 1024))
    {
        ULONG64 Blocks = (Chipset->MemorySize - (16 * 1024 * 1024)) / (64 * 1024);

        Chipset->Cmos[CMOS_MEMORY_HIGH_LOW] = (UCHAR)(Blocks & 0xFF);
        Chipset->Cmos[CMOS_MEMORY_HIGH_HIGH] = (UCHAR)((Blocks >> 8) & 0xFF);
    }
}

/**
 * @brief
 * Holds the timer's line up for as long as nothing has taken the interrupt.
 *
 * @remarks
 * The counter is not really counting: what matters to anything waiting on it
 * is that the line goes up at about the right rate, and a thread asleep for
 * the right number of milliseconds does that without burning a processor
 * pretending to be a crystal.
 */
static
DWORD
WINAPI
ChipsetTicker(
    _In_ LPVOID Parameter)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Parameter;
    const RTVM_HOST_INTERFACE *Host = Chipset->Device.Host;

    while (InterlockedCompareExchange(&Chipset->Stopping, 0, 0) == 0)
    {
        ULONG Hertz = Chipset->TickHertz;
        DWORD Wait;

        if (Hertz == 0)
            Hertz = 18;

        Wait = 1000 / Hertz;
        if (Wait == 0)
            Wait = 1;

        Sleep(Wait);

        if (InterlockedCompareExchange(&Chipset->Stopping, 0, 0) != 0)
            break;

        /*
         * Raised and let go at once. The controller latches the edge, so a
         * guest that is slow to answer still gets exactly one interrupt
         * rather than a line held up until it looks.
         */
        Host->SetInterruptLine(Host->Context, PIT_LINE, TRUE);
        Host->SetInterruptLine(Host->Context, PIT_LINE, FALSE);
    }

    return 0;
}

/* How often counter zero was set up to fire */
static
VOID
ChipsetRateChanged(
    _Inout_ PCHIPSET_DEVICE Chipset)
{
    ULONG Reload = Chipset->Counter[0].Reload;

    /* Zero means the whole range, which is the slowest it will go */
    if (Reload == 0)
        Reload = 65536;

    Chipset->TickHertz = PIT_FREQUENCY / Reload;

    if (Chipset->TickHertz == 0)
        Chipset->TickHertz = 1;

    /* Faster than this costs more in exits than it is worth to anything */
    if (Chipset->TickHertz > 1000)
        Chipset->TickHertz = 1000;
}

/* THE DEVICE *****************************************************************/

static
RTVM_STATUS
RTVMAPI
ChipsetStart(
    _In_ PRTVM_DEVICE Device)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    Status = Host->ClaimPortRange(Host->Context, Device, PIT_COUNTER0, 4);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, CMOS_ADDRESS, 2);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, SYSTEM_CONTROL, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    ChipsetFillCmos(Chipset);

    Chipset->Ticker = CreateThread(NULL, 0, ChipsetTicker, Chipset, 0, NULL);
    if (Chipset->Ticker == NULL)
    {
        Host->Log(Host->Context, RtvmLogWarning,
                  "%s: nothing will keep time, the ticker would not start\n",
                  Device->Name);
    }

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: timer at %04x on line %u, clock at %04x, %llu MB reported\n",
              Device->Name,
              PIT_COUNTER0,
              PIT_LINE,
              CMOS_ADDRESS,
              Chipset->MemorySize / (1024 * 1024));

    return RtvmOk;
}

static
VOID
RTVMAPI
ChipsetStop(
    _In_ PRTVM_DEVICE Device)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;

    InterlockedExchange(&Chipset->Stopping, 1);

    if (Chipset->Ticker != NULL)
    {
        WaitForSingleObject(Chipset->Ticker, 2000);
        CloseHandle(Chipset->Ticker);
        Chipset->Ticker = NULL;
    }
}

static
VOID
RTVMAPI
ChipsetReset(
    _In_ PRTVM_DEVICE Device)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Chipset->Counter); Index++)
    {
        memset(&Chipset->Counter[Index], 0, sizeof(Chipset->Counter[Index]));
        Chipset->Counter[Index].Access = PIT_ACCESS_BOTH;
    }

    /* What the firmware sets it to, and what it is left at if nothing does */
    Chipset->Counter[0].Reload = 0;
    Chipset->TickHertz = 18;

    Chipset->CmosAddress = 0;

    /*
     * Closed, the way it comes up. Something has to open it before an address
     * with its twenty first bit set means what it says, and an operating
     * system that is going to use memory above a megabyte opens it early.
     */
    Chipset->A20Enabled = FALSE;
}

static
VOID
RTVMAPI
ChipsetDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;

    ChipsetStop(Device);
    DeleteCriticalSection(&Chipset->Lock);
    free(Chipset);
}

static
RTVM_STATUS
RTVMAPI
ChipsetIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;
    UCHAR Result = 0xFF;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Chipset->Lock);

    switch (Port)
    {
        case PIT_COUNTER0:
        case PIT_COUNTER1:
        case PIT_COUNTER2:
        {
            PPIT_COUNTER Counter = &Chipset->Counter[Port - PIT_COUNTER0];
            USHORT Reading = Counter->Latch ? Counter->Latched : Counter->Reload;

            switch (Counter->Access)
            {
                case PIT_ACCESS_LOW:
                    Result = (UCHAR)(Reading & 0xFF);
                    break;

                case PIT_ACCESS_HIGH:
                    Result = (UCHAR)(Reading >> 8);
                    break;

                default:
                    /* Both halves, and which one is next alternates */
                    if (Counter->ReadHigh)
                    {
                        Result = (UCHAR)(Reading >> 8);
                        Counter->Latch = FALSE;
                    }
                    else
                    {
                        Result = (UCHAR)(Reading & 0xFF);
                    }

                    Counter->ReadHigh = !Counter->ReadHigh;
                    break;
            }
            break;
        }

        case CMOS_ADDRESS:
            /* Reading this back was never guaranteed, and nothing relies on it */
            Result = Chipset->CmosAddress;
            break;

        case CMOS_DATA:
            Result = Chipset->Cmos[Chipset->CmosAddress & CMOS_ADDRESS_MASK];

            /*
             * Reading the third status register is how a guest finds out what
             * the clock wanted, and doing so clears it.
             */
            if ((Chipset->CmosAddress & CMOS_ADDRESS_MASK) == CMOS_STATUS_C)
                Chipset->Cmos[CMOS_STATUS_C] = 0;
            break;

        case SYSTEM_CONTROL:
            Result = Chipset->A20Enabled ? SYSTEM_A20 : 0;
            break;
    }

    LeaveCriticalSection(&Chipset->Lock);

    *Value = Result;
    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
ChipsetIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    UCHAR Byte = (UCHAR)(Value & 0xFF);

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Chipset->Lock);

    switch (Port)
    {
        case PIT_COUNTER0:
        case PIT_COUNTER1:
        case PIT_COUNTER2:
        {
            ULONG Which = Port - PIT_COUNTER0;
            PPIT_COUNTER Counter = &Chipset->Counter[Which];

            switch (Counter->Access)
            {
                case PIT_ACCESS_LOW:
                    Counter->Reload = (USHORT)((Counter->Reload & 0xFF00) | Byte);
                    break;

                case PIT_ACCESS_HIGH:
                    Counter->Reload = (USHORT)((Counter->Reload & 0x00FF) | (Byte << 8));
                    break;

                default:
                    if (Counter->WriteHigh)
                        Counter->Reload = (USHORT)((Counter->Reload & 0x00FF) | (Byte << 8));
                    else
                        Counter->Reload = (USHORT)((Counter->Reload & 0xFF00) | Byte);

                    Counter->WriteHigh = !Counter->WriteHigh;
                    break;
            }

            Counter->Running = TRUE;

            if (Which == 0)
                ChipsetRateChanged(Chipset);
            break;
        }

        case PIT_CONTROL:
        {
            ULONG Which = (Byte >> PIT_SELECT_SHIFT) & 3;
            ULONG Access = (Byte >> PIT_ACCESS_SHIFT) & 3;

            /* The fourth is not a counter, it is a way of asking about them */
            if (Which == 3)
                break;

            if (Access == PIT_ACCESS_LATCH)
            {
                /* Freeze what it says now, so it can be read without moving */
                Chipset->Counter[Which].Latched = Chipset->Counter[Which].Reload;
                Chipset->Counter[Which].Latch = TRUE;
                Chipset->Counter[Which].ReadHigh = FALSE;
                break;
            }

            Chipset->Counter[Which].Access = (UCHAR)Access;
            Chipset->Counter[Which].Mode = (UCHAR)((Byte >> 1) & 7);
            Chipset->Counter[Which].ReadHigh = FALSE;
            Chipset->Counter[Which].WriteHigh = FALSE;
            Chipset->Counter[Which].Latch = FALSE;
            break;
        }

        case CMOS_ADDRESS:
            Chipset->CmosAddress = Byte;
            break;

        case CMOS_DATA:
        {
            UCHAR Where = Chipset->CmosAddress & CMOS_ADDRESS_MASK;

            /* The parts that describe the machine are not the guest's to change */
            if ((Where >= CMOS_STATUS_A) && (Where <= CMOS_STATUS_D))
                Chipset->Cmos[Where] = Byte;
            else if (Where > CMOS_STATUS_D)
                Chipset->Cmos[Where] = Byte;
            break;
        }

        case SYSTEM_CONTROL:
        {
            BOOLEAN Wanted = (Byte & SYSTEM_A20) != 0;

            if (Wanted != Chipset->A20Enabled)
            {
                Chipset->A20Enabled = Wanted;

                Host->Log(Host->Context, RtvmLogTrace,
                          "%s: the gate is %s\n",
                          Device->Name,
                          Wanted ? "open" : "closed");
            }

            /*
             * The low bit resets the machine. Nothing here acts on it yet, and
             * saying so is better than appearing to have done it.
             */
            if (Byte & 0x01)
            {
                Host->Log(Host->Context, RtvmLogWarning,
                          "%s: a reset was asked for and is not carried out\n",
                          Device->Name);
            }
            break;
        }
    }

    LeaveCriticalSection(&Chipset->Lock);
    return RtvmOk;
}

static const RTVM_DEVICE_VTABLE ChipsetVtable =
{
    sizeof(ChipsetVtable),
    ChipsetStart,
    ChipsetStop,
    ChipsetReset,
    ChipsetDestroy,
    ChipsetIoRead,
    ChipsetIoWrite,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

static
PCSTR
ChipsetSetting(
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
ChipsetCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PCHIPSET_DEVICE Chipset;
    PCSTR Value;

    Chipset = (PCHIPSET_DEVICE)calloc(1, sizeof(*Chipset));
    if (Chipset == NULL)
        return RtvmNoMemory;

    Chipset->Device.Size = sizeof(Chipset->Device);
    Chipset->Device.Vtable = &ChipsetVtable;
    Chipset->Device.Host = Host;
    Chipset->Device.DeviceContext = Chipset;
    StringCchCopyA(Chipset->Device.Name, sizeof(Chipset->Device.Name), "chipset");

    /*
     * How much memory to say the machine has. The manager knows and the guest
     * asks the clock, so the number has to be passed in rather than guessed.
     */
    Chipset->MemorySize = 128ull * 1024 * 1024;

    Value = ChipsetSetting(Parameters, "memory");
    if ((Value != NULL) && (*Value != '\0'))
        Chipset->MemorySize = (ULONG64)strtoul(Value, NULL, 0) * 1024 * 1024;

    InitializeCriticalSection(&Chipset->Lock);
    ChipsetReset(&Chipset->Device);

    *Device = &Chipset->Device;
    return RtvmOk;
}

/* WHAT THIS MODULE HAS *******************************************************/

static const RTVM_DEVICE_CLASS ChipsetClasses[] =
{
    {
        "chipset",
        "Interval timer, clock and the gate on the twenty first address line",
        ChipsetCreate
    }
};

static const RTVM_DEVICE_MODULE ChipsetModule =
{
    sizeof(ChipsetModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmchipset",
    "Board parts",
    RTL_NUMBER_OF(ChipsetClasses),
    ChipsetClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &ChipsetModule;
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
