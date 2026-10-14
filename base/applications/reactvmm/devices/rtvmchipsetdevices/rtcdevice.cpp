/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The clock that keeps running with the machine off
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Reached by naming a location on one port and reading or writing it on the
 * next. The first few locations are the date and time, the next four say how
 * the clock behaves, and the rest are whatever the firmware left there.
 */

#include "rtcdevice.h"

namespace rtvm
{

#define RTC_INDEX           0x0070
#define RTC_DATA            0x0071
#define RTC_INDEX_MASK      0x7F

/* The line the clock is wired to */
#define RTC_LINE            8

/* Where the date and time are kept */
#define RTC_SECONDS         0x00
#define RTC_MINUTES         0x02
#define RTC_HOURS           0x04
#define RTC_WEEKDAY         0x06
#define RTC_DAY             0x07
#define RTC_MONTH           0x08
#define RTC_YEAR            0x09
#define RTC_CENTURY         0x32

#define RTC_STATUS_A        0x0A
#define RTC_STATUS_B        0x0B
#define RTC_STATUS_C        0x0C
#define RTC_STATUS_D        0x0D

/* What the bits of the status registers mean */
#define RTC_A_RATE_MASK     0x0F
#define RTC_B_24_HOUR       0x02
#define RTC_B_BINARY        0x04
#define RTC_B_PERIODIC      0x40
#define RTC_B_SET           0x80
#define RTC_C_PERIODIC      0x40
#define RTC_C_RAISED        0x80
#define RTC_D_VALID         0x80
#define RTC_HOUR_PM         0x80

/* The usual divider and a 1024 Hz rate, with the clock counting in 24 hours */
#define RTC_A_DEFAULT       0x26
#define RTC_B_DEFAULT       RTC_B_24_HOUR

/* What the rate is divided from */
#define RTC_CRYSTAL         32768

/* WHAT IT IS *****************************************************************/

RtcDevice::RtcDevice()
{
    InitializeCriticalSection(&m_Lock);
}

RtcDevice::~RtcDevice()
{
    PowerOff(VDEV_STATE_NONE);
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP
RtcDevice::QueryInterface(
    _In_ REFIID Interface,
    _Outptr_ void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVirtualDevice))
    {
        *Object = static_cast<IVirtualDevice *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVndIoPortHandler))
    {
        *Object = static_cast<IVndIoPortHandler *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

/* The firmware is asked for what it left behind, and may not be there to ask */
STDMETHODIMP
RtcDevice::GetDependencies(
    _In_opt_ void *Repository,
    _Out_ ULONG *Count,
    _Outptr_ GUID **Services,
    _Out_ ULONG *Required)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmIoApic,
        &IID_IVmBios
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted), 1,
                               Count, Services, Required);
}

STDMETHODIMP
RtcDevice::StartReservingResources(
    _In_opt_ void *Repository,
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(Repository);
    UNREFERENCED_PARAMETER(State);

    return ReservePorts(RTC_INDEX, RTC_DATA, this);
}

/* WHAT IT HOLDS **************************************************************/

/**
 * @brief
 * Fills the clock the way it would be found with the power coming on.
 *
 * @remarks
 * Everything past the status registers is the firmware's, so that is asked for
 * first and the parts that belong to the clock itself are laid over it.
 */
void RtcDevice::Load()
{
    IVmBios *Firmware = nullptr;

    memset(m_Cmos, 0, sizeof(m_Cmos));

    if (SUCCEEDED(FindService(IID_IVmBios, reinterpret_cast<void **>(&Firmware))))
    {
        Firmware->GetDefaultCmosValues(m_Cmos);
        Firmware->Release();
    }

    m_Cmos[RTC_STATUS_A] = RTC_A_DEFAULT;
    m_Cmos[RTC_STATUS_B] = RTC_B_DEFAULT;
    m_Cmos[RTC_STATUS_C] = 0;
    m_Cmos[RTC_STATUS_D] = RTC_D_VALID;
    m_Address = 0;
}

/* A number the way the guest asked to see them, packed decimal or not */
UCHAR
RtcDevice::Encode(
    _In_ ULONG Value) const
{
    if (m_Cmos[RTC_STATUS_B] & RTC_B_BINARY)
        return (UCHAR)Value;

    return (UCHAR)(((Value / 10) << 4) | (Value % 10));
}

/**
 * @brief
 * Puts the host's time into the date and time locations.
 *
 * @remarks
 * Done every time one of them is read rather than kept and advanced. A guest
 * waiting for the seconds to change is counting something down, and a clock
 * that never moved would leave it counting forever.
 */
void RtcDevice::Stamp()
{
    SYSTEMTIME Now;
    ULONG Hour;
    UCHAR Pm = 0;

    /* The guest is setting it, so what it wrote is left alone */
    if (m_Cmos[RTC_STATUS_B] & RTC_B_SET)
        return;

    GetLocalTime(&Now);

    Hour = Now.wHour;

    if (!(m_Cmos[RTC_STATUS_B] & RTC_B_24_HOUR))
    {
        if (Hour >= 12)
            Pm = RTC_HOUR_PM;

        Hour %= 12;

        if (Hour == 0)
            Hour = 12;
    }

    m_Cmos[RTC_SECONDS] = Encode(Now.wSecond);
    m_Cmos[RTC_MINUTES] = Encode(Now.wMinute);
    m_Cmos[RTC_HOURS] = Encode(Hour) | Pm;
    m_Cmos[RTC_WEEKDAY] = Encode(Now.wDayOfWeek + 1);
    m_Cmos[RTC_DAY] = Encode(Now.wDay);
    m_Cmos[RTC_MONTH] = Encode(Now.wMonth);
    m_Cmos[RTC_YEAR] = Encode(Now.wYear % 100);
    m_Cmos[RTC_CENTURY] = Encode(Now.wYear / 100);
}

