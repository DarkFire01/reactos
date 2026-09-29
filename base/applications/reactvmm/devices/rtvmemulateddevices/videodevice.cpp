/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The display, and the page the guest writes it into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The page is not the guest's memory. It is this device's, and every character
 * the guest writes arrives as an access to a window it does not have, which is
 * what a display has always been: something that remembers what was put in it
 * and shows it, rather than a corner of memory that happens to be looked at.
 *
 * Nothing is handed to whatever draws it. This says which part of the page
 * stopped being what it was, and whatever is drawing comes back for as much of
 * it as it wants, when it is ready.
 */

#include "videodevice.h"

namespace rtvm
{

/* The two that say where the cursor is, and the two that say where the page is */
#define CRTC_START_HIGH     0x0C
#define CRTC_START_LOW      0x0D
#define CRTC_CURSOR_HIGH    0x0E
#define CRTC_CURSOR_LOW     0x0F

#define VIDEO_CRTC_ADDRESS  0x03D4
#define VIDEO_CRTC_DATA     0x03D5
#define VIDEO_STATUS        0x03DA

/* How often whatever is drawing is told, at most */
#define VIDEO_TELL_EVERY    40

/* WHAT IT IS *****************************************************************/

VideoDevice::VideoDevice()
{
    InitializeCriticalSection(&m_Lock);
    Reset();
}

VideoDevice::~VideoDevice()
{
    PowerOff();
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP VideoDevice::QueryInterface(REFIID Interface, void **Object)
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
    else if (IsEqualIID(Interface, IID_IVndMmioHandler))
    {
        *Object = static_cast<IVndMmioHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVideoVdev))
    {
        *Object = static_cast<IVideoVdev *>(this);
    }
    else if (IsEqualIID(Interface, IID_IRtvmTextSurface))
    {
        *Object = static_cast<IRtvmTextSurface *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP VideoDevice::GetDependencies(void *Repository, ULONG *Count,
                                          GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IMonitorDevice
    };

    UNREFERENCED_PARAMETER(Repository);

    const HRESULT Status = PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                                               Count, Services, Optional);

    /* A machine with nothing looking at it still has a page to write into */
    if (SUCCEEDED(Status) && (Optional != nullptr))
        *Optional = 1u << 1;

    return Status;
}

STDMETHODIMP VideoDevice::StartReservingResources()
{
    HRESULT Status = ReservePorts(VIDEO_CRTC_ADDRESS, VIDEO_CRTC_DATA, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(VIDEO_STATUS, VIDEO_STATUS, this);

    if (FAILED(Status))
        return Status;

    /* The page, which is this device's rather than anything the guest has */
    if (Emulation() == nullptr)
        return E_UNEXPECTED;

    void *Registration = nullptr;

    return Emulation()->RegisterMmioHandler(VIDEO_TEXT_BASE / VDEV_PAGE_SIZE,
                                            VIDEO_TEXT_SIZE / VDEV_PAGE_SIZE,
                                            this, TRUE, &Registration);
}

STDMETHODIMP VideoDevice::Reset()
{
    EnterCriticalSection(&m_Lock);

    /* Every cell a space in the colour a screen comes up in */
    for (ULONG Index = 0; Index < VIDEO_TEXT_SIZE; Index += VIDEO_CELL_SIZE)
    {
        m_Text[Index] = ' ';
        m_Text[Index + 1] = 0x07;
    }

    memset(m_Crtc, 0, sizeof(m_Crtc));
    m_CrtcAddress = 0;
    m_Retrace = 0;
    m_Dirty = true;

    m_Changed.left = 0;
    m_Changed.top = 0;
    m_Changed.right = VIDEO_COLUMNS;
    m_Changed.bottom = VIDEO_ROWS;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP VideoDevice::PowerOnCold()
{
    if (m_Monitor == nullptr)
        FindService(IID_IMonitorDevice, reinterpret_cast<void **>(&m_Monitor));

    if (m_Thread != nullptr)
        return S_OK;

    InterlockedExchange(&m_Stopping, 0);

    m_Thread = CreateThread(nullptr, 0, Telling, this, 0, nullptr);

    return (m_Thread != nullptr) ? S_OK : E_FAIL;
}

STDMETHODIMP VideoDevice::PowerOff()
{
    if (m_Thread != nullptr)
    {
        InterlockedExchange(&m_Stopping, 1);
        WaitForSingleObject(m_Thread, 2000);
        CloseHandle(m_Thread);
        m_Thread = nullptr;
    }

    if (m_Monitor != nullptr)
    {
        m_Monitor->Release();
        m_Monitor = nullptr;
    }

    return S_OK;
}

/* SAYING WHAT MOVED **********************************************************/

/**
 * @brief
 * Widens what is owed to whatever is drawing so that it takes in this write.
 *
 * @remarks
 * One rectangle rather than a list of them. A guest clearing a screen writes
 * two thousand cells one at a time, and a display told two thousand times is a
 * display that spends its whole life being told.
 */
void VideoDevice::Soil(ULONG64 Address, ULONG Length)
{
    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;
    const ULONG First = (ULONG)(Offset / VIDEO_CELL_SIZE);
    const ULONG Last = (ULONG)((Offset + Length - 1) / VIDEO_CELL_SIZE);

    if (First >= (VIDEO_COLUMNS * VIDEO_ROWS))
        return;

    const LONG FirstRow = (LONG)(First / VIDEO_COLUMNS);
    const LONG LastRow = (LONG)(Last / VIDEO_COLUMNS);

    if (!m_Dirty)
    {
        m_Changed.left = (LONG)(First % VIDEO_COLUMNS);
        m_Changed.top = FirstRow;
        m_Changed.right = (LONG)(Last % VIDEO_COLUMNS) + 1;
        m_Changed.bottom = LastRow + 1;
        m_Dirty = true;
        return;
    }

    if (FirstRow < m_Changed.top)
        m_Changed.top = FirstRow;

    if ((LastRow + 1) > m_Changed.bottom)
        m_Changed.bottom = LastRow + 1;

    /* Once it covers more than one row, the columns are the whole width */
    if (m_Changed.bottom > (m_Changed.top + 1))
    {
        m_Changed.left = 0;
        m_Changed.right = VIDEO_COLUMNS;
        return;
    }

    const LONG Left = (LONG)(First % VIDEO_COLUMNS);
    const LONG Right = (LONG)(Last % VIDEO_COLUMNS) + 1;

    if (Left < m_Changed.left)
        m_Changed.left = Left;

    if (Right > m_Changed.right)
        m_Changed.right = Right;
}

DWORD WINAPI VideoDevice::Telling(LPVOID Parameter)
{
    auto *Self = static_cast<VideoDevice *>(Parameter);

    while (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) == 0)
    {
        Sleep(VIDEO_TELL_EVERY);

        if (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) != 0)
            break;

        Self->Tell();
    }

    return 0;
}

