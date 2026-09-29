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

/* The clock, reached by naming a location and then reading it */
#define CMOS_ADDRESS        0x0070
#define CMOS_DATA           0x0071
#define CMOS_LINE           8

/* The one bit of the keyboard controller that was never about keys */
#define SYSTEM_CONTROL      0x0092
#define SYSTEM_A20          0x02

/*
 * The part that handles power, which is where everything written since the
 * clock stopped being enough looks for a count of how long the machine has
 * been running. Where each of these answers is fixed by nothing: it is
 * whatever the tables the machine describes itself with say, and this is what
 * those tables say.
 */
#define POWER_EVENT         0x0400
#define POWER_CONTROL       0x0404
#define POWER_TIMER         0x0408
#define POWER_GENERAL       0x0420
#define POWER_GENERAL_LENGTH 4

/* The run of them below the general ones, which is claimed in one go */
#define POWER_FIRST         POWER_EVENT
#define POWER_COUNT         12

/* How fast the count runs, which is the same on every machine that has one */
#define POWER_TIMER_RATE    3579545

/* The bit of the control register saying the system is handling this itself */
#define POWER_TAKEN_OVER    0x0001

/*
 * What the clock's own three status registers mean. The one thing the clock
 * does besides hold the date is raise its line over and over at a rate the
 * guest picks, which is how anything works out how fast the processor runs:
 * it counts the processor's own ticks between two of these.
 */
#define CLOCK_RATE_MASK     0x0F
#define CLOCK_PERIODIC_ON   0x40
#define CLOCK_HAPPENED      0x40
#define CLOCK_RAISED        0x80

/* What the rate is counted down from, which no clock of this kind changes */
#define CLOCK_CRYSTAL       32768


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

typedef struct _CHIPSET_DEVICE
{
    RTVM_DEVICE Device;

    /* Where the clock is pointing, and what is behind it */
    UCHAR CmosAddress;
    UCHAR Cmos[CMOS_SIZE];

    /* How much memory the machine has, because the clock is asked for it */
    ULONG64 MemorySize;

    BOOLEAN A20Enabled;

    volatile LONG Stopping;

    /* The one that raises the clock's line over and over, and whether it is up */
    HANDLE Ticking;
    volatile LONG Raised;

    /* What the part handling power was last told, and where its count began */
    USHORT PowerStatus;
    USHORT PowerEnable;
    USHORT PowerControl;
    USHORT GeneralStatus;
    USHORT GeneralEnable;
    ULONG64 PowerStarted;
    ULONG64 PowerRate;

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
 * Raises the clock's line over and over, as often as the guest asked.
 *
 * @remarks
 * This is what everything written for a machine of this kind measures the
 * processor against: it counts the processor's own ticks between two of these
 * and works out how fast it runs from the difference. A clock that never
 * raises its line leaves that measurement waiting for a sample that never
 * arrives, and the whole system stops there before it has started.
 *
 * The line stays up until the guest reads the register that says what
 * happened, which is what a clock of this kind does and what lets a guest tell
 * one of these apart from anything else sharing the line.
 */
static
DWORD
WINAPI
ChipsetTicking(
    _In_ LPVOID Parameter)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Parameter;
    const RTVM_HOST_INTERFACE *Host = Chipset->Device.Host;

    while (InterlockedCompareExchange(&Chipset->Stopping, 0, 0) == 0)
    {
        ULONG Rate;
        ULONG Every;
        BOOLEAN Wanted;

        EnterCriticalSection(&Chipset->Lock);

        Wanted = (Chipset->Cmos[CMOS_STATUS_B] & CLOCK_PERIODIC_ON) != 0;
        Rate = Chipset->Cmos[CMOS_STATUS_A] & CLOCK_RATE_MASK;

        LeaveCriticalSection(&Chipset->Lock);

        /* Nothing asked for, so there is nothing to raise */
        if (!Wanted || (Rate == 0))
        {
            Sleep(10);
            continue;
        }

        /*
         * The rate names a division of the crystal rather than a frequency.
         * The slowest two are the two the counter starts at, which is why the
         * count below begins where it does.
         */
        {
            const ULONG Divided = CLOCK_CRYSTAL >> (Rate - 1);

            Every = (Divided != 0) ? (1000 / Divided) : 10;
        }

        if (Every == 0)
            Every = 1;

        Sleep(Every);

        if (InterlockedCompareExchange(&Chipset->Stopping, 0, 0) != 0)
            break;

        EnterCriticalSection(&Chipset->Lock);

        /* Still wanted, and not already waiting to be looked at */
        Wanted = (Chipset->Cmos[CMOS_STATUS_B] & CLOCK_PERIODIC_ON) != 0;

        if (Wanted)
            Chipset->Cmos[CMOS_STATUS_C] |= (CLOCK_HAPPENED | CLOCK_RAISED);

        LeaveCriticalSection(&Chipset->Lock);

        if (Wanted && (Host->SetInterruptLine != NULL) &&
            (InterlockedExchange(&Chipset->Raised, 1) == 0))
        {
            Host->SetInterruptLine(Host->Context, CMOS_LINE, TRUE);
        }
    }

    return 0;
}