STDMETHODIMP
RtcDevice::Reset(
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    EnterCriticalSection(&m_Lock);
    Load();
    LeaveCriticalSection(&m_Lock);

    Lower();
    return S_OK;
}

/* RAISING ITS LINE ***********************************************************/

STDMETHODIMP
RtcDevice::PowerOnCold(
    _In_ VDEV_STATE State)
{
    if (m_Lines == nullptr)
        FindService(IID_IVmIoApic, reinterpret_cast<void **>(&m_Lines));

    Reset(State);

    if (m_Thread != nullptr)
        return S_OK;

    InterlockedExchange(&m_Stopping, 0);

    m_Thread = CreateThread(nullptr, 0, Ticking, this, 0, nullptr);

    return (m_Thread != nullptr) ? S_OK : E_FAIL;
}

STDMETHODIMP
RtcDevice::PowerOff(
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    if (m_Thread != nullptr)
    {
        InterlockedExchange(&m_Stopping, 1);
        WaitForSingleObject(m_Thread, 2000);
        CloseHandle(m_Thread);
        m_Thread = nullptr;
    }

    Lower();

    if (m_Lines != nullptr)
    {
        m_Lines->Release();
        m_Lines = nullptr;
    }

    return S_OK;
}

DWORD
WINAPI
RtcDevice::Ticking(
    _In_ LPVOID Parameter)
{
    auto *Self = static_cast<RtcDevice *>(Parameter);

    while (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) == 0)
    {
        EnterCriticalSection(&Self->m_Lock);

        const bool Wanted = (Self->m_Cmos[RTC_STATUS_B] & RTC_B_PERIODIC) != 0;
        const ULONG Rate = Self->m_Cmos[RTC_STATUS_A] & RTC_A_RATE_MASK;

        LeaveCriticalSection(&Self->m_Lock);

        if (!Wanted || (Rate == 0))
        {
            Sleep(10);
            continue;
        }

        /* Rates one and two are the same as eight and nine */
        const ULONG Shift = (Rate < 3) ? (Rate + 7) : Rate;
        const ULONG Hertz = RTC_CRYSTAL >> (Shift - 1);
        DWORD Wait = (Hertz != 0) ? (1000u / Hertz) : 10;

        if (Wait == 0)
            Wait = 1;

        Sleep(Wait);

        if (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) != 0)
            break;

        Self->Tick();
    }

    return 0;
}

/**
 * @brief
 * One period gone by.
 *
 * @remarks
 * The line stays up until the guest reads the third status register. That is
 * how a guest sharing the line tells that this is what raised it.
 */
void RtcDevice::Tick()
{
    EnterCriticalSection(&m_Lock);

    const bool Wanted = (m_Cmos[RTC_STATUS_B] & RTC_B_PERIODIC) != 0;

    if (Wanted)
        m_Cmos[RTC_STATUS_C] |= (RTC_C_PERIODIC | RTC_C_RAISED);

    LeaveCriticalSection(&m_Lock);

    if (Wanted && (m_Lines != nullptr) &&
        (InterlockedExchange(&m_Raised, 1) == 0))
    {
        m_Lines->AssertIrq(RTC_LINE, VDEV_IRQ_SOURCE_ONLY);
    }
}

/* Lets the line go, if it was up */
void RtcDevice::Lower()
{
    if ((m_Lines != nullptr) && (InterlockedExchange(&m_Raised, 0) != 0))
        m_Lines->DeassertIrq(RTC_LINE, VDEV_IRQ_SOURCE_ONLY);
}

/* THE REGISTERS **************************************************************/

STDMETHODIMP
RtcDevice::NotifyIoPortRead(
    _In_ USHORT Port,
    _In_ USHORT Width,
    _Out_ ULONG *Value)
{
    bool Looked = false;

    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (Port == RTC_INDEX)
    {
        *Value = m_Address;
    }
    else
    {
        const UCHAR Where = m_Address & RTC_INDEX_MASK;

        if ((Where <= RTC_YEAR) || (Where == RTC_CENTURY))
            Stamp();

        *Value = m_Cmos[Where];

        /* Reading what happened is what clears it */
        if (Where == RTC_STATUS_C)
        {
            m_Cmos[RTC_STATUS_C] = 0;
            Looked = true;
        }
    }

    LeaveCriticalSection(&m_Lock);

    if (Looked)
        Lower();

    return S_OK;
}

STDMETHODIMP
RtcDevice::NotifyIoPortWrite(
    _In_ USHORT Port,
    _In_ USHORT Width,
    _In_ ULONG Value)
{
    const UCHAR Byte = (UCHAR)(Value & 0xFF);
    bool Stopped = false;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&m_Lock);

    if (Port == RTC_INDEX)
    {
        m_Address = Byte;
    }
    else
    {
        const UCHAR Where = m_Address & RTC_INDEX_MASK;

        /* The last two status registers are read only */
        if ((Where != RTC_STATUS_C) && (Where != RTC_STATUS_D))
            m_Cmos[Where] = Byte;

        if ((Where == RTC_STATUS_B) && !(Byte & RTC_B_PERIODIC))
            Stopped = true;
    }

    LeaveCriticalSection(&m_Lock);

    if (Stopped)
        Lower();

    return S_OK;
}

} /* namespace rtvm */