void VideoDevice::Tell()
{
    RECT Changed;

    EnterCriticalSection(&m_Lock);

    if (!m_Dirty)
    {
        LeaveCriticalSection(&m_Lock);
        return;
    }

    Changed = m_Changed;
    m_Dirty = false;

    LeaveCriticalSection(&m_Lock);

    if (m_Monitor != nullptr)
        m_Monitor->OnVideoDirt(&Changed);
}

/* WHAT IS DRAWN **************************************************************/

STDMETHODIMP VideoDevice::IsVideoEnabled(BOOL *Enabled)
{
    if (Enabled == nullptr)
        return E_POINTER;

    /* Nothing here has a way of being turned off */
    *Enabled = TRUE;
    return S_OK;
}

STDMETHODIMP VideoDevice::Activate()
{
    return S_OK;
}

STDMETHODIMP VideoDevice::GetSurfaceData(VDEV_SURFACE_DATA *Surface)
{
    if (Surface == nullptr)
        return E_POINTER;

    /* Counted in characters, because that is what this surface holds */
    Surface->Format = VIDEO_CELL_SIZE;
    Surface->Width = VIDEO_COLUMNS;
    Surface->Height = VIDEO_ROWS;
    Surface->Pitch = VIDEO_COLUMNS * VIDEO_CELL_SIZE;

    return S_OK;
}

STDMETHODIMP VideoDevice::ReadCells(void *Cells, ULONG Length,
                                    ULONG *CursorColumn, ULONG *CursorRow)
{
    if ((Cells == nullptr) || (Length > VIDEO_PAGE_SIZE))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    memcpy(Cells, m_Text, Length);

    const ULONG Where = ((ULONG)m_Crtc[CRTC_CURSOR_HIGH] << 8) |
                        m_Crtc[CRTC_CURSOR_LOW];

    LeaveCriticalSection(&m_Lock);

    if (CursorColumn != nullptr)
        *CursorColumn = Where % VIDEO_COLUMNS;

    if (CursorRow != nullptr)
        *CursorRow = Where / VIDEO_COLUMNS;

    return S_OK;
}

/* THE PAGE, AND THE REGISTERS ************************************************/

STDMETHODIMP VideoDevice::NotifyMmioRead(ULONG64 Address, ULONG Length,
                                         void *Buffer)
{
    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;

    if ((Buffer == nullptr) || ((Offset + Length) > VIDEO_TEXT_SIZE))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);
    memcpy(Buffer, &m_Text[Offset], Length);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP VideoDevice::NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                          const void *Buffer)
{
    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;

    if ((Buffer == nullptr) || ((Offset + Length) > VIDEO_TEXT_SIZE))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);
    memcpy(&m_Text[Offset], Buffer, Length);
    Soil(Address, Length);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP VideoDevice::NotifyIoPortRead(USHORT Port, ULONG Width,
                                           ULONG *Value)
{
    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (Port == VIDEO_STATUS)
    {
        /*
         * Something waiting for the beam to come back round. Nothing here has
         * a beam, so the bits it watches are simply never the same twice: a
         * guest that waited for a change it never saw would wait for good.
         */
        m_Retrace ^= 0x09;
        *Value = m_Retrace;
    }
    else if (Port == VIDEO_CRTC_DATA)
    {
        *Value = m_Crtc[m_CrtcAddress & 0x1F];
    }
    else
    {
        *Value = m_CrtcAddress;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP VideoDevice::NotifyIoPortWrite(USHORT Port, ULONG Width,
                                            ULONG Value)
{
    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&m_Lock);

    if (Port == VIDEO_CRTC_ADDRESS)
    {
        m_CrtcAddress = (UCHAR)(Value & 0x1F);
    }
    else if (Port == VIDEO_CRTC_DATA)
    {
        m_Crtc[m_CrtcAddress & 0x1F] = (UCHAR)Value;

        /* Where the cursor is drawn is part of what is on the screen */
        if ((m_CrtcAddress == CRTC_CURSOR_HIGH) ||
            (m_CrtcAddress == CRTC_CURSOR_LOW))
        {
            m_Dirty = true;
        }
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

} /* namespace rtvm */
