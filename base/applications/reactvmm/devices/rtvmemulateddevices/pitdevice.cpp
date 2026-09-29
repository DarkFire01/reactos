/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The three counters a PC keeps time and makes noise with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Counted down from a crystal nobody ever changed, which is why every machine
 * of this kind agrees about how long a second is. The first counter is the
 * clock everything schedules on, the second refreshed memory on hardware that
 * needed it, and the third is wired to the speaker.
 *
 * Only the first is counted here. A guest reads the others to measure how fast
 * it is running, and answering them as though they were counting is enough for
 * that; nothing has ever cared what the second one says.
 */

#include "pitdevice.h"

namespace rtvm
{

#define PIT_COUNTER0        0x0040
#define PIT_CONTROL         0x0043
#define PIT_REGISTER_COUNT  4

/* The line the first counter is wired to */
#define PIT_LINE            0

/* What it counts down from, which no machine of this kind has ever changed */
#define PIT_FREQUENCY       1193182

/* What the control byte says */
#define PIT_SELECT_SHIFT    6
#define PIT_ACCESS_SHIFT    4
#define PIT_ACCESS_LATCH    0
#define PIT_ACCESS_LOW      1
#define PIT_ACCESS_HIGH     2
#define PIT_ACCESS_BOTH     3
#define PIT_MODE_SHIFT      1
#define PIT_MODE_MASK       7

/* Faster than this costs more in exits than it is worth to anything */
#define PIT_FASTEST         1000

/* WHAT IT IS *****************************************************************/

PitDevice::PitDevice()
{
    InitializeCriticalSection(&m_Lock);
    Clear();
}

PitDevice::~PitDevice()
{
    PowerOff();
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP PitDevice::QueryInterface(REFIID Interface, void **Object)
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
    else if (IsEqualIID(Interface, IID_IVmPitService))
    {
        *Object = static_cast<IVmPitService *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP PitDevice::GetDependencies(void *Repository, ULONG *Count,
                                        GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmIoApic,
        &IID_IVmTimeSource
    };

    UNREFERENCED_PARAMETER(Repository);

    const HRESULT Status = PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                                               Count, Services, Optional);

    /* Nothing here keeps time from anywhere but the host, so the last is spare */
    if (SUCCEEDED(Status) && (Optional != nullptr))
        *Optional = 1u << 2;

    return Status;
}

STDMETHODIMP PitDevice::StartReservingResources()
{
    return ReservePorts(PIT_COUNTER0, PIT_COUNTER0 + PIT_REGISTER_COUNT - 1,
                        this);
}

void PitDevice::Clear()
{
    memset(m_Counter, 0, sizeof(m_Counter));

    for (Counter &One : m_Counter)
        One.Access = PIT_ACCESS_BOTH;

    m_Hertz = 18;
    m_Speaker = false;
}

STDMETHODIMP PitDevice::Reset()
{
    EnterCriticalSection(&m_Lock);
    Clear();
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

/* KEEPING TIME ***************************************************************/

STDMETHODIMP PitDevice::PowerOnCold()
{
    /*
     * Where its line goes, asked for now rather than when it was initialised:
     * the thing that decides is a device too, and one device coming up says
     * nothing about whether another already has.
     */
    if (m_Lines == nullptr)
        FindService(IID_IVmIoApic, reinterpret_cast<void **>(&m_Lines));

    if (m_Thread != nullptr)
        return S_OK;

    InterlockedExchange(&m_Stopping, 0);

    m_Thread = CreateThread(nullptr, 0, Ticking, this, 0, nullptr);

    return (m_Thread != nullptr) ? S_OK : E_FAIL;
}

STDMETHODIMP PitDevice::PowerOff()
{
    if (m_Thread == nullptr)
        return S_OK;

    InterlockedExchange(&m_Stopping, 1);
    WaitForSingleObject(m_Thread, 2000);
    CloseHandle(m_Thread);
    m_Thread = nullptr;

    if (m_Lines != nullptr)
    {
        m_Lines->Release();
        m_Lines = nullptr;
    }

    return S_OK;
}

DWORD WINAPI PitDevice::Ticking(LPVOID Parameter)
{
    auto *Self = static_cast<PitDevice *>(Parameter);

    while (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) == 0)
    {
        LONG Hertz = InterlockedCompareExchange(&Self->m_Hertz, 0, 0);

        if (Hertz <= 0)
            Hertz = 18;

        DWORD Wait = 1000u / (ULONG)Hertz;

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
 * One count reaching zero, which is the only thing anything waits for.
 *
 * @remarks
 * Raised and let go at once. What the line is triggered by is an edge, so a
 * guest that is slow to answer still gets exactly one interrupt rather than a
 * line held up until it looks.
 */
void PitDevice::Tick()
{
    if (m_Lines == nullptr)
        return;

    m_Lines->AssertIrq(PIT_LINE);
    m_Lines->DeassertIrq(PIT_LINE);
}

/* How often the first counter was set up to fire */
void PitDevice::RateChanged()
{
    ULONG Reload = m_Counter[0].Reload;

    /* Zero means the whole range, which is the slowest it will go */
    if (Reload == 0)
        Reload = 65536;

    ULONG Hertz = PIT_FREQUENCY / Reload;

    if (Hertz == 0)
        Hertz = 1;

    if (Hertz > PIT_FASTEST)
        Hertz = PIT_FASTEST;

    InterlockedExchange(&m_Hertz, (LONG)Hertz);
}

/* WHAT A COUNTER IS DOING ****************************************************/

STDMETHODIMP PitDevice::EnableSpeakerTimer(BOOL Enabled)
{
    EnterCriticalSection(&m_Lock);
    m_Speaker = (Enabled != FALSE);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

/**
 * @brief
 * Whether a counter's output is high, which is what something wired to one
 * reads instead of counting itself.
 *
 * @remarks
 * Nothing here counts between ticks, so the answer is worked out from the
 * clock rather than from a count: a counter of a given reload is high for the
 * second half of each of its periods. That is what the third one is asked for,
 * and it is asked to find out how loud rather than how long.
 */
STDMETHODIMP PitDevice::GetTimerOutputSignal(ULONG Counter, BOOL *High)
{
    if ((Counter >= ARRAYSIZE(m_Counter)) || (High == nullptr))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    ULONG Reload = m_Counter[Counter].Reload;

    LeaveCriticalSection(&m_Lock);

    if (Reload == 0)
        Reload = 65536;

    const ULONGLONG Ticks = (ULONGLONG)GetTickCount() * (PIT_FREQUENCY / 1000);

    *High = ((Ticks % Reload) >= (Reload / 2)) ? TRUE : FALSE;
    return S_OK;
}

/* THE REGISTERS **************************************************************/

STDMETHODIMP PitDevice::NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value)
{
    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    *Value = 0;

    if (Port == PIT_CONTROL)
        return S_OK;

    EnterCriticalSection(&m_Lock);

    Counter &One = m_Counter[Port - PIT_COUNTER0];

    /*
     * Never the same twice. A guest measuring how fast it runs counts how far
     * this moves between two reads, and one that never moved would be taken
     * for a machine of no speed at all.
     */
    USHORT Now = One.Latch
               ? One.Latched
               : (USHORT)(GetTickCount() * (PIT_FREQUENCY / 1000));

    switch (One.Access)
    {
        case PIT_ACCESS_LOW:
            *Value = Now & 0xFF;
            break;

        case PIT_ACCESS_HIGH:
            *Value = (Now >> 8) & 0xFF;
            break;

        default:
            *Value = One.ReadHigh ? ((Now >> 8) & 0xFF) : (Now & 0xFF);
            One.ReadHigh = !One.ReadHigh;

            /* Both halves of a latched count taken is what releases it */
            if (!One.ReadHigh)
                One.Latch = false;

            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP PitDevice::NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value)
{
    const UCHAR Byte = (UCHAR)(Value & 0xFF);

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&m_Lock);

    if (Port == PIT_CONTROL)
    {
        const ULONG Which = (Byte >> PIT_SELECT_SHIFT) & 3;
        const ULONG Access = (Byte >> PIT_ACCESS_SHIFT) & 3;

        /* The one that reads back the whole chip, which nothing here answers */
        if (Which == 3)
        {
            LeaveCriticalSection(&m_Lock);
            return S_OK;
        }

        Counter &One = m_Counter[Which];

        if (Access == PIT_ACCESS_LATCH)
        {
            /* Held still so that both halves of it agree with each other */
            One.Latched = (USHORT)(GetTickCount() * (PIT_FREQUENCY / 1000));
            One.Latch = true;
            One.ReadHigh = false;
        }
        else
        {
            One.Access = (UCHAR)Access;
            One.Mode = (UCHAR)((Byte >> PIT_MODE_SHIFT) & PIT_MODE_MASK);
            One.Latch = false;
            One.ReadHigh = false;
            One.WriteHigh = false;
            One.Running = false;
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    Counter &One = m_Counter[Port - PIT_COUNTER0];

    switch (One.Access)
    {
        case PIT_ACCESS_LOW:
            One.Reload = (USHORT)((One.Reload & 0xFF00) | Byte);
            One.Running = true;
            break;

        case PIT_ACCESS_HIGH:
            One.Reload = (USHORT)((One.Reload & 0x00FF) | (Byte << 8));
            One.Running = true;
            break;

        default:
            if (One.WriteHigh)
            {
                One.Reload = (USHORT)((One.Reload & 0x00FF) | (Byte << 8));
                One.Running = true;
            }
            else
            {
                One.Reload = (USHORT)((One.Reload & 0xFF00) | Byte);
            }

            One.WriteHigh = !One.WriteHigh;
            break;
    }

    const bool First = ((Port - PIT_COUNTER0) == 0) && One.Running;

    LeaveCriticalSection(&m_Lock);

    if (First)
        RateChanged();

    return S_OK;
}

} /* namespace rtvm */