/**
 * @brief
 * How far the count that never stops has got.
 *
 * @remarks
 * Worked out from the host's own counter rather than kept, because nothing
 * here runs at the rate this is meant to. What matters to a guest is that it
 * moves and that it moves at the stated rate: one that stood still would have
 * anything measuring itself against it measure forever.
 */
static
ULONG
ChipsetPowerTimer(
    _In_ PCHIPSET_DEVICE Chipset)
{
    LARGE_INTEGER Now;

    if ((Chipset->PowerRate == 0) || (Chipset->PowerStarted == 0))
        return 0;

    QueryPerformanceCounter(&Now);

    if ((ULONG64)Now.QuadPart <= Chipset->PowerStarted)
        return 0;

    return (ULONG)((((ULONG64)Now.QuadPart - Chipset->PowerStarted) *
                    POWER_TIMER_RATE) / Chipset->PowerRate);
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
ChipsetFillTime(
    _Inout_ PCHIPSET_DEVICE Chipset)
{
    SYSTEMTIME Time;

    GetLocalTime(&Time);

    Chipset->Cmos[CMOS_SECONDS] = ChipsetToPacked(Time.wSecond);
    Chipset->Cmos[CMOS_MINUTES] = ChipsetToPacked(Time.wMinute);
    Chipset->Cmos[CMOS_HOURS] = ChipsetToPacked(Time.wHour);
    Chipset->Cmos[CMOS_WEEKDAY] = ChipsetToPacked(Time.wDayOfWeek + 1);
    Chipset->Cmos[CMOS_DAY] = ChipsetToPacked(Time.wDay);
    Chipset->Cmos[CMOS_MONTH] = ChipsetToPacked(Time.wMonth);
    Chipset->Cmos[CMOS_YEAR] = ChipsetToPacked(Time.wYear % 100);
    Chipset->Cmos[CMOS_CENTURY] = ChipsetToPacked(Time.wYear / 100);
}

static
VOID
ChipsetFillCmos(
    _Inout_ PCHIPSET_DEVICE Chipset)
{
    ULONG64 Kilobytes;
    ULONG64 Extended;

    ChipsetFillTime(Chipset);

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

    Status = Host->ClaimPortRange(Host->Context, Device, CMOS_ADDRESS, 2);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, SYSTEM_CONTROL, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, POWER_FIRST,
                                  POWER_COUNT);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, POWER_GENERAL,
                                  POWER_GENERAL_LENGTH);
    if (!RTVM_SUCCESS(Status))
        return Status;

    /* The count starts the moment the machine does, and never stops */
    {
        LARGE_INTEGER Rate;
        LARGE_INTEGER Now;

        QueryPerformanceFrequency(&Rate);
        QueryPerformanceCounter(&Now);

        Chipset->PowerRate = (ULONG64)Rate.QuadPart;
        Chipset->PowerStarted = (ULONG64)Now.QuadPart;
    }

    ChipsetFillCmos(Chipset);

    /* The one that raises its line over and over once the guest asks */
    InterlockedExchange(&Chipset->Stopping, 0);
    Chipset->Ticking = CreateThread(NULL, 0, ChipsetTicking, Chipset, 0, NULL);

    if (Chipset->Ticking == NULL)
    {
        Host->Log(Host->Context, RtvmLogWarning,
                  "%s: its line will never be raised, the ticker would not start\n",
                  Device->Name);
    }

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: clock at %04x, gate at %04x, %llu MB reported\n",
              Device->Name,
              CMOS_ADDRESS,
              SYSTEM_CONTROL,
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

    if (Chipset->Ticking != NULL)
    {
        WaitForSingleObject(Chipset->Ticking, 2000);
        CloseHandle(Chipset->Ticking);
        Chipset->Ticking = NULL;
    }
}

static
VOID
RTVMAPI
ChipsetReset(
    _In_ PRTVM_DEVICE Device)
{
    PCHIPSET_DEVICE Chipset = (PCHIPSET_DEVICE)Device->DeviceContext;

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
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    UCHAR Result = 0xFF;
    BOOLEAN Dropping = FALSE;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Chipset->Lock);

    switch (Port)
    {
        case CMOS_ADDRESS:
            /* Reading this back was never guaranteed, and nothing relies on it */
            Result = Chipset->CmosAddress;
            break;

        case CMOS_DATA:
            /*
             * The clock is running, so what it says is worked out when it is
             * asked rather than written down once. A guest counting a menu
             * down watches the seconds, and a clock that answered with the
             * second the machine started in counts nothing down at all.
             */
            ChipsetFillTime(Chipset);

            Result = Chipset->Cmos[Chipset->CmosAddress & CMOS_ADDRESS_MASK];

            /*
             * Reading the third status register is how a guest finds out what
             * the clock wanted, and doing so clears it and lets the line go.
             * Until it is read the line stays up, which is what tells a guest
             * sharing that line that this is the one that raised it.
             */
            if ((Chipset->CmosAddress & CMOS_ADDRESS_MASK) == CMOS_STATUS_C)
            {
                Chipset->Cmos[CMOS_STATUS_C] = 0;
                Dropping = TRUE;
            }
            break;

        case SYSTEM_CONTROL:
            Result = Chipset->A20Enabled ? SYSTEM_A20 : 0;
            break;

        default:
            break;
    }

    /*
     * The part that handles power is read whole rather than a byte at a time,
     * because the count is four bytes and a guest reading it in halves would
     * get two halves of two different moments.
     */
    if ((Port >= POWER_FIRST) && (Port < (POWER_FIRST + POWER_COUNT)))
    {
        ULONG Whole = 0;

        switch (Port)
        {
            case POWER_EVENT:
                Whole = Chipset->PowerStatus;
                break;

            case POWER_EVENT + 2:
                Whole = Chipset->PowerEnable;
                break;

            case POWER_CONTROL:
                Whole = Chipset->PowerControl;
                break;

            case POWER_TIMER:
                Whole = ChipsetPowerTimer(Chipset);
                break;

            default:
                break;
        }

        LeaveCriticalSection(&Chipset->Lock);

        *Value = Whole;
        return RtvmOk;
    }

    if ((Port >= POWER_GENERAL) && (Port < (POWER_GENERAL + POWER_GENERAL_LENGTH)))
    {
        const ULONG Whole = (Port < (POWER_GENERAL + 2))
                          ? Chipset->GeneralStatus
                          : Chipset->GeneralEnable;

        LeaveCriticalSection(&Chipset->Lock);

        *Value = Whole;
        return RtvmOk;
    }

    LeaveCriticalSection(&Chipset->Lock);

    /* Let go outside the lock, because where the line goes is not this to hold */
    if (Dropping && (Host->SetInterruptLine != NULL) &&
        (InterlockedExchange(&Chipset->Raised, 0) != 0))
    {
        Host->SetInterruptLine(Host->Context, CMOS_LINE, FALSE);
    }

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

        default:
            break;
    }

    /*
     * The two halves of what has happened are cleared by writing back the
     * bits that are set, not by writing what they should become. Everything
     * else here is simply kept.
     */
    if ((Port >= POWER_FIRST) && (Port < (POWER_FIRST + POWER_COUNT)))
    {
        const USHORT Half = (USHORT)(Value & 0xFFFF);

        switch (Port)
        {
            case POWER_EVENT:
                Chipset->PowerStatus &= (USHORT)~Half;
                break;

            case POWER_EVENT + 2:
                Chipset->PowerEnable = Half;
                break;

            case POWER_CONTROL:
                Chipset->PowerControl = Half;

                if ((Half & POWER_TAKEN_OVER) != 0)
                {
                    Host->Log(Host->Context, RtvmLogTrace,
                              "%s: the system is handling power itself now\n",
                              Device->Name);
                }
                break;

            default:
                /* The count is not the guest's to set */
                break;
        }
    }
    else if ((Port >= POWER_GENERAL) &&
             (Port < (POWER_GENERAL + POWER_GENERAL_LENGTH)))
    {
        const USHORT Half = (USHORT)(Value & 0xFFFF);

        if (Port < (POWER_GENERAL + 2))
            Chipset->GeneralStatus &= (USHORT)~Half;
        else
            Chipset->GeneralEnable = Half;
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
